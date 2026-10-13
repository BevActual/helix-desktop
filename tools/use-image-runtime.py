#!/usr/bin/env python3
"""Use the locally built image-capable inference runtime with the existing systemd sandbox.

This creates one user drop-in; it needs no root access and preserves the system-installed binaries.
Remove only 90-mobius-images.conf and restart the service to return to the installed runtime.
"""
import json
import os
from pathlib import Path
import subprocess
import time
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

root = Path(__file__).resolve().parent.parent
build = Path(os.environ.get("HELIX_BUILD_DIR", str(root / "build-native-clean"))).resolve()
for name in ("helix-inference", "helix-image"):
    if not (build / name).is_file():
        raise SystemExit(f"Build {name} first: ninja -C {build} helix-inference helix-image")
connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
for _ in range(120):
    try:
        snapshot = connection.call_sync("org.helix.Inference", "/org/helix/Inference", "org.helix.Inference1", "GetSnapshot", None, GLib.VariantType("(a{sv})"), Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
        if not snapshot['queue']['running'] and snapshot['queue']['waiting'] == 0: break
    except GLib.Error:
        break
    time.sleep(0.5)
else:
    raise SystemExit("Inference is busy. Let the current requests finish before changing the runtime.")

config_home = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
dropin = config_home / "systemd/user/helix-inference.service.d/90-mobius-images.conf"
dropin.parent.mkdir(parents=True, exist_ok=True)
previous = dropin.read_text() if dropin.exists() else None
if previous is not None:
    backup = dropin.with_suffix(".conf.before-images")
    if not backup.exists(): backup.write_text(previous)
content = "[Service]\nExecStart=\nExecStart=" + json.dumps(str(build / "helix-inference")) + "\n"
content += "Environment=" + json.dumps("HELIX_IMAGE_BIN=" + str(build / "helix-image")) + "\n"
content += "Environment=" + json.dumps("HELIX_TRANSCRIBE_BIN=/usr/local/bin/helix-transcribe") + "\n"
content += "CacheDirectory=helix-inference\nCacheDirectoryMode=0700\nEnvironment=XDG_CACHE_HOME=%C/helix-inference\n"
dropin.write_text(content)
try:
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "--user", "restart", "helix-inference.service"], check=True)
except subprocess.CalledProcessError:
    if previous is None: dropin.unlink()
    else: dropin.write_text(previous)
    subprocess.run(["systemctl", "--user", "daemon-reload"])
    subprocess.run(["systemctl", "--user", "restart", "helix-inference.service"])
    raise
print(f"Image-capable runtime active. User override: {dropin}")
