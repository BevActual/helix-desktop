#!/usr/bin/env python3
"""Use a locally built Cargo inference runtime (helix-inference, helix-transcribe, helix-image) with the systemd sandbox.

Creates one user drop-in; needs no root. Remove only 90-mobius-images.conf and restart to revert.
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
profile = os.environ.get("HELIX_CARGO_PROFILE", "fast")
cargo_bin = Path(os.environ.get("HELIX_CARGO_TARGET", str(root / "target" / profile))).resolve()

inference = Path(os.environ.get("HELIX_INFERENCE_BIN", str(cargo_bin / "helix-inference")))
transcribe = Path(os.environ.get("HELIX_TRANSCRIBE_BIN", str(cargo_bin / "helix-transcribe")))
image = Path(os.environ.get("HELIX_IMAGE_BIN", str(cargo_bin / "helix-image")))

if not inference.is_file():
    raise SystemExit(
        f"Build helix-inference first: cargo build --profile {profile} -p helix-inference "
        f"(expected {inference})"
    )
if not image.is_file():
    raise SystemExit(
        f"Build helix-image first: cargo build --profile {profile} -p helix-image "
        f"(expected {image}; needs third_party/sd-install)"
    )

connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
for _ in range(120):
    try:
        snapshot = connection.call_sync(
            "org.helix.Inference",
            "/org/helix/Inference",
            "org.helix.Inference1",
            "GetSnapshot",
            None,
            GLib.VariantType("(a{sv})"),
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        ).unpack()[0]
        if not snapshot["queue"]["running"] and snapshot["queue"]["waiting"] == 0:
            break
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
    if not backup.exists():
        backup.write_text(previous)
content = "[Service]\nExecStart=\nExecStart=" + json.dumps(str(inference)) + "\n"
content += "Environment=" + json.dumps("HELIX_IMAGE_BIN=" + str(image)) + "\n"
if transcribe.is_file():
    content += "Environment=" + json.dumps("HELIX_TRANSCRIBE_BIN=" + str(transcribe)) + "\n"
content += "CacheDirectory=helix-inference\nCacheDirectoryMode=0700\nEnvironment=XDG_CACHE_HOME=%C/helix-inference\n"
dropin.write_text(content)
try:
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    subprocess.run(["systemctl", "--user", "restart", "helix-inference.service"], check=True)
except subprocess.CalledProcessError:
    if previous is None:
        dropin.unlink()
    else:
        dropin.write_text(previous)
    subprocess.run(["systemctl", "--user", "daemon-reload"])
    raise
print(f"Using {inference} (image={image})")
