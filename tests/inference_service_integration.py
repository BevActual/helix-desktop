"""helix-inference over D-Bus with the scripted backend: streaming, privacy of replies, validation, limits, cancel, cleanup.

Run under dbus-run-session:  inference_service_integration.py <helix-inference>
"""
import json
import os
import subprocess
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

service_bin = sys.argv[1]
NAME, PATH, IFACE = "org.helix.Inference", "/org/helix/Inference", "org.helix.Inference1"
REPLY = "hello from the fake backend, streamed in small pieces"

env = dict(os.environ, HELIX_INFERENCE_BACKEND="fake", HELIX_FAKE_REPLY=REPLY, HELIX_FAKE_DELAY_MS="25")
service = subprocess.Popen([service_bin], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def bus():
    return Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)


class Client:
    def __init__(self):
        self.conn = bus()
        self.deltas, self.done = {}, {}
        self.conn.signal_subscribe(NAME, IFACE, "Delta", PATH, None, Gio.DBusSignalFlags.NONE, self._delta)
        self.conn.signal_subscribe(NAME, IFACE, "Completed", PATH, None, Gio.DBusSignalFlags.NONE, self._completed)

    def _delta(self, *args):
        op, text = args[-1].unpack()
        self.deltas.setdefault(op, []).append(text)

    def _completed(self, *args):
        op, ok, text = args[-1].unpack()
        self.done[op] = (ok, json.loads(text))

    def call(self, method, params=None, reply="()"):
        return self.conn.call_sync(NAME, PATH, IFACE, method, params, GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 5000, None).unpack()

    def refused(self, method, params, reply="()"):
        try:
            self.call(method, params, reply)
        except GLib.Error as error:
            return error.message
        raise AssertionError(f"{method} unexpectedly succeeded")

    def chat(self, **args):
        options = {"messages": GLib.Variant("av", [GLib.Variant("a{sv}", {"role": GLib.Variant("s", "user"), "content": GLib.Variant("s", "hi")})])}
        for key, value in args.items():
            options[key] = value
        return self.call("Action", GLib.Variant("(sa{sv})", ("chat", options)), "(t)")[0]

    def wait(self, op, timeout=10):
        end = time.time() + timeout
        ctx = GLib.MainContext.default()
        while op not in self.done and time.time() < end:
            while ctx.iteration(False): pass
            time.sleep(0.01)
        assert op in self.done, f"operation {op} did not finish"
        return self.done[op]

    def pump(self, seconds):
        end = time.time() + seconds
        ctx = GLib.MainContext.default()
        while time.time() < end:
            while ctx.iteration(False): pass
            time.sleep(0.01)


try:
    probe = bus()
    for _ in range(100):
        if probe.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                           GLib.Variant("(s)", (NAME,)), None, Gio.DBusCallFlags.NONE, 1000, None)[0]:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("helix-inference did not start: " + service.stderr.read().decode()[:400])

    a, b = Client(), Client()

    # Introspection and listing.
    (snapshot,) = a.call("GetSnapshot", None, "(a{sv})")
    assert snapshot["backend"] == "fake" and snapshot["queue"]["max"] == 8 and snapshot["queue"]["running"] is False, snapshot
    (models,) = a.call("ListModels", None, "(av)")
    assert models == [{"id": "fake-model", "bytes": 1, "loaded": False}], models

    # A reply streams to the requester only, in order, and the pieces add up to the whole.
    op = a.chat()
    ok, result = a.wait(op)
    assert ok and result["ok"] and result["text"] == REPLY, result
    assert "".join(a.deltas[op]) == REPLY and len(a.deltas[op]) > 3, a.deltas.get(op)
    b.pump(0.2)
    assert op not in b.deltas and op not in b.done, "another client received someone else's reply"

    # Arguments are checked strictly, and the errors are D-Bus errors a client can act on.
    def action(client, name, options):
        return client.refused("Action", GLib.Variant("(sa{sv})", (name, options)), "(t)")
    good = {"role": GLib.Variant("s", "user"), "content": GLib.Variant("s", "x")}
    messages = GLib.Variant("av", [GLib.Variant("a{sv}", good)])
    assert "unknown action" in action(a, "shell", {"messages": messages})
    assert "unknown argument" in action(a, "chat", {"messages": messages, "sudo": GLib.Variant("b", True)})
    assert "messages is required" in action(a, "chat", {})
    assert "priority" in action(a, "chat", {"messages": messages, "priority": GLib.Variant("s", "urgent")})
    assert "max_tokens" in action(a, "chat", {"messages": messages, "max_tokens": GLib.Variant("s", "lots")})
    assert "each message" in action(a, "chat", {"messages": GLib.Variant("av", [GLib.Variant("a{sv}", {"role": GLib.Variant("s", "user")})])})
    assert "non-empty" in action(a, "chat", {"messages": GLib.Variant("av", [])})
    assert "too large" in action(a, "chat", {"messages": GLib.Variant("av", [GLib.Variant("a{sv}", {"role": GLib.Variant("s", "user"), "content": GLib.Variant("s", "x" * 70000)})])})

    # The per-client limit (3) counts running and waiting requests.
    first, second, third = a.chat(), a.chat(), a.chat()
    assert "too many" in action(a, "chat", {"messages": messages})
    for op in (first, second, third):
        a.wait(op, 30)

    # Cancel: the requester can; another connection cannot; a running reply stops early and keeps what it had.
    op = a.chat()
    a.pump(0.15)
    assert "no cancellable" in b.refused("Cancel", GLib.Variant("(t)", (op,)))
    a.call("Cancel", GLib.Variant("(t)", (op,)))
    ok, result = a.wait(op)
    assert not ok and result["cancelled"] and len(result["text"]) < len(REPLY), result
    assert "no cancellable" in a.refused("Cancel", GLib.Variant("(t)", (op,)))  # already finished

    # A caller that disappears does not leave work behind.
    gone = Client()
    for _ in range(3):
        gone.chat()
    gone.conn.close_sync(None)
    deadline = time.time() + 10
    while time.time() < deadline:
        (snapshot,) = a.call("GetSnapshot", None, "(a{sv})")
        if snapshot["queue"]["waiting"] == 0 and snapshot["queue"]["running"] is False:
            break
        time.sleep(0.1)
    else:
        raise AssertionError(f"abandoned requests were not cleaned up: {snapshot}")

    # And the service is still healthy afterwards.
    op = a.chat()
    assert a.wait(op)[1]["text"] == REPLY
    print("inference service integration ok")
finally:
    service.terminate()
    try:
        service.wait(timeout=10)
    except subprocess.TimeoutExpired:
        service.kill()
