#!/bin/bash
# Idle CPU of the Tauri shell only (not helix-service). $1 is the scratch directory.
set -u
SCRATCH=$1
LOG=$SCRATCH/program.log
for _ in $(seq 1 40); do
  [ -s "$SCRATCH/state/shell.pid" ] && break
  sleep 0.25
done
PID=$(cat "$SCRATCH/state/shell.pid")
for _ in $(seq 1 80); do
  grep -q "LIVE Sound " "$LOG" 2>/dev/null && break
  sleep 0.25
done
sleep 3
python3 - "$PID" <<'PY'
import os, sys, time
root = int(sys.argv[1])
def children():
    ppid = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            stat = open(f"/proc/{name}/stat").read().rsplit(")", 1)[1].split()
            ppid[int(name)] = int(stat[1])
        except OSError:
            pass
    found, frontier = [root], [root]
    while frontier:
        nxt = [pid for pid, parent in ppid.items() if parent in frontier and pid not in found]
        found += nxt
        frontier = nxt
    return found
def cpu(pid):
    try:
        stat = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
        return (int(stat[11]) + int(stat[12])) / os.sysconf("SC_CLK_TCK")
    except OSError:
        return 0.0
def sample():
    rows = []
    for pid in children():
        try:
            comm = open(f"/proc/{pid}/comm").read().strip()
        except OSError:
            continue
        if comm == "helix-service":
            continue
        rows.append((comm, pid, cpu(pid)))
    return rows
before = sample()
time.sleep(10)
after = sample()
by_pid = {pid: seconds for _, pid, seconds in before}
delta = 0.0
for comm, pid, seconds in after:
    delta += seconds - by_pid.get(pid, seconds)
print(f"idle-cpu {delta / 10 * 100:.3f}% over 10s across {len(after)} shell processes")
PY
if [ -f "$SCRATCH/state/helix-service.pid" ]; then
  kill "$(cat "$SCRATCH/state/helix-service.pid")" 2>/dev/null || true
fi
