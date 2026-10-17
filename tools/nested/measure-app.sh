#!/bin/bash
# Memory of one of our applications in a nested run: what the application's processes (its own and its WebKit helpers) hold, as PSS, which
# splits shared pages fairly, once its page has settled. Used to fill the budget tables. First paint is not measured: the nested compositor's
# start-up dominates it (14 s for every app), so it says nothing about the application.
#   MEASURE_READY=<control id> MEASURE_BIN=<process name> HELIX_NESTED_SCRIPT=tools/nested/measure-app.sh \
#     tools/nested/run.sh measure-software tools/nested/software-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LAYOUT=$SCRATCH/state/layout.json
source "$ROOT/tools/nested/ui-lib.sh"
READY=${MEASURE_READY:?set MEASURE_READY to a control the page names}; BIN=${MEASURE_BIN:?set MEASURE_BIN to the process name}
for _ in $(seq 1 160); do has_id "$READY" && break; sleep 0.25; done
has_id "$READY" || { echo "FAIL: $READY never appeared"; exit 1; }
rss() { # total KiB (PSS) of the processes started with this run's state directory
  local total=0 p
  for p in $(pgrep -f "WebKit|$BIN"); do
    tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q "^XDG_STATE_HOME=$SCRATCH/state$" || continue
    total=$((total + $(awk '/^Pss:/{s+=$2} END{print s+0}' /proc/$p/smaps_rollup 2>/dev/null)))
  done
  echo $total
}
sleep 5
echo "settled: $(rss) KiB PSS"
sleep 10
echo "15 s later: $(rss) KiB PSS"
