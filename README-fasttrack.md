# phosh-home-assistant-plugin — fasttrack

Out-of-tree Phosh quick-setting that toggles a Home Assistant entity.
Based on upstream `phosh/plugins/home-assistant-quick-setting` (v0.55.0,
matches FuriOS phosh 0.55.0 on device). Old broken `hass-light-toggle`
skeleton preserved in `attic/` for reference.

No .deb packaging yet — fast iteration only.

## State
- Builds as two shared modules: quick-setting (`gtk3/libphosh`) + prefs (`gtk4/adwaita`)
- Installs to system plugin dirs from `phosh-plugins.pc`, plus gsettings schema
- Source lives here, builds natively ON device (avoids cross-compile)

## Quick iteration (run on the phone, no SSH)
```bash
cd /mnt/msdcard/dev/phosh-home-assistant-plugin
./deploy.sh --setup   # once: install build deps (asks sudo pw)
./deploy.sh           # every iteration: build, install, restart phosh
./deploy.sh --restart-only
./uninstall.sh        # revert: remove plugin files, restart phosh
```

## Configure on device
```bash
ssh flx1sp
gsettings set mobi.phosh.plugins.home-assistant-quick-setting base-url 'http://homeassistant.local:8123'
gsettings set mobi.phosh.plugins.home-assistant-quick-setting token 'LONG_LIVED_TOKEN'
gsettings set mobi.phosh.plugins.home-assistant-quick-setting entity-id 'light.living_room'
systemctl --user restart phosh
journalctl --user -u phosh -f   # logs
```

## Later: .deb
`./deploy.sh` calls `ninja install`; packaging = wrap same file list
with `dpkg-deb` + schema compile in postinst. Not done yet.
