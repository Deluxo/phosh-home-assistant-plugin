# Local entry point for the on-device fasttrack workflow.
# Builds natively ON the phone (avoids cross-compile); this machine only
# orchestrates over ssh. Replaces deploy.sh / uninstall.sh.
#
#   make            # sync + build + install + restart phosh (main loop)
#   make local      # run ON the device: build + install + restart here, no ssh
#   make setup      # once: install build deps on device (asks sudo pw)
#   make sync       # push sources to device (no build)
#   make pull       # pull device-side edits of tracked files back here
#   make restart    # restart phosh on device only
#   make uninstall  # remove installed plugin files, restart phosh
#   make validate   # local sanity check: XML well-formedness of .ui/schema

DEVICE      ?= flx1sp
DEVICE_DIR  ?= /mnt/msdcard/dev/phosh-home-assistant-plugin
PLUGINS_DIR  = /usr/lib/aarch64-linux-gnu/phosh/plugins

SSH = ssh $(DEVICE)

.PHONY: deploy local setup sync pull restart uninstall validate

deploy: sync
	$(SSH) 'cd $(DEVICE_DIR) && make local'

# Build + install + restart right here. Run ON the device (needs the
# toolchain from `make setup`); fails on the dev machine by design.
local:
	set -e; \
	if [ ! -f build/build.ninja ]; then \
	  meson setup --prefix=/usr build; \
	elif grep -q /usr/local build/meson-private/cmd_line.txt 2>/dev/null; then \
	  echo "dropping stale build dir (was configured with /usr/local prefix)"; \
	  rm -rf build && meson setup --prefix=/usr build; \
	fi; \
	ninja -C build; \
	echo "installing (sudo will prompt)"; \
	sudo ninja -C build install; \
	sudo glib-compile-schemas /usr/share/glib-2.0/schemas; \
	sudo systemctl restart phosh; \
	echo "done. Check quick-settings panel for Home Assistant."

setup:
	$(SSH) 'sudo apt update && sudo apt install -y \
	  make meson ninja-build gcc pkg-config \
	  phosh-dev libphosh-0.45-dev \
	  libsoup-3.0-dev libadwaita-1-dev libgtk-4-dev libgtk-3-dev \
	  libjson-glib-dev \
	  libhandy-1-dev gsettings-desktop-schemas-dev libglib2.0-dev'

sync:
	tar czf - --exclude=build --exclude=.git . \
	  | $(SSH) 'mkdir -p $(DEVICE_DIR) && tar xzf - -C $(DEVICE_DIR)'
	@echo "synced to $(DEVICE):$(DEVICE_DIR)"

pull:
	$(SSH) 'cd $(DEVICE_DIR) && tar czf - $(shell git ls-files)' | tar xzf -
	@echo "pulled device edits into $$(pwd)"

restart:
	$(SSH) 'sudo systemctl restart phosh'

uninstall:
	$(SSH) 'set -e; \
	sudo rm -f \
	  $(PLUGINS_DIR)/libphosh-plugin-home-assistant-quick-setting.so \
	  $(PLUGINS_DIR)/home-assistant-quick-setting.plugin \
	  $(PLUGINS_DIR)/prefs/libphosh-plugin-prefs-home-assistant-quick-setting.so \
	  /usr/share/phosh/plugins/icons/home-assistant-quick-setting-symbolic.svg \
	  /usr/share/glib-2.0/schemas/mobi.phosh.plugins.home-assistant-quick-setting.gschema.xml \
	  /usr/local/share/phosh/plugins/icons/home-assistant-quick-setting-symbolic.svg \
	  /usr/local/share/glib-2.0/schemas/mobi.phosh.plugins.home-assistant-quick-setting.gschema.xml; \
	sudo glib-compile-schemas /usr/share/glib-2.0/schemas; \
	if [ -d /usr/local/share/glib-2.0/schemas ]; then \
	  sudo glib-compile-schemas /usr/local/share/glib-2.0/schemas; \
	fi; \
	sudo systemctl restart phosh \
	echo reverted'

validate:
	python3 -c "import sys, xml.dom.minidom as m; \
	  [m.parse(f) for f in ['src/prefs/prefs.ui', 'src/qs.ui', \
	    'src/mobi.phosh.plugins.home-assistant-quick-setting.gschema.xml']]; \
	  print('XML OK')"
