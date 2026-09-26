#!/usr/bin/env bash
# On-device fasttrack deploy. Run THIS FILE on the phone, no SSH.
# Usage (on device):  cd /mnt/msdcard/dev/phosh-home-assistant-plugin
#   ./deploy.sh --setup   # once: install build deps (asks sudo pw)
#   ./deploy.sh           # every iteration: build, install, restart phosh
#   ./deploy.sh --restart-only
set -e
cd "$(dirname "$0")"

setup_deps() {
  sudo apt update && sudo apt install -y \
    meson ninja-build gcc pkg-config \
    phosh-dev libphosh-0.45-dev \
    libsoup-3.0-dev libadwaita-1-dev libgtk-4-dev libgtk-3-dev \
    libhandy-1-dev gsettings-desktop-schemas-dev libglib2.0-dev
}

restart_phosh() {
  echo "==> restarting phosh"
  systemctl --user restart phosh || pkill -x phosh || true
}

if [ "$1" = "--setup" ]; then setup_deps; exit 0; fi
if [ "$1" = "--restart-only" ]; then restart_phosh; exit 0; fi

echo "==> building"
if [ ! -f build/build.ninja ]; then meson setup build; fi
ninja -C build
echo "==> installing (sudo will prompt)"
sudo ninja -C build install
sudo glib-compile-schemas /usr/share/glib-2.0/schemas
restart_phosh
echo "==> done. Check quick-settings panel for Home Assistant."
