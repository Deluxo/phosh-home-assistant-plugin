#include "home-assistant-quick-setting.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define HOME_ASSISTANT_QUICK_SETTING_SCHEMA "mobi.phosh.plugins.home-assistant-quick-setting"
#define BASE_URL_KEY                        "base-url"
#define TOKEN_KEY                           "token"
#define ENTITY_ID_KEY                       "entity-id"
#define ENTITIES_KEY                        "entities"
#define CACHE_KEY                           "cache"

#define HA_ON_ICON    "home-assistant-quick-setting-symbolic"
#define HA_OFF_ICON   "home-assistant-quick-setting-symbolic"

/* In-flight requests are aborted after this many seconds so an unreachable
 * server can never leave a row stuck in its busy state. */
#define REQUEST_TIMEOUT_SECONDS 3
/* Transient errors on the tile info label clear back to the computed state
 * after this many seconds without any new failure. */
#define ERROR_DISPLAY_SECONDS   5
/* After a successful service call, re-fetch the states after this many
 * seconds (debounced) so every row shows what the server actually has —
 * group calls only report member states, and other rows may have changed. */
#define POST_REQUEST_REFRESH_SECONDS 2

/* Domains handled by the desired-state services below. */
static const char *toggleable_domains[] = {
  "automation", "cover", "fan", "humidifier", "input_boolean", "light",
  "lock", "media_player", "remote", "script", "switch", "vacuum",
  NULL,
};

typedef struct {
  const char *domain;
  const char *on_service;
  const char *off_service;
} DomainAction;

/* Writes converge on a desired state, so a retry never flips twice. */
static const DomainAction domain_actions[] = {
  { "automation",    "turn_on",    "turn_off" },
  { "cover",         "open_cover", "close_cover" },
  { "fan",           "turn_on",    "turn_off" },
  { "humidifier",    "turn_on",    "turn_off" },
  { "input_boolean", "turn_on",    "turn_off" },
  { "light",         "turn_on",    "turn_off" },
  { "lock",          "unlock",     "lock" },
  { "media_player",  "turn_on",    "turn_off" },
  { "remote",        "turn_on",    "turn_off" },
  { "script",        "turn_on",    "turn_off" },
  { "switch",        "turn_on",    "turn_off" },
  { "vacuum",        "start",      "stop" },
  { NULL, NULL, NULL },
};

typedef struct {
  char *entity_id;
  char *friendly_name;
  char *state;
} HaEntity;

typedef struct {
  PhoshHomeAssistantQuickSetting *self;
  SoupMessage                    *msg;
  GCancellable                   *cancellable;
  guint                           timeout_id;
  gboolean                        timed_out;
} FetchData;

typedef struct {
  PhoshHomeAssistantQuickSetting *self;
  SoupMessage                    *msg;
  char                           *entity_id;
  gboolean                        old_on;
  GCancellable                   *cancellable;
  guint                           timeout_id;
  gboolean                        timed_out;
} RequestData;

struct _PhoshHomeAssistantQuickSetting {
  PhoshQuickSetting        parent;

  PhoshStatusIcon         *info;
  PhoshStatusPage         *status_page;
  GtkListBox              *entity_list;

  SoupSession             *session;
  GSettings               *settings;

  char                    *base_url;
  char                    *token;
  char                    *entity_id;
  char                   **entities;

  GList                   *states;       /* ordered HaEntity list from cache/API */
  GHashTable              *rows;         /* entity_id -> GtkSwitch* */
  GHashTable              *busy;         /* entity_id -> entity_id */
  gboolean                 states_in_flight;
  gboolean                 states_rerun;
  gboolean                 cache_loaded;
  char                    *last_cache;
  guint                    conn_timeout;
  guint                    error_flash_id;        /* pending "Error" reset, 0 = none */
  guint                    post_request_refresh_id; /* debounced re-fetch after a service call, 0 = none */
  gboolean                 main_is_on;
};

G_DEFINE_TYPE (PhoshHomeAssistantQuickSetting,
               phosh_home_assistant_quick_setting,
               PHOSH_TYPE_QUICK_SETTING);


static gboolean on_row_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data);
static gboolean on_row_button_press (GtkWidget *row, GdkEventButton *event, gpointer user_data);


static void
ha_entity_free (HaEntity *entity)
{
  g_free (entity->entity_id);
  g_free (entity->friendly_name);
  g_free (entity->state);
  g_free (entity);
}


static gboolean
ha_domain_toggleable (const char *entity_id)
{
  g_autofree char *domain = NULL;
  const char *dot;

  dot = strchr (entity_id, '.');
  if (dot == NULL)
    return FALSE;
  domain = g_strndup (entity_id, dot - entity_id);

  return g_strv_contains (toggleable_domains, domain);
}


static const DomainAction *
ha_domain_action (const char *entity_id)
{
  g_autofree char *domain = NULL;
  const char *dot;

  dot = strchr (entity_id, '.');
  if (dot == NULL)
    return NULL;
  domain = g_strndup (entity_id, dot - entity_id);

  for (guint i = 0; domain_actions[i].domain != NULL; i++) {
    if (g_str_equal (domain_actions[i].domain, domain))
      return &domain_actions[i];
  }
  return NULL;
}


/* Display mapping: states that mean "active" for a switch. Covers
 * media players (playing/paused/idle), covers (open) and locks. */
static gboolean
ha_state_is_on (const char *state)
{
  static const char *on_states[] = {
    "on", "playing", "paused", "idle", "buffering",
    "open", "opening", "closing", "locked", "cleaning", "returning",
    NULL,
  };

  return state != NULL && g_strv_contains (on_states, state);
}


/* Parse a /api/states body into a GList of HaEntity*, filtered to
 * toggleable domains. Sets *ok to FALSE on parse error (empty but valid
 * responses yield an empty list with *ok TRUE). */
static GList *
ha_parse_states (const char *text, gssize len, gboolean *ok)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) err = NULL;
  JsonNode *root;
  JsonArray *array;
  GList *entities = NULL;
  GHashTable *seen = NULL;
  guint n;

  *ok = FALSE;
  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, text, len, &err)) {
    g_warning ("Failed to parse HA states: %s", err->message);
    return NULL;
  }

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root)) {
    g_warning ("HA states response is not a JSON array");
    return NULL;
  }

  array = json_node_get_array (root);
  n = json_array_get_length (array);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < n; i++) {
    JsonObject *obj = json_array_get_object_element (array, i);
    const char *entity_id;
    const char *state;
    JsonObject *attrs;
    const char *friendly;
    HaEntity *entity;

    if (obj == NULL)
      continue;
    if (!json_object_has_member (obj, "entity_id") ||
        !json_object_has_member (obj, "state"))
      continue;

    entity_id = json_object_get_string_member (obj, "entity_id");
    state = json_object_get_string_member (obj, "state");
    if (entity_id == NULL || !ha_domain_toggleable (entity_id))
      continue;

    /* Canonical lowercase id: HA treats ids case-insensitively, and a
     * case twin would produce two rows for one device. */
    char *id = g_ascii_strdown (entity_id, -1);
    if (g_hash_table_contains (seen, id)) {
      g_free (id);
      continue;
    }
    /* The hash owns its own copy; the entity keeps `id`. */
    g_hash_table_add (seen, g_strdup (id));

    friendly = entity_id;
    if (json_object_has_member (obj, "attributes")) {
      attrs = json_object_get_object_member (obj, "attributes");
      if (attrs != NULL && json_object_has_member (attrs, "friendly_name")) {
        const char *name = json_object_get_string_member (attrs, "friendly_name");
        if (name != NULL && name[0] != '\0')
          friendly = name;
      }
    }

    entity = g_new0 (HaEntity, 1);
    entity->entity_id = id;
    entity->friendly_name = g_strdup (friendly);
    entity->state = g_strdup (state ? state : "unknown");
    entities = g_list_prepend (entities, entity);
  }

  *ok = TRUE;
  g_hash_table_unref (seen);
  return g_list_reverse (entities);
}


static HaEntity *
find_state (PhoshHomeAssistantQuickSetting *self, const char *entity_id)
{
  for (GList *l = self->states; l != NULL; l = l->next) {
    HaEntity *entity = l->data;
    if (g_strcmp0 (entity->entity_id, entity_id) == 0)
      return entity;
  }
  return NULL;
}


static void
clear_states (PhoshHomeAssistantQuickSetting *self)
{
  g_list_free_full (self->states, (GDestroyNotify) ha_entity_free);
  self->states = NULL;
}


static void
set_state_string (PhoshHomeAssistantQuickSetting *self,
                  const char                      *entity_id,
                  const char                      *state)
{
  HaEntity *entity = find_state (self, entity_id);

  if (entity == NULL)
    return;
  g_free (entity->state);
  entity->state = g_strdup (state);
}


static void
fetch_data_free (FetchData *data)
{
  /* The completion handler always runs, so the timer is either already
   * fired (id cleared by its callback) or still pending. */
  if (data->timeout_id != 0)
    g_source_remove (data->timeout_id);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->msg);
  g_clear_object (&data->self);
  g_free (data);
}


static void
request_data_free (RequestData *data)
{
  if (data->timeout_id != 0)
    g_source_remove (data->timeout_id);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->msg);
  g_clear_object (&data->self);
  g_free (data->entity_id);
  g_free (data);
}


static gboolean
cache_load (PhoshHomeAssistantQuickSetting *self)
{
  g_autofree char *cache = NULL;
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) err = NULL;
  JsonNode *root;
  JsonObject *object;
  JsonArray *items;
  const char *cached_url = NULL;
  GList *states = NULL;
  GHashTable *seen = NULL;

  self->cache_loaded = FALSE;

  cache = g_settings_get_string (self->settings, CACHE_KEY);
  if (cache == NULL || cache[0] == '\0')
    return FALSE;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, cache, -1, &err)) {
    g_warning ("Ignoring invalid entity cache: %s", err->message);
    return FALSE;
  }

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return FALSE;
  object = json_node_get_object (root);

  if (json_object_has_member (object, "base-url"))
    cached_url = json_object_get_string_member (object, "base-url");
  if (cached_url != NULL && cached_url[0] != '\0' &&
      self->base_url != NULL && self->base_url[0] != '\0' &&
      !g_str_equal (cached_url, self->base_url)) {
    clear_states (self);
    return FALSE;
  }

  if (!json_object_has_member (object, "items"))
    return FALSE;
  items = json_object_get_array_member (object, "items");
  if (items == NULL)
    return FALSE;

  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < json_array_get_length (items); i++) {
    JsonObject *item = json_array_get_object_element (items, i);
    const char *id;
    const char *name;
    const char *state;
    HaEntity *entity;

    if (item == NULL || !json_object_has_member (item, "id"))
      continue;

    id = json_object_get_string_member (item, "id");
    if (id == NULL || id[0] == '\0')
      continue;

    /* Same canonicalization as ha_parse_states(): one row per device. */
    char *canon = g_ascii_strdown (id, -1);
    if (g_hash_table_contains (seen, canon)) {
      g_free (canon);
      continue;
    }
    /* The hash owns its own copy; the entity keeps `canon`. */
    g_hash_table_add (seen, g_strdup (canon));

    name = json_object_has_member (item, "name")
      ? json_object_get_string_member (item, "name")
      : id;
    state = json_object_has_member (item, "state")
      ? json_object_get_string_member (item, "state")
      : "unknown";

    entity = g_new0 (HaEntity, 1);
    entity->entity_id = canon;
    entity->friendly_name = g_strdup (name != NULL && name[0] != '\0' ? name : id);
    entity->state = g_strdup (state != NULL ? state : "unknown");
    states = g_list_prepend (states, entity);
  }
  g_hash_table_unref (seen);

  clear_states (self);
  self->states = g_list_reverse (states);
  self->cache_loaded = TRUE;

  g_free (self->last_cache);
  self->last_cache = g_strdup (cache);
  return TRUE;
}


static void
cache_save (PhoshHomeAssistantQuickSetting *self)
{
  g_autoptr (JsonBuilder) builder = NULL;
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *data = NULL;
  g_autofree char *old = NULL;

  if (self->base_url == NULL || self->base_url[0] == '\0')
    return;

  builder = json_builder_new ();
  json_builder_begin_object (builder);

  json_builder_set_member_name (builder, "v");
  json_builder_add_int_value (builder, 1);
  json_builder_set_member_name (builder, "base-url");
  json_builder_add_string_value (builder, self->base_url);
  json_builder_set_member_name (builder, "items");
  json_builder_begin_array (builder);
  for (GList *l = self->states; l != NULL; l = l->next) {
    HaEntity *entity = l->data;

    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "id");
    json_builder_add_string_value (builder, entity->entity_id);
    json_builder_set_member_name (builder, "name");
    json_builder_add_string_value (builder, entity->friendly_name);
    json_builder_set_member_name (builder, "state");
    json_builder_add_string_value (builder, entity->state);
    json_builder_end_object (builder);
  }
  json_builder_end_array (builder);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  data = json_to_string (root, FALSE);

  old = g_settings_get_string (self->settings, CACHE_KEY);
  if (g_strcmp0 (old, data) == 0)
    return;

  g_free (self->last_cache);
  self->last_cache = g_strdup (data);
  g_settings_set_string (self->settings, CACHE_KEY, data);
  self->cache_loaded = TRUE;
}


static void
phosh_home_assistant_quick_setting_read_settings (PhoshHomeAssistantQuickSetting *self)
{
  g_autofree char *raw_url = NULL;
  g_autofree char *raw_token = NULL;
  g_autofree char *raw_entity = NULL;
  gsize len;

  raw_url = g_settings_get_string (self->settings, BASE_URL_KEY);
  g_strstrip (raw_url);
  while ((len = strlen (raw_url)) > 0 && raw_url[len - 1] == '/')
    raw_url[len - 1] = '\0';
  g_free (self->base_url);
  self->base_url = g_steal_pointer (&raw_url);

  g_free (self->token);
  raw_token = g_settings_get_string (self->settings, TOKEN_KEY);
  g_strstrip (raw_token);
  if (raw_token && !g_str_equal (raw_token, ""))
    self->token = g_strdup_printf ("Bearer %s", raw_token);
  else
    self->token = g_strdup ("");

  raw_entity = g_settings_get_string (self->settings, ENTITY_ID_KEY);
  g_strstrip (raw_entity);
  if (raw_entity[0] != '\0')
    g_ascii_strdown (raw_entity, -1);
  g_free (self->entity_id);
  self->entity_id = g_steal_pointer (&raw_entity);

  /* Owned, trimmed, lowercased copy of the configured entity list. */
  g_clear_pointer (&self->entities, g_strfreev);
  {
    char **raw_entities = g_settings_get_strv (self->settings, ENTITIES_KEY);
    GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);

    for (char **it = raw_entities; *it != NULL; it++) {
      char *id = g_strdup (*it);
      g_strstrip (id);
      if (id[0] != '\0')
        g_ptr_array_add (ids, g_ascii_strdown (id, -1));
      g_free (id);
    }
    g_free (raw_entities);

    if (ids->len > 0) {
      g_ptr_array_add (ids, NULL);
      self->entities = (char **) ids->pdata;
      g_ptr_array_free (ids, FALSE);
    } else {
      g_ptr_array_free (ids, TRUE);
    }
  }
}


static gboolean
can_connect (PhoshHomeAssistantQuickSetting *self)
{
  return self->base_url && self->token &&
    !g_str_equal (self->base_url, "") &&
    !g_str_equal (self->token, "");
}


static gboolean
has_main_entity (PhoshHomeAssistantQuickSetting *self)
{
  return can_connect (self) && self->entity_id &&
    !g_str_equal (self->entity_id, "");
}


static gboolean
entities_missing_from_cache (PhoshHomeAssistantQuickSetting *self)
{
  if (self->entities == NULL)
    return FALSE;

  for (guint i = 0; self->entities[i] != NULL; i++) {
    if (find_state (self, self->entities[i]) == NULL)
      return TRUE;
  }
  return FALSE;
}


static gboolean
strv_equal (char **a, char **b)
{
  gboolean a_empty = a == NULL || a[0] == NULL;
  gboolean b_empty = b == NULL || b[0] == NULL;

  if (a_empty || b_empty)
    return a_empty && b_empty;
  return g_strv_equal ((const char * const *) a, (const char * const *) b);
}


static void refresh_states (PhoshHomeAssistantQuickSetting *self);


static void
phosh_home_assistant_quick_setting_update_icon (PhoshHomeAssistantQuickSetting *self,
                                                const char                     *state)
{
  gboolean is_on = ha_state_is_on (state);

  self->main_is_on = is_on;
  phosh_quick_setting_set_active (PHOSH_QUICK_SETTING (self), is_on);

  if (is_on)
    phosh_status_icon_set_info (self->info, _("On"));
  else
    phosh_status_icon_set_info (self->info, _("Off"));
}


static void
update_tile_from_cache (PhoshHomeAssistantQuickSetting *self)
{
  if (has_main_entity (self)) {
    HaEntity *entity = find_state (self, self->entity_id);

    if (entity != NULL) {
      phosh_home_assistant_quick_setting_update_icon (self, entity->state);
    } else if (self->cache_loaded) {
      phosh_quick_setting_set_active (PHOSH_QUICK_SETTING (self), FALSE);
      phosh_status_icon_set_info (self->info, _("Unavailable"));
    }
    return;
  }

  guint n_on = 0;
  guint n_total = 0;

  if (self->entities != NULL) {
    for (guint i = 0; self->entities[i] != NULL; i++) {
      HaEntity *entity = find_state (self, self->entities[i]);
      if (entity == NULL)
        continue;
      n_total++;
      if (ha_state_is_on (entity->state))
        n_on++;
    }
  } else {
    for (GList *l = self->states; l != NULL; l = l->next) {
      HaEntity *entity = l->data;
      n_total++;
      if (ha_state_is_on (entity->state))
        n_on++;
    }
  }

  phosh_quick_setting_set_active (PHOSH_QUICK_SETTING (self), n_on > 0);
  if (n_total == 0) {
    phosh_status_icon_set_info (self->info, _("No entities"));
  } else if (n_on > 0) {
    g_autofree char *info = g_strdup_printf ("%u on", n_on);
    phosh_status_icon_set_info (self->info, info);
  } else {
    phosh_status_icon_set_info (self->info, _("All off"));
  }
}


static void
clear_entity_list (PhoshHomeAssistantQuickSetting *self)
{
  g_autoptr (GList) children = NULL;

  g_hash_table_remove_all (self->rows);
  children = gtk_container_get_children (GTK_CONTAINER (self->entity_list));
  for (GList *l = children; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
}


static void
add_entity_row (PhoshHomeAssistantQuickSetting *self,
                const char                     *entity_id,
                const char                     *name,
                HaEntity                       *state)
{
  GtkWidget *row;
  GtkWidget *box;
  GtkWidget *label;
  GtkWidget *sw;
  g_autofree char *title = NULL;

  if (g_hash_table_contains (self->rows, entity_id))
    return;

  row = gtk_list_box_row_new ();
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  if (state != NULL)
    title = g_strdup (name != NULL && name[0] != '\0' ? name : entity_id);
  else
    title = g_strdup_printf ("%s (unavailable)", entity_id);

  label = gtk_label_new (title);
  sw = gtk_switch_new ();

  g_object_set (box,
                "margin-start", 12,
                "margin-end", 12,
                "margin-top", 6,
                "margin-bottom", 6,
                NULL);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);
  gtk_switch_set_active (GTK_SWITCH (sw),
                         state != NULL && ha_state_is_on (state->state));
  /* A switch is only ever disabled while its own request is in flight or
   * the plugin cannot connect — never because the state is unknown. */
  gtk_widget_set_sensitive (sw,
                            can_connect (self) &&
                            !g_hash_table_contains (self->busy, entity_id));

  g_object_set_data_full (G_OBJECT (sw),
                          "entity-id",
                          g_strdup (entity_id),
                          g_free);
  g_object_set_data (G_OBJECT (row), "switch", sw);
  g_signal_connect (sw, "state-set", G_CALLBACK (on_row_switch_toggled), self);
  g_signal_connect (row, "button-press-event", G_CALLBACK (on_row_button_press), self);

  g_hash_table_replace (self->rows, g_strdup (entity_id), sw);

  gtk_container_add (GTK_CONTAINER (box), label);
  gtk_container_add (GTK_CONTAINER (box), sw);
  gtk_container_add (GTK_CONTAINER (row), box);
  gtk_widget_show_all (row);
  gtk_container_add (GTK_CONTAINER (self->entity_list), row);
}


static void
rebuild_entity_list (PhoshHomeAssistantQuickSetting *self)
{
  guint added = 0;

  clear_entity_list (self);

  if (self->entities != NULL) {
    for (guint i = 0; self->entities[i] != NULL; i++) {
      HaEntity *state = find_state (self, self->entities[i]);

      add_entity_row (self,
                      self->entities[i],
                      state != NULL ? state->friendly_name : self->entities[i],
                      state);
      added++;
    }
  } else {
    for (GList *l = self->states; l != NULL; l = l->next) {
      HaEntity *entity = l->data;

      add_entity_row (self, entity->entity_id, entity->friendly_name, entity);
      added++;
    }
  }

  if (added == 0) {
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *label = gtk_label_new (_("No cached entities — refresh in plugin settings"));

    gtk_widget_set_sensitive (row, FALSE);
    gtk_container_add (GTK_CONTAINER (row), label);
    gtk_widget_show_all (row);
    gtk_container_add (GTK_CONTAINER (self->entity_list), row);
  }
}


static void
update_row_states (PhoshHomeAssistantQuickSetting *self)
{
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  g_hash_table_iter_init (&iter, self->rows);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    const char *entity_id = key;
    GtkSwitch *sw = value;
    HaEntity *entity;

    if (g_hash_table_contains (self->busy, entity_id))
      continue;

    entity = find_state (self, entity_id);
    gtk_switch_set_active (sw,
                           entity != NULL && ha_state_is_on (entity->state));
    gtk_widget_set_sensitive (GTK_WIDGET (sw), can_connect (self));
  }
}


/* TRUE if any existing row's entity is unknown, i.e. it was created with a
 * stale "(unavailable)" label that only a rebuild can fix. */
static gboolean
rows_need_rebuild (PhoshHomeAssistantQuickSetting *self)
{
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  g_hash_table_iter_init (&iter, self->rows);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    if (find_state (self, (const char *) key) == NULL)
      return TRUE;
  }
  return FALSE;
}


/* Per-request deadline: flip the flag and abort the transfer so the
 * completion handler runs with G_IO_ERROR_CANCELLED. */
static gboolean
on_fetch_timed_out (gpointer user_data)
{
  FetchData *data = user_data;

  data->timed_out = TRUE;
  data->timeout_id = 0;   /* source is gone; don't remove it again */
  g_cancellable_cancel (data->cancellable);
  return G_SOURCE_REMOVE;
}


static gboolean
on_request_timed_out (gpointer user_data)
{
  RequestData *data = user_data;

  data->timed_out = TRUE;
  data->timeout_id = 0;   /* source is gone; don't remove it again */
  g_cancellable_cancel (data->cancellable);
  return G_SOURCE_REMOVE;
}


/* The pending flash keeps self alive until it fires, so no removal is
 * needed in finalize. */
static gboolean
on_error_flash_timeout (gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = user_data;

  self->error_flash_id = 0;
  update_tile_from_cache (self);
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}


/* After a service call succeeds, re-fetch the states (debounced) so every
 * row reconciles with what the server actually has. */
static gboolean
on_post_request_refresh (gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = user_data;

  self->post_request_refresh_id = 0;
  refresh_states (self);
  return G_SOURCE_REMOVE;
}

static void
schedule_post_request_refresh (PhoshHomeAssistantQuickSetting *self)
{
  if (self->post_request_refresh_id != 0)
    return;
  self->post_request_refresh_id = g_timeout_add_seconds (POST_REQUEST_REFRESH_SECONDS,
                                                         on_post_request_refresh,
                                                         self);
}


/* Show a transient "Error" on the tile info label. Every call re-arms the
 * reset timer, so a stream of failures keeps the label visible and it
 * returns to the computed state ERROR_DISPLAY_SECONDS after the last one. */
static void
flash_error (PhoshHomeAssistantQuickSetting *self)
{
  phosh_status_icon_set_info (self->info, _("Error"));

  if (self->error_flash_id != 0)
    g_source_remove (self->error_flash_id);
  self->error_flash_id = g_timeout_add_seconds (ERROR_DISPLAY_SECONDS,
                                                 on_error_flash_timeout,
                                                 g_object_ref (self));
}


static void
on_states_fetched (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  FetchData *data = user_data;
  PhoshHomeAssistantQuickSetting *self = data->self;
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GError) err = NULL;
  gconstpointer bytes;
  gsize len;
  gboolean ok = FALSE;
  GList *entities = NULL;
  guint status = 0;
  gboolean rerun;

  self->states_in_flight = FALSE;

  body = soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);
  if (body != NULL && data->msg != NULL)
    status = soup_message_get_status (data->msg);

  if (body == NULL) {
    if (data->timed_out) {
      g_warning ("HA states request timed out after %d s", REQUEST_TIMEOUT_SECONDS);
      flash_error (self);
    } else if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning ("Failed to fetch HA states: %s", err->message);
      flash_error (self);
    }
    goto out;
  }

  if (status < 200 || status >= 300) {
    g_warning ("HA states request returned HTTP %u %s",
               status,
               soup_message_get_reason_phrase (data->msg) ?: "");
    flash_error (self);
    goto out;
  }

  bytes = g_bytes_get_data (body, &len);
  if (bytes == NULL)
    goto out;

  entities = ha_parse_states (bytes, len, &ok);
  if (!ok) {
    flash_error (self);
    goto out;
  }

  clear_states (self);
  self->states = entities;
  entities = NULL;
  self->cache_loaded = TRUE;
  cache_save (self);

  /* Rebuild (not just in-place update) when a row's stale label needs the
   * real name back; otherwise keep the cheap update so switches are not
   * recreated under the user's finger. */
  if (self->entities != NULL && !rows_need_rebuild (self))
    update_row_states (self);
  else
    rebuild_entity_list (self);
  update_tile_from_cache (self);

out:
  g_list_free_full (entities, (GDestroyNotify) ha_entity_free);
  rerun = self->states_rerun;
  self->states_rerun = FALSE;
  fetch_data_free (data);

  if (rerun)
    refresh_states (self);
}


static void
on_entity_request_done (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  RequestData *data = user_data;
  PhoshHomeAssistantQuickSetting *self = data->self;
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GError) err = NULL;
  GtkSwitch *sw;
  guint status = 0;
  gboolean success;

  body = soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);
  if (body != NULL && data->msg != NULL)
    status = soup_message_get_status (data->msg);
  success = err == NULL && body != NULL && status >= 200 && status < 300;

  if (!success) {
    if (data->timed_out)
      g_warning ("Setting %s timed out after %d s", data->entity_id, REQUEST_TIMEOUT_SECONDS);
    else if (err != NULL && !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Failed to set %s: %s", data->entity_id, err->message);
    else if (body != NULL)
      g_warning ("Setting %s returned HTTP %u %s",
                 data->entity_id,
                 status,
                 soup_message_get_reason_phrase (data->msg) ?: "");

    /* Revert the optimistic flip so the switch shows what the server
     * actually has. */
    set_state_string (self, data->entity_id, data->old_on ? "on" : "off");
    sw = g_hash_table_lookup (self->rows, data->entity_id);
    if (sw != NULL)
      gtk_switch_set_active (sw, data->old_on);
    if (g_strcmp0 (data->entity_id, self->entity_id) == 0)
      phosh_home_assistant_quick_setting_update_icon (self,
                                                      data->old_on ? "on" : "off");
    else
      update_tile_from_cache (self);
    flash_error (self);
    cache_save (self);
  }
  else
    schedule_post_request_refresh (self);

  g_hash_table_remove (self->busy, data->entity_id);
  sw = g_hash_table_lookup (self->rows, data->entity_id);
  if (sw != NULL)
    gtk_widget_set_sensitive (GTK_WIDGET (sw), can_connect (self));

  request_data_free (data);
}


static gboolean
set_entity_state (PhoshHomeAssistantQuickSetting *self,
                  const char                     *entity_id,
                  gboolean                        desired_on)
{
  const DomainAction *action;
  g_autofree char *uri_str = NULL;
  g_autofree char *body_str = NULL;
  g_autoptr (GBytes) body = NULL;
  GtkSwitch *sw;
  HaEntity *entity;
  SoupMessage *msg;
  RequestData *data;
  gboolean old_on;
  /* The id may alias self->entity_id, which a GSettings change can reassign
   * mid-call (read_settings frees + reallocates it). Capture our own copy up
   * front so the request and completion always see a stable string. */
  g_autofree char *id = g_strdup (entity_id);

  if (g_hash_table_contains (self->busy, id))
    return FALSE;

  if (!can_connect (self)) {
    phosh_status_icon_set_info (self->info, _("Not configured"));
    return FALSE;
  }

  action = ha_domain_action (id);
  if (action == NULL) {
    phosh_status_icon_set_info (self->info, _("Unsupported entity"));
    return FALSE;
  }

  entity = find_state (self, id);
  sw = g_hash_table_lookup (self->rows, id);
  old_on = entity != NULL
    ? ha_state_is_on (entity->state)
    : (sw != NULL ? gtk_switch_get_active (sw) : !desired_on);

  g_hash_table_add (self->busy, g_strdup (id));
  if (sw != NULL) {
    gtk_switch_set_active (sw, desired_on);
    gtk_widget_set_sensitive (GTK_WIDGET (sw), FALSE);
  }

  set_state_string (self, id, desired_on ? "on" : "off");
  if (g_strcmp0 (id, self->entity_id) == 0)
    phosh_home_assistant_quick_setting_update_icon (self, desired_on ? "on" : "off");
  else
    update_tile_from_cache (self);
  cache_save (self);

  uri_str = g_strdup_printf ("%s/api/services/%s/%s",
                             self->base_url,
                             action->domain,
                             desired_on ? action->on_service : action->off_service);
  msg = soup_message_new ("POST", uri_str);
  if (msg == NULL) {
    g_hash_table_remove (self->busy, id);
    set_state_string (self, id, old_on ? "on" : "off");
    if (sw != NULL) {
      gtk_switch_set_active (sw, old_on);
      gtk_widget_set_sensitive (GTK_WIDGET (sw), can_connect (self));
    }
    update_tile_from_cache (self);
    cache_save (self);
    return FALSE;
  }

  soup_message_headers_append (soup_message_get_request_headers (msg),
                               "Authorization",
                               self->token);
  soup_message_headers_set_content_type (soup_message_get_request_headers (msg),
                                         "application/json",
                                         NULL);

  body_str = g_strdup_printf ("{\"entity_id\": \"%s\"}", id);
  body = g_bytes_new (body_str, strlen (body_str));
  soup_message_set_request_body_from_bytes (msg, "application/json", body);

  data = g_new0 (RequestData, 1);
  data->self = g_object_ref (self);
  data->msg = g_object_ref (msg);
  data->entity_id = g_strdup (id);
  data->old_on = old_on;
  data->cancellable = g_cancellable_new ();
  data->timeout_id = g_timeout_add_seconds (REQUEST_TIMEOUT_SECONDS,
                                            on_request_timed_out,
                                            data);

  soup_session_send_and_read_async (self->session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    data->cancellable,
                                    on_entity_request_done,
                                    data);
  g_object_unref (msg);
  return TRUE;
}


static gboolean
on_row_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);
  const char *entity_id = g_object_get_data (G_OBJECT (sw), "entity-id");

  if (entity_id != NULL)
    set_entity_state (self, entity_id, state);

  /* set_entity_state() already updates the switch; never let GTK apply
   * a second transition. */
  return TRUE;
}


static gboolean
on_row_button_press (GtkWidget *row, GdkEventButton *event, gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);
  GtkWidget *target;
  GtkWidget *sw;
  const char *entity_id;
  gboolean new_on;

  if (event->type != GDK_BUTTON_PRESS || event->button != 1)
    return FALSE;

  sw = g_object_get_data (G_OBJECT (row), "switch");
  if (sw == NULL)
    return FALSE;

  /* Clicks on the switch itself are handled via state-set; ignore them
   * here so the toggle fires exactly once. */
  target = gtk_get_event_widget ((GdkEvent *) event);
  if (target == sw || (target != NULL && gtk_widget_is_ancestor (target, sw)))
    return FALSE;

  entity_id = g_object_get_data (G_OBJECT (sw), "entity-id");
  if (entity_id == NULL)
    return FALSE;

  new_on = !gtk_switch_get_active (GTK_SWITCH (sw));
  set_entity_state (self, entity_id, new_on);
  return TRUE;
}


static void
refresh_states (PhoshHomeAssistantQuickSetting *self)
{
  SoupMessage *msg = NULL;
  g_autofree char *uri_str = NULL;
  FetchData *data;

  if (!can_connect (self)) {
    phosh_status_icon_set_info (self->info, _("Not configured"));
    return;
  }

  if (self->states_in_flight) {
    self->states_rerun = TRUE;
    return;
  }

  uri_str = g_strdup_printf ("%s/api/states", self->base_url);
  msg = soup_message_new ("GET", uri_str);
  if (msg == NULL)
    return;

  soup_message_headers_append (soup_message_get_request_headers (msg),
                               "Authorization",
                               self->token);

  self->states_in_flight = TRUE;
  data = g_new0 (FetchData, 1);
  data->self = g_object_ref (self);
  data->msg = g_object_ref (msg);
  data->cancellable = g_cancellable_new ();
  data->timeout_id = g_timeout_add_seconds (REQUEST_TIMEOUT_SECONDS,
                                            on_fetch_timed_out,
                                            data);

  soup_session_send_and_read_async (self->session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    data->cancellable,
                                    on_states_fetched,
                                    data);
  g_object_unref (msg);
}


static gboolean
on_conn_refresh (gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);

  self->conn_timeout = 0;
  refresh_states (self);
  return G_SOURCE_REMOVE;
}


static void
on_settings_changed (PhoshHomeAssistantQuickSetting *self,
                     GSettings                      *settings,
                     const char                     *key)
{
  g_autofree char *old_url = g_strdup (self->base_url);
  g_autofree char *old_token = g_strdup (self->token);
  g_autofree char *old_main = g_strdup (self->entity_id);
  g_auto (GStrv) old_entities = self->entities ? g_strdupv (self->entities) : NULL;
  gboolean conn_changed;
  gboolean entities_changed;
  gboolean main_changed;

  (void) settings;

  if (g_strcmp0 (key, CACHE_KEY) == 0) {
    g_autofree char *current = g_settings_get_string (self->settings, CACHE_KEY);

    if (g_strcmp0 (current, self->last_cache) == 0)
      return;

    if (self->conn_timeout != 0) {
      g_source_remove (self->conn_timeout);
      self->conn_timeout = 0;
    }
    cache_load (self);
    rebuild_entity_list (self);
    update_tile_from_cache (self);
    return;
  }

  phosh_home_assistant_quick_setting_read_settings (self);

  conn_changed = g_strcmp0 (old_url, self->base_url) != 0 ||
    g_strcmp0 (old_token, self->token) != 0;
  entities_changed = !strv_equal (old_entities, self->entities);
  main_changed = g_strcmp0 (old_main, self->entity_id) != 0;

  if (conn_changed) {
    clear_states (self);
    self->cache_loaded = FALSE;
    rebuild_entity_list (self);
    update_tile_from_cache (self);
    if (self->conn_timeout != 0)
      g_source_remove (self->conn_timeout);
    self->conn_timeout = g_timeout_add (500, on_conn_refresh, self);
    return;
  }

  if (entities_changed) {
    rebuild_entity_list (self);
    update_tile_from_cache (self);
    if (entities_missing_from_cache (self))
      refresh_states (self);
    return;
  }

  if (main_changed)
    update_tile_from_cache (self);
}


/* The refresh button in the status page title row fetches the states once.
 * refresh_states() is a no-op when unconfigured and dedupes against an
 * in-flight fetch, so rapid taps are safe. */
static void
on_refresh_clicked (PhoshHomeAssistantQuickSetting *self)
{
  refresh_states (self);
}


static void
on_clicked (PhoshHomeAssistantQuickSetting *self)
{
  if (!can_connect (self)) {
    phosh_status_icon_set_info (self->info, _("Not configured"));
    return;
  }

  if (has_main_entity (self)) {
    set_entity_state (self, self->entity_id, !self->main_is_on);
  } else {
    refresh_states (self);
  }
}


static void
on_footer_clicked (PhoshHomeAssistantQuickSetting *self)
{
  g_autoptr (GError) err = NULL;

  if (!can_connect (self)) {
    phosh_status_icon_set_info (self->info, _("Not configured"));
    return;
  }

  if (!g_app_info_launch_default_for_uri (self->base_url, NULL, &err)) {
    g_warning ("Failed to open %s in browser: %s", self->base_url, err->message);
    flash_error (self);
  }
}


static void
phosh_home_assistant_quick_setting_finalize (GObject *gobject)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (gobject);

  /* A pending error flash keeps itself alive via its user-data ref and
   * fires harmlessly after destruction, so it is left alone here. The
   * refresh timer carries a plain self pointer, so it must not fire
   * after destruction. */
  if (self->conn_timeout != 0) {
    g_source_remove (self->conn_timeout);
    self->conn_timeout = 0;
  }
  if (self->post_request_refresh_id != 0) {
    g_source_remove (self->post_request_refresh_id);
    self->post_request_refresh_id = 0;
  }
  clear_states (self);
  g_clear_pointer (&self->rows, g_hash_table_unref);
  g_clear_pointer (&self->busy, g_hash_table_unref);
  g_clear_pointer (&self->last_cache, g_free);
  g_clear_object (&self->session);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->token, g_free);
  g_clear_pointer (&self->entity_id, g_free);
  g_clear_pointer (&self->entities, g_strfreev);

  G_OBJECT_CLASS (phosh_home_assistant_quick_setting_parent_class)->finalize (gobject);
}


static void
phosh_home_assistant_quick_setting_class_init (PhoshHomeAssistantQuickSettingClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = phosh_home_assistant_quick_setting_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
    "/mobi/phosh/plugins/home-assistant-quick-setting/qs.ui");

  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSetting, info);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSetting, status_page);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSetting, entity_list);

  gtk_widget_class_bind_template_callback (widget_class, on_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_footer_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_refresh_clicked);
}


static void
phosh_home_assistant_quick_setting_init (PhoshHomeAssistantQuickSetting *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->session = soup_session_new ();
  self->settings = g_settings_new (HOME_ASSISTANT_QUICK_SETTING_SCHEMA);
  self->rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->busy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  phosh_home_assistant_quick_setting_read_settings (self);
  cache_load (self);
  rebuild_entity_list (self);
  update_tile_from_cache (self);

  g_signal_connect_object (self->settings,
                           "changed",
                           G_CALLBACK (on_settings_changed),
                           self,
                           G_CONNECT_SWAPPED);

  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_default (),
    "/mobi/phosh/plugins/home-assistant-quick-setting/icons");

  phosh_status_icon_set_icon_name (self->info, HA_OFF_ICON);

  /* No fetch at load: the tile renders from cache and networking only
   * happens on user action (refresh button, toggles). A failed or absent
   * fetch leaves the last known state in place. */
  if (!can_connect (self))
    phosh_status_icon_set_info (self->info, _("Not configured"));
}
