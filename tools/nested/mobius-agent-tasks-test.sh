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
sleep 4

# 1. A command asks in the sheet, runs after Allow, and the answer is a chat.
printf '[{"tool":"terminal","command":"printf bg > bg.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'Write a note file in the project.'
click_id agent-task-choose
sleep 2
click_id agent-task-start
sleep 1
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

# 5. On a git project the task works on its own copy: the project is untouched until the changes are reviewed and applied.
REPO="$SCRATCH/home/repo"
mkdir -p "$REPO" && git -C "$REPO" init -q && git -C "$REPO" config user.email t@t && git -C "$REPO" config user.name t
printf 'one\n' > "$REPO/a.txt" && git -C "$REPO" add . && git -C "$REPO" commit -qm first
printf '#!/bin/bash\nprintf "%%s/repo\\n" "$HOME"\n' > "$SCRATCH/state/tools-bin/zenity"
printf '[{"tool":"edit_file","path":"a.txt","old":"one","new":"two"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'Change one to two.'
click_id agent-task-choose
sleep 2
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$FOUR" ] && break; sleep 0.25; done
FIVE=$(newest_task)
wait_state "$FIVE" done
[ "$(cat "$REPO/a.txt")" = one ] || { echo 'FAIL: the project changed before review'; exit 1; }
click_id "agent-task-$FIVE"
wait_id agent-task-diff
wait_id "agent-task-apply-$FIVE"
click_id "agent-task-apply-$FIVE"
for _ in $(seq 1 40); do [ "$(cat "$REPO/a.txt")" = two ] && break; sleep 0.25; done
[ "$(cat "$REPO/a.txt")" = two ] || { echo 'FAIL: applying did not change the project'; exit 1; }
python3 - "$TASKS/$FIVE.json" <<'PY'
import json,sys,os
t=json.load(open(sys.argv[1]))
assert t['review']=='applied' and t['workdir']=='',t
PY
# Discarding leaves the project as it was.
printf '[{"tool":"edit_file","path":"a.txt","old":"two","new":"three"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
wtype 'Change two to three.'
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$FIVE" ] && break; sleep 0.25; done
SIX=$(newest_task)
wait_state "$SIX" done
click_id "agent-task-$SIX"
wait_id "agent-task-discard-$SIX"
click_id "agent-task-discard-$SIX"
for _ in $(seq 1 40); do [ "$(python3 -c "import json;print(json.load(open('$TASKS/$SIX.json'))['review'])")" = discarded ] && break; sleep 0.25; done
[ "$(cat "$REPO/a.txt")" = two ] || { echo 'FAIL: discarding changed the project'; exit 1; }

# 7. "Always allow" remembers the step for the folder: the next task there runs it without asking.
printf '[{"tool":"terminal","command":"touch always1.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'Make a file.'
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$SIX" ] && break; sleep 0.25; done
SEVEN=$(newest_task)
wait_state "$SEVEN" waiting
wait_id "agent-task-always-$SEVEN"
click_id "agent-task-always-$SEVEN"
wait_state "$SEVEN" done
printf '[{"tool":"terminal","command":"touch always2.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'Make another file.'
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$SEVEN" ] && break; sleep 0.25; done
EIGHT=$(newest_task)
wait_state "$EIGHT" done
python3 - "$TASKS/$EIGHT.json" <<'PY'
import json,sys
t=json.load(open(sys.argv[1]))
assert t['steps'][0]['tool']=='terminal' and t['steps'][0]['ok'] and 'touch' in t['policy']['allow_commands'][-1],t['policy']['allow_commands']
PY

# 8. Plan first: the task writes a plan and does nothing until it is approved; the person can edit it.
printf '[{"tool":"terminal","command":"touch planned.txt"},{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'Do the planned thing.'
click_id agent-task-planfirst
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$EIGHT" ] && break; sleep 0.25; done
NINE=$(newest_task)
wait_state "$NINE" waiting
wait_id "agent-task-plan-approve-$NINE"
python3 - "$TASKS/$NINE.json" <<'PY'
import json,sys
t=json.load(open(sys.argv[1]))
assert t['plan'] and t['steps']==[] and t['state']['why'].startswith('Review the plan'),t
PY
click_id agent-task-plan-text
wtype ' Edited by me.'
click_id "agent-task-plan-approve-$NINE"
wait_state "$NINE" done
python3 - "$TASKS/$NINE.json" <<'PY'
import json,sys
t=json.load(open(sys.argv[1]))
assert t['plan'].endswith('Edited by me.'),t['plan']
PY

# 9. "Run as task" on a chat message opens a new task with that message as the goal, in the chat's folder.
click_id agents-back
click_id new-chat
sleep 1
paste_id input 'Tidy the project.'
click_id send
for _ in $(seq 1 60); do ! has_id stop && has_id msg-0 && break; sleep 0.25; done
printf '[{"tool":"finish"}]' > "$SCRATCH/state/tools-steps.json"
click_id task-0
wait_id agent-task-goal
sleep 1.5
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$NINE" ] && break; sleep 0.25; done
TEN=$(newest_task)
python3 - "$TASKS/$TEN.json" <<'PY'
import json,sys
t=json.load(open(sys.argv[1]))
assert t['goal']=='Tidy the project.',t['goal']
PY

# 10. Save as skill from a finished task; a later task reads it; the skills page deletes it.
click_id agents-back
click_id mode-tasks
sleep 3
click_id "agent-task-$ONE"
wait_id "agent-task-skill-$ONE"
click_id "agent-task-skill-$ONE"
wait_id skill-save
click_id skill-save
SKILLS="$SCRATCH/data/helix/mobius/skills"
for _ in $(seq 1 40); do ls "$SKILLS"/*.json >/dev/null 2>&1 && break; sleep 0.25; done
SKILL=$(basename "$(ls "$SKILLS"/*.json | head -1)" .json)
python3 - "$SKILLS/$SKILL.json" <<'PY'
import json,sys
k=json.load(open(sys.argv[1]))
assert k['description'] and 'terminal' in k['body'],k
PY
printf '[{"tool":"skill_read","name":"%s"},{"tool":"finish"}]' "$SKILL" > "$SCRATCH/state/tools-steps.json"
click_id agent-task-new
wait_id agent-task-goal
sleep 1.5
sleep 1; wtype 'Use the skill.'
click_id agent-task-start
for _ in $(seq 1 60); do [ "$(newest_task)" != "$TEN" ] && break; sleep 0.25; done
ELEVEN=$(newest_task)
wait_state "$ELEVEN" done
python3 - "$TASKS/$ELEVEN.json" <<'PY'
import json,sys
t=json.load(open(sys.argv[1]))
assert t['steps'][0]['tool']=='skill_read' and t['steps'][0]['ok'] and 'terminal' in t['steps'][0]['outcome'],t['steps']
PY
click_id agent-skills
wait_id "skill-delete-$SKILL"
click_id "skill-delete-$SKILL"
for _ in $(seq 1 40); do [ ! -e "$SKILLS/$SKILL.json" ] && break; sleep 0.25; done
[ ! -e "$SKILLS/$SKILL.json" ] || { echo 'FAIL: the skill was not deleted'; exit 1; }
echo "PASS: background tasks ask in the sheet, run when allowed, run checks without asking under the folder policy, practise, stop, leave a chat with the steps, review a git project's changes on a copy (apply and discard), remember Always allow per folder, wait for a plan to be approved or edited, start a task from a chat message, and save, use and delete a skill"
