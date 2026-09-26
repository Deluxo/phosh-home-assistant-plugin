#!/usr/bin/env bash
# Revert fasttrack install: remove exactly the files deploy.sh installs,
# recompile schemas, restart phosh. Run ON DEVICE (needs sudo for rm).
set -e
PLUGINS_DIR="/usr/lib/aarch64-linux-gnu/phosh/plugins"
PREFS_DIR="$PLUGINS_DIR/prefs"
sudo rm -f \
  "$PLUGINS_DIR/libphosh-plugin-home-assistant-quick-setting.so" \
  "$PLUGINS_DIR/home-assistant-quick-setting.plugin" \
  "$PREFS_DIR/libphosh-plugin-prefs-home-assistant-quick-setting.so" \
  "/usr/share/phosh/plugins/icons/home-assistant-quick-setting-symbolic.svg" \
  "/usr/share/glib-2.0/schemas/mobi.phosh.plugins.home-assistant-quick-setting.gschema.xml" \
  "/usr/local/share/phosh/plugins/icons/home-assistant-quick-setting-symbolic.svg" \
  "/usr/local/share/glib-2.0/schemas/mobi.phosh.plugins.home-assistant-quick-setting.gschema.xml"
sudo glib-compile-schemas /usr/share/glib-2.0/schemas
if [ -d /usr/local/share/glib-2.0/schemas ]; then
  sudo glib-compile-schemas /usr/local/share/glib-2.0/schemas
fi
systemctl --user restart phosh || pkill -x phosh || true
echo "reverted"
