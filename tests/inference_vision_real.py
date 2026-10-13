"""Real-model vision check: helix-inference with llama.cpp + a VLM and its projector. Slow; run by hand.

Run under dbus-run-session:  inference_vision_real.py <helix-inference> [model.gguf]
Needs the model and its mmproj-*.gguf in ~/.local/share/helix/models (or HELIX_MODEL_DIR).
"""
import io, json, os, subprocess, sys, tempfile, time
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
from PIL import Image, ImageDraw, ImageFont

service_bin = sys.argv[1]
model = sys.argv[2] if len(sys.argv) > 2 else "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf"
NAME, PATH, IFACE = "org.helix.Inference", "/org/helix/Inference", "org.helix.Inference1"

state = tempfile.mkdtemp(prefix="helix-vision-")
os.makedirs(state + "/helix")
open(state + "/helix/inference-routing.toml", "w").write(f"""
[[backends]]
id = "local"
kind = "local"
base_url = ""
label = "local"
api_key_ref = ""
[profiles.vision]
backend = "local"
model = "{model}"
""")
env = dict(os.environ, XDG_STATE_HOME=state, HELIX_MODEL_DIR=os.path.expanduser("~/.local/share/helix/models"))
service = subprocess.Popen([service_bin], env=env, stdout=subprocess.DEVNULL, stderr=open(state + "/service.log", "w"))

img = Image.new("RGB", (480, 320), (250, 250, 250))
d = ImageDraw.Draw(img)
d.rectangle((40, 60, 200, 260), fill=(220, 30, 30))
d.ellipse((260, 60, 440, 240), fill=(30, 60, 220))
try:
    font = ImageFont.truetype("DejaVuSans-Bold.ttf", 40)
except OSError:
    font = None
d.text((150, 255), "HELIX", fill=(0, 0, 0), font=font)
buf = io.BytesIO(); img.save(buf, "PNG")

conn = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
done, deltas = {}, []
conn.signal_subscribe(NAME, IFACE, "Delta", PATH, None, Gio.DBusSignalFlags.NONE, lambda *a: deltas.append(a[-1].unpack()[1]))
conn.signal_subscribe(NAME, IFACE, "Completed", PATH, None, Gio.DBusSignalFlags.NONE,
                      lambda *a: done.setdefault(a[-1].unpack()[0], (a[-1].unpack()[1], json.loads(a[-1].unpack()[2]))))
try:
    for _ in range(100):
        try:
            if conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                              GLib.Variant("(s)", (NAME,)), None, 0, 1000, None)[0]:
                break
        except GLib.Error:
            pass
        time.sleep(0.1)
    parts = [{"type": "text", "text": "Describe this picture in one sentence. Name the colours and shapes, and any text."}, {"type": "image", "index": 0}]
    msg = {"role": GLib.Variant("s", "user"), "content": GLib.Variant("av", [GLib.Variant("a{sv}", {
        k: GLib.Variant("s", str(v)) if k != "index" else GLib.Variant("u", v) for k, v in p.items()}) for p in parts])}
    options = {"messages": GLib.Variant("av", [GLib.Variant("a{sv}", msg)]),
               "images": GLib.Variant("aay", [list(buf.getvalue())]),
               "task": GLib.Variant("s", "vision"), "max_tokens": GLib.Variant("u", 120), "temperature": GLib.Variant("d", 0.1)}
    start = time.time()
    (op,) = conn.call_sync(NAME, PATH, IFACE, "Action", GLib.Variant("(sa{sv})", ("chat", options)), GLib.VariantType("(t)"), 0, 5000, None).unpack()
    ctx = GLib.MainContext.default()
    while op not in done and time.time() - start < 120 and service.poll() is None:
        while ctx.iteration(False): pass
        time.sleep(0.02)
    if op not in done:
        print(f"no completion after {time.time()-start:.0f}s; service exit code: {service.poll()}")
        sys.exit(1)
    ok, result = done[op]
    print(f"{time.time()-start:.1f}s ok={ok}\n{result}")
    text = result.get("text", "").lower()
    assert ok and "red" in text and "blue" in text, "the model did not describe the picture"
    print("vision real ok")
finally:
    service.terminate()
    print("service log:", state + "/service.log")
