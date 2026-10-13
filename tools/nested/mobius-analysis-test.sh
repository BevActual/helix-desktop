#!/bin/bash
# Real Ghidra, fake local inference: selection, evidence, local explanation, saved evidence, and cancel.
# Requires tools/scripts/install_analysis.py. Pass HELIX_TEST_ANALYSIS_CONFIG to the nested runner.
set -u
SCRATCH=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; exit 1; }
paste_id() {
  click_id "$1" || fail "could not focus $1"
  wl-copy --primary "$2"
  local pos
  pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1") || fail "no $1"
  "$PT" $pos mclick
  sleep 0.5
}
CONFIG=${HELIX_TEST_ANALYSIS_CONFIG:-}
[ -f "$CONFIG" ] || fail "set HELIX_TEST_ANALYSIS_CONFIG to the installed analysis-tools/config.json"
mkdir -p "$SCRATCH/data/helix/analysis-tools"
cp "$CONFIG" "$SCRATCH/data/helix/analysis-tools/config.json"
mkdir -p "$SCRATCH/state/helix"
cat > "$SCRATCH/state/helix/inference-routing.toml" <<'TOML'
version = 1
[[backends]]
id = "local"
kind = "local"
label = "On this computer"
[[backends]]
id = "cloud-test"
kind = "openai"
base_url = "http://127.0.0.1:9/v1"
label = "Unreachable cloud route"
[profiles.chat]
backend = "cloud-test"
model = "remote-model"
[profiles.code]
backend = "cloud-test"
model = "remote-model"
TOML
cat > "$SCRATCH/fixture.c" <<'C'
#include <stdio.h>
int helix_answer(int x) { return x + 42; }
int main(void) { puts("HELIX_ANALYSIS_FIXTURE"); return helix_answer(0); }
C
cc -g -O0 "$SCRATCH/fixture.c" -o "$SCRATCH/fixture" || fail "fixture did not compile"
for _ in $(seq 1 80); do has_id input && break; sleep 0.25; done
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
echo "UI geometry: $loc"
click_id hub-toggle || fail "no hub menu"
click_id open-analysis || fail "no analysis entry"
has_id analysis-path || fail "analysis panel did not open"
cp "$LAYOUT" "$SCRATCH/analysis-open-layout.json"
niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1
sleep 1
ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/analysis-open.png"
# WebKitGTK may retain the hardware map when wtype installs its synthetic map.
# Primary selection is private to this nested Wayland display and avoids that keyboard path.
paste_id analysis-path "$SCRATCH/fixture"
paste_id analysis-procedure "helix_answer"
click_id analysis-start || fail "no Analyze button"
for _ in $(seq 1 240); do has_id analysis-explain && break; sleep 0.5; done
has_id analysis-explain || fail "analysis never became ready"
EVIDENCE=$(find "$SCRATCH/data/helix/mobius/analysis" -name evidence.json | head -1)
python3 - "$EVIDENCE" <<'PY' || fail "missing canonical evidence"
import json, sys
job = json.load(open(sys.argv[1]))
assert job['state'] == 'ready', job
records = job['records']
assert len(records) == 4
assert any(r['tool'] == 'analyze_function' and any(expr in r['evidence']['normalized_result'].get('pseudocode', '') for expr in ('x + 42', 'x + 0x2a')) for r in records)
assert any(r['tool'] == 'list_strings' and 'HELIX_ANALYSIS_FIXTURE' in json.dumps(r['evidence']) for r in records)
print('ok: real Ghidra function and string evidence saved')
PY
niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1
sleep 1
ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/analysis-ready.png"
click_id analysis-explain || fail "could not explain locally"
for _ in $(seq 1 100); do grep -q 'MOBIUS turn ended Finished' "$SCRATCH/program.log" && break; sleep 0.25; done
grep -q 'MOBIUS turn ended Finished' "$SCRATCH/program.log" || fail "local explanation did not finish"
CHAT=$(find "$SCRATCH/data/helix/mobius/chats" -name '*.json' | head -1)
python3 - "$CHAT" <<'PY' || fail "analysis conversation lost its local route"
import json, sys
chat = json.load(open(sys.argv[1]))
assert chat['analysis_job']
assert chat['analysis_model'] == 'fake-model'
assert '[E3]' in chat['messages'][0]['text']
assert any(m['role'] == 'assistant' and m['text'] for m in chat['messages'])
print('ok: explanation used local inference despite cloud chat/code routes')
PY
click_id analysis-evidence || fail "no evidence link in analysis chat"
has_id analysis-E3 || fail "saved function evidence did not reopen"
# Re-run the same fixture and stop its fresh import.
click_id analysis-start
click_id analysis-cancel || fail "no cancellation control"
for _ in $(seq 1 60); do
  grep -rq '"state":"cancelled"\|"state": "cancelled"' "$SCRATCH/data/helix/mobius/analysis" && break
  sleep 0.25
done
grep -rq '"state":"cancelled"\|"state": "cancelled"' "$SCRATCH/data/helix/mobius/analysis" || fail "cancellation did not finish"
echo "ok: UI cancellation completed"
click_id analysis-close
echo "PASS"
