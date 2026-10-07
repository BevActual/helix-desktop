#!/bin/bash
# HELIX_NESTED_SCRIPT: open the panel, destroy it, and watch WebKit PIDs for reclaim.
set -u
SCRATCH=$1
LOG=$SCRATCH/program.log
CONTROL=$SCRATCH/state/surface-control
PID=$(cat "$SCRATCH/state/shell.pid")
HERE=$(cd "$(dirname "$0")" && pwd)
wait_count() {
  local needle=$1 need=$2
  for _ in $(seq 1 80); do
    local n
    n=$(grep -c "$needle" "$LOG" 2>/dev/null || true)
    [ "$n" -ge "$need" ] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $need x $needle" >&2
  return 1
}
webkit_pids() {
  python3 - <<PY
import os
root=$PID
ppid={}
for p in os.listdir("/proc"):
    if p.isdigit():
        try:
            ppid[int(p)]=int(open(f"/proc/{p}/stat").read().rsplit(")",1)[1].split()[1])
        except Exception:
            pass
found, frontier=[root],[root]
while frontier:
    nxt=[p for p,pp in ppid.items() if pp in frontier and p not in found]
    found+=nxt; frontier=nxt
rows=[]
for p in found:
    try:
        name=open(f"/proc/{p}/comm").read().strip()
    except Exception:
        continue
    if "WebKit" not in name and name != "helix-shell-tau":
        continue
    pss=0.0
    try:
        for l in open(f"/proc/{p}/smaps_rollup"):
            if l.startswith("Pss:"):
                pss=int(l.split()[1])/1024
                break
    except Exception:
        pass
    rows.append((name,p,pss))
for n,p,m in sorted(rows, key=lambda r: (-r[2], r[1])):
    print(f"    {n:18s} {p:<8d} {m:6.0f} MB")
print(f"  webkit-web-count {sum(1 for n,_,_ in rows if 'WebKitWeb' in n)}")
PY
}
sample() {
  local label=$1
  echo "--- $label ---"
  python3 "$HERE/process_tree.py" "$PID" "$label"
  webkit_pids
}
cleanup() {
  if [ -f "$SCRATCH/state/helix-service.pid" ]; then
    kill "$(cat "$SCRATCH/state/helix-service.pid")" 2>/dev/null || true
  fi
}
trap cleanup EXIT
wait_count "LIVE Sound " 1
wait_count "READY bar" 1
sample "bar-only"
echo open >> "$CONTROL"
wait_count "PANEL open" 1
wait_count "READY panel" 1
sleep 1
sample "panel-open"
PANEL_WEBKIT_BEFORE=$(webkit_pids | awk '/WebKitWeb/{print $2}' | sort | tr '\n' ' ')
echo escape >> "$CONTROL"
wait_count "PANEL destroyed" 1
last=0
for t in 1 3 8 15 30; do
  sleep $((t - last))
  last=$t
  sample "after-destroy-t${t}s"
done
echo open >> "$CONTROL"
wait_count "PANEL open" 2
wait_count "READY panel" 2
sleep 1
sample "panel-reopen"
echo close >> "$CONTROL"
wait_count "PANEL destroyed" 2
sleep 8
sample "after-second-destroy-t8s"
echo "--- WEBVIEW lines ---"
grep "WEBVIEW " "$LOG" || true
echo "--- PANEL lines ---"
grep "PANEL " "$LOG" || true
echo "--- webkit pids at first panel open ---"
echo "$PANEL_WEBKIT_BEFORE"
