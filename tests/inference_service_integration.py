"""helix-inference over D-Bus with the scripted backend: streaming, privacy of replies, validation, limits, cancel, cleanup.

Run under dbus-run-session:  inference_service_integration.py <helix-inference>
"""
import json
import base64
import struct
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

import tempfile
# A private state directory: the service reads inference-routing.toml from it, never the person's own.
state_dir = tempfile.mkdtemp(prefix="helix-inference-test-")
env = dict(os.environ, HELIX_INFERENCE_BACKEND="fake", HELIX_FAKE_REPLY=REPLY, HELIX_FAKE_DELAY_MS="25",
           XDG_STATE_HOME=state_dir, HOME=state_dir)
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
        self.loading = {}
        self.progress = {}
        self.conn.signal_subscribe(NAME, IFACE, "Progress", PATH, None, Gio.DBusSignalFlags.NONE,
                                   lambda *args: self.progress.setdefault(args[-1].unpack()[0], []).append(args[-1].unpack()[1:]))
        self.conn.signal_subscribe(NAME, IFACE, "Loading", PATH, None, Gio.DBusSignalFlags.NONE,
                                   lambda *args: self.loading.setdefault(args[-1].unpack()[0], []).append(args[-1].unpack()[1:]))

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
    assert models == [{"id": "fake-model", "bytes": 1, "loaded": False, "vision": True}], models

    # A reply streams to the requester only, in order, and the pieces add up to the whole.
    op = a.chat()
    ok, result = a.wait(op)
    assert ok and result["ok"] and result["text"] == REPLY, result
    assert "".join(a.deltas[op]) == REPLY and len(a.deltas[op]) > 3, a.deltas.get(op)
    b.pump(0.2)
    assert op not in b.deltas and op not in b.done, "another client received someone else's reply"

    # Loading a model reports its progress to the requester only, before the reply.
    op = a.chat(model=GLib.Variant("s", "fake-model"))
    a.wait(op)
    a.pump(0.1)
    assert [f for _, f in a.loading.get(op, [])] == [0.0, 0.5, 1.0], a.loading
    assert op not in b.loading

    # Arguments are checked strictly, and the errors are D-Bus errors a client can act on.
    def action(client, name, options):
        return client.refused("Action", GLib.Variant("(sa{sv})", (name, options)), "(t)")
    good = {"role": GLib.Variant("s", "user"), "content": GLib.Variant("s", "x")}
    messages = GLib.Variant("av", [GLib.Variant("a{sv}", good)])
    # Speech to text: raw 16 kHz 16-bit samples in, text out; strict about everything else.
    def with_audio(samples, **extra):
        opts = {"audio": GLib.Variant("ay", samples)}
        opts.update(extra)
        return opts
    pcm = bytes(2 * 16000)
    op = a.call("Action", GLib.Variant("(sa{sv})", ("transcribe", with_audio(pcm))), "(t)")[0]
    ok, result = a.wait(op)
    assert ok and result["text"] == "heard 16000 samples" and result["language"] == "en", result
    op = a.call("Action", GLib.Variant("(sa{sv})", ("transcribe", with_audio(pcm, language=GLib.Variant("s", "de")))), "(t)")[0]
    assert a.wait(op)[1]["language"] == "de"
    assert "16-bit" in action(a, "transcribe", with_audio(b"\x00\x01\x02"))
    assert "audio is required" in action(a, "transcribe", {})
    assert "language" in action(a, "transcribe", with_audio(pcm, language=GLib.Variant("s", "../../etc")))
    assert "unknown argument" in action(a, "transcribe", with_audio(pcm, model=GLib.Variant("s", "x")))
    assert "too long" in action(a, "transcribe", with_audio(bytes(2 * 16000 * 601)))
    assert "audio" in action(a, "chat", with_audio(pcm, messages=messages))

    assert "unknown action" in action(a, "shell", {"messages": messages})
    assert "unknown argument" in action(a, "chat", {"messages": messages, "sudo": GLib.Variant("b", True)})
    assert "messages is required" in action(a, "chat", {})
    assert "priority" in action(a, "chat", {"messages": messages, "priority": GLib.Variant("s", "urgent")})
    assert "max_tokens" in action(a, "chat", {"messages": messages, "max_tokens": GLib.Variant("s", "lots")})
    assert "each message" in action(a, "chat", {"messages": GLib.Variant("av", [GLib.Variant("a{sv}", {"role": GLib.Variant("s", "user")})])})
    assert "non-empty" in action(a, "chat", {"messages": GLib.Variant("av", [])})
    assert "too large" in action(a, "chat", {"messages": GLib.Variant("av", [GLib.Variant("a{sv}", {"role": GLib.Variant("s", "user"), "content": GLib.Variant("s", "x" * 70000)})])})

    # Pictures: sent as byte arrays, decoded and bounded by the service, referenced from message parts, refused when odd.
    import struct, zlib

    def png(width, height, pixel=(10, 20, 30, 255)):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        raw = b"".join(b"\x00" + bytes(pixel) * width for _ in range(height))
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

    def with_images(pictures, parts=None):
        content = parts if parts is not None else [{"type": "text", "text": "what is this?"}] + [{"type": "image", "index": i} for i in range(len(pictures))]
        message = {"role": GLib.Variant("s", "user"), "content": GLib.Variant("av", [GLib.Variant("a{sv}", {
            key: GLib.Variant("s", str(value)) if key != "index" else GLib.Variant("u", value) for key, value in part.items()}) for part in content])}
        options = {"messages": GLib.Variant("av", [GLib.Variant("a{sv}", message)]),
                   "images": GLib.Variant("aay", [list(p) for p in pictures])}
        return options

    op = a.call("Action", GLib.Variant("(sa{sv})", ("chat", with_images([png(40, 30), png(8, 8)]))), "(t)")[0]
    ok, result = a.wait(op)
    assert ok and result["text"] == "I can see 2 image(s): 40x30 8x8.", result
    assert "not a picture" in action(a, "chat", with_images([b"<svg onload=alert(1)/>"])) or "unsupported" in action(a, "chat", with_images([b"<svg onload=alert(1)/>"]))
    assert "too many" in action(a, "chat", with_images([png(2, 2)] * 5))
    assert "not sent" in action(a, "chat", with_images([png(2, 2)], [{"type": "image", "index": 3}]))
    assert "must be used" in action(a, "chat", with_images([png(2, 2), png(2, 2)], [{"type": "image", "index": 0}]))
    assert "too large" in action(a, "chat", with_images([png(60000, 60000)[:60]]))  # a header that claims 60000 x 60000

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
    image_args = {"prompt": GLib.Variant("s", "a blue mug"), "width": GLib.Variant("u", 512), "height": GLib.Variant("u", 512), "seed": GLib.Variant("u", 42)}
    for bad in [{"width": GLib.Variant("u", 9999)}, {"steps": GLib.Variant("i", -1)}, {"model": GLib.Variant("s", "../../private.gguf")}, {"prompt": GLib.Variant("s", "x" * 4001)}]:
        options = dict(image_args, **bad)
        a.refused("Action", GLib.Variant("(sa{sv})", ("image", options)), "(t)")
    image_op = a.call("Action", GLib.Variant("(sa{sv})", ("image", image_args)), "(t)")[0]
    ok, result = a.wait(image_op)
    png = base64.b64decode(result["image"]["png"], validate=True)
    assert ok and png.startswith(b"\x89PNG\r\n\x1a\n") and struct.unpack(">II", png[16:24]) == (512, 512)
    assert result["image"]["seed"] == 42 and result["image"]["prompt"] == "a blue mug"
    assert image_op in a.progress and image_op not in b.progress and image_op not in b.done
    image_op = a.call("Action", GLib.Variant("(sa{sv})", ("image", image_args)), "(t)")[0]
    a.call("Cancel", GLib.Variant("(t)", (image_op,)))
    assert a.wait(image_op)[1]["cancelled"]
    op = a.chat()
    assert a.wait(op)[1]["text"] == REPLY
    print("inference service integration ok")
finally:
    service.terminate()
    try:
        service.wait(timeout=10)
    except subprocess.TimeoutExpired:
        service.kill()
