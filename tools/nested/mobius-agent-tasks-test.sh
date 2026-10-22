#!/bin/bash
# Background agent tasks: started from the Tasks sheet, they run by themselves. A command waits for Allow in the sheet; "Work in the folder" runs a check
# without asking; a practice run goes ahead and lists what it would have asked. The answer is a new chat with the steps in it.
#   tools/nested/ui-suite.sh mobius-agent-tasks
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/tools/nested/ui-lib.sh"
refresh_geometry() {
  local loc; loc=$(python3 - <<'PY'
import json,subprocess
w=next(x for x in json.loads(subprocess.check_output(['niri','msg','--json','windows'])) if 'mobius' in (x['app_id'] or '').lower())
o=next(iter(json.loads(subprocess.check_output(['niri','msg','--json','outputs'])).values()))['logical']
l=w['layout'];t=l.get('tile_pos_in_workspace_view') or [0,0];f=l.get('window_offset_in_tile') or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
  export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
}
click_id() { refresh_geometry; local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.5}"); "$PT" $pos click; sleep 0.8; }
wait_id() { for _ in $(seq 1 ${2:-60}); do has_id "$1" && return; sleep 0.25; done; echo "FAIL: missing $1"; exit 1; }
paste_id() { click_id "$1"; wl-copy --primary "$2"; refresh_geometry; local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1"); "$PT" $pos mclick; sleep 0.3; }
TASKS="$SCRATCH/data/helix/mobius/tasks"
PROJECT="$SCRATCH/home/project"
newest_task() { ls -t "$TASKS"/*.json 2>/dev/null | head -1 | xargs -r basename -s .json; }
state_of() { python3 -c "import json,sys;print(json.load(open('$TASKS/$1.json'))['state']['state'])"; }
wait_state() { for _ in $(seq 1 120); do [ "$(state_of "$1" 2>/dev/null)" = "$2" ] && return; sleep 0.25; done; echo "FAIL: task $1 is $(state_of "$1"), wanted $2"; exit 1; }
wait_id mode
click_id mode-tasks
wait_id agent-tasks-panel

# 1. A command asks in the sheet, runs after Allow, and the answer is a chat.
printf '[{"tool":"terminal","command":"printf bg > bg.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'Write a note file in the project.'
click_id agent-task-choose
sleep 2
click_id agent-task-start
for _ in $(seq 1 60); do [ -n "$(newest_task)" ] && break; sleep 0.25; done
ONE=$(newest_task)
wait_state "$ONE" waiting
[ ! -e "$PROJECT/bg.txt" ] || { echo 'FAIL: the command ran before it was allowed'; exit 1; }
wait_id "agent-task-approve-$ONE"
click_id "agent-task-approve-$ONE"
wait_state "$ONE" done
[ "$(cat "$PROJECT/bg.txt")" = bg ] || { echo 'FAIL: the allowed command did not run'; exit 1; }
python3 - "$TASKS/$ONE.json" "$SCRATCH/data/helix/mobius/chats" <<'PY'
import json,pathlib,sys
t=json.load(open(sys.argv[1]))
assert [s['tool'] for s in t['steps']]==['terminal'] and t['steps'][0]['ok'] and t['steps'][0]['place']=='local',t['steps']
chat=json.load(open(pathlib.Path(sys.argv[2])/(t['result_chat']+'.json')))
assert chat['messages'][0]['text'].startswith('Write a note') and chat['messages'][-1]['activities'][0]['tool']=='terminal'
PY
wait_id "agent-task-open-$ONE"

# 2. "Work in the folder": a plain check runs without asking.
printf '[{"tool":"terminal","command":"ls"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'List the project.'
click_id agent-task-policy-folder
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$ONE" ] && break; sleep 0.25; done
TWO=$(newest_task)
wait_state "$TWO" done

# 3. A practice run goes ahead and records what it would have asked.
printf '[{"tool":"terminal","command":"printf p > practice.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'Practise writing a file.'
click_id agent-task-more
click_id agent-task-practice
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$TWO" ] && break; sleep 0.25; done
THREE=$(newest_task)
wait_state "$THREE" done
[ "$(cat "$PROJECT/practice.txt")" = p ] || { echo 'FAIL: the practice run did not go ahead'; exit 1; }
python3 - "$TASKS/$THREE.json" <<'PY'
import json,sys
t=json.load(open(sys.argv[1]))
assert t['policy']['mode']=='practice' and t['steps'][0]['would_ask'],t
PY
wait_id "agent-task-again-$THREE"

# 4. A task that is stopped while it waits ends stopped.
printf '[{"tool":"terminal","command":"printf no > stopped.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'This one will be stopped.'
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$THREE" ] && break; sleep 0.25; done
FOUR=$(newest_task)
wait_state "$FOUR" waiting
wait_id "agent-task-stop-$FOUR"
sleep 1
click_id "agent-task-stop-$FOUR"
wait_state "$FOUR" stopped
[ ! -e "$PROJECT/stopped.txt" ] || { echo 'FAIL: a stopped task ran its command'; exit 1; }
echo 'PASS: background tasks ask in the sheet, run when allowed, run checks without asking under the folder policy, practise, stop, and leave a chat with the steps'
