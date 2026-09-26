# phosh Home Assistant Quick-Setting Plugin

A [phosh](https://github.com/mojave/phosh) quick-setting plugin that connects a
Phosphan phone to your [Home Assistant](https://www.home-assistant.io/) server.

- A **quick-setting tile** in the status panel: tap toggles the main entity,
  or refreshes all states when no main entity is set.
- A **status page** listing every toggleable entity (lights, switches, covers,
  locks, fans, …) with a switch per row; clicking anywhere on a row also
  toggles it. Toggles are optimistic and confirmed by polling `/api/states`
  until the flip is observed.
- A **prefs dialog** (GTK4/libadwaita) for configuration: server URL, long-lived
  access token, main entity ID, and hidden entities.

Settings live in GSettings under
`mobi.phosh.plugins.home-assistant-quick-setting`.

## Repository layout

```
Makefile                  on-device fasttrack workflow (deploy/test entry point)
meson.build               top-level meson project
src/                      plugin + prefs sources, .ui templates, gschema, icon
attic/                    dead code, kept for reference only
.envrc / shell.nix        nix dev shell: make + ssh + tar + python3
```

## Prerequisites

- SSH access to the phone. The Makefile targets `DEVICE ?= flx1sp` and
  `DEVICE_DIR ?= /mnt/msdcard/dev/phosh-home-assistant-plugin`; override either
  with `make DEVICE=... DEVICE_DIR=...`.
- A nix dev shell (loaded automatically via `.envrc`) providing `make`, `ssh`,
  `tar` and `python3`.

The build is done **natively on the phone** — no cross-compilation. The dev
machine only orchestrates over ssh, so it needs none of the C toolchain.

## One-time setup

```sh
make setup    # install build deps on the device (prompts for sudo password)
```

This installs make/meson/ninja/gcc plus the phosh and GTK development packages
on the phone.

## Deploying

The main loop is a single command from the dev machine:

```sh
make            # == make deploy: sync sources, build + install on device, restart phosh
```

Individual steps, if you need them separately:

| Target       | What it does                                              | Runs where   |
|--------------|-----------------------------------------------------------|--------------|
| `deploy`     | `sync`, then `local` over ssh                             | dev machine  |
| `sync`       | tar-pipe sources to the device (no build)                 | dev machine  |
| `pull`       | pull tracked files back from the device                   | dev machine  |
| `restart`    | restart phosh only                                        | dev machine  |
| `local`      | configure + build + install (`sudo`) + restart, no ssh    | **on-device**|
| `uninstall`  | remove installed plugin files, recompile schemas, restart | dev machine  |

To run the workflow directly on the phone (e.g. from an on-device shell):

```sh
cd /mnt/msdcard/dev/phosh-home-assistant-plugin && make local
```

Note: `local` intentionally fails off-device — it requires the toolchain from
`make setup`. After deploying, check that **Home Assistant** appears in the
quick-settings panel.

## Testing

- `make validate` — fast local sanity check (no device needed): verifies XML
  well-formedness of `src/qs.ui`, `src/prefs/prefs.ui` and the gschema file.
- On-device, meson registers a schema test that runs
  `glib-compile-schemas --strict --dry-run`; run it with `meson test` after
  configuring (`meson setup build`).
- Manual: open the quick-settings panel, toggle an entity from both the tile
  and its row switch, confirm the state flips in Home Assistant's own UI, and
  verify the prefs dialog saves settings.

## Uninstalling

```sh
make uninstall    # remove all installed files, recompile schemas, restart phosh
```
