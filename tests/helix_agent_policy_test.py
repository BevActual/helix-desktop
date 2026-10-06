"""The agent MCP adapter must enforce msg-risk.toml at its single `noctalia msg` choke point."""
import importlib.util
import os
import pathlib
import stat
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
with tempfile.TemporaryDirectory() as tmp:
    fake = pathlib.Path(tmp) / "noctalia"
    fake.write_text('#!/bin/sh\nprintf "argv:"; for a in "$@"; do printf " [%s]" "$a"; done; printf "\\n"\n')
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    os.environ["HELIX_NOCTALIA"] = str(fake)

    spec = importlib.util.spec_from_file_location("helix_agent_server", root / "tools/helix-agent/server.py")
    server = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(server)

    msg = server.noctalia_msg

    # Blocked commands are unreachable even though the fake binary would happily run them.
    for blocked in (("session", "reboot"), ("session", "shutdown"), ("firewall-panic-toggle",)):
        out = msg(*blocked)
        assert out.startswith("error:") and "argv:" not in out, f"{blocked} reached the binary: {out}"
    assert msg("session", "lock").startswith("argv:"), "session lock is explicitly granted at act tier"
    assert msg("session", "logout").startswith("error:")

    # Unknown commands fail closed.
    assert msg("not-a-real-command").startswith("error:")

    # "blocked" can never be granted, even by configuration.
    assert "blocked" not in server.GRANTED_TIERS
    server.GRANTED_TIERS.add("blocked")
    assert msg("session", "reboot").startswith("error:")
    server.GRANTED_TIERS.discard("blocked")

    # Removing a tier from the grants makes its commands refuse.
    assert msg("app-launch", literal=("Settings",)).startswith("argv:")
    saved = set(server.GRANTED_TIERS)
    server.GRANTED_TIERS.discard("confirm")
    assert msg("app-launch", literal=("Settings",)).startswith("error:")
    assert msg("status").startswith("argv:"), "observe tier still allowed"
    server.GRANTED_TIERS.clear()
    server.GRANTED_TIERS.update(saved)

    # Model-supplied text is passed after `--` so it cannot be read as a flag.
    out = msg("clipboard-copy", literal=("--help",))
    assert out == "argv: [msg] [clipboard-copy] [--] [--help]", out

    # Every tool the adapter exposes maps to commands that have a tier (no silent fail-closed surprises).
    tiers = server.RISK_POLICY["tiers"]
    for command in ("status", "desktop-capture", "panel-open", "panel-toggle", "panel-close", "settings-open", "bar-show",
                    "clipboard-text", "clipboard-copy", "notification-history", "notification-show", "workspace-switch",
                    "app-launch", "window-close", "notification-dnd-toggle", "notification-dnd-status"):
        assert command in tiers, f"{command} has no tier"
