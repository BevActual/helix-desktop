"""Real speech-to-text: helix-inference with the real helix-transcribe worker and a whisper model, on whisper.cpp's JFK sample.

Run under dbus-run-session:  inference_transcribe_real.py <helix-inference> <helix-transcribe>
Needs ggml-*.bin in ~/.local/share/helix/models/speech and third_party/whisper.cpp-*/samples/jfk.wav.
"""
import glob, json, os, struct, subprocess, sys, tempfile, time, wave
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

service_bin, worker = sys.argv[1], os.path.abspath(sys.argv[2])
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME, PATH, IFACE = "org.helix.Inference", "/org/helix/Inference", "org.helix.Inference1"
sample = glob.glob(os.path.join(root, "third_party/whisper.cpp-*/samples/jfk.wav"))[0]
w = wave.open(sample)
assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (16000, 1, 2)
pcm = w.readframes(w.getnframes())

state = tempfile.mkdtemp(prefix="helix-stt-")
env = dict(os.environ, HELIX_INFERENCE_BACKEND="fake", HELIX_TRANSCRIBER="process", HELIX_TRANSCRIBE_BIN=worker,
           HELIX_MODEL_DIR=os.path.expanduser("~/.local/share/helix/models"), XDG_STATE_HOME=state)
service = subprocess.Popen([service_bin], env=env, stdout=subprocess.DEVNULL, stderr=open(state + "/service.log", "w"))
conn = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
done = {}
conn.signal_subscribe(NAME, IFACE, "Completed", PATH, None, Gio.DBusSignalFlags.NONE,
                      lambda *a: done.setdefault(a[-1].unpack()[0], json.loads(a[-1].unpack()[2])))
try:
    for _ in range(100):
        try:
            if conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                              GLib.Variant("(s)", (NAME,)), None, 0, 1000, None)[0]:
                break
        except GLib.Error:
            pass
        time.sleep(0.1)
    start = time.time()
    options = {"audio": GLib.Variant("ay", pcm)}
    (op,) = conn.call_sync(NAME, PATH, IFACE, "Action", GLib.Variant("(sa{sv})", ("transcribe", options)), GLib.VariantType("(t)"), 0, 10000, None).unpack()
    ctx = GLib.MainContext.default()
    while op not in done and time.time() - start < 120:
        while ctx.iteration(False): pass
        time.sleep(0.02)
    result = done.get(op)
    print(f"{time.time()-start:.1f}s {result}")
    assert result and result["ok"] and "country" in result["text"].lower() and result["language"] == "en", "no transcript"
    print("transcribe real ok")
finally:
    service.terminate()
