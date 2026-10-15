# `helix-desktop` session soak

Goal: exercise the **installed** Rust `org.helix.Desktop` service on real hardware with the live
shell — NetworkManager, BlueZ, PipeWire, niri, Settings, and Agent Surface flows that nested tests
only fake.

What CI already covers without hardware: the C++ behavioural contract test
(`tests/desktop_service_test.cpp`, run against `target/*/helix-desktop`); frozen settings catalog
goldens; theme/template/Wallhaven parity via C++ reference generators in `core/helix-theme`.

## Run it

```sh
cargo build --release -p helix-desktop
tools/soak/rust-desktop.sh status
tools/soak/rust-desktop.sh capture /tmp/before.json   # snapshot while on the installed binary
tools/soak/rust-desktop.sh enable                       # optional: user drop-in → local build
tools/soak/rust-desktop.sh capture /tmp/dev.json
tools/soak/snapshot.py diff /tmp/before.json /tmp/dev.json
journalctl --user -u helix-service -f
tools/soak/rust-desktop.sh disable                      # remove drop-in, back to install
```

`enable` adds `~/.config/systemd/user/helix-service.service.d/rust-desktop.conf` so you can soak a
**local** `helix-desktop` without touching `/usr`. `disable` removes it. Restarting the unit makes
shell clients reconnect; if the bar does not recover, run `disable` and check the journal.

When you are already running a dev build via the drop-in, `capture` is enough — skip the C++/Rust
A/B flow; the C++ `helix-service` binary no longer exists.

## Expected `diff` output

When comparing two captures from the **same** implementation (install vs drop-in dev build),
snapshots should match structurally. Volatile values (signal strength, levels) are compared by type
only. Known environment differences:

- `shell.polkit_agent` defaults to `true` (the Tauri shell registers the native agent).
- `theme.templates.*` option lists depend on the installed template assets.

## Checklist

Use the real shell for each item and note pass/fail plus the journal line if it fails.

**Network (NetworkManager)**
- [ ] Wi-Fi list populates; signal and the connected network are right
- [ ] Connect to a saved network; connect to a new WPA2 network (password prompt appears once, reconnects)
- [ ] Forget a network; toggle Wi-Fi off/on
- [ ] Enterprise network (if available) prompts for identity/password
- [ ] DNS profile change (`Network1.SetDns`) sticks and reverts to automatic
- [ ] `systemctl restart NetworkManager` while the panel is open: panel shows unavailable, then recovers without restarting the service; a pending connect completes with "Device service disconnected"

**Bluetooth (BlueZ)**
- [ ] Adapter on/off; scan lists nearby devices
- [ ] Pair a device (PIN/confirm prompt appears), connect, disconnect, forget
- [ ] Cancel a pairing in progress
- [ ] `systemctl restart bluetooth`: same recovery behaviour as NetworkManager

**Audio (PipeWire/WirePlumber)**
- [ ] Output/input device lists, default device switch, volume and mute for devices and programs
- [ ] Microphone level meter in Settings → Sound moves and stops when the page closes
- [ ] `systemctl --user restart wireplumber pipewire`: service recovers, meters stop cleanly

**Displays (niri)**
- [ ] Display inventory matches `niri msg outputs`; hot-plug a monitor updates it
- [ ] Night Light rows appear once the shell registers gamma control

**Configuration**
- [ ] Change settings from Settings (toggle, slider, list reorder); values persist across a service restart
- [ ] Edit `config.toml` by hand: change shows up within a second without restarting anything
- [ ] Reset an overridden setting; ordered lists (dock pins, bar widgets, shortcuts, notification rules) round-trip
- [ ] `HELIX_CONFIG_HOME` / `XDG_*` overrides resolve to the same files as before

**Appearance**
- [ ] Switch builtin/custom/community palette; Reapply; template outputs written (kitty, gtk, qt, …)
- [ ] Save palette / set accent (light and dark mode)
- [ ] Wallpaper palette source, `auto` mode, `firefox-theme`: no errors (C++ helpers may still run for edge cases)
- [ ] **Wallhaven**: search, thumbnails appear, download sets the wallpaper, cancel works, offline mode blocks it

**Agent surface / assistant**
- [ ] Assistant tool call requiring confirmation shows the dialog and honours Allow/Deny
- [ ] Kill switch and activity view behave as before

**Lifecycle**
- [ ] Log out/in and a cold boot
- [ ] `kill -9` the service: systemd restarts it and the shell reconnects
- [ ] One full day of normal use; `journalctl --user -u helix-service -p warning` is quiet

## Recording results

Open an issue per failure with: the checklist item, `journalctl` excerpt, and the `capture`
diff when you used a dev drop-in.
