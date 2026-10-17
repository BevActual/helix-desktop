#!/usr/bin/env bash
# Measures the model behind the assistant on realistic desktop requests (tests/eval/*.json) through the real stack, on a private session bus.
# Needs a model in ~/.local/share/helix/models and a GPU or patience. Nothing is run on your desktop. Arguments go to helix-assistant-eval:
#   tools/run-assistant-eval.sh                                        both case sets, the model routing prefers (else the largest)
#   tools/run-assistant-eval.sh --model qwen2.5-1.5b-instruct-q4_k_m.gguf --json out.json
#   tools/run-assistant-eval.sh tests/eval/desktop_requests_heldout.json
# Prints the same table for each case file given (default: both). See docs/design/agent-and-inference.md "Measured".
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
cargo build --profile fast --quiet -p helix-assistant -p helix-inference
BIN=${CARGO_TARGET_DIR:-$ROOT/target}/fast
# Options and their values go through unchanged; a .json value after --json is an output path, not a case file.
files=(); opts=(); prev=""
for a in "$@"; do
  if [[ "$prev" == "--json" ]]; then opts+=("$a"); prev=""; continue; fi
  case "$a" in
    --json) opts+=("$a"); prev="--json" ;;
    --model|--limit|--category|--min-exact) opts+=("$a"); prev="value" ;;
    --raw) opts+=("$a") ;;
    *) if [[ "$prev" == "value" ]]; then opts+=("$a"); prev=""; else files+=("$a"); fi ;;
  esac
done
[[ ${#files[@]} -eq 0 ]] && files=(tests/eval/desktop_requests.json tests/eval/desktop_requests_heldout.json)
export EVAL_BIN=$BIN EVAL_ROOT=$ROOT
export EVAL_FILES=$(printf '%s\n' "${files[@]}") EVAL_OPTS=$(printf '%s\n' "${opts[@]:-}")
exec dbus-run-session -- env XDG_DATA_DIRS=/var/empty bash -c '
  "$EVAL_BIN/helix-inference" >/dev/null 2>&1 &
  server=$!; trap "kill $server 2>/dev/null" EXIT
  for _ in $(seq 1 60); do
    gdbus call --session --dest org.helix.Inference --object-path /org/helix/Inference --method org.helix.Inference1.ListModels >/dev/null 2>&1 && break
    sleep 0.5
  done
  export HELIX_RISK_POLICY="$EVAL_ROOT/protocols/ipc/msg-risk.toml"
  mapfile -t opts <<<"$EVAL_OPTS"; [[ "${opts[0]:-}" == "" ]] && opts=()
  status=0
  while IFS= read -r file; do
    echo "=== $file"
    "$EVAL_BIN/helix-assistant-eval" "$file" "${opts[@]}" || status=$?
  done <<<"$EVAL_FILES"
  exit $status
'
