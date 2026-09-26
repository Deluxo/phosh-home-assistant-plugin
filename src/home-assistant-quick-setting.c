#include "home-assistant-quick-setting.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define HOME_ASSISTANT_QUICK_SETTING_SCHEMA "mobi.phosh.plugins.home-assistant-quick-setting"
#define BASE_URL_KEY                        "base-url"
#define TOKEN_KEY                           "token"
#define ENTITY_ID_KEY                       "entity-id"
#define HIDDEN_ENTITIES_KEY                 "hidden-entities"

#define HA_ON_ICON    "home-assistant-quick-setting-symbolic"
#define HA_OFF_ICON   "home-assistant-quick-setting-symbolic"

/* Domains handled by the generic homeassistant.toggle service */
static const char *toggleable_domains[] = {
  "cover", "fan", "humidifier", "input_boolean", "light",
  "lock", "media_player", "remote", "switch", "vacuum",
  NULL,
};

typedef struct {
  char *entity_id;
  char *friendly_name;
  char *state;
} HaEntity;

struct _PhoshHomeAssistantQuickSetting {
  PhoshQuickSetting        parent;

  PhoshStatusIcon         *info;
  PhoshStatusPage         *status_page;
  GtkListBox              *entity_list;

  SoupSession             *session;
  GCancellable            *cancellable;
  GSettings               *settings;

  char                    *base_url;
  char                    *token;
  char                    *entity_id;
  char                   **hidden_entities;

  /* Post-toggle confirmation: entity awaiting a state flip, what it
   * was when toggled, poll attempts so far, and the pending timeout. */
  char                    *pending_entity;
  gboolean                 pending_from_on;
  guint                    pending_attempts;
  guint                    pending_source;
  gboolean                 main_is_on;
};

G_DEFINE_TYPE (PhoshHomeAssistantQuickSetting,
               phosh_home_assistant_quick_setting,
               PHOSH_TYPE_QUICK_SETTING);


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


/* Display mapping: states that mean "active" for a switch. Covers
 * media players (playing/paused/idle), covers (open) and locks. */
static gboolean
ha_state_is_on (const char *state)
{
  static const char *on_states[] = {
    "on", "playing", "paused", "idle", "buffering",
    "open", "opening", "closing", "locked",
    NULL,
  };

  return state != NULL && g_strv_contains (on_states, state);
}


/* Delays between post-toggle state polls; total span ~8s */
static const guint retry_delays_ms[] = { 1000, 2000, 2000, 3000 };
#define N_RETRIES G_N_ELEMENTS (retry_delays_ms)


static void refresh_states (PhoshHomeAssistantQuickSetting *self);


static void
clear_pending (PhoshHomeAssistantQuickSetting *self)
{
  g_clear_pointer (&self->pending_entity, g_free);
  self->pending_attempts = 0;
  if (self->pending_source != 0) {
    g_source_remove (self->pending_source);
    self->pending_source = 0;
  }
}


static void
set_pending (PhoshHomeAssistantQuickSetting *self,
             const char                      *entity_id,
             gboolean                         from_on)
{
  clear_pending (self);
  self->pending_entity = g_strdup (entity_id);
  self->pending_from_on = from_on;
  self->pending_attempts = 0;
}


static gboolean
on_pending_refresh_tick (gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);

  self->pending_source = 0;
  refresh_states (self);
  return G_SOURCE_REMOVE;
}


static void
schedule_refresh_in (PhoshHomeAssistantQuickSetting *self, guint ms)
{
  if (self->pending_source != 0)
    g_source_remove (self->pending_source);
  self->pending_source = g_timeout_add (ms, on_pending_refresh_tick, self);
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
    entity->entity_id = g_strdup (entity_id);
    entity->friendly_name = g_strdup (friendly);
    entity->state = g_strdup (state ? state : "unknown");
    entities = g_list_prepend (entities, entity);
  }

  *ok = TRUE;
  return g_list_reverse (entities);
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
  g_free (self->entity_id);
  self->entity_id = g_steal_pointer (&raw_entity);

  g_clear_pointer (&self->hidden_entities, g_strfreev);
  self->hidden_entities = g_settings_get_strv (self->settings, HIDDEN_ENTITIES_KEY);
}


static gboolean
is_hidden (PhoshHomeAssistantQuickSetting *self, const char *entity_id)
{
  return self->hidden_entities != NULL &&
    g_strv_contains ((const char * const *) self->hidden_entities, entity_id);
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


static void toggle_entity (PhoshHomeAssistantQuickSetting *self, const char *entity_id);


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
clear_entity_list (PhoshHomeAssistantQuickSetting *self)
{
  g_autoptr (GList) children = NULL;

  children = gtk_container_get_children (GTK_CONTAINER (self->entity_list));
  for (GList *l = children; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
}


static gboolean
on_row_switch_toggled (GtkSwitch *sw, gboolean state, gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);
  const char *entity_id = g_object_get_data (G_OBJECT (sw), "entity-id");

  if (entity_id == NULL)
    return FALSE;

  /* Optimistic: the switch already shows the intended position via the
   * default handler; confirm against fresh states below. */
  set_pending (self, entity_id, gtk_switch_get_active (sw));
  if (g_strcmp0 (entity_id, self->entity_id) == 0)
    phosh_home_assistant_quick_setting_update_icon (self, state ? "on" : "off");

  toggle_entity (self, entity_id);

  return FALSE;
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
  g_signal_handlers_block_by_func (sw, G_CALLBACK (on_row_switch_toggled), self);
  gtk_switch_set_active (GTK_SWITCH (sw), new_on);
  g_signal_handlers_unblock_by_func (sw, G_CALLBACK (on_row_switch_toggled), self);

  set_pending (self, entity_id, !new_on);
  if (g_strcmp0 (entity_id, self->entity_id) == 0)
    phosh_home_assistant_quick_setting_update_icon (self, new_on ? "on" : "off");

  toggle_entity (self, entity_id);

  return TRUE;
}


static void
rebuild_entity_list (PhoshHomeAssistantQuickSetting *self, GList *entities)
{
  guint added = 0;
  guint skipped_hidden = 0;

  clear_entity_list (self);

  for (GList *l = entities; l != NULL; l = l->next) {
    HaEntity *entity = l->data;
    GtkWidget *row;
    GtkWidget *box;
    GtkWidget *label;
    GtkWidget *sw;

    if (is_hidden (self, entity->entity_id)) {
      skipped_hidden++;
      continue;
    }

    row = gtk_list_box_row_new ();
    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    label = gtk_label_new (entity->friendly_name);
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
    gtk_switch_set_active (GTK_SWITCH (sw), ha_state_is_on (entity->state));

    g_object_set_data_full (G_OBJECT (sw),
                            "entity-id",
                            g_strdup (entity->entity_id),
                            g_free);
    g_object_set_data (G_OBJECT (row), "switch", sw);
    g_signal_connect (sw, "state-set", G_CALLBACK (on_row_switch_toggled), self);
    g_signal_connect (row, "button-press-event", G_CALLBACK (on_row_button_press), self);

    gtk_container_add (GTK_CONTAINER (box), label);
    gtk_container_add (GTK_CONTAINER (box), sw);
    gtk_container_add (GTK_CONTAINER (row), box);
    gtk_widget_show_all (row);
    gtk_container_add (GTK_CONTAINER (self->entity_list), row);
    added++;
  }

  if (added == 0) {
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *label;
    if (skipped_hidden > 0)
      label = gtk_label_new (_("All entities hidden — enable them in settings"));
    else
      label = gtk_label_new (_("No toggleable entities found"));
    gtk_widget_set_sensitive (row, FALSE);
    gtk_container_add (GTK_CONTAINER (row), label);
    gtk_widget_show_all (row);
    gtk_container_add (GTK_CONTAINER (self->entity_list), row);
  }
}


static void
on_states_fetched (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GError) err = NULL;
  gconstpointer data;
  gsize len;
  gboolean ok = FALSE;
  GList *entities = NULL;

  body = soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);
  if (body == NULL) {
    phosh_status_icon_set_info (self->info, _("Error"));
    g_warning ("Failed to fetch HA states: %s", err->message);
    return;
  }

  data = g_bytes_get_data (body, &len);
  if (data == NULL)
    return;

  entities = ha_parse_states (data, len, &ok);
  if (!ok) {
    phosh_status_icon_set_info (self->info, _("Error"));
    return;
  }

  /* While a toggle is unconfirmed, only rebuild once the entity's state
   * actually flipped; otherwise keep the optimistic UI and poll again.
   * HA is eventually consistent here, especially slow devices (TVs). */
  if (self->pending_entity != NULL) {
    gboolean found = FALSE;
    for (GList *l = entities; l != NULL; l = l->next) {
      HaEntity *entity = l->data;
      if (g_strcmp0 (entity->entity_id, self->pending_entity) == 0) {
        found = TRUE;
        if (ha_state_is_on (entity->state) != self->pending_from_on)
          clear_pending (self);
        break;
      }
    }
    if (self->pending_entity != NULL) {
      if (!found) {
        g_warning ("Pending entity %s vanished from HA states", self->pending_entity);
        clear_pending (self);
      } else if (++self->pending_attempts >= N_RETRIES) {
        g_warning ("Toggle of %s unconfirmed after %u polls, showing truth",
                   self->pending_entity, self->pending_attempts);
        clear_pending (self);
      } else {
        schedule_refresh_in (self, retry_delays_ms[self->pending_attempts]);
        g_list_free_full (entities, (GDestroyNotify) ha_entity_free);
        return;
      }
    }
  }

  /* Tile reflects the main entity when set, otherwise a summary count */
  if (has_main_entity (self)) {
    const char *state = "off";
    for (GList *l = entities; l != NULL; l = l->next) {
      HaEntity *entity = l->data;
      if (g_strcmp0 (entity->entity_id, self->entity_id) == 0) {
        state = entity->state;
        break;
      }
    }
    phosh_home_assistant_quick_setting_update_icon (self, state);
  } else {
    guint n_on = 0;
    for (GList *l = entities; l != NULL; l = l->next) {
      if (ha_state_is_on (((HaEntity *) l->data)->state))
        n_on++;
    }
    phosh_quick_setting_set_active (PHOSH_QUICK_SETTING (self), n_on > 0);
    if (n_on > 0) {
      g_autofree char *info = g_strdup_printf ("%u on", n_on);
      phosh_status_icon_set_info (self->info, info);
    } else {
      phosh_status_icon_set_info (self->info, _("All off"));
    }
  }

  rebuild_entity_list (self, entities);
  g_list_free_full (entities, (GDestroyNotify) ha_entity_free);
}


static void
on_toggle_done (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);
  g_autoptr (GError) err = NULL;

  soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);
  if (err) {
    phosh_status_icon_set_info (self->info, _("Error"));
    g_warning ("Failed to toggle HA entity: %s", err->message);
    clear_pending (self);
    refresh_states (self); /* nothing changed: restore truth right away */
    return;
  }

  /* HA applies the toggle asynchronously; poll until fresh states
   * confirm the flip instead of trusting the first response. */
  schedule_refresh_in (self, retry_delays_ms[0]);
}


static void
toggle_entity (PhoshHomeAssistantQuickSetting *self, const char *entity_id)
{
  SoupMessage *msg = NULL;
  g_autofree char *uri_str = NULL;
  g_autofree char *body_str = NULL;
  g_autoptr (GBytes) body = NULL;

  phosh_status_icon_set_info (self->info, _("Toggling..."));

  uri_str = g_strdup_printf ("%s/api/services/homeassistant/toggle", self->base_url);
  msg = soup_message_new ("POST", uri_str);
  if (msg == NULL)
    return;

  soup_message_headers_append (soup_message_get_request_headers (msg),
                               "Authorization",
                               self->token);
  soup_message_headers_set_content_type (soup_message_get_request_headers (msg),
                                         "application/json",
                                         NULL);

  body_str = g_strdup_printf ("{\"entity_id\": \"%s\"}", entity_id);
  body = g_bytes_new (body_str, strlen (body_str));
  soup_message_set_request_body_from_bytes (msg, "application/json", body);

  soup_session_send_and_read_async (self->session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    self->cancellable,
                                    on_toggle_done,
                                    self);
  g_object_unref (msg);
}


static void
on_clicked (PhoshHomeAssistantQuickSetting *self)
{
  if (!can_connect (self)) {
    phosh_status_icon_set_info (self->info, _("Not configured"));
    return;
  }

  if (has_main_entity (self)) {
    set_pending (self, self->entity_id, self->main_is_on);
    phosh_home_assistant_quick_setting_update_icon (self, self->main_is_on ? "off" : "on");
    toggle_entity (self, self->entity_id);
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
    phosh_status_icon_set_info (self->info, _("Error"));
  }
}


static void
refresh_states (PhoshHomeAssistantQuickSetting *self)
{
  SoupMessage *msg = NULL;
  g_autofree char *uri_str = NULL;

  if (!can_connect (self))
    return;

  uri_str = g_strdup_printf ("%s/api/states", self->base_url);
  msg = soup_message_new ("GET", uri_str);
  if (msg == NULL)
    return;

  soup_message_headers_append (soup_message_get_request_headers (msg),
                               "Authorization",
                               self->token);

  soup_session_send_and_read_async (self->session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    self->cancellable,
                                    on_states_fetched,
                                    self);
  g_object_unref (msg);
}


static void
on_settings_changed (PhoshHomeAssistantQuickSetting *self)
{
  phosh_home_assistant_quick_setting_read_settings (self);
  clear_pending (self);
  refresh_states (self);
}


static void
phosh_home_assistant_quick_setting_finalize (GObject *gobject)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (gobject);

  g_cancellable_cancel (self->cancellable);
  clear_pending (self);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->token, g_free);
  g_clear_pointer (&self->entity_id, g_free);
  g_clear_pointer (&self->hidden_entities, g_strfreev);

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
}


static void
phosh_home_assistant_quick_setting_init (PhoshHomeAssistantQuickSetting *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->session = soup_session_new ();
  self->cancellable = g_cancellable_new ();
  self->settings = g_settings_new (HOME_ASSISTANT_QUICK_SETTING_SCHEMA);

  phosh_home_assistant_quick_setting_read_settings (self);

  g_signal_connect_object (self->settings,
                           "changed",
                           G_CALLBACK (on_settings_changed),
                           self,
                           G_CONNECT_SWAPPED);

  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_default (),
    "/mobi/phosh/plugins/home-assistant-quick-setting/icons");

  phosh_status_icon_set_icon_name (self->info, HA_OFF_ICON);

  if (can_connect (self)) {
    refresh_states (self);
  } else {
    phosh_status_icon_set_info (self->info, _("Not configured"));
  }
}
