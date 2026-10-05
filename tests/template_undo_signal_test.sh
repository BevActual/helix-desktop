#!/usr/bin/env bash

# An undo hook must only reload the client it just un-themed. A reload sent to an
# unrelated process that happens to be starting (SIGUSR2 before Ghostty installs its
# handler) kills it, so a no-op undo has to stay completely silent.

set -euo pipefail

templates_dir=$1

fail() {
  printf '%s\n' "template_undo_signal_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
# ghostty/reload.sh hands the pgrep result to bash's `kill` builtin, which no PATH stub
# can intercept, so the stub must name a process this test owns. Ignore SIGUSR2 before
# forking so the child cannot receive it before installing a handler.
trap '' USR2
sleep 300 &
signal_target=$!
trap - USR2
cleanup() {
  kill "$signal_target" 2>/dev/null || true
  wait "$signal_target" 2>/dev/null || true
  rm -rf "$work_dir"
}
trap cleanup EXIT

stub_dir="$work_dir/bin"
mkdir -p "$stub_dir"
calls_file="$work_dir/calls"

# Every command an undo hook can use to reload a client records its invocation instead.
for stub in pkill mmsg gdbus emacsclient; do
  cat >"$stub_dir/$stub" <<EOF
#!/bin/sh
printf '%s %s\n' "$stub" "\$*" >>"$calls_file"
exit 0
EOF
  chmod +x "$stub_dir/$stub"
done

# The guards in front of a reload must not decide the outcome: report every client as
# running, in a mode that accepts the reload.
cat >"$stub_dir/pgrep" <<EOF
#!/bin/sh
echo "$signal_target \$*"
exit 0
EOF
chmod +x "$stub_dir/pgrep"

export PATH="$stub_dir:$PATH"

run_undo() {
  local script=$1 config_home=$2
  : >"$calls_file"
  XDG_CONFIG_HOME="$config_home" \
    XDG_DATA_HOME="$config_home/data" \
    XDG_CACHE_HOME="$config_home/cache" \
    bash "$templates_dir/$script" >/dev/null 2>&1 \
    || fail "$script exited non-zero"
}

# script:theme file relative to XDG_CONFIG_HOME
cases="
ghostty/undo.sh:ghostty/themes/noctalia
kitty/undo.sh:kitty/themes/noctalia.conf
btop/undo.sh:btop/themes/noctalia.theme
cava/undo.sh:cava/themes/noctalia
"

for entry in $cases; do
  script=${entry%%:*}
  theme_file=${entry#*:}

  config_home="$work_dir/$(dirname "$script")-clean"
  mkdir -p "$config_home"
  run_undo "$script" "$config_home"
  if [ -s "$calls_file" ]; then
    fail "$script reloaded its client with nothing to undo: $(cat "$calls_file")"
  fi

  config_home="$work_dir/$(dirname "$script")-themed"
  mkdir -p "$config_home/$(dirname "$theme_file")"
  printf 'noctalia\n' >"$config_home/$theme_file"
  run_undo "$script" "$config_home"
  if [ ! -s "$calls_file" ]; then
    fail "$script removed its theme without reloading its client"
  fi
  if [ -e "$config_home/$theme_file" ]; then
    fail "$script left its theme file behind"
  fi
done

# A no-op undo must not rewrite the user's config either, and wezterm's reload is an
# mtime bump on wezterm.lua rather than a signal.
config_home="$work_dir/wezterm-clean"
mkdir -p "$config_home/wezterm"
config_file="$config_home/wezterm/wezterm.lua"
printf 'local config = {}\nreturn config\n' >"$config_file"
touch -t 202001010000 "$config_file"
before=$(stat -c '%Y %s' "$config_file")
run_undo "wezterm/undo.sh" "$config_home"
after=$(stat -c '%Y %s' "$config_file")
if [ "$before" != "$after" ]; then
  fail "wezterm/undo.sh touched wezterm.lua with nothing to undo"
fi

config_home="$work_dir/wezterm-themed"
mkdir -p "$config_home/wezterm/colors"
config_file="$config_home/wezterm/wezterm.lua"
printf 'local config = {}\nconfig.color_scheme = "Noctalia"\nreturn config\n' >"$config_file"
printf 'colors\n' >"$config_home/wezterm/colors/Noctalia.toml"
touch -t 202001010000 "$config_file"
run_undo "wezterm/undo.sh" "$config_home"
if grep -q 'Noctalia' "$config_file"; then
  fail "wezterm/undo.sh left the Noctalia color scheme in wezterm.lua"
fi
if [ "$(stat -c '%Y' "$config_file")" = "$(date -d '2020-01-01 00:00' +%s)" ]; then
  fail "wezterm/undo.sh did not bump wezterm.lua after removing the color scheme"
fi

# Warp reloads by rewriting settings.toml; a no-op undo must not rewrite it.
config_home="$work_dir/warp-clean"
mkdir -p "$config_home/warp-terminal" "$config_home/data/warp-terminal/themes"
config_file="$config_home/warp-terminal/settings.toml"
printf '[appearance.themes]\nsystem_theme = true\n' >"$config_file"
touch -t 202001010000 "$config_file"
before=$(stat -c '%Y %s' "$config_file")
run_undo "warp/undo.sh" "$config_home"
after=$(stat -c '%Y %s' "$config_file")
if [ "$before" != "$after" ]; then
  fail "warp/undo.sh touched settings.toml with nothing to undo"
fi

config_home="$work_dir/warp-themed"
mkdir -p "$config_home/warp-terminal" "$config_home/data/warp-terminal/themes"
config_file="$config_home/warp-terminal/settings.toml"
theme_file="$config_home/data/warp-terminal/themes/noctalia.yaml"
printf '[appearance.themes]\nsystem_theme = true\ntheme = { custom = { name = "Noctalia", path = "%s" } }\n' "$theme_file" >"$config_file"
printf 'name: Noctalia\n' >"$theme_file"
run_undo "warp/undo.sh" "$config_home"
if grep -q 'noctalia.yaml' "$config_file"; then
  fail "warp/undo.sh left the Noctalia theme in settings.toml"
fi
if [ -e "$theme_file" ]; then
  fail "warp/undo.sh left its theme file behind"
fi

# Cursor Agent undo removes the YAML and, when present, the Glass theme record.
config_home="$work_dir/cursor-clean"
mkdir -p "$config_home/Cursor/User/themes"
run_undo "cursor/undo.sh" "$config_home"

config_home="$work_dir/cursor-themed"
mkdir -p "$config_home/Cursor/User/themes" "$config_home/Cursor/User/globalStorage"
theme_file="$config_home/Cursor/User/themes/noctalia.yaml"
db_file="$config_home/Cursor/User/globalStorage/state.vscdb"
printf 'name: Noctalia\n' >"$theme_file"
python3 - "$db_file" <<'PY'
import json, sqlite3, sys
con = sqlite3.connect(sys.argv[1])
con.execute("CREATE TABLE ItemTable (key TEXT PRIMARY KEY, value TEXT)")
themes = {"_v": 1, "themes": {"noctalia": {"name": "Noctalia", "source": "imported", "definition": {"name": "Noctalia"}}}}
con.execute("INSERT INTO ItemTable(key, value) VALUES(?, ?)", ("glass.theme.customThemesV1", json.dumps(themes)))
con.execute("INSERT INTO ItemTable(key, value) VALUES(?, ?)", ("glass.theme.settingsId", "cursor-custom-theme:noctalia"))
con.commit()
con.close()
PY
run_undo "cursor/undo.sh" "$config_home"
if [ -e "$theme_file" ]; then
  fail "cursor/undo.sh left its theme file behind"
fi
python3 - "$db_file" <<'PY'
import json, sqlite3, sys
con = sqlite3.connect(sys.argv[1])
themes = json.loads(con.execute("SELECT value FROM ItemTable WHERE key = ?", ("glass.theme.customThemesV1",)).fetchone()[0])
settings = con.execute("SELECT value FROM ItemTable WHERE key = ?", ("glass.theme.settingsId",)).fetchone()[0]
con.close()
if themes.get("themes"):
    raise SystemExit("cursor/undo.sh left the Noctalia Glass theme in state.vscdb")
if settings != "Cursor Dark":
    raise SystemExit("cursor/undo.sh did not restore the default Agents window theme")
PY

