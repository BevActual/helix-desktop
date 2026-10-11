# Greeter (Tauri)

**Path:** `greeter/helix-greeter-tauri` · **crate:** `helix-greeter` · **product notes:** `greeter/HELIX.md`

Login UI started by greetd. Same visual grammar as the lock screen (chrome over wallpaper; leftover form). Shares `core/shared-ui/` tokens with the shell and apps.

## Stack

Tauri 2 + Rust host, React + TypeScript + Vite page (`web/`, workspace `helix-greeter-web`, built into `dist/react/helix-greeter.js`).
The page shows a plain message if the bundle is missing, so a build without node still produces a greeter that fails visibly rather than silently.

## What Rust does (and the page cannot)

| Piece | Where | Rule |
|---|---|---|
| greetd protocol | `src-tauri/src/greetd.rs` | Length-prefixed JSON over `$GREETD_SOCK`; messages over 64 KiB refused; PAM text stripped of control characters and bounded. Unit-tested against a fake greetd. |
| Accounts and sessions | `src-tauri/src/system.rs` | Accounts from passwd (uid 1000–59999, real shell); sessions from `/usr/share/wayland-sessions/*.desktop`. |
| Commands | `main.rs` | `greeter_begin(username)` only for a listed account; `greeter_start(session_id)` only for a listed session id, command split on spaces (no shell) and sent with an empty environment. The page never names a command. |
| Appearance | `greeter_offer` | Reads `dark` from `/var/lib/helix-greeter/appearance.json` if present; defaults to dark. |

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

1. Wallpaper behind the card and the full appearance sync (palette, glass, font, scale) from the shell's greeter state; the privileged sync helper does not exist in this tree.
2. Power actions (restart, shut down) from the greeter.
3. Remembering the last account and session.
4. Packaging (distro packages and Nix) for the session script and compositor config; today `tools/install-greeter.sh` does it.

Do not merge the greeter into the session shell binary.
