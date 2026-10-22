# The lock screen

`helix-lock` (`services/helix-lock`) is the session's lock screen. It replaces swaylock on a plain colour, and it is drawn in the same grammar as the
login screen (`greeter-tauri.md`): the wallpaper blurred and dimmed, a large clock and date in the upper part, and a card with the account's avatar and
name over a pill-shaped password field. The accent colour is the person's (`[theme] accent` in the settings file).

## How it works

* It is an **ext-session-lock-v1** client. It asks the compositor to lock, puts one lock surface on every output (a display plugged in while locked
  gets one too), and draws on each. **If the process dies the compositor keeps the session locked**, so a crash shows a locked screen and never the
  desktop. A second locker started while one is running is refused by the compositor and exits with status 1.
* The password is typed into a buffer that is wiped when cleared, refused or dropped (`entry.rs`), shown only as dots, and handed to **PAM** once
  (`pam.rs`: `pam_authenticate` then `pam_acct_mgmt`, on a thread, so a refusal PAM delays on purpose does not freeze the screen). A wrong password
  empties the field, shakes it, and says "That is not right. Try again."; Caps Lock is announced. Escape or Ctrl+U clears the field.
* PAM service: `helix-lock` (the package installs `/etc/pam.d/helix-lock`, which includes `system-auth`, the same stack as the login), else `swaylock`,
  else `login`, so a development machine without the package still unlocks.
* `helix-lock --daemonize` returns, with status 0, once the lock is in place. "Lock before sleep" in `helix-idle` waits on exactly that. It does this by
  starting the locker as its own process and waiting on a pipe, not by `fork`: a fork after the display and fonts are set up deadlocked in testing.
* Exit codes: 0 locked and later unlocked, 1 the compositor refused (already locked), 2 this machine cannot do it (no Wayland display, no
  ext-session-lock). Everything that starts the locker (`helix-idle`, the shell's lock action, `helix-panel lock`, the `Super+Alt+L` bind) falls back to
  swaylock on 2 or when `helix-lock` is not installed, and never on 1.

## Tests

`tools/nested/helix-lock-test.sh` runs it in a nested compositor against a real PAM stack made for the test (`pam_exec` and a script that accepts one
value, read from a folder of its own through `HELIX_LOCK_PAM_DIR`; there is no way to skip the check). It asserts: `--daemonize` returns when locked; a
wrong password does not release the lock; a second locker cannot take over; the right password releases it and the locker leaves. A locked session
cannot be screenshotted, so `HELIX_LOCK_SNAPSHOT=<dir>` makes the locker write the picture it drew (dots, never text) to `<dir>/lock.png` for review.
Unit tests cover the typing state (`entry.rs`), PAM through a real stack, the clock text and accent (`facts.rs`) and the drawing (`paint.rs`).

## Not done

Per-output wallpapers are used, but the blur is the same strength everywhere. Not yet on the screen: a battery indicator, media controls, notifications
(count only, never text), a keyboard-layout switch, and a "switch user" or power button. A fingerprint or a security key would be a second PAM prompt
the screen does not yet draw. The lock screen does not follow the interface scale.
