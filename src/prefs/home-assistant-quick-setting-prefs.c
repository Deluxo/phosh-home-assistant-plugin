#include "phosh-plugin-prefs-config.h"

#include "home-assistant-quick-setting-prefs.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define HOME_ASSISTANT_QUICK_SETTING_SCHEMA "mobi.phosh.plugins.home-assistant-quick-setting"
#define BASE_URL_KEY                        "base-url"
#define TOKEN_KEY                           "token"
#define ENTITY_ID_KEY                       "entity-id"
#define ENTITIES_KEY                        "entities"
#define CACHE_KEY                           "cache"

/* Bound prefs-side memory/UI work the same way the quick setting does. */
#define MAX_RESPONSE_BYTES      (8 * 1024 * 1024)
#define MAX_ENTITIES            500
#define REQUEST_TIMEOUT_SECONDS 10

/* Domains handled by the desired-state services in the quick setting */
static const char *toggleable_domains[] = {
  "automation", "cover", "fan", "humidifier", "input_boolean", "light",
  "lock", "media_player", "remote", "script", "switch", "vacuum",
  NULL,
};

struct _PhoshHomeAssistantQuickSettingPrefs {
  AdwPreferencesDialog  parent;

  AdwEntryRow           *url_row;
  AdwPasswordEntryRow   *token_row;

  AdwActionRow          *load_row;
  GtkButton             *load_button;
  GtkSpinner            *load_spinner;
  AdwComboRow           *entity_combo;

  AdwPreferencesGroup   *visibility_group;

  AdwActionRow          *test_row;
  GtkButton             *test_button;
  GtkSpinner            *test_spinner;

  GSettings             *settings;
  SoupSession           *session;

  GtkStringList         *entity_model;
  GPtrArray             *entity_ids; /* parallel to the model, owned */
  gboolean               populating;
  gboolean               destroyed;
};

G_DEFINE_TYPE (PhoshHomeAssistantQuickSettingPrefs,
               phosh_home_assistant_quick_setting_prefs,
               ADW_TYPE_PREFERENCES_DIALOG);

static gboolean prefs_is_live (PhoshHomeAssistantQuickSettingPrefs *self);


/* Look the schema up non-fatally: g_settings_new() aborts the whole process
 * (phosh) when the schema is missing after a partial install. */
static GSettings *
prefs_settings_new (void)
{
  GSettingsSchemaSource *source;
  GSettingsSchema *schema;
  GSettings *settings;

  source = g_settings_schema_source_get_default ();
  schema = source != NULL
    ? g_settings_schema_source_lookup (source,
                                       HOME_ASSISTANT_QUICK_SETTING_SCHEMA,
                                       TRUE)
    : NULL;
  if (schema == NULL) {
    g_warning ("GSettings schema %s is not installed; Home Assistant prefs disabled",
               HOME_ASSISTANT_QUICK_SETTING_SCHEMA);
    return NULL;
  }

  settings = g_settings_new_full (schema, NULL, NULL);
  g_settings_schema_unref (schema);
  return settings;
}


static const char *
json_object_get_string_or_null (JsonObject *obj, const char *member)
{
  JsonNode *node;

  if (obj == NULL || member == NULL)
    return NULL;
  node = json_object_get_member (obj, member);
  if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
    return NULL;
  if (json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string (node);
}


static JsonObject *
json_object_get_object_or_null (JsonObject *obj, const char *member)
{
  JsonNode *node;

  if (obj == NULL || member == NULL)
    return NULL;
  node = json_object_get_member (obj, member);
  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    return NULL;
  return json_node_get_object (node);
}


static JsonObject *
json_array_get_object_or_null (JsonArray *array, guint index)
{
  JsonNode *node;

  if (array == NULL)
    return NULL;
  node = json_array_get_element (array, index);
  if (node == NULL || !JSON_NODE_HOLDS_OBJECT (node))
    return NULL;
  return json_node_get_object (node);
}


static char *
normalize_base_url (const char *url)
{
  g_autofree char *copy = g_strdup (url ? url : "");
  gsize len;

  g_strstrip (copy);
  while ((len = strlen (copy)) > 0 && copy[len - 1] == '/')
    copy[len - 1] = '\0';

  return g_steal_pointer (&copy);
}


static void
get_credentials (PhoshHomeAssistantQuickSettingPrefs *self, char **url, char **token)
{
  *url = normalize_base_url (gtk_editable_get_text (GTK_EDITABLE (self->url_row)));
  *token = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->token_row)));
  g_strstrip (*token);
}


static gboolean
credentials_valid (PhoshHomeAssistantQuickSettingPrefs *self,
                   const char                          *url,
                   const char                          *token,
                   AdwActionRow                        *row)
{
  if (url[0] == '\0') {
    adw_action_row_set_subtitle (row, _("Enter your server URL first"));
    return FALSE;
  }
  if (!g_str_has_prefix (url, "http://") && !g_str_has_prefix (url, "https://")) {
    adw_action_row_set_subtitle (row, _("URL must start with http:// or https://"));
    return FALSE;
  }
  if (token[0] == '\0') {
    adw_action_row_set_subtitle (row, _("Enter your access token first"));
    return FALSE;
  }
  return TRUE;
}


static void
on_url_apply (PhoshHomeAssistantQuickSettingPrefs *self)
{
  const char *url = gtk_editable_get_text (GTK_EDITABLE (self->url_row));
  g_autofree char *normalized = normalize_base_url (url);

  if (!prefs_is_live (self) || self->settings == NULL)
    return;

  g_settings_set_string (self->settings, BASE_URL_KEY, normalized);
  if (g_strcmp0 (url, normalized) != 0)
    gtk_editable_set_text (GTK_EDITABLE (self->url_row), normalized);
}


static void
on_token_apply (PhoshHomeAssistantQuickSettingPrefs *self)
{
  g_autofree char *token = NULL;

  if (!prefs_is_live (self) || self->settings == NULL)
    return;

  token = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->token_row)));
  g_strstrip (token);
  g_settings_set_string (self->settings, TOKEN_KEY, token);
  if (g_strcmp0 (gtk_editable_get_text (GTK_EDITABLE (self->token_row)), token) != 0)
    gtk_editable_set_text (GTK_EDITABLE (self->token_row), token);
}


typedef struct {
  PhoshHomeAssistantQuickSettingPrefs *self;
  SoupMessage                          *msg;
  GCancellable                         *cancellable;
  guint                                 timeout_id;
  gboolean                              timed_out;
} TestData;


static void
test_data_free (TestData *data)
{
  if (data->timeout_id != 0) {
    g_source_remove (data->timeout_id);
    data->timeout_id = 0;
  }
  g_clear_object (&data->cancellable);
  g_clear_object (&data->msg);
  g_clear_object (&data->self);
  g_free (data);
}


static gboolean
on_test_timed_out (gpointer user_data)
{
  TestData *data = user_data;

  data->timeout_id = 0;
  data->timed_out = TRUE;
  g_cancellable_cancel (data->cancellable);
  return G_SOURCE_REMOVE;
}


static gboolean
prefs_is_live (PhoshHomeAssistantQuickSettingPrefs *self)
{
  return self != NULL && !self->destroyed;
}


static void
on_test_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  TestData *data = user_data;
  PhoshHomeAssistantQuickSettingPrefs *self = data->self;
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GError) err = NULL;
  guint status;

  body = soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);

  if (!prefs_is_live (self)) {
    test_data_free (data);
    return;
  }

  gtk_spinner_stop (self->test_spinner);
  gtk_widget_set_visible (GTK_WIDGET (self->test_spinner), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->test_button), TRUE);

  if (body == NULL) {
    g_autofree char *msg;

    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && !data->timed_out) {
      test_data_free (data);
      return;
    }

    msg = data->timed_out
      ? g_strdup (_("Connection timed out"))
      : g_strdup_printf (_("Connection failed: %s"),
                         err != NULL ? err->message : _("unknown error"));
    adw_action_row_set_subtitle (self->test_row, msg);
    test_data_free (data);
    return;
  }

  status = soup_message_get_status (data->msg);
  if (status == SOUP_STATUS_OK) {
    adw_action_row_set_subtitle (self->test_row, _("Connected — API running"));
  } else if (status == SOUP_STATUS_UNAUTHORIZED || status == SOUP_STATUS_FORBIDDEN) {
    adw_action_row_set_subtitle (self->test_row, _("Invalid access token"));
  } else {
    g_autofree char *msg = g_strdup_printf (_("Server replied: HTTP %u %s"),
                                            status,
                                            soup_message_get_reason_phrase (data->msg) ?: "");
    adw_action_row_set_subtitle (self->test_row, msg);
  }

  test_data_free (data);
}


static SoupMessage *
auth_get_message (PhoshHomeAssistantQuickSettingPrefs *self,
                  const char                          *url,
                  const char                          *token,
                  const char                          *path)
{
  g_autofree char *uri_str = NULL;
  g_autofree char *auth = NULL;
  SoupMessage *msg;

  uri_str = g_strdup_printf ("%s%s", url, path);
  msg = soup_message_new ("GET", uri_str);
  if (msg == NULL)
    return NULL;

  auth = g_strdup_printf ("Bearer %s", token);
  soup_message_headers_append (soup_message_get_request_headers (msg),
                               "Authorization",
                               auth);
  return msg;
}


static void
on_test_clicked (PhoshHomeAssistantQuickSettingPrefs *self)
{
  g_autofree char *url = NULL;
  g_autofree char *token = NULL;
  SoupMessage *msg = NULL;
  TestData *data;

  if (!prefs_is_live (self))
    return;

  get_credentials (self, &url, &token);
  if (!credentials_valid (self, url, token, self->test_row))
    return;

  msg = auth_get_message (self, url, token, "/api/");
  if (msg == NULL) {
    adw_action_row_set_subtitle (self->test_row, _("Invalid server URL"));
    return;
  }

  /* Persist what was just tested so the quick-setting picks it up */
  if (self->settings != NULL) {
    g_settings_set_string (self->settings, BASE_URL_KEY, url);
    g_settings_set_string (self->settings, TOKEN_KEY, token);
  }

  data = g_new0 (TestData, 1);
  data->self = g_object_ref (self);
  data->msg = g_object_ref (msg);
  data->cancellable = g_cancellable_new ();
  data->timeout_id = g_timeout_add_seconds (REQUEST_TIMEOUT_SECONDS,
                                            on_test_timed_out,
                                            data);

  gtk_widget_set_sensitive (GTK_WIDGET (self->test_button), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->test_spinner), TRUE);
  gtk_spinner_start (self->test_spinner);
  adw_action_row_set_subtitle (self->test_row, _("Connecting…"));

  soup_session_send_and_read_async (self->session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    data->cancellable,
                                    on_test_finished,
                                    data);
  g_object_unref (msg);
}


static gboolean
domain_toggleable (const char *entity_id)
{
  g_autofree char *domain = NULL;
  const char *dot;

  dot = strchr (entity_id, '.');
  if (dot == NULL)
    return FALSE;
  domain = g_strndup (entity_id, dot - entity_id);

  return g_strv_contains (toggleable_domains, domain);
}


typedef struct {
  char *entity_id;
  char *name;
  char *label;
  char *state;
} FoundEntity;


static FoundEntity *
found_entity_new (const char *entity_id, const char *name, const char *state)
{
  FoundEntity *found = g_new0 (FoundEntity, 1);

  found->entity_id = g_strdup (entity_id);
  found->name = g_strdup (name != NULL && name[0] != '\0' ? name : entity_id);
  found->state = g_strdup (state != NULL && state[0] != '\0' ? state : "unknown");
  if (g_strcmp0 (found->name, found->entity_id) == 0)
    found->label = g_strdup (found->entity_id);
  else
    found->label = g_strdup_printf ("%s (%s)", found->name, found->entity_id);

  return found;
}


static void
found_entity_free (FoundEntity *found)
{
  g_free (found->entity_id);
  g_free (found->name);
  g_free (found->label);
  g_free (found->state);
  g_free (found);
}


static gboolean
is_entity_shown (PhoshHomeAssistantQuickSettingPrefs *self, const char *entity_id)
{
  g_auto (GStrv) shown = NULL;

  if (self->settings == NULL)
    return TRUE;

  shown = g_settings_get_strv (self->settings, ENTITIES_KEY);
  if (shown == NULL || shown[0] == NULL)
    return TRUE;
  /* Configured ids may predate canonicalization and carry mixed case. */
  for (guint i = 0; shown[i] != NULL; i++)
    if (g_ascii_strcasecmp (shown[i], entity_id) == 0)
      return TRUE;
  return FALSE;
}


static void
set_entity_shown (PhoshHomeAssistantQuickSettingPrefs *self,
                  const char                          *entity_id,
                  gboolean                             shown)
{
  g_auto (GStrv) current = NULL;
  g_autoptr (GPtrArray) updated = g_ptr_array_new_with_free_func (g_free);

  if (!prefs_is_live (self) || self->settings == NULL || self->entity_ids == NULL)
    return;

  current = g_settings_get_strv (self->settings, ENTITIES_KEY);

  if (current == NULL || current[0] == NULL) {
    if (shown)
      return; /* empty already means "show everything" */

    /* Materialise the current set, minus this entity. */
    for (guint i = 0; i < self->entity_ids->len; i++) {
      const char *id = g_ptr_array_index (self->entity_ids, i);
      if (g_strcmp0 (id, entity_id) != 0)
        g_ptr_array_add (updated, g_strdup (id));
    }
  } else {
    for (guint i = 0; current[i] != NULL; i++) {
      /* Case-insensitive: a mixed-case entry is the same entity and gets
       * replaced by the canonical id, not duplicated. */
      if (g_ascii_strcasecmp (current[i], entity_id) != 0)
        g_ptr_array_add (updated, g_strdup (current[i]));
    }
    if (shown)
      g_ptr_array_add (updated, g_strdup (entity_id));
  }

  g_ptr_array_add (updated, NULL);
  g_settings_set_strv (self->settings, ENTITIES_KEY,
                       (const char * const *) updated->pdata);
}


static void
clear_visibility_group (PhoshHomeAssistantQuickSettingPrefs *self)
{
  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (self->visibility_group));

  while (child != NULL) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    adw_preferences_group_remove (self->visibility_group, child);
    child = next;
  }
}


static void
on_visibility_toggled (AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data)
{
  PhoshHomeAssistantQuickSettingPrefs *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING_PREFS (user_data);
  const char *entity_id = g_object_get_data (G_OBJECT (row), "entity-id");

  if (entity_id != NULL)
    set_entity_shown (self, entity_id, adw_switch_row_get_active (row));
}


static void
add_visibility_placeholder (PhoshHomeAssistantQuickSettingPrefs *self, const char *text)
{
  GtkWidget *row = adw_action_row_new ();

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), text);
  gtk_widget_set_sensitive (row, FALSE);
  adw_preferences_group_add (self->visibility_group, row);
}


static void
rebuild_visibility_group (PhoshHomeAssistantQuickSettingPrefs *self, GList *found)
{
  clear_visibility_group (self);

  if (found == NULL) {
    add_visibility_placeholder (self, _("No entities loaded — press Refresh"));
    return;
  }

  for (GList *l = found; l != NULL; l = l->next) {
    FoundEntity *entity = l->data;
    GtkWidget *row = adw_switch_row_new ();

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), entity->label);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), entity->entity_id);
    adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                               is_entity_shown (self, entity->entity_id));
    g_object_set_data_full (G_OBJECT (row),
                            "entity-id",
                            g_strdup (entity->entity_id),
                            g_free);
    g_signal_connect (row, "notify::active", G_CALLBACK (on_visibility_toggled), self);
    adw_preferences_group_add (self->visibility_group, row);
  }
}


static void
apply_found (PhoshHomeAssistantQuickSettingPrefs *self, GList *found)
{
  g_autofree char *saved = NULL;
  guint select_idx = GTK_INVALID_LIST_POSITION;
  guint idx = 0;

  saved = self->settings != NULL
    ? g_settings_get_string (self->settings, ENTITY_ID_KEY)
    : NULL;
  if (saved != NULL && saved[0] != '\0') {
    char *lower = g_ascii_strdown (saved, -1);
    g_free (saved);
    saved = lower;
  }

  self->populating = TRUE;
  gtk_string_list_splice (self->entity_model, 0,
                          g_list_model_get_n_items (G_LIST_MODEL (self->entity_model)),
                          NULL);
  g_ptr_array_set_size (self->entity_ids, 0);

  for (GList *l = found; l != NULL; l = l->next, idx++) {
    FoundEntity *entity = l->data;

    gtk_string_list_append (self->entity_model, entity->label);
    g_ptr_array_add (self->entity_ids, g_strdup (entity->entity_id));
    if (g_strcmp0 (entity->entity_id, saved) == 0)
      select_idx = idx;
  }

  adw_combo_row_set_selected (self->entity_combo, select_idx);
  self->populating = FALSE;

  rebuild_visibility_group (self, found);

  if (found == NULL) {
    adw_action_row_set_subtitle (self->load_row, _("No toggleable entities found"));
  } else {
    g_autofree char *msg = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                                         "%u entity", "%u entities",
                                                         g_list_length (found)),
                                            g_list_length (found));
    adw_action_row_set_subtitle (self->load_row, msg);
  }
}


static void
save_cache (PhoshHomeAssistantQuickSettingPrefs *self, GList *found)
{
  g_autoptr (JsonBuilder) builder = NULL;
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *url = NULL;
  g_autofree char *data = NULL;

  url = normalize_base_url (gtk_editable_get_text (GTK_EDITABLE (self->url_row)));
  if (url[0] == '\0' || self->settings == NULL)
    return;

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "v");
  json_builder_add_int_value (builder, 1);
  json_builder_set_member_name (builder, "base-url");
  json_builder_add_string_value (builder, url);
  json_builder_set_member_name (builder, "items");
  json_builder_begin_array (builder);
  for (GList *l = found; l != NULL; l = l->next) {
    FoundEntity *entity = l->data;

    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "id");
    json_builder_add_string_value (builder, entity->entity_id);
    json_builder_set_member_name (builder, "name");
    json_builder_add_string_value (builder, entity->name);
    json_builder_set_member_name (builder, "state");
    json_builder_add_string_value (builder, entity->state);
    json_builder_end_object (builder);
  }
  json_builder_end_array (builder);
  json_builder_end_object (builder);

  root = json_builder_get_root (builder);
  data = json_to_string (root, FALSE);

  g_settings_set_string (self->settings, CACHE_KEY, data);
}


static void
populate_entities (PhoshHomeAssistantQuickSettingPrefs *self, GBytes *body)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) err = NULL;
  gconstpointer data;
  gsize len;
  JsonNode *root;
  JsonArray *array;
  GHashTable *seen = NULL;
  guint n;
  guint added = 0;
  GList *found = NULL;

  if (body == NULL)
    return;

  len = g_bytes_get_size (body);
  if (len == 0) {
    adw_action_row_set_subtitle (self->load_row, _("Empty response from server"));
    return;
  }
  if (len > MAX_RESPONSE_BYTES) {
    adw_action_row_set_subtitle (self->load_row, _("Response from server is too large"));
    return;
  }

  data = g_bytes_get_data (body, NULL);
  if (data == NULL) {
    adw_action_row_set_subtitle (self->load_row, _("Empty response from server"));
    return;
  }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, len, &err)) {
    g_autofree char *msg = g_strdup_printf (_("Could not parse response: %s"),
                                            err != NULL ? err->message
                                                        : _("unknown error"));
    adw_action_row_set_subtitle (self->load_row, msg);
    return;
  }

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root)) {
    adw_action_row_set_subtitle (self->load_row, _("Unexpected response from server"));
    return;
  }

  array = json_node_get_array (root);
  n = json_array_get_length (array);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < n && added < MAX_ENTITIES; i++) {
    JsonObject *obj = json_array_get_object_or_null (array, i);
    const char *entity_id;
    const char *state;
    JsonObject *attrs;
    const char *friendly = NULL;

    if (obj == NULL)
      continue;

    entity_id = json_object_get_string_or_null (obj, "entity_id");
    if (entity_id == NULL || !domain_toggleable (entity_id))
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

    state = json_object_get_string_or_null (obj, "state");
    if (state == NULL)
      state = "unknown";

    attrs = json_object_get_object_or_null (obj, "attributes");
    if (attrs != NULL)
      friendly = json_object_get_string_or_null (attrs, "friendly_name");

    found = g_list_prepend (found, found_entity_new (id, friendly, state));
    added++;
  }
  g_hash_table_unref (seen);
  found = g_list_reverse (found);

  apply_found (self, found);
  save_cache (self, found);

  g_list_free_full (found, (GDestroyNotify) found_entity_free);
}


static void
on_load_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  TestData *data = user_data;
  PhoshHomeAssistantQuickSettingPrefs *self = data->self;
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GError) err = NULL;
  guint status;

  body = soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);

  if (!prefs_is_live (self)) {
    test_data_free (data);
    return;
  }

  gtk_spinner_stop (self->load_spinner);
  gtk_widget_set_visible (GTK_WIDGET (self->load_spinner), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->load_button), TRUE);

  if (body == NULL) {
    g_autofree char *msg;

    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && !data->timed_out) {
      test_data_free (data);
      return;
    }

    msg = data->timed_out
      ? g_strdup (_("Connection timed out"))
      : g_strdup_printf (_("Connection failed: %s"),
                         err != NULL ? err->message : _("unknown error"));
    adw_action_row_set_subtitle (self->load_row, msg);
    test_data_free (data);
    return;
  }

  status = soup_message_get_status (data->msg);
  if (status != SOUP_STATUS_OK) {
    g_autofree char *msg = g_strdup_printf (_("Server replied: HTTP %u %s"),
                                            status,
                                            soup_message_get_reason_phrase (data->msg) ?: "");
    adw_action_row_set_subtitle (self->load_row, msg);
    test_data_free (data);
    return;
  }

  populate_entities (self, body);
  test_data_free (data);
}


static void
on_load_clicked (PhoshHomeAssistantQuickSettingPrefs *self)
{
  g_autofree char *url = NULL;
  g_autofree char *token = NULL;
  SoupMessage *msg = NULL;
  TestData *data;

  if (!prefs_is_live (self))
    return;

  get_credentials (self, &url, &token);
  if (!credentials_valid (self, url, token, self->load_row))
    return;

  msg = auth_get_message (self, url, token, "/api/states");
  if (msg == NULL) {
    adw_action_row_set_subtitle (self->load_row, _("Invalid server URL"));
    return;
  }

  if (self->settings != NULL) {
    g_settings_set_string (self->settings, BASE_URL_KEY, url);
    g_settings_set_string (self->settings, TOKEN_KEY, token);
  }

  data = g_new0 (TestData, 1);
  data->self = g_object_ref (self);
  data->msg = g_object_ref (msg);
  data->cancellable = g_cancellable_new ();
  data->timeout_id = g_timeout_add_seconds (REQUEST_TIMEOUT_SECONDS,
                                            on_test_timed_out,
                                            data);

  gtk_widget_set_sensitive (GTK_WIDGET (self->load_button), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->load_spinner), TRUE);
  gtk_spinner_start (self->load_spinner);
  adw_action_row_set_subtitle (self->load_row, _("Refreshing…"));

  soup_session_send_and_read_async (self->session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    data->cancellable,
                                    on_load_finished,
                                    data);
  g_object_unref (msg);
}


static void
on_entity_selected (PhoshHomeAssistantQuickSettingPrefs *self)
{
  guint idx;

  if (!prefs_is_live (self) || self->populating ||
      self->settings == NULL || self->entity_ids == NULL)
    return;

  idx = adw_combo_row_get_selected (self->entity_combo);
  if (idx == GTK_INVALID_LIST_POSITION || idx >= self->entity_ids->len)
    return;

  g_settings_set_string (self->settings, ENTITY_ID_KEY,
                         g_ptr_array_index (self->entity_ids, idx));
}


/* Pending requests hold a reference to self and are bounded by a timeout, so
 * they cannot outlive the dialog indefinitely. */
static void
on_destroy (GtkWidget *widget, gpointer user_data)
{
  PhoshHomeAssistantQuickSettingPrefs *self =
    PHOSH_HOME_ASSISTANT_QUICK_SETTING_PREFS (widget);

  (void) user_data;
  self->destroyed = TRUE;
}


static void
phosh_home_assistant_quick_setting_prefs_finalize (GObject *object)
{
  PhoshHomeAssistantQuickSettingPrefs *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING_PREFS (object);

  g_clear_object (&self->entity_model);
  g_clear_pointer (&self->entity_ids, g_ptr_array_unref);
  g_clear_object (&self->session);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (phosh_home_assistant_quick_setting_prefs_parent_class)->finalize (object);
}


static void
phosh_home_assistant_quick_setting_prefs_class_init (PhoshHomeAssistantQuickSettingPrefsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = phosh_home_assistant_quick_setting_prefs_finalize;

  gtk_widget_class_set_template_from_resource (widget_class,
    "/mobi/phosh/plugins/home-assistant-quick-setting-prefs/prefs.ui");

  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, url_row);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, token_row);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, load_row);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, load_button);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, load_spinner);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, entity_combo);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, visibility_group);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, test_row);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, test_button);
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, test_spinner);

  gtk_widget_class_bind_template_callback (widget_class, on_url_apply);
  gtk_widget_class_bind_template_callback (widget_class, on_token_apply);
  gtk_widget_class_bind_template_callback (widget_class, on_load_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_test_clicked);
}


static void
load_settings (PhoshHomeAssistantQuickSettingPrefs *self)
{
  g_autofree char *url = NULL;
  g_autofree char *token = NULL;
  g_autofree char *entity = NULL;

  if (self->settings != NULL) {
    url = g_settings_get_string (self->settings, BASE_URL_KEY);
    token = g_settings_get_string (self->settings, TOKEN_KEY);
    entity = g_settings_get_string (self->settings, ENTITY_ID_KEY);
  }

  gtk_editable_set_text (GTK_EDITABLE (self->url_row), url != NULL ? url : "");
  gtk_editable_set_text (GTK_EDITABLE (self->token_row), token != NULL ? token : "");

  /* The configuration window only ever shows live data from the server —
   * the cache exists for the quick-setting page, not for configuring. */
  if (url != NULL && url[0] != '\0' && token != NULL && token[0] != '\0')
    on_load_clicked (self);
  else
    apply_found (self, NULL);

  if (entity != NULL && entity[0] != '\0')
    adw_action_row_set_subtitle (self->load_row,
                                 _("Main entity is selected below"));
}


static void
phosh_home_assistant_quick_setting_prefs_init (PhoshHomeAssistantQuickSettingPrefs *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  if (self->url_row == NULL || self->token_row == NULL ||
      self->load_row == NULL || self->load_button == NULL ||
      self->load_spinner == NULL || self->entity_combo == NULL ||
      self->visibility_group == NULL || self->test_row == NULL ||
      self->test_button == NULL || self->test_spinner == NULL) {
    g_warning ("Home Assistant prefs template is incomplete; dialog disabled");
    self->destroyed = TRUE;
    return;
  }

  self->settings = prefs_settings_new ();
  self->session = soup_session_new ();
  self->entity_model = gtk_string_list_new (NULL);
  self->entity_ids = g_ptr_array_new_with_free_func (g_free);

  g_signal_connect (self, "destroy", G_CALLBACK (on_destroy), NULL);

  adw_combo_row_set_model (self->entity_combo, G_LIST_MODEL (self->entity_model));

  g_signal_connect_object (self->entity_combo,
                           "notify::selected",
                           G_CALLBACK (on_entity_selected),
                           self,
                           G_CONNECT_SWAPPED);

  load_settings (self);
}
