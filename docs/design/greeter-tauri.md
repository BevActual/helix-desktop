# Greeter (Tauri)

**Path:** `greeter/helix-greeter-tauri` · **crate:** `helix-greeter` · **product notes:** `greeter/HELIX.md`

Login UI started by greetd. Same visual grammar as the lock screen (Chrome over wallpaper; leftover form). Shares `core/shared-ui/` tokens with shell and apps.

## Status

Scaffold: fullscreen Tauri window + shared-ui copy at build. Not yet wired to greetd, AccountsService, or appearance apply helpers.

## Next

1. greetd client protocol (user list, password/auth, session start).
2. Read synced appearance from the shell’s greeter state (palette, wallpaper, glass, refraction, …).
3. Session script that starts helix-niri + shell after a successful login.
4. Trusted-surface / anti-spoof framing aligned with `docs/design/trusted-surface.md` where it applies at login.

Do not merge greeter into the session shell binary.
