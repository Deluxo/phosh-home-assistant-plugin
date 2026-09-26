#include "phosh-plugin-prefs-config.h"

#include "home-assistant-quick-setting-prefs.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#define HOME_ASSISTANT_QUICK_SETTING_SCHEMA "mobi.phosh.plugins.home-assistant-quick-setting"
#define BASE_URL_KEY                        "base-url"
#define TOKEN_KEY                           "token"
#define ENTITY_ID_KEY                       "entity-id"
#define HIDDEN_ENTITIES_KEY                 "hidden-entities"

/* Domains handled by the generic homeassistant.toggle service */
static const char *toggleable_domains[] = {
  "cover", "fan", "humidifier", "input_boolean", "light",
  "lock", "media_player", "remote", "switch", "vacuum",
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
  GCancellable          *cancellable;

  GtkStringList         *entity_model;
  GPtrArray             *entity_ids; /* parallel to the model, owned */
  gboolean               populating;
};

G_DEFINE_TYPE (PhoshHomeAssistantQuickSettingPrefs,
               phosh_home_assistant_quick_setting_prefs,
               ADW_TYPE_PREFERENCES_DIALOG);


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

  g_settings_set_string (self->settings, BASE_URL_KEY, normalized);
  if (g_strcmp0 (url, normalized) != 0)
    gtk_editable_set_text (GTK_EDITABLE (self->url_row), normalized);
}


static void
on_token_apply (PhoshHomeAssistantQuickSettingPrefs *self)
{
  g_autofree char *token = NULL;

  token = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->token_row)));
  g_strstrip (token);
  g_settings_set_string (self->settings, TOKEN_KEY, token);
  if (g_strcmp0 (gtk_editable_get_text (GTK_EDITABLE (self->token_row)), token) != 0)
    gtk_editable_set_text (GTK_EDITABLE (self->token_row), token);
}


typedef struct {
  PhoshHomeAssistantQuickSettingPrefs *self;
  SoupMessage                         *msg;
} TestData;


static void
test_data_free (TestData *data)
{
  g_clear_object (&data->msg);
  g_clear_object (&data->self);
  g_free (data);
}


static void
on_test_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  TestData *data = user_data;
  PhoshHomeAssistantQuickSettingPrefs *self = data->self;
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GError) err = NULL;
  guint status;

  gtk_spinner_stop (self->test_spinner);
  gtk_widget_set_visible (GTK_WIDGET (self->test_spinner), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->test_button), TRUE);

  body = soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);
  if (body == NULL) {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      test_data_free (data);
      return;
    }
    g_autofree char *msg = g_strdup_printf (_("Connection failed: %s"), err->message);
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

  get_credentials (self, &url, &token);
  if (!credentials_valid (self, url, token, self->test_row))
    return;

  msg = auth_get_message (self, url, token, "/api/");
  if (msg == NULL) {
    adw_action_row_set_subtitle (self->test_row, _("Invalid server URL"));
    return;
  }

  /* Persist what was just tested so the quick-setting picks it up */
  g_settings_set_string (self->settings, BASE_URL_KEY, url);
  g_settings_set_string (self->settings, TOKEN_KEY, token);

  data = g_new0 (TestData, 1);
  data->self = g_object_ref (self);
  data->msg = g_object_ref (msg);

  gtk_widget_set_sensitive (GTK_WIDGET (self->test_button), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->test_spinner), TRUE);
  gtk_spinner_start (self->test_spinner);
  adw_action_row_set_subtitle (self->test_row, _("Connecting…"));

  soup_session_send_and_read_async (self->session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    self->cancellable,
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
  char *label;
} FoundEntity;


static void
found_entity_free (FoundEntity *found)
{
  g_free (found->entity_id);
  g_free (found->label);
  g_free (found);
}


static gboolean
is_entity_hidden (PhoshHomeAssistantQuickSettingPrefs *self, const char *entity_id)
{
  g_auto (GStrv) hidden = g_settings_get_strv (self->settings, HIDDEN_ENTITIES_KEY);

  return hidden != NULL &&
    g_strv_contains ((const char * const *) hidden, entity_id);
}


static void
set_entity_hidden (PhoshHomeAssistantQuickSettingPrefs *self,
                   const char                          *entity_id,
                   gboolean                             hidden)
{
  g_auto (GStrv) current = g_settings_get_strv (self->settings, HIDDEN_ENTITIES_KEY);
  g_autoptr (GPtrArray) updated = g_ptr_array_new_with_free_func (g_free);

  if (current != NULL) {
    for (guint i = 0; current[i] != NULL; i++) {
      if (g_strcmp0 (current[i], entity_id) != 0)
        g_ptr_array_add (updated, g_strdup (current[i]));
    }
  }
  if (hidden)
    g_ptr_array_add (updated, g_strdup (entity_id));
  g_ptr_array_add (updated, NULL);

  g_settings_set_strv (self->settings, HIDDEN_ENTITIES_KEY,
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
    set_entity_hidden (self, entity_id, !adw_switch_row_get_active (row));
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
    add_visibility_placeholder (self, _("No toggleable entities found"));
    return;
  }

  for (GList *l = found; l != NULL; l = l->next) {
    FoundEntity *entity = l->data;
    GtkWidget *row = adw_switch_row_new ();

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), entity->label);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), entity->entity_id);
    adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                               !is_entity_hidden (self, entity->entity_id));
    g_object_set_data_full (G_OBJECT (row),
                            "entity-id",
                            g_strdup (entity->entity_id),
                            g_free);
    g_signal_connect (row, "notify::active", G_CALLBACK (on_visibility_toggled), self);
    adw_preferences_group_add (self->visibility_group, row);
  }
}


static void
populate_entities (PhoshHomeAssistantQuickSettingPrefs *self, GBytes *body)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GError) err = NULL;
  g_autofree char *saved = NULL;
  gconstpointer data;
  gsize len;
  JsonNode *root;
  JsonArray *array;
  guint n;
  GList *found = NULL;
  guint select_idx = GTK_INVALID_LIST_POSITION;
  guint idx = 0;

  data = g_bytes_get_data (body, &len);
  if (data == NULL || len == 0) {
    adw_action_row_set_subtitle (self->load_row, _("Empty response from server"));
    return;
  }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data, len, &err)) {
    g_autofree char *msg = g_strdup_printf (_("Could not parse response: %s"), err->message);
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
  for (guint i = 0; i < n; i++) {
    JsonObject *obj = json_array_get_object_element (array, i);
    const char *entity_id;
    JsonObject *attrs;
    const char *friendly;
    FoundEntity *entity;

    if (obj == NULL ||
        !json_object_has_member (obj, "entity_id"))
      continue;

    entity_id = json_object_get_string_member (obj, "entity_id");
    if (entity_id == NULL || !domain_toggleable (entity_id))
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

    entity = g_new0 (FoundEntity, 1);
    entity->entity_id = g_strdup (entity_id);
    if (g_strcmp0 (friendly, entity_id) == 0)
      entity->label = g_strdup (entity_id);
    else
      entity->label = g_strdup_printf ("%s (%s)", friendly, entity_id);
    found = g_list_prepend (found, entity);
  }
  found = g_list_reverse (found);

  saved = g_settings_get_string (self->settings, ENTITY_ID_KEY);

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

  gtk_spinner_stop (self->load_spinner);
  gtk_widget_set_visible (GTK_WIDGET (self->load_spinner), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (self->load_button), TRUE);

  body = soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);
  if (body == NULL) {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      test_data_free (data);
      return;
    }
    g_autofree char *msg = g_strdup_printf (_("Connection failed: %s"), err->message);
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

  get_credentials (self, &url, &token);
  if (!credentials_valid (self, url, token, self->load_row))
    return;

  msg = auth_get_message (self, url, token, "/api/states");
  if (msg == NULL) {
    adw_action_row_set_subtitle (self->load_row, _("Invalid server URL"));
    return;
  }

  g_settings_set_string (self->settings, BASE_URL_KEY, url);
  g_settings_set_string (self->settings, TOKEN_KEY, token);

  data = g_new0 (TestData, 1);
  data->self = g_object_ref (self);
  data->msg = g_object_ref (msg);

  gtk_widget_set_sensitive (GTK_WIDGET (self->load_button), FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->load_spinner), TRUE);
  gtk_spinner_start (self->load_spinner);
  adw_action_row_set_subtitle (self->load_row, _("Loading…"));

  soup_session_send_and_read_async (self->session,
                                    msg,
                                    G_PRIORITY_DEFAULT,
                                    self->cancellable,
                                    on_load_finished,
                                    data);
  g_object_unref (msg);
}


static void
on_entity_selected (PhoshHomeAssistantQuickSettingPrefs *self)
{
  guint idx;

  if (self->populating)
    return;

  idx = adw_combo_row_get_selected (self->entity_combo);
  if (idx == GTK_INVALID_LIST_POSITION || idx >= self->entity_ids->len)
    return;

  g_settings_set_string (self->settings, ENTITY_ID_KEY,
                         g_ptr_array_index (self->entity_ids, idx));
}


static void
phosh_home_assistant_quick_setting_prefs_finalize (GObject *object)
{
  PhoshHomeAssistantQuickSettingPrefs *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING_PREFS (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->entity_model);
  g_clear_pointer (&self->entity_ids, g_ptr_array_unref);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
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
  g_autofree char *url = g_settings_get_string (self->settings, BASE_URL_KEY);
  g_autofree char *token = g_settings_get_string (self->settings, TOKEN_KEY);
  g_autofree char *entity = g_settings_get_string (self->settings, ENTITY_ID_KEY);

  gtk_editable_set_text (GTK_EDITABLE (self->url_row), url);
  gtk_editable_set_text (GTK_EDITABLE (self->token_row), token);

  if (entity[0] != '\0') {
    g_autofree char *subtitle = g_strdup_printf (_("Current: %s — load the list to change"), entity);
    adw_action_row_set_subtitle (self->load_row, subtitle);
  }
}


static void
phosh_home_assistant_quick_setting_prefs_init (PhoshHomeAssistantQuickSettingPrefs *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->settings = g_settings_new (HOME_ASSISTANT_QUICK_SETTING_SCHEMA);
  self->session = soup_session_new ();
  self->cancellable = g_cancellable_new ();
  self->entity_model = gtk_string_list_new (NULL);
  self->entity_ids = g_ptr_array_new_with_free_func (g_free);

  adw_combo_row_set_model (self->entity_combo, G_LIST_MODEL (self->entity_model));

  add_visibility_placeholder (self, _("Load the entity list to choose"));

  g_signal_connect_object (self->entity_combo,
                           "notify::selected",
                           G_CALLBACK (on_entity_selected),
                           self,
                           G_CONNECT_SWAPPED);

  load_settings (self);
}
