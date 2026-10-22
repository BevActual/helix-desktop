# Greeter (Tauri)

**Path:** `greeter/helix-greeter-tauri` · **crate:** `helix-greeter` · **product notes:** `greeter/HELIX.md`

Login UI started by greetd. Same visual grammar as the lock screen (`lock-screen.md`: chrome over wallpaper; leftover form). Shares `core/shared-ui/` tokens with the shell and apps.

## Stack

Tauri 2 + Rust host, React + TypeScript + Vite page (`web/`, workspace `helix-greeter-web`, built into `dist/react/helix-greeter.js`).
The page shows a plain message if the bundle is missing, so a build without node still produces a greeter that fails visibly rather than silently.

## What Rust does (and the page cannot)

| Piece | Where | Rule |
|---|---|---|
| greetd protocol | `src-tauri/src/greetd.rs` | Length-prefixed JSON over `$GREETD_SOCK`; messages over 64 KiB refused; PAM text stripped of control characters and bounded. Unit-tested against a fake greetd. |
| Accounts and sessions | `src-tauri/src/system.rs` | Accounts from passwd (uid 1000–59999, real shell); sessions from `/usr/share/wayland-sessions/*.desktop`. |
| Commands | `main.rs` | `greeter_begin(username)` only for a listed account; `greeter_start(session_id)` only for a listed session id, command split on spaces (no shell) and sent with an empty environment. The page never names a command. |
| Appearance | `greeter_offer` | Reads `/var/lib/helix-greeter/appearance.json` (`HELIX_GREETER_STATE` in tests). Carries dark/light, palette colors, wallpaper file name, font, UI scale, corner radius, glass (`transparency_mode`), refraction, high contrast, and reduced motion. Missing file stays dark. The wallpaper URL is an asset-protocol path inside that directory. |

The page shows one of four steps (prompt, info, ready, failed) and sends back typed text. Wrong passwords read as one generic retry message.

## Tests

- `cargo test -p helix-greeter`: framing, step mapping, passwd and session parsing, a full login against a fake greetd.
- `tools/nested/ui-suite.sh greeter`: the real greeter in the nested compositor against `tools/nested/fake-greetd.py`: wrong password refused, retry, session started with a listed command.

## Session and install

`greeter/session/helix-greeter-session` is what greetd runs as the greeter user: a private niri (`/usr/bin/niri`) with `greeter-niri.kdl`
(no key bindings at all) and the greeter as its only client; niri exits when the greeter does. If the greeter or compositor is missing, or
exits with an error, the script falls back to `agreety` (greetd's text login) so a broken greeter cannot lock the machine out.
`/etc/helix-greeter/env` can hold machine-specific environment (for example `WEBKIT_DISABLE_COMPOSITING_MODE=1`).

```sh
cargo build --release -p helix-greeter
sudo tools/install-greeter.sh            # takes effect at the next login screen; greetd is not restarted
sudo tools/install-greeter.sh --revert   # restores the previous /etc/greetd/config.toml
```

`tools/nested/ui-suite.sh greeter-session` runs the real script in the nested compositor (chain starts and reaches greetd; both fallbacks).
Not covered: a real DRM seat as the `greetd` user and real PAM; the first real login screen is the proof.

## Not yet

1. ~~Wallpaper behind the card and the full appearance sync~~ — Settings **Appearance** has **Sync login screen** and **Sync greeter theme automatically**. `helix-greeter-apply-appearance` (`cargo build -p helix-desktop --bin helix-greeter-apply-appearance`) stages the payload and, when the state directory is not writable, installs it through `pkexec` (or `shell.greeter_sync.privilege_command`). Passwordless Polkit rules are still packaging.
2. ~~Power actions (restart, shut down) from the greeter~~ — two buttons at the bottom right, each asking "Shut down this computer?" / "Restart this computer?" first. The greeter runs `systemctl poweroff` or `systemctl reboot` (fixed arguments; the page can only name one of the two), so logind and polkit decide; `packaging/50-helix-greeter-power.rules` lets the `greetd` account do exactly those two actions. A refusal is shown as the system worded it. `HELIX_GREETER_POWER_LOG=<file>` makes the greeter write the action instead of doing it (nested tests).
3. ~~Remembering the last account and session~~ — after a successful start the greeter writes `~/.local/state/helix-greeter/last.json` (the greeter user's own file, `HELIX_GREETER_REMEMBER` to move it) with the login name and session id. The next screen lists that account first, marked "Last signed in" when there are several, and preselects that session; either is used only if the screen itself lists it.
4. Packaging (distro packages and Nix) for the session script, the apply helper, and compositor config; today `tools/install-greeter.sh` does the session script.

Do not merge the greeter into the session shell binary.
