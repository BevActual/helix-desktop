"""helix-assistant end to end: real helix-service, real helix-inference (scripted replies), a fake shell, a fake confirmer.

The model's reply is scripted, so this tests what the assistant does with a reply, not how good a model is: what it shows,
what it asks, what reaches the shell, and that Agent1's decisions still apply on top.

Run under dbus-run-session: assistant_integration.py <helix-assistant> <helix-service> <helix-inference> <repo-root>
"""
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

assistant_bin, service_bin, inference_bin, repo = sys.argv[1:5]
NAME, PATH, AGENT, CFG = "org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Agent1", "org.helix.Desktop.Configuration1"

scratch = pathlib.Path(tempfile.mkdtemp(prefix="helix-assistant-it-"))
runtime = pathlib.Path(os.environ["XDG_RUNTIME_DIR"])
display = f"wayland-helix-asst-{os.getpid()}"
sock_path = runtime / f"noctalia-{display}.sock"

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
            received.append(line.split("\x1e", 1)[-1])
            command = line.split("\x1e", 1)[-1]
            conn.sendall(b'{"barVisible":true}\n' if command == "status" else b"ok\n")


threading.Thread(target=serve, daemon=True).start()

env = dict(os.environ, WAYLAND_DISPLAY=display, HELIX_RISK_POLICY=f"{repo}/protocols/ipc/msg-risk.toml",
           HELIX_TOOL_HINTS=f"{repo}/protocols/ipc/tool-hints.toml", XDG_STATE_HOME=str(scratch / "state"),
           XDG_CONFIG_HOME=str(scratch / "config"), XDG_DATA_HOME=str(scratch / "data"), XDG_CACHE_HOME=str(scratch / "cache"),
           HELIX_ASSISTANT_ANSWER_FROM_STDIN="1")
for key in ("NOCTALIA_STATE_HOME", "NOCTALIA_CONFIG_HOME", "NOCTALIA_DATA_HOME", "NOCTALIA_CACHE_HOME"):
    env.pop(key, None)


def bus():
    return Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)


def wait_for_name(name):
    probe = bus()
    for _ in range(100):
        if probe.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                           GLib.Variant("(s)", (name,)), None, Gio.DBusCallFlags.NONE, 1000, None)[0]:
            probe.close_sync(None)
            return
        time.sleep(0.1)
    raise AssertionError(f"{name} did not start")


service = subprocess.Popen([service_bin], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
inference = None
processes = [service]


class Confirmer:
    """Plays the shell: registers as desktop and confirmer, records what it is asked, answers as scripted."""

    def __init__(self):
        self.conn = bus()
        self.requests, self.answer = [], True
        self.conn.signal_subscribe(NAME, AGENT, "ConfirmationRequested", PATH, None, Gio.DBusSignalFlags.NONE, self._asked)
        self.conn.call_sync(NAME, PATH, CFG, "RegisterDesktop", None, None, Gio.DBusCallFlags.NONE, 5000, None)
        self.conn.call_sync(NAME, PATH, AGENT, "RegisterConfirmer", None, None, Gio.DBusCallFlags.NONE, 5000, None)

    def _asked(self, conn, sender, path, iface, name, params):
        request = params.unpack()[0]
        self.requests.append(request)
        options = {}
        if request["kind"] == "grant":
            options = {"tiers": GLib.Variant("as", request["tiers"])}
        self.conn.call(NAME, PATH, AGENT, "Confirm", GLib.Variant("(tba{sv})", (request["operation"], self.answer, options)),
                       None, Gio.DBusCallFlags.NONE, 5000, None, None)


def run_assistant(request, answer="", *flags, timeout=60):
    process = subprocess.Popen([assistant_bin, *flags, request], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    process.stdin.write(answer + "\n")
    process.stdin.close()
    end = time.time() + timeout
    context = GLib.MainContext.default()
    while process.poll() is None and time.time() < end:
        while context.iteration(False):
            pass
        time.sleep(0.01)
    assert process.poll() is not None, "assistant did not finish"
    return process.returncode, process.stdout.read(), process.stderr.read()


def start_inference(reply):
    global inference
    if inference:
        inference.terminate()
        inference.wait(timeout=10)
        processes.remove(inference)
    # D-Bus activation would find the installed unit; the test owns the name instead.
    inference = subprocess.Popen([inference_bin], env=dict(env, HELIX_INFERENCE_BACKEND="fake", HELIX_FAKE_REPLY=json.dumps(reply)),
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    processes.append(inference)
    wait_for_name("org.helix.Inference")


try:
    wait_for_name(NAME)
    shell = Confirmer()

    # 1. First use: the person is asked once for permission, then a read-only request just runs.
    start_inference({"tool": "status", "args": []})
    code, out, err = run_assistant("Is the bar visible?")
    grants = [r for r in shell.requests if r["kind"] == "grant"]
    assert code == 0 and len(grants) == 1 and set(grants[0]["tiers"]) == {"observe", "act"}, (code, out, err, shell.requests)
    assert "Checking:" in out and "(`status`)" in out and "barVisible" in out, (out, err)
    assert received == ["status"], received

    # 2. A change is shown first. Answering no does nothing at all.
    start_inference({"tool": "osd-disable", "args": []})
    code, out, err = run_assistant("Turn the popups off.", "n")
    assert code == 0 and "About to: Turn off the on-screen popups for volume and brightness (`osd-disable`)" in out and "Cancelled" in out, (out, err)
    assert received == ["status"], received

    # 2b. Not answering is not consent.
    code, out, err = run_assistant("Turn the popups off.", "")
    assert "Cancelled" in out and received == ["status"], (out, received)

    # 3. Answering yes sends it through Agent1, which reaches the shell.
    code, out, err = run_assistant("Turn the popups off.", "y")
    assert code == 0 and received == ["status", "osd-disable"], (out, err, received)
    assert len([r for r in shell.requests if r["kind"] == "grant"]) == 1, "asked for permission again"

    # 4. A dry run shows the plan and does nothing.
    code, out, err = run_assistant("Turn the popups off.", "y", "--dry-run")
    assert code == 0 and "dry run" in out and received == ["status", "osd-disable"], (out, received)

    # 5. A decline is shown and nothing runs, even if the model attaches arguments to it.
    start_inference({"tool": "decline", "args": ["session", "reboot"], "reason": "I have no way to restart the computer."})
    code, out, err = run_assistant("Restart my laptop.", "y")
    assert code == 3 and "I can't do that: I have no way to restart the computer." in out and len(received) == 2, (out, received)

    # 6. If a model ever names something it may not use (a blocked tool, an invented one), it is refused and never sent.
    for reply in ({"tool": "session", "args": ["reboot"]}, {"tool": "firewall-panic-toggle", "args": []}, {"tool": "nope", "args": []}):
        start_inference(reply)
        code, out, err = run_assistant("Do it.", "y")
        assert code == 3 and "not something this assistant is allowed" in out and len(received) == 2, (reply, out, received)

    # 7. Garbage from the model is explained, not executed.
    start_inference("this is not a tool call")
    code, out, err = run_assistant("Do something.", "y")
    assert code == 3 and "couldn't work out" in out and len(received) == 2, (out, err)

    # 7b. Warming loads the model and runs nothing.
    start_inference({"tool": "osd-disable", "args": []})
    before = len(received)
    code, out, err = run_assistant("", "", "--warm")
    assert code == 0 and "is ready" in out and len(received) == before, (code, out, err)

    # 8. The audit trail shows the assistant as its own client, with what ran.
    lines = [json.loads(line) for line in (scratch / "state" / "noctalia" / "agent" / "audit.jsonl").read_text().splitlines()]
    assert ("osd-disable", "allowed") in {(e["tool"], e["decision"]) for e in lines}, lines
    assert not any(e["tool"] in ("session", "nope", "firewall-panic-toggle") for e in lines), "refused calls must not reach Agent1"
    print("assistant integration ok")
finally:
    for process in processes:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
    server.close()
    sock_path.unlink(missing_ok=True)
    shutil.rmtree(scratch, ignore_errors=True)
