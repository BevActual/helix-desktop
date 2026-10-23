#!/bin/bash
# The Helix lock screen in a nested compositor: it takes the session lock, a wrong password does not release it (and the screen says so), a second
# locker cannot take over, and the right one releases it. The password is checked by a real PAM stack made for the test (pam_exec and a script that
# accepts one value), read from a folder of its own with HELIX_LOCK_PAM_DIR.
#   HELIX_NESTED_SCRIPT=tools/nested/helix-lock-test.sh tools/nested/run.sh helix-lock tools/nested/idle-program.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
BIN=${HELIX_LOCK_BIN:-$(ls -t "$ROOT/target/fast/helix-lock" "$ROOT/target/release/helix-lock" 2>/dev/null | head -1)}
PAM=$SCRATCH/pam; mkdir -p "$PAM"
PAM_EXEC=$(ls /usr/lib64/security/pam_exec.so /lib64/security/pam_exec.so /usr/lib/security/pam_exec.so 2>/dev/null | head -1)
[ -n "$PAM_EXEC" ] || { echo "skip: no pam_exec on this machine"; exit 0; }
printf '#!/bin/sh\ntyped=$(tr -d "\\0\\n")\n[ "$typed" = "correct horse" ]\n' > "$PAM/check.sh"; chmod +x "$PAM/check.sh"
printf 'auth required %s expose_authtok quiet %s\naccount required pam_permit.so\n' "$PAM_EXEC" "$PAM/check.sh" > "$PAM/helix-lock-test"
mkdir -p "$SCRATCH/snap"; export HELIX_LOCK_SNAPSHOT=$SCRATCH/snap
snap() { sleep 0.6; cp "$SCRATCH/snap/lock.png" "$SCRATCH/$1.png" 2>/dev/null; }   # the locked session cannot be screenshotted: the locker writes its own picture
# A wallpaper with some structure, so the blur and the dimming can be seen (written with Pillow when it is there; otherwise the plain colour shows).
mkdir -p "$XDG_CONFIG_HOME/helix"
python3 - "$SCRATCH/wall.png" <<'PY' && printf '[wallpaper.default]\npath = "%s"\n' "$SCRATCH/wall.png" > "$XDG_CONFIG_HOME/helix/wallpaper.toml"
import sys
from PIL import Image, ImageDraw
w, h = 1600, 900
img = Image.new("RGB", (w, h))
px = img.load()
for y in range(h):
    for x in range(w):
        px[x, y] = (int(40 + 150 * x / w), int(60 + 120 * y / h), int(200 - 120 * x / w))
d = ImageDraw.Draw(img)
for i in range(12):
    d.ellipse([100 + i * 120, 150 + (i % 4) * 160, 260 + i * 120, 310 + (i % 4) * 160], fill=(255 - i * 12, 200, 60 + i * 15))
img.save(sys.argv[1])
PY
# A battery to show (the nested machine may have none).
mkdir -p "$SCRATCH/power/BAT0"; printf Battery > "$SCRATCH/power/BAT0/type"; printf 64 > "$SCRATCH/power/BAT0/capacity"; printf Discharging > "$SCRATCH/power/BAT0/status"
export HELIX_LOCK_POWER_DIR=$SCRATCH/power
# The interface scale (the shell writes this file): the lock screen is laid out larger by it.
mkdir -p "$XDG_RUNTIME_DIR"; printf 1.25 > "$XDG_RUNTIME_DIR/helix-ui-scale"
export HELIX_LOCK_PAM_DIR=$PAM HELIX_LOCK_PAM_SERVICE=helix-lock-test
# No account picture for the first lock (this machine's own would otherwise replace the initials circle that is measured below).
export HELIX_LOCK_AVATAR=$SCRATCH/no-such-picture
sleep 3
echo "--- locking"
"$BIN" --daemonize 2>"$SCRATCH/lock.err"; code=$?
[ "$code" = 0 ] && echo "ok: --daemonize returned once the lock was in place" || fail "daemonize exit $code"
sleep 1.5
LOCKER=$(pgrep -x helix-lock | head -1)
[ -n "$LOCKER" ] && echo "ok: the locker is running" || fail "no locker"
snap locked-empty
python3 - "$SCRATCH/locked-empty.png" <<'PY' && echo "ok: at an interface scale of 1.25 the avatar is $(python3 -c 'print(round(76*1.25))') px wide, not 76" || fail "the lock screen did not follow the interface scale"
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB"); w, h = img.size
xs = [x for y in range(h) for x in range(w) if img.getpixel((x, y)) == (0x0a, 0x84, 0xff)]
width = max(xs) - min(xs) + 1
assert abs(width - 95) <= 3, width
PY
echo "--- a wrong password"
wtype "wrong horse"; sleep 0.4; snap typing; wtype -k Return; sleep 0.25; snap wrong; sleep 2
kill -0 "$LOCKER" 2>/dev/null && echo "ok: a wrong password did not release the lock" || fail "the lock was released by a wrong password"
echo "--- a second locker"
before=$(pgrep -cx helix-lock)
"$BIN"; second=$?
after=$(pgrep -cx helix-lock)
[ "$second" != 0 ] && [ "$after" = "$before" ] && echo "ok: a second locker could not take over (exit $second)" || fail "second locker: exit $second, $before -> $after"
echo "--- the right password"
wtype -k Escape; wtype "correct horse"; sleep 0.3; wtype -k Return
for _ in $(seq 1 30); do kill -0 "$LOCKER" 2>/dev/null || break; sleep 0.25; done
kill -0 "$LOCKER" 2>/dev/null && fail "the right password did not release the lock: $(cat "$SCRATCH/lock.err")" || echo "ok: the right password released it and the locker left"
sleep 1
niri msg --json outputs >/dev/null 2>&1 && echo "ok: the compositor is running normally after the unlock" || fail "compositor gone"
echo "--- an account picture replaces the initials"
python3 - "$SCRATCH/face.png" <<'PY'
import sys
from PIL import Image
Image.new("RGB", (300, 200), (0, 200, 0)).save(sys.argv[1])
PY
HELIX_LOCK_AVATAR=$SCRATCH/face.png "$BIN" --daemonize 2>"$SCRATCH/lock2.err"
sleep 1.5
LOCKER=$(pgrep -x helix-lock | head -1)
snap locked-picture
python3 - "$SCRATCH/locked-picture.png" <<'PY' && echo "ok: the picture is in the avatar circle and the accent circle is gone" || fail "no picture on the lock screen"
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB"); w, h = img.size
green = [(x, y) for y in range(h) for x in range(w) if img.getpixel((x, y)) == (0, 200, 0)]
accent = [1 for y in range(0, h, 2) for x in range(0, w, 2) if img.getpixel((x, y)) == (0x0a, 0x84, 0xff)]
assert len(green) > 3000 and not accent, (len(green), len(accent))
PY
wtype "correct horse"; sleep 0.3; wtype -k Return
for _ in $(seq 1 30); do kill -0 "$LOCKER" 2>/dev/null || break; sleep 0.25; done
kill -0 "$LOCKER" 2>/dev/null && fail "could not unlock the second time" || echo "ok: unlocked again"
[ "$FAILED" = 0 ] && echo "PASS: helix-lock" || exit 1
