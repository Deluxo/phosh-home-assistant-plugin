#include "home-assistant-quick-setting.h"

#include <glib/gi18n.h>
#include <libsoup/soup.h>

#define HOME_ASSISTANT_QUICK_SETTING_SCHEMA "mobi.phosh.plugins.home-assistant-quick-setting"
#define BASE_URL_KEY                        "base-url"
#define TOKEN_KEY                           "token"
#define ENTITY_ID_KEY                       "entity-id"

#define HA_ON_ICON    "home-assistant-quick-setting-symbolic"
#define HA_OFF_ICON   "home-assistant-quick-setting-symbolic"

struct _PhoshHomeAssistantQuickSetting {
  PhoshQuickSetting        parent;

  PhoshStatusIcon         *info;
  PhoshStatusPage         *status_page;

  SoupSession             *session;
  GCancellable            *cancellable;
  GSettings               *settings;

  char                    *base_url;
  char                    *token;
  char                    *entity_id;
};

G_DEFINE_TYPE (PhoshHomeAssistantQuickSetting,
               phosh_home_assistant_quick_setting,
               PHOSH_TYPE_QUICK_SETTING);


static void
phosh_home_assistant_quick_setting_read_settings (PhoshHomeAssistantQuickSetting *self)
{
  g_autofree char *raw_token = NULL;

  g_free (self->base_url);
  self->base_url = g_settings_get_string (self->settings, BASE_URL_KEY);

  g_free (self->token);
  raw_token = g_settings_get_string (self->settings, TOKEN_KEY);
  if (raw_token && !g_str_equal (raw_token, ""))
    self->token = g_strdup_printf ("Bearer %s", raw_token);
  else
    self->token = g_strdup ("");

  g_free (self->entity_id);
  self->entity_id = g_settings_get_string (self->settings, ENTITY_ID_KEY);
}


static void
phosh_home_assistant_quick_setting_update_icon (PhoshHomeAssistantQuickSetting *self,
                                                const char                     *state)
{
  gboolean is_on = g_strcmp0 (state, "on") == 0;

  phosh_quick_setting_set_active (PHOSH_QUICK_SETTING (self), is_on);

  if (is_on)
    phosh_status_icon_set_info (self->info, _("On"));
  else
    phosh_status_icon_set_info (self->info, _("Off"));
}


static void
on_state_fetched (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);
  g_autoptr (GBytes) body = NULL;
  g_autoptr (GError) err = NULL;
  const char *text;
  g_autoptr (GRegex) regex = NULL;
  g_autoptr (GMatchInfo) match = NULL;

  body = soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);
  if (body == NULL) {
    phosh_status_icon_set_info (self->info, _("Error"));
    g_warning ("Failed to fetch HA state: %s", err->message);
    return;
  }

  text = g_bytes_get_data (body, NULL);
  if (text == NULL)
    return;

  regex = g_regex_new ("\"state\"\\s*:\\s*\"([^\"]+)\"", G_REGEX_DEFAULT, 0, NULL);
  if (regex == NULL)
    return;

  g_regex_match (regex, text, 0, &match);
  if (g_match_info_matches (match)) {
    g_autofree char *state = g_match_info_fetch (match, 1);
    phosh_home_assistant_quick_setting_update_icon (self, state);
  }
}


static void
on_toggle_done (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (user_data);
  g_autoptr (GError) err = NULL;
  SoupMessage *msg = NULL;
  g_autofree char *uri_str = NULL;

  soup_session_send_and_read_finish (SOUP_SESSION (source_object), res, &err);
  if (err) {
    phosh_status_icon_set_info (self->info, _("Error"));
    g_warning ("Failed to toggle HA entity: %s", err->message);
    return;
  }

  uri_str = g_strdup_printf ("%s/api/states/%s", self->base_url, self->entity_id);
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
                                    on_state_fetched,
                                    self);
  g_object_unref (msg);
}


static void
on_clicked (PhoshHomeAssistantQuickSetting *self)
{
  SoupMessage *msg = NULL;
  g_autofree char *uri_str = NULL;
  g_autofree char *body_str = NULL;
  g_autoptr (GBytes) body = NULL;

  if (self->base_url == NULL || self->token == NULL || self->entity_id == NULL) {
    phosh_status_icon_set_info (self->info, _("Not configured"));
    return;
  }

  if (g_str_equal (self->base_url, "") || g_str_equal (self->token, "") ||
      g_str_equal (self->entity_id, "")) {
    phosh_status_icon_set_info (self->info, _("Not configured"));
    return;
  }

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

  body_str = g_strdup_printf ("{\"entity_id\": \"%s\"}", self->entity_id);
  body = g_bytes_new_static (body_str, strlen (body_str));
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
on_footer_clicked (PhoshHomeAssistantQuickSetting *self)
{
  g_message ("Footer clicked; open settings panel of the plugin");
}


static void
refresh_state (PhoshHomeAssistantQuickSetting *self)
{
  SoupMessage *msg = NULL;
  g_autofree char *uri_str = NULL;

  if (self->base_url == NULL || self->token == NULL || self->entity_id == NULL)
    return;

  if (g_str_equal (self->base_url, "") || g_str_equal (self->token, "") ||
      g_str_equal (self->entity_id, ""))
    return;

  uri_str = g_strdup_printf ("%s/api/states/%s", self->base_url, self->entity_id);
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
                                    on_state_fetched,
                                    self);
  g_object_unref (msg);
}


static void
on_settings_changed (PhoshHomeAssistantQuickSetting *self)
{
  phosh_home_assistant_quick_setting_read_settings (self);
  refresh_state (self);
}


static void
phosh_home_assistant_quick_setting_finalize (GObject *gobject)
{
  PhoshHomeAssistantQuickSetting *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING (gobject);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->token, g_free);
  g_clear_pointer (&self->entity_id, g_free);

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

  if (self->base_url && self->token && self->entity_id &&
      !g_str_equal (self->base_url, "") &&
      !g_str_equal (self->token, "") &&
      !g_str_equal (self->entity_id, "")) {
    refresh_state (self);
  } else {
    phosh_status_icon_set_info (self->info, _("Not configured"));
  }
}
