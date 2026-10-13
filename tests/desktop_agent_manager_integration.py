"""The Agent Surface manager role (what Settings uses): set, revoke, read grants and audit, against the real service.

The service only lets the installed Settings executable (by canonical path) register as manager, so the manager calls run in a
child process whose interpreter is a copy of python at the one path the service is told to accept (HELIX_MANAGER_EXE). A second
copy with the same file name in another directory must be refused. Run under dbus-run-session:
  desktop_agent_manager_integration.py <helix-service> <msg-risk.toml>
"""
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

service_bin, policy = sys.argv[1], sys.argv[2]
NAME, PATH, AGENT = "org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.AgentSurface1"
scratch = pathlib.Path(tempfile.mkdtemp(prefix="helix-agent-mgr-"))

CHILD = r'''
import json, os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
conn = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
def call(method, params=None, reply="()"):
    return conn.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.AgentSurface1", method, params,
                          GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 5000, None).unpack()
def fails(method, params, reply="()"):
    try:
        call(method, params, reply)
    except GLib.Error as error:
        return error.message
    return None
out = {}
client = "exe:/opt/agents/example.py"
call("RegisterManager")
call("SetGrant", GLib.Variant("(sas)", (client, ["observe", "act"])))
out["after_set"] = {g["client"]: g["tiers"] for g in call("GetGrants", None, "(av)")[0]}
out["blocked_refused"] = fails("SetGrant", GLib.Variant("(sas)", (client, ["observe", "blocked"])))
out["unknown_refused"] = fails("SetGrant", GLib.Variant("(sas)", (client, ["godmode"])))
out["still_set"] = {g["client"]: g["tiers"] for g in call("GetGrants", None, "(av)")[0]}
call("SetGrant", GLib.Variant("(sas)", (client, ["observe"])))
call("RevokeClient", GLib.Variant("(s)", (client,)))
out["after_revoke"] = {g["client"]: g["tiers"] for g in call("GetGrants", None, "(av)")[0]}
out["audit"] = [r for r in call("GetAudit", GLib.Variant("(tu)", (0, 50)), "(av)")[0] if r["tool"] == "grant-change"]
print(json.dumps(out))
'''

# Named like Settings but not at the accepted path: it must not be able to manage anything.
IMPOSTOR = r'''
import os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
conn = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
try:
    conn.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.AgentSurface1", "RegisterManager", None, None, Gio.DBusCallFlags.NONE, 5000, None)
    print("REGISTERED")
except GLib.Error as error:
    print("REFUSED " + error.message)
'''

copy = scratch / "bin" / "helix-settings"
copy.parent.mkdir()
shutil.copy2(sys.executable, copy)
copy.chmod(copy.stat().st_mode | stat.S_IXUSR)
(scratch / "child.py").write_text(CHILD)

impostor = scratch / "elsewhere" / "helix-settings"
impostor.parent.mkdir()
shutil.copy2(sys.executable, impostor)
impostor.chmod(impostor.stat().st_mode | stat.S_IXUSR)
(scratch / "impostor.py").write_text(IMPOSTOR)

env = dict(os.environ, HELIX_MANAGER_EXE=str(copy), HELIX_RISK_POLICY=policy, XDG_STATE_HOME=str(scratch / "state"), XDG_CONFIG_HOME=str(scratch / "config"),
           XDG_DATA_HOME=str(scratch / "data"), XDG_CACHE_HOME=str(scratch / "cache"))
for key in ("HELIX_STATE_HOME", "HELIX_CONFIG_HOME", "HELIX_DATA_HOME", "HELIX_CACHE_HOME"):
    env.pop(key, None)
service = subprocess.Popen([service_bin], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    probe = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
    for _ in range(100):
        if probe.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                           GLib.Variant("(s)", (NAME,)), None, Gio.DBusCallFlags.NONE, 1000, None)[0]:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("service did not start")

    # An ordinary client cannot read grants or become manager.
    def refused(method, params=None):
        try:
            probe.call_sync(NAME, PATH, AGENT, method, params, None, Gio.DBusCallFlags.NONE, 5000, None)
        except GLib.Error as error:
            return error.message
        raise AssertionError(f"{method} unexpectedly succeeded for a non-manager")
    assert "Only Helix Settings" in refused("RegisterManager")
    assert "registered manager" in refused("GetGrants")

    fake = subprocess.run([str(impostor), str(scratch / "impostor.py")], env=dict(env, PYTHONHOME=sys.base_prefix), capture_output=True, text=True, timeout=30)
    assert fake.stdout.strip().startswith("REFUSED") and "Only Helix Settings" in fake.stdout, f"an impostor with the right name registered: {fake.stdout!r} {fake.stderr[-300:]}"

    result = subprocess.run([str(copy), str(scratch / "child.py")], env=dict(env, PYTHONHOME=sys.base_prefix, DBUS_SESSION_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"]),
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr[-800:]
    out = json.loads(result.stdout.strip().splitlines()[-1])

    client = "exe:/opt/agents/example.py"
    assert out["after_set"] == {client: ["observe", "act"]} or out["after_set"] == {client: ["act", "observe"]}, out["after_set"]
    assert out["blocked_refused"] and "ungrantable" in out["blocked_refused"], out["blocked_refused"]
    assert out["unknown_refused"], out["unknown_refused"]
    assert set(out["still_set"][client]) == {"observe", "act"}, "a refused change must not alter grants"
    assert out["after_revoke"] == {}, out["after_revoke"]

    # Every change that succeeded is in the audit log, with who and what; refused changes are not.
    changes = [(r["client"], r["outcome"], sorted(r["args"])) for r in out["audit"]]
    assert changes == [(client, "set", ["act", "observe"]), (client, "set", ["observe"]), (client, "revoked", [])], changes
    state = scratch / "state" / "helix" / "agent"
    assert stat.S_IMODE((state / "audit.jsonl").stat().st_mode) == 0o600
    print("manager integration ok")
finally:
    service.terminate()
    try:
        service.wait(timeout=10)
    except subprocess.TimeoutExpired:
        service.kill()
    shutil.rmtree(scratch, ignore_errors=True)
