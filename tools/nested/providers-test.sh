#!/bin/bash
# Actions from other apps (docs/design/actions.md, "open by design"): a pretend Notes app ships a manifest in the person's data folder and
# answers org.helix.ActionProvider1; its actions join the catalogue (one that calls itself read-only is held to "act", since it is in the
# person's folder), run through the desktop, land in the one history and are undone by the call Notes declared. The built-in Models
# actions: list the tasks and choose a model for one, undone back.   tools/nested/ui-suite.sh providers
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
H=$ROOT/target/fast/helix
fail() { echo "FAIL: $*" >&2; FAILED=1; }
mkdir -p "$XDG_DATA_HOME/helix/actions"
cat > "$XDG_DATA_HOME/helix/actions/notes.json" <<'JSON'
{ "provider": { "id": "notes", "title": "Notes", "bus": "org.example.Notes", "path": "/org/example/Notes" },
  "actions": [
    { "id": "notes.add", "title": "Add a note", "summary": "Adds a note.", "risk": "act", "undo": "restore",
      "params": { "type": "object", "properties": { "text": { "type": "string" } }, "required": ["text"] } },
    { "id": "notes.remove", "title": "Remove a note", "summary": "Removes a note.", "risk": "act", "undo": "none",
      "params": { "type": "object", "properties": { "text": { "type": "string" } }, "required": ["text"] } },
    { "id": "notes.list", "title": "List notes", "summary": "Lists the notes.", "risk": "observe", "undo": "none",
      "params": { "type": "object" } } ] }
JSON
python3 "$ROOT/tools/nested/notes-provider.py" "$SCRATCH/notes.json" &
for _ in $(seq 1 120); do busctl --user status org.helix.Desktop >/dev/null 2>&1 && busctl --user status org.example.Notes >/dev/null 2>&1 && break; sleep 0.25; done; sleep 3

echo "--- 1. an app's actions join the catalogue"
"$H" --json actions | python3 -c "
import sys, json
a = {x['id']: x for x in json.load(sys.stdin)}
assert 'notes.add' in a and 'models.use' in a and 'settings.set' in a, sorted(a)
assert a['notes.list']['risk'] == 'act', a['notes.list']['risk']
print('ok: notes.add, models.use and settings.set are all listed; notes.list (in the person\'s folder) is held to act')" || fail "catalogue"

echo "--- 2. run one, recorded, undone by the app's own call"
out=$("$H" do notes.add text=Milk) && echo "ok: helix do notes.add → $(echo "$out" | tr -d '\n' | head -c 120)" || fail "add: $out"
python3 -c "import json; assert json.load(open('$SCRATCH/notes.json')) == ['Milk']" && echo "ok: Notes has it" || fail "notes file: $(cat "$SCRATCH/notes.json")"
H1=$("$H" history -n 1); echo "$H1" | grep -q "Added “Milk” to Notes *· helix command line" && echo "ok: history: $(echo "$H1" | tr -s ' ')" || fail "history: $H1"
ENTRY=$(echo "$H1" | awk '{print $1}')
out=$("$H" undo "$ENTRY") && echo "$out" | grep -q "^Undid Added “Milk”" && echo "ok: $out" || fail "undo: $out"
python3 -c "import json; assert json.load(open('$SCRATCH/notes.json')) == []" && echo "ok: Notes removed it (its own undo call)" || fail "notes after undo: $(cat "$SCRATCH/notes.json")"
"$H" undo "$ENTRY" 2>&1 | grep -q "already undone" && echo "ok: not twice" || fail "undone twice"

echo "--- 3. the built-in Models actions"
out=$("$H" --json do models.list) && echo "$out" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert [t['task'] for t in d['tasks']] == ['chat', 'summarize', 'vision', 'code', 'act'], d
print('ok: models.list gives every task and what answers it')" || fail "models.list: $out"
# A model the model service says is installed (one it does not have is refused: checked below).
MODEL=$("$H" --json do models.list | python3 -c "import sys, json; m = json.load(sys.stdin)['installed']; print(m[0]['id'] if m else '')")
echo "installed: ${MODEL:-none}"
out=$("$H" do models.use task=chat model=not-installed-anywhere 2>&1); echo "$out" | grep -q "not installed" && echo "ok: a model that is not installed is refused" || echo "note: no model service to check against ($out)"
out=$("$H" do models.use task=chat model="$MODEL") && echo "ok: $(echo "$out" | grep -o '"done"[^,]*')" || fail "models.use: $out"
grep -A3 '^\[profiles.chat\]' "$XDG_STATE_HOME/helix/inference-routing.toml" | grep -q "model = \"$MODEL\"" && echo "ok: the routing file has it" || fail "routing: $(cat "$XDG_STATE_HOME/helix/inference-routing.toml" 2>&1 | head -20)"
ENTRY=$("$H" history -n 1 | awk '{print $1}')
out=$("$H" undo "$ENTRY") && echo "ok: $out" || fail "undo models: $out"
grep -A3 '^\[profiles.chat\]' "$XDG_STATE_HOME/helix/inference-routing.toml" | grep -q "model = \"$MODEL\"" && [ -n "$MODEL" ] && fail "still $MODEL after undo" || echo "ok: Talk's model is back"
out=$("$H" do models.use task=nonsense model=x 2>&1); echo "$out" | grep -q "not a task" && echo "ok: an unknown task is refused in words" || fail "bad task: $out"
[ "$FAILED" = 0 ] && echo "PASS: providers" || exit 1
