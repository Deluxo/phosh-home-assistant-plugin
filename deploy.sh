#!/usr/bin/env bash
# Fasttrack deploy: rsync source to device, build ON device (native aarch64),
# install system-wide, restart phosh. No .deb packaging.
# Usage: ./deploy.sh [--setup] [--restart-only] [--configure URL TOKEN ENTITY]
set -e
DEVICE="${DEVICE:-flx1sp}"
REMOTE_DIR="/mnt/msdcard/dev/phosh-home-assistant-plugin"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

rsync_project() {
  echo "==> syncing to $DEVICE:$REMOTE_DIR"
  ssh "$DEVICE" "mkdir -p $REMOTE_DIR"
  # tar pipe: no rsync needed on device
  tar -cz --exclude=attic --exclude=.git --exclude=build -f - -C "$SRC_DIR" . \
    | ssh "$DEVICE" "tar -xz -f - -C $REMOTE_DIR"
}

remote_build_install() {
  echo "==> building on device"
  ssh "$DEVICE" "cd $REMOTE_DIR && \
    if [ ! -f build/build.ninja ]; then meson setup build; fi && \
    ninja -C build && \
    echo '--> installing (sudo password will prompt)' && \
    sudo ninja -C build install && \
    sudo glib-compile-schemas /usr/share/glib-2.0/schemas"
}

remote_restart() {
  echo "==> restarting phosh"
  ssh "$DEVICE" "systemctl --user restart phosh || (pkill -x phosh; echo 'sent pkill, phosh should respawn')" || true
}

remote_setup_deps() {
  echo "==> installing build deps on device (sudo password will prompt)"
  ssh -t "$DEVICE" "sudo apt update && sudo apt install -y \
    meson ninja-build gcc pkg-config \
    phosh-dev libphosh-0.45-dev \
    libsoup-3.0-dev libadwaita-1-dev libgtk-4-dev libgtk-3-dev \
    libhandy-1-dev gsettings-desktop-schemas-dev glib2.0-dev"
}

if [ "$1" = "--setup" ]; then remote_setup_deps; exit 0; fi
if [ "$1" = "--restart-only" ]; then remote_restart; exit 0; fi

rsync_project
remote_build_install
remote_restart
echo "==> done. Check quick-settings panel for Home Assistant."
echo "    Configure: gsettings set mobi.phosh.plugins.home-assistant-quick-setting base-url 'http://HA:8123'"
