"""Real image worker behind the inference service. Run in a private dbus-run-session."""
import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

root = Path(sys.argv[1]).resolve()
models = Path(os.environ.get("HELIX_MODEL_DIR", str(Path.home() / ".local/share/helix/models")))
scratch = tempfile.TemporaryDirectory(prefix="helix-image-real-")
env = dict(os.environ, HELIX_INFERENCE_BACKEND="fake", HELIX_IMAGE_GENERATOR="process", HELIX_MODEL_DIR=str(models), XDG_STATE_HOME=scratch.name)
service = subprocess.Popen([str(root / "build-native-clean/helix-inference")], env=env, stderr=open(Path(scratch.name) / "service.log", "w"))
connection = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"], Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
name, path, interface = "org.helix.Inference", "/org/helix/Inference", "org.helix.Inference1"
done, progress = {}, {}
connection.signal_subscribe(name, interface, "Completed", path, None, Gio.DBusSignalFlags.NONE, lambda *a: done.__setitem__(a[-1].unpack()[0], (a[-1].unpack()[1], json.loads(a[-1].unpack()[2]))))
connection.signal_subscribe(name, interface, "Progress", path, None, Gio.DBusSignalFlags.NONE, lambda *a: progress.setdefault(a[-1].unpack()[0], []).append(a[-1].unpack()[1:]))
def call(method, args=None, reply="()"):
    return connection.call_sync(name, path, interface, method, args, GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 5000, None).unpack()
def wait(operation, timeout=180):
    end = time.monotonic() + timeout
    while operation not in done and time.monotonic() < end:
        while GLib.MainContext.default().iteration(False): pass
        time.sleep(0.01)
    assert operation in done, "image request timed out"
    return done[operation]
try:
    for _ in range(100):
        owned = connection.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", GLib.Variant("(s)", (name,)), GLib.VariantType("(b)"), Gio.DBusCallFlags.NONE, 1000, None).unpack()[0]
        if owned: break
        time.sleep(0.1)
    else: raise AssertionError("image test service did not acquire its bus name")
    options = {"prompt": GLib.Variant("s", "a blue ceramic mug on a wooden table, soft daylight"), "width": GLib.Variant("u", 512), "height": GLib.Variant("u", 512), "steps": GLib.Variant("u", 4), "seed": GLib.Variant("u", 42)}
    started = time.monotonic()
    operation = call("Action", GLib.Variant("(sa{sv})", ("image", options)), "(t)")[0]
    ok, result = wait(operation)
    assert ok, result
    assert result["image"]["model"] == "stable-diffusion-v2-1-turbo-Q4_0.gguf", result["image"]["model"]
    png = base64.b64decode(result["image"]["png"], validate=True)
    assert png.startswith(b"\x89PNG\r\n\x1a\n") and struct.unpack(">II", png[16:24]) == (512, 512)
    output = Path(scratch.name) / "image.png"; output.write_bytes(png)
    print(f"Real D-Bus generation: {len(png)} PNG bytes in {time.monotonic()-started:.2f}s; {len(progress.get(operation, []))} progress events", flush=True)
    operation = call("Action", GLib.Variant("(sa{sv})", ("image", options)), "(t)")[0]
    time.sleep(0.5); call("Cancel", GLib.Variant("(t)", (operation,)))
    assert wait(operation, 20)[1]["cancelled"]
    call("GetSnapshot", reply="(a{sv})")
    print("Real worker cancellation and service survival passed", flush=True)
finally:
    service.terminate(); service.wait(timeout=15)
    scratch.cleanup()
