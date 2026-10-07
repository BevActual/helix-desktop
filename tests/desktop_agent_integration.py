"""Agent1 end to end: real helix-service, a fake shell socket, a client connection and a 'shell' connection.

Run under dbus-run-session: desktop_agent_integration.py <helix-service> <msg-risk.toml>
"""
import json
import os
import pathlib
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

service_bin, policy = sys.argv[1], sys.argv[2]
NAME, PATH = "org.helix.Desktop", "/org/helix/Desktop"
AGENT = "org.helix.Desktop.Agent1"
CFG = "org.helix.Desktop.Configuration1"

scratch = pathlib.Path(tempfile.mkdtemp(prefix="helix-agent-it-"))
runtime = pathlib.Path(os.environ["XDG_RUNTIME_DIR"])
display = f"wayland-helix-it-{os.getpid()}"
sock_path = runtime / f"noctalia-{display}.sock"

# --- fake shell: records each wire line, answers `status` and `app-launch`, errors on anything else ---
received = []
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(str(sock_path))
server.listen(8)


def serve():
    while True:
        try:
            conn, _ = server.accept()
        except OSError:
            return
        with conn:
            data = b""
            while chunk := conn.recv(4096):
                data += chunk
            line = data.decode()
            received.append(line)
            command = line.split("\x1e", 1)[-1]
            conn.sendall(b'{"barVisible":true}\n' if command == "status" else (b"ok\n" if command.startswith("app-launch") else b"error: unexpected\n"))


threading.Thread(target=serve, daemon=True).start()

env = dict(os.environ, WAYLAND_DISPLAY=display, HELIX_RISK_POLICY=policy, XDG_STATE_HOME=str(scratch / "state"),
           XDG_CONFIG_HOME=str(scratch / "config"), XDG_DATA_HOME=str(scratch / "data"), XDG_CACHE_HOME=str(scratch / "cache"))
for key in ("NOCTALIA_STATE_HOME", "NOCTALIA_CONFIG_HOME", "NOCTALIA_DATA_HOME", "NOCTALIA_CACHE_HOME"):
    env.pop(key, None)


def start_service():
    process = subprocess.Popen([service_bin], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    probe = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
    for _ in range(100):
        owner = probe.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                                GLib.Variant("(s)", (NAME,)), None, Gio.DBusCallFlags.NONE, 1000, None)
        if owner[0]:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("helix-service did not start")
    probe.close_sync(None)
    return process


class Peer:
    def __init__(self):
        self.conn = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.signals = []
        self.conn.signal_subscribe(NAME, AGENT, None, PATH, None, Gio.DBusSignalFlags.NONE, self._signal)

    def _signal(self, conn, sender, path, iface, name, params):
        self.signals.append((name, params.unpack()))

    def call(self, interface, method, params=None, reply="()"):
        result = self.conn.call_sync(NAME, PATH, interface, method, params, GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 5000, None)
        return result.unpack()

    def agent(self, method, signature=None, *values, reply="()"):
        params = GLib.Variant(signature, values) if signature else None
        return self.call(AGENT, method, params, reply)

    def error(self, method, signature, *values):
        try:
            self.agent(method, signature, *values, reply="()" if method.startswith("Register") else "(a{sv})")
        except GLib.Error as err:
            return err.message
        raise AssertionError(f"{method} unexpectedly succeeded")

    def wait(self, name, predicate=lambda payload: True, timeout=5.0):
        end = time.time() + timeout
        context = GLib.MainContext.default()
        while time.time() < end:
            for index, (signal, payload) in enumerate(self.signals):
                if signal == name and predicate(payload):
                    del self.signals[index]
                    return payload
            while context.iteration(False):
                pass
            time.sleep(0.01)
        raise AssertionError(f"timed out waiting for {name}; saw {self.signals}")

    def action(self, tool, argv=(), literal=(), reason=""):
        options = {"argv": GLib.Variant("as", list(argv)), "literal": GLib.Variant("as", list(literal)), "reason": GLib.Variant("s", reason)}
        (operation,) = self.agent("Action", "(sa{sv})", tool, options, reply="(t)")
        return operation

    def finish(self, operation):
        _, ok, text = self.wait("Completed", lambda payload: payload[0] == operation)
        return ok, json.loads(text)

    def request_access(self, *tiers):
        (operation,) = self.agent("RequestAccess", "(as)", list(tiers), reply="(t)")
        return operation


process = start_service()
try:
    client, shell = Peer(), Peer()
    client.agent("Hello", "(ss)", "integration test", "1", reply="(a{sv})")

    # 1. No grants: refused before anything reaches the shell.
    ok, result = client.finish(client.action("status"))
    assert not ok and "observe grant" in result["error"], result
    assert received == []

    # 2. No confirmer registered: access cannot be granted by anyone, including the client itself.
    ok, result = client.finish(client.request_access("observe", "act"))
    assert not ok and "no approval dialog" in result["error"], result

    # 3. Only the registered shell may become the confirmer.
    shell.call(CFG, "RegisterDesktop")
    assert "Only the registered shell" in client.error("RegisterConfirmer", None)
    shell.agent("RegisterConfirmer")
    assert "Only the registered confirmer" in client.error("Confirm", "(tba{sv})", 1, True, {})
    assert "Only Helix Settings" in client.error("RegisterManager", None)

    # 4. First-use consent. `blocked` is never offered; the user narrows the request to `observe`.
    operation = client.request_access("observe", "act", "blocked")
    request = shell.wait("ConfirmationRequested")[0]
    assert request["kind"] == "grant" and request["tiers"] == ["observe", "act"], request
    shell.agent("Confirm", "(tba{sv})", operation, True, {"tiers": GLib.Variant("as", ["observe"])})
    ok, result = client.finish(operation)
    assert ok and result["granted"] == ["observe"], result

    # 5. Granted tier works; the command reaches the shell with the protective cwd prefix.
    ok, result = client.finish(client.action("status"))
    assert ok and result["trust"] == "trusted" and json.loads(result["content"]) == {"barVisible": True}, result
    assert received == ["/\x1estatus"], received

    # 6. Blocked and ungranted commands never reach the shell.
    for tool, argv in (("session", ["reboot"]), ("firewall-panic-toggle", []), ("panel-open", ["control-center"]), ("nope", [])):
        ok, result = client.finish(client.action(tool, argv))
        assert not ok, (tool, result)
    assert received == ["/\x1estatus"], received

    # 7. Confirm tier: shell sees the request, approval runs it, rejection does not.
    operation = client.request_access("confirm")
    shell.agent("Confirm", "(tba{sv})", shell.wait("ConfirmationRequested")[0]["operation"], True, {})
    assert client.finish(operation)[0]

    operation = client.action("app-launch", literal=["Settings"], reason="open settings for you")
    request = shell.wait("ConfirmationRequested")[0]
    assert request["tool"] == "app-launch" and request["reason"] == "open settings for you" and request["tainted"] is False, request
    assert len(received) == 1  # nothing executed while waiting
    shell.agent("Confirm", "(tba{sv})", request["operation"], True, {})
    ok, _ = client.finish(operation)
    assert ok and received[-1] == "/\x1eapp-launch Settings", received

    operation = client.action("app-launch", literal=["Terminal"])
    shell.agent("Confirm", "(tba{sv})", shell.wait("ConfirmationRequested")[0]["operation"], False, {})
    ok, result = client.finish(operation)
    assert not ok and "declined" in result["error"] and len(received) == 2, (result, received)

    # 8. A control byte inside model-supplied text is refused, not smuggled to the shell.
    # The user approves, so the wire check itself must be what refuses.
    operation = client.action("app-launch", literal=["x\x1esession reboot"])
    shell.agent("Confirm", "(tba{sv})", shell.wait("ConfirmationRequested")[0]["operation"], True, {})
    ok, result = client.finish(operation)
    assert not ok and "reserved control byte" in result["error"] and len(received) == 2, (result, received)

    # 8b. If the requester cancels a pending confirmation, the shell is told so it can stop showing it.
    operation = client.action("app-launch", literal=["Files"])
    pending = shell.wait("ConfirmationRequested")[0]["operation"]
    client.agent("Cancel", "(t)", operation)
    assert shell.wait("ConfirmationEnded")[0] == pending
    ok, result = client.finish(operation)
    assert not ok and "cancelled" in result["error"], result

    # 9. If the confirmer disappears, pending confirmations are denied.
    operation = client.action("app-launch", literal=["Files"])
    shell.wait("ConfirmationRequested")
    shell.conn.close_sync(None)
    ok, result = client.finish(operation)
    assert not ok and "unavailable" in result["error"], result

    # 10. State is private and the audit trail records decisions.
    state = scratch / "state" / "noctalia" / "agent"
    for name in ("grants.json", "audit.jsonl"):
        assert stat.S_IMODE((state / name).stat().st_mode) == 0o600, name
    lines = [json.loads(line) for line in (state / "audit.jsonl").read_text().splitlines()]
    decisions = {(entry["tool"], entry["decision"]) for entry in lines}
    assert ("status", "denied") in decisions and ("status", "allowed") in decisions, decisions
    assert ("grant-request", "granted") in decisions and ("app-launch", "rejected") in decisions, decisions
    assert ("session", "denied") in decisions, decisions

    # 11. Grants survive a restart.
    client.conn.close_sync(None)
    process.terminate()
    process.wait(timeout=10)
    process = start_service()
    again = Peer()
    (session,) = again.agent("Hello", "(ss)", "integration test", "1", reply="(a{sv})")
    assert set(session["tiers"]) == {"observe", "confirm"}, session
    print("agent integration ok")
finally:
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
    server.close()
    sock_path.unlink(missing_ok=True)
    shutil.rmtree(scratch, ignore_errors=True)
