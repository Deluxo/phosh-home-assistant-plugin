#include "phosh-plugin-prefs-config.h"

#include "home-assistant-quick-setting-prefs.h"

#include <glib/gi18n.h>

#define HOME_ASSISTANT_QUICK_SETTING_SCHEMA "mobi.phosh.plugins.home-assistant-quick-setting"
#define BASE_URL_KEY                        "base-url"
#define TOKEN_KEY                           "token"
#define ENTITY_ID_KEY                       "entity-id"

struct _PhoshHomeAssistantQuickSettingPrefs {
  AdwPreferencesDialog  parent;

  AdwEntryRow           *url_row;
  AdwEntryRow           *token_row;
  AdwEntryRow           *entity_row;
  GSettings             *settings;
};

G_DEFINE_TYPE (PhoshHomeAssistantQuickSettingPrefs,
               phosh_home_assistant_quick_setting_prefs,
               ADW_TYPE_PREFERENCES_DIALOG);


static void
on_url_apply (PhoshHomeAssistantQuickSettingPrefs *self)
{
  const char *url = gtk_editable_get_text (GTK_EDITABLE (self->url_row));
  g_settings_set_string (self->settings, BASE_URL_KEY, url);
}


static void
on_token_apply (PhoshHomeAssistantQuickSettingPrefs *self)
{
  const char *token = gtk_editable_get_text (GTK_EDITABLE (self->token_row));
  g_settings_set_string (self->settings, TOKEN_KEY, token);
}


static void
on_entity_apply (PhoshHomeAssistantQuickSettingPrefs *self)
{
  const char *entity = gtk_editable_get_text (GTK_EDITABLE (self->entity_row));
  g_settings_set_string (self->settings, ENTITY_ID_KEY, entity);
}


static void
phosh_home_assistant_quick_setting_prefs_finalize (GObject *object)
{
  PhoshHomeAssistantQuickSettingPrefs *self = PHOSH_HOME_ASSISTANT_QUICK_SETTING_PREFS (object);

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
  gtk_widget_class_bind_template_child (widget_class, PhoshHomeAssistantQuickSettingPrefs, entity_row);

  gtk_widget_class_bind_template_callback (widget_class, on_url_apply);
  gtk_widget_class_bind_template_callback (widget_class, on_token_apply);
  gtk_widget_class_bind_template_callback (widget_class, on_entity_apply);
}


static void
load_settings (PhoshHomeAssistantQuickSettingPrefs *self)
{
  g_autofree char *url = g_settings_get_string (self->settings, BASE_URL_KEY);
  g_autofree char *token = g_settings_get_string (self->settings, TOKEN_KEY);
  g_autofree char *entity = g_settings_get_string (self->settings, ENTITY_ID_KEY);

  gtk_editable_set_text (GTK_EDITABLE (self->url_row), url);
  gtk_editable_set_text (GTK_EDITABLE (self->token_row), token);
  gtk_editable_set_text (GTK_EDITABLE (self->entity_row), entity);
}


static void
phosh_home_assistant_quick_setting_prefs_init (PhoshHomeAssistantQuickSettingPrefs *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->settings = g_settings_new (HOME_ASSISTANT_QUICK_SETTING_SCHEMA);

  load_settings (self);
}
