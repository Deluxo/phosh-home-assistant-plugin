#include "phosh-plugin.h"
#include "hass-light-toggle.h"
#include <giomodule.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <dbus/dbus.h>

static char **g_io_phoss_hass_light_toggle_query (void);

static void
on_clicked (PhossHassLightToggle *self)
{
  GDBusConnection *connection = NULL;
  GVariant *result = NULL;
  const char *service = "org.freedesktop.DBus.Properties";
  const char *path = "/";
  const char *interface = "org.freedesktop.DBus.Properties";
  const char *property = "Active";

  connection = dbus_bus_get_sync (DBUS_BUS_SESSION, NULL, NULL);
  if (connection == NULL) {
    g_warning ("Cannot get session bus");
    return;
  }

  result = g_dbus_connection_call_sync (
                              connection,
                              dbus_node_get_name_sync (dbus_connection_get_unix_connection_for_bus_type_sync (connection, DBUS_BUS_TYPE_SESSION)),
                              service,
                              path,
                              interface,
                              property,
                              G_VARIANT_TYPE("bool"));

  if (result != NULL) {
    boolean active = g_variant_get_boolean (result);

    if (active) {
      phosh_status_icon_set_icon_name (self->info, "lightbulb-off-symbolic");
      phosh_status_icon_set_info (self->info, _("Light Off"));
    } else {
      phosh_status_icon_set_icon_name (self->info, "lightbulb-symbolic");
      phosh_status_icon_set_info (self->info, _("Light On"));
    }

    phoss_hass_light_toggle_set_active (PHOSS_HASS_LIGHT_TOGGLE (self), !active);
  }

  dbus_connection_flush (connection);
  dbus_connection_unref (connection);
}

static void
on_footer_clicked (PhossHassLightToggle *self)
{
  g_warning ("Config button clicked");
}

gboolean
phoss_hass_light_toggle_get_active (PhossHassLightToggle *self)
{
  return self->active;
}

void
phoss_hass_light_toggle_set_active (PhossHassLightToggle *self, Gboolean active)
{
  self->active = active;
}

char **
g_io_phoss_hass_light_toggle_query (void)
{
  char *extension_points[] = {PHOSH_PLUGIN_EXTENSION_POINT_QUICK_SETTING_WIDGET, NULL};

  return g_strdupv (extension_points);
}

typedef struct _PhossHassLightToggle PhossHassLightToggle;

struct _PhossHassLightToggle {
  PhoshQuickSetting parent;

  GtkWidget *info;
  GtkWidget *status_page;
  GtkWidget *placeholder;
  GtkWidget *label;
  GtkWidget *footer;

  Gboolean active;
  GDBusConnection *connection;
};

typedef struct {
  GIBusConnection *connection;
  GDBusObjectManager *object_manager;
  GDBusInterfaceSkeleton *skeleton;
} PhossHassLightTogglePrivate;

G_DECLARE_FINAL_TYPE (PhossHassLightToggle, phoss_hass_light_toggle, PHOSS, HASS_LIGHT_TOGGLE, PhoshQuickSetting)

G_DEFINE_TYPE (PhossHassLightToggle, phoss_hass_light_toggle, PHOSH_TYPE_QUICK_SETTING)

typedef struct {
  GIBusConnection *connection;
  GDBusObjectManager *object_manager;
  GDBusInterfaceSkeleton *skeleton;
} PhossHassLightTogglePrivate;

G_DEFINE_TYPE (PhossHassLightToggle, phoss_hass_light_toggle, PHOSH_TYPE_QUICK_SETTING)

static void
phoss_hass_light_toggle_class_init (PhossHassLightToggleClass *klass)
{
}

static void
phoss_hass_light_toggle_init (PhossHassLightToggle *self)
{
  self->active = FALSE;

  self->info = PHOSH_STATUS_ICON (phosh_quick_setting_get_status_icon (PHOSH_QUICK_SETTING (self)));
  self->status_page = phosh_quick_setting_get_status_page (PHOSH_QUICK_SETTING (self));
  self->placeholder = PHOSH_STATUS_PAGE_PLACEHOLDER (phosh_status_page_get_placeholder (PHOSH_STATUS_PAGE (self->status_page)));
  self->label = GTK_LABEL (gtk_builder_get_object (self->placeholder, "label"));
  self->footer = GTK_BUTTON (gtk_builder_get_object (self->placeholder, "footer"));
}
