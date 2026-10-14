# Rust `helix-desktop` soak

Goal: prove the Rust `org.helix.Desktop` service is a drop-in for C++ `helix-service` on real
hardware before the default bus owner is flipped (`cpp-to-rust.md`, "One bus owner").

What is already covered without hardware: every method exists; the C++ service contract test
(`tests/desktop_service_test.cpp`, run in CI against the Rust binary); the Rust catalog matches
the C++ pristine catalog; theme/template/Wallhaven behaviour has C++ goldens. What a soak adds is
the parts those fake: NetworkManager, BlueZ, PipeWire, niri and the live shell.

## Run it

```sh
cargo build --release -p helix-desktop
tools/soak/rust-desktop.sh capture /tmp/cpp.json          # C++ service, current session
tools/soak/rust-desktop.sh enable                         # drop-in override + restart the unit
tools/soak/rust-desktop.sh capture /tmp/rust.json
tools/soak/snapshot.py diff /tmp/cpp.json /tmp/rust.json  # structural diff, exit 1 on differences
journalctl --user -u helix-service -f                     # watch warnings while you use the desktop
tools/soak/rust-desktop.sh disable                        # back to C++ (always safe)
```

`enable` only adds `~/.config/systemd/user/helix-service.service.d/rust-desktop.conf`; the install
is untouched and `disable` removes it. Restarting the unit makes shell clients reconnect; if the
bar does not recover, `disable` first and report what the journal said.

## Expected `diff` output

`implementation` differs by design. Volatile values (signal strength, levels) are compared by type
only. Anything else is a bug to report: the structure of every domain snapshot and every settings
row (ids, types, pages, groups, options) should be identical. Known and accepted:

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
- [ ] `HELIX_CONFIG_HOME` / `XDG_*` overrides resolve to the same files as C++

**Appearance**
- [ ] Switch builtin/custom/community palette; Reapply; template outputs written (kitty, gtk, qt, …)
- [ ] Save palette / set accent (light and dark mode)
- [ ] Wallpaper palette source, `auto` mode, `firefox-theme`: still handled (via the C++ helpers), no errors
- [ ] **Wallhaven**: search, thumbnails appear, download sets the wallpaper, cancel works, offline mode blocks it

**Agent surface / assistant**
- [ ] Assistant tool call requiring confirmation shows the dialog and honours Allow/Deny
- [ ] Kill switch and activity view behave as before

**Lifecycle**
- [ ] Log out/in and a cold boot with the override enabled
- [ ] `kill -9` the service: systemd restarts it and the shell reconnects
- [ ] One full day of normal use; `journalctl --user -u helix-service -p warning` is quiet

## Recording results

Open an issue per failure with: the checklist item, `journalctl` excerpt, and the `capture`
diff. When every box has passed on at least two machines (one with Bluetooth + enterprise Wi-Fi),
the default can flip: make `helix-desktop` the installed `helix-service` unit's `ExecStart`, keep
the C++ service for one release behind `HELIX_SERVICE_BIN`, then delete `src/desktop/` per the
cutover rules in `cpp-to-rust.md`.
