"""Every `helix-panel` command needs a risk tier; tiers must be known; overrides must name real choices."""
import json
import pathlib
import subprocess
import sys
import tomllib

root, helix = pathlib.Path(sys.argv[1]), sys.argv[2]
policy = tomllib.loads((root / "protocols/ipc/msg-risk.toml").read_text())
schema = json.loads(subprocess.check_output([helix, "schema-json"], text=True))
commands = {c["name"]: c for c in schema["commands"]}

TIERS = {"observe", "observe-private", "act", "confirm", "blocked"}
tiers = policy["tiers"]
assert set(tiers.values()) <= TIERS, f"unknown tier: {set(tiers.values()) - TIERS}"

missing = sorted(set(commands) - set(tiers))
assert not missing, f"commands without a risk tier (add them to protocols/ipc/msg-risk.toml): {missing}"
stale = sorted(set(tiers) - set(commands))
assert not stale, f"risk entries for commands that no longer exist: {stale}"

for key, tier in policy.get("choices", {}).items():
    assert tier in TIERS, f"{key}: unknown tier {tier}"
    name, choice = key.split(" ", 1)
    assert name in commands, f"{key}: unknown command"
    positionals = commands[name]["positionals"]
    assert positionals and choice in positionals[0]["choices"], f"{key}: {choice!r} is not a choice of {name}"

# A command that can power off, reboot or end the session must never be agent-reachable by default.
assert tiers["session"] == "blocked"

# Tool hints describe real commands, say something, and are not mistaken for policy.
hints = tomllib.loads((root / "protocols/ipc/tool-hints.toml").read_text())["hints"]
unknown = sorted(set(hints) - set(commands))
assert not unknown, f"tool hints for commands that do not exist: {unknown}"
assert all(isinstance(v, str) and 8 <= len(v) <= 140 for v in hints.values()), "hints must be 8-140 character strings"
