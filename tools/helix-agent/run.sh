#!/usr/bin/env bash
# Launch the Helix Agent Surface MCP server (stdio).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
VENV="$ROOT/.venv"
if [[ ! -x "$VENV/bin/python" ]]; then
  # System site packages provide PyGObject, which the adapter uses to reach the desktop service (Agent1).
  python3 -m venv --system-site-packages "$VENV"
fi
if grep -q '^include-system-site-packages = false' "$VENV/pyvenv.cfg" 2>/dev/null; then
  sed -i 's/^include-system-site-packages = false/include-system-site-packages = true/' "$VENV/pyvenv.cfg"
fi
# Keep mcp on the v1 FastMCP API (requirements pins mcp<2).
if ! "$VENV/bin/python" -c "from mcp.server.fastmcp import FastMCP" 2>/dev/null; then
  "$VENV/bin/pip" install -q -r "$ROOT/requirements.txt"
fi
exec "$VENV/bin/python" "$ROOT/server.py" "$@"
