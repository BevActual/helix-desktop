#!/usr/bin/env python3
"""Helix Agent Surface MCP — session eyes/hands for Cursor and other MCP clients.

Wraps `noctalia msg` and `niri msg`. See docs/design/agentic-os.md.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent
ALLOWLIST_PATH = ROOT / "allowlist.toml"

# Prefer the built noctalia next to a common build dir, else PATH.
def _noctalia_bin() -> str:
    env = os.environ.get("HELIX_NOCTALIA")
    if env:
        return env
    repo = ROOT.parents[1]
    for candidate in (
        repo / "build-debug" / "noctalia",
        repo / "build-native-clean" / "noctalia",
        repo / "build" / "noctalia",
    ):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    found = shutil.which("noctalia")
    if found:
        return found
    return "noctalia"


def _load_allowlist() -> set[str]:
    enabled: set[str] = set()
    if not ALLOWLIST_PATH.is_file():
        return enabled
    in_tools = False
    for raw in ALLOWLIST_PATH.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            in_tools = line == "[tools]"
            continue
        if not in_tools or "=" not in line:
            continue
        key, _, value = line.partition("=")
        if value.strip().lower() in {"true", "1", "yes"}:
            enabled.add(key.strip())
    return enabled


def _run(cmd: list[str], *, timeout: float = 30.0) -> tuple[int, str, str]:
    try:
        proc = subprocess.run(
            cmd,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=os.environ.copy(),
        )
    except FileNotFoundError as exc:
        return 127, "", str(exc)
    except subprocess.TimeoutExpired:
        return 124, "", "command timed out"
    return proc.returncode, proc.stdout, proc.stderr


def noctalia_msg(*args: str, timeout: float = 30.0) -> str:
    code, out, err = _run([_noctalia_bin(), "msg", *args], timeout=timeout)
    text = (out or "") + (("\n" + err) if err and not out.endswith(err) else "")
    text = text.strip()
    if code != 0 and not text.startswith("error:"):
        return f"error: noctalia msg exited {code}" + (f": {text}" if text else "")
    return text if text else ("ok" if code == 0 else f"error: noctalia msg exited {code}")


def niri_focus_window(window_id: int) -> str:
    niri = shutil.which("niri") or "niri"
    # Prefer CLI action form when available.
    code, out, err = _run(
        [niri, "msg", "action", "focus-window", "--id", str(window_id)],
        timeout=15.0,
    )
    if code == 0:
        return (out or "ok").strip() or "ok"
    # Fallback: raw JSON Action on the socket via `niri msg action` alternatives.
    payload = json.dumps({"Action": {"FocusWindow": {"id": window_id}}})
    sock = os.environ.get("NIRI_SOCKET")
    if not sock:
        return f"error: focus-window failed and NIRI_SOCKET unset: {(err or out).strip()}"
    try:
        import socket

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(5.0)
            s.connect(sock)
            s.sendall((payload + "\n").encode())
            # Read until reply newline(s); niri replies with Ok/Err JSON.
            data = b""
            while b"\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
        reply = data.decode(errors="replace").strip()
        return reply or "ok"
    except OSError as exc:
        return f"error: {exc}"


ALLOWLIST = _load_allowlist()


def _guard(name: str) -> None:
    if name not in ALLOWLIST:
        raise RuntimeError(f"tool {name!r} is disabled by allowlist")


try:
    from mcp.server.fastmcp import FastMCP
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "helix-agent: install deps with\n"
        f"  python3 -m venv {ROOT / '.venv'}\n"
        f"  {ROOT / '.venv' / 'bin' / 'pip'} install -r {ROOT / 'requirements.txt'}\n"
    )
    raise

mcp = FastMCP("helix-agent")


@mcp.tool()
def desktop_status() -> str:
    """Return Helix Shell session status JSON (bar, panel, lock)."""
    _guard("desktop_status")
    return noctalia_msg("status")


@mcp.tool()
def desktop_capture(target: str = "") -> str:
    """Capture the desktop to a PNG for vision. target: empty=preferred output, 'all', or connector name.

    Returns 'ok <path>' or an error line. Read the PNG path with the Read tool.
    """
    _guard("desktop_capture")
    args = ["desktop-capture"]
    if target.strip():
        args.append(target.strip())
    return noctalia_msg(*args, timeout=60.0)


@mcp.tool()
def panel_toggle(panel_id: str, context: str = "") -> str:
    """Toggle a Helix panel (e.g. control-center, launcher). Optional context (home, notifications)."""
    _guard("panel_toggle")
    args = ["panel-toggle", panel_id]
    if context.strip():
        args.append(context.strip())
    return noctalia_msg(*args)


@mcp.tool()
def panel_open(panel_id: str, context: str = "") -> str:
    """Open a Helix panel. Optional context."""
    _guard("panel_open")
    args = ["panel-open", panel_id]
    if context.strip():
        args.append(context.strip())
    return noctalia_msg(*args)


@mcp.tool()
def panel_close(panel_id: str = "") -> str:
    """Close the active panel, or a specific panel id when given."""
    _guard("panel_close")
    args = ["panel-close"]
    if panel_id.strip():
        args.append(panel_id.strip())
    return noctalia_msg(*args)


@mcp.tool()
def settings_open(context: str = "") -> str:
    """Open Helix Settings (GTK), optionally at a section context."""
    _guard("settings_open")
    args = ["settings-open"]
    if context.strip():
        args.append(context.strip())
    return noctalia_msg(*args)


@mcp.tool()
def bar_visibility(action: str = "toggle") -> str:
    """Show, hide, or toggle the bar. action: show | hide | toggle."""
    _guard("bar_visibility")
    action = action.strip().lower()
    if action not in {"show", "hide", "toggle"}:
        return "error: action must be show, hide, or toggle"
    return noctalia_msg(f"bar-{action}")


@mcp.tool()
def clipboard_get() -> str:
    """Read clipboard text from Helix."""
    _guard("clipboard_get")
    return noctalia_msg("clipboard-text")


@mcp.tool()
def clipboard_set(text: str) -> str:
    """Copy text to the clipboard via Helix."""
    _guard("clipboard_set")
    return noctalia_msg("clipboard-copy", text)


@mcp.tool()
def notifications_recent() -> str:
    """Recent notification history JSON (unread count, DND, last entries)."""
    _guard("notifications_recent")
    return noctalia_msg("notification-history")


@mcp.tool()
def notify(summary: str, body: str = "") -> str:
    """Show a desktop notification (saved to history so the bar badge updates)."""
    _guard("notify")
    payload = {"summary": summary, "history": True}
    if body.strip():
        payload["body"] = body
    return noctalia_msg("notification-show", json.dumps(payload))


@mcp.tool()
def desktop_context() -> str:
    """Shell status plus the focused niri window (observe context for agents)."""
    _guard("desktop_context")
    shell_raw = noctalia_msg("status")
    try:
        shell = json.loads(shell_raw) if shell_raw.strip().startswith("{") else {"raw": shell_raw}
    except json.JSONDecodeError:
        shell = {"raw": shell_raw}

    focused = None
    niri = shutil.which("niri") or "niri"
    code, out, err = _run([niri, "msg", "-j", "windows"], timeout=15.0)
    if code == 0 and out.strip():
        try:
            windows = json.loads(out)
            if isinstance(windows, list):
                for win in windows:
                    if isinstance(win, dict) and win.get("is_focused"):
                        focused = win
                        break
        except json.JSONDecodeError:
            focused = {"error": (out or err or "invalid windows JSON").strip()}
    elif code != 0:
        focused = {"error": f"niri msg exited {code}: {(err or out or '').strip()}"}

    return json.dumps({"shell": shell, "focused": focused}, indent=2)


@mcp.tool()
def workspaces_list() -> str:
    """List niri workspaces as JSON (ids, names, active output)."""
    _guard("workspaces_list")
    niri = shutil.which("niri") or "niri"
    code, out, err = _run([niri, "msg", "-j", "workspaces"], timeout=15.0)
    text = (out or err or "").strip()
    if code != 0:
        return f"error: niri msg exited {code}" + (f": {text}" if text else "")
    return text


@mcp.tool()
def workspace_step(direction: str = "next") -> str:
    """Switch workspace. direction: next | prev."""
    _guard("workspace_step")
    direction = direction.strip().lower()
    if direction not in {"next", "prev"}:
        return "error: direction must be next or prev"
    return noctalia_msg("workspace-switch", direction)


@mcp.tool()
def app_launch(query: str) -> str:
    """Launch an installed application by desktop id or name (for example org.helix.Settings or Helix Software)."""
    _guard("app_launch")
    query = query.strip()
    if not query:
        return "error: query is required"
    return noctalia_msg("app-launch", query)


@mcp.tool()
def windows_list() -> str:
    """List niri windows as JSON."""
    _guard("windows_list")
    niri = shutil.which("niri") or "niri"
    code, out, err = _run([niri, "msg", "-j", "windows"], timeout=15.0)
    text = (out or err or "").strip()
    if code != 0:
        return f"error: niri msg exited {code}" + (f": {text}" if text else "")
    return text


@mcp.tool()
def window_focus(window_id: int) -> str:
    """Focus a niri window by id (from windows_list)."""
    _guard("window_focus")
    return niri_focus_window(window_id)


@mcp.tool()
def window_close(window_id: str = "") -> str:
    """Close the focused window, or a compositor window id from windows_list."""
    _guard("window_close")
    window_id = window_id.strip()
    if window_id:
        return noctalia_msg("window-close", window_id)
    return noctalia_msg("window-close")


@mcp.tool()
def dnd_toggle() -> str:
    """Toggle Do Not Disturb. Returns on or off."""
    _guard("dnd_toggle")
    toggled = noctalia_msg("notification-dnd-toggle")
    if toggled.startswith("error"):
        return toggled
    return noctalia_msg("notification-dnd-status")


def main() -> None:
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
