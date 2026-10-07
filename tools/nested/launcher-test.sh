#!/bin/bash
# HELIX_NESTED_SCRIPT: the launcher in the Tauri shell, end to end. Creates fake applications that leave marker files, then opens
# the launcher, types, launches with Enter and with a click, and checks what did and did not run.
#   tools/nested/run.sh launcher tools/nested/tauri-shell.sh   (with HELIX_NESTED_SCRIPT=tools/nested/launcher-test.sh)
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
M=$SCRATCH/markers; mkdir -p "$M" "$SCRATCH/data/applications"
app() { printf '[Desktop Entry]\nType=Application\nName=%s\nExec=touch %s/%s\n%s\n' "$1" "$M" "$2" "$3" > "$SCRATCH/data/applications/$2.desktop"; }
app "Helix Test App" helix-test "GenericName=Test Tool"
app "Another Thing" another-thing ""
app "Another Tool" another-tool ""
app "Hidden Thing" hidden-thing "NoDisplay=true"
app "Terminal Thing" terminal-thing "Terminal=true"
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
gone() { sleep 1.5; [ ! -e "$M/$1" ] && echo "ok: $1 did not run" || echo "FAIL: $1 ran"; }
wait_for "LIVE Sound" 1 120 || exit 1

echo "--- 1. open, empty query lists applications"
echo launcher >> "$CONTROL"; wait_for "PANEL open" 1 && grep "LAUNCHER apps" "$LOG" | tail -1; sleep 1; shot launcher-empty
echo "--- 2. type, then Enter launches the selected application and closes the launcher"
wtype "helix test"; sleep 1; shot launcher-typed; wtype -k Return
wait_for "LAUNCH helix-test.desktop" 1 20 && echo "ok: launch logged"; wait_for "PANEL closed" 1 20 && echo "ok: panel closed"
sleep 1; [ -e "$M/helix-test" ] && echo "ok: the application ran (marker exists)" || echo "FAIL: marker missing"
echo "--- 3. an application that is hidden or needs a terminal is not offered"
echo launcher >> "$CONTROL"; wait_for "PANEL open" 2; sleep 0.8; wtype "hidden thing"; sleep 0.8; wtype -k Return; gone hidden-thing
wtype -M ctrl a -m ctrl; wtype "terminal thing"; sleep 0.8; wtype -k Return; gone terminal-thing
echo "--- 4. no match: Enter does nothing, Escape closes"
wtype -M ctrl a -m ctrl; wtype "zzzqqq"; sleep 0.8; shot launcher-nomatch; wtype -k Return; sleep 0.8; wtype -k Escape
wait_for "PANEL closed" 2 20 && echo "ok: Escape closed the launcher"
echo "--- 5. a click launches the first row (the shorter name ranks first: Another Tool)"
echo launcher >> "$CONTROL"; wait_for "PANEL open" 3; sleep 0.8; wtype "another"; sleep 1
"$PT" 0.5 0.262 click; wait_for "LAUNCH another-tool.desktop" 1 20 && echo "ok: click launched"; sleep 1
[ -e "$M/another-tool" ] && echo "ok: the application ran (marker exists)" || echo "FAIL: marker missing"
echo "--- 6. arrow keys move the selection (query limited to the fake apps: Another Tool, then Another Thing)"
echo launcher >> "$CONTROL"; wait_for "PANEL open" 4; sleep 0.8; wtype "another"; sleep 1; wtype -k Down; sleep 0.4; wtype -k Return
wait_for "LAUNCH another-thing.desktop" 1 20 && echo "ok: Down moved to the second result and launched it"; sleep 1
[ -e "$M/another-thing" ] && echo "ok: the application ran (marker exists)" || echo "FAIL: marker missing"
echo "--- 7. nothing but the fake applications was ever launched"
LAUNCHED=$(grep '^LAUNCH ' "$LOG" | grep -v 'refused' | awk '{print $2}' | sort -u | tr '\n' ' ')
echo "launched: $LAUNCHED"
case "$LAUNCHED" in *[!a-z.\ -]*) echo "FAIL: odd ids";; esac
for id in $LAUNCHED; do case "$id" in helix-test.desktop|another-thing.desktop|another-tool.desktop) ;; *) echo "FAIL: launched a real application: $id";; esac; done
echo "refusals: $(grep -c 'LAUNCH refused' "$LOG")"
