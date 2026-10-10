#!/bin/bash
# HELIX_NESTED_SCRIPT: how often do clients call the desktop service when nothing is happening?
# Run with the native shell as the program and Settings started here:
#   HELIX_NESTED_SCRIPT=tools/nested/call-rate.sh tools/nested/run.sh rate build-debug/helix
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); WINDOW=${HELIX_RATE_SECONDS:-20}
"$ROOT/build-debug/helix" msg status >/dev/null 2>&1
for _ in $(seq 1 60); do "$ROOT/build-debug/helix" msg status >/dev/null 2>&1 && break; sleep 0.5; done
"$ROOT/build-debug/helix-settings" --page appearance >/dev/null 2>&1 &
SETTINGS=$!
sleep "${HELIX_RATE_WARMUP:-12}"
timeout "$WINDOW" dbus-monitor --session "type='method_call'" > "$SCRATCH/calls.txt" 2>/dev/null
python3 - "$SCRATCH/calls.txt" "$WINDOW" <<'PY'
import re, sys, collections
text = open(sys.argv[1]).read(); secs = float(sys.argv[2])
c = collections.Counter()
for block in re.split(r"(?=^method call )", text, flags=re.M):
    m = re.search(r"destination=([\w.:]+) .*?interface=([\w.]+); member=(\w+)", block)
    if m and (m.group(1) == "org.helix.Desktop" or m.group(3) == "NameHasOwner"):
        c[(m.group(2).split(".")[-1], m.group(3))] += 1
total = sum(c.values())
print(f"calls to the desktop service (and NameHasOwner) over {secs:.0f} s idle: {total}  ({total / secs:.2f} per second)")
for (iface, member), n in c.most_common(8): print(f"  {n:4d}  {iface}.{member}")
PY
kill $SETTINGS 2>/dev/null
