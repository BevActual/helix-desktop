#!/usr/bin/env python3
"""Proves what helix-inference's sandbox blocks and allows, using the properties from its unit file.

Runs short probes in transient systemd user units that carry exactly the unit's [Service] hardening, and checks:
  blocked: network access, writing to the home directory
  allowed: reading the model directory, finding the GPU through Vulkan
Usage: check-inference-sandbox.py <repo-root>
"""
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
unit = (root / "assets/services/helix-inference.service.in").read_text()
SKIP = ("ExecStart", "Type", "BusName", "Restart", "RestartSec", "Description", "Documentation")
properties = []
in_service = False
for line in unit.splitlines():
    line = line.strip()
    if line.startswith("["):
        in_service = line == "[Service]"
        continue
    if not in_service or not line or line.startswith("#") or "=" not in line:
        continue
    key, _, value = line.partition("=")
    if key not in SKIP:
        properties.append(f"{key}={value}")

models = pathlib.Path.home() / ".local/share/helix/models"
engine = root / "third_party/llama-vulkan-install/bin"
probe_file = pathlib.Path.home() / ".helix-sandbox-probe"


def run(label, argv, expect_ok):
    command = ["systemd-run", "--user", "--wait", "--pipe", "--collect", "--quiet"]
    for item in properties:
        command += ["-p", item]
    command += [f"--setenv=LD_LIBRARY_PATH={engine}"]
    result = subprocess.run(command + argv, capture_output=True, text=True, timeout=60)
    ok = result.returncode == 0
    verdict = "ok " if ok == expect_ok else "FAIL"
    print(f"{verdict}  {label}: {'allowed' if ok else 'blocked'} (expected {'allowed' if expect_ok else 'blocked'})")
    if ok != expect_ok:
        print("      " + (result.stdout + result.stderr).strip().replace("\n", "\n      ")[:600])
    return ok == expect_ok, result


print(f"{len(properties)} hardening properties from the unit file")
results = [
    run("connect to the network", [sys.executable, "-c", "import socket; socket.create_connection(('1.1.1.1', 53), 3)"], False),
    run("write a file in the home directory", [sys.executable, "-c", f"open({str(probe_file)!r}, 'w').write('x')"], False),
    run("read the model directory", [sys.executable, "-c", f"import os; assert os.listdir({str(models)!r})"], True),
]

# The real service, hardened with the same properties: can it still reach the GPU and run a model?
sys.path.insert(0, str(root / "tests"))
import gi  # noqa: E402

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

UNIT = "helix-inference-sandbox-check"
binary = next(p for p in (root / "target/fast/helix-inference", root / "target/release/helix-inference", root / "target/debug/helix-inference") if p.exists())
command = ["systemd-run", "--user", f"--unit={UNIT}", "--collect", "--quiet", "-p", "Type=dbus", "-p", "BusName=org.helix.Inference",
           f"--setenv=HELIX_LLAMA_SERVER={engine}/llama-server", f"--setenv=HELIX_LLAMA_LIBS={engine}:{pathlib.Path.home()}/.local/cuda/lib", "--setenv=HELIX_ENGINE_SANDBOX=require"]
for item in properties:
    command += ["-p", item]
subprocess.run(command + [str(binary)], check=True)
service_ok = False
try:
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(method, params=None, reply="()"):
        return conn.call_sync("org.helix.Inference", "/org/helix/Inference", "org.helix.Inference1", method, params,
                              GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 120000, None).unpack()

    import time
    for _ in range(100):
        try:
            (snapshot,) = call("GetSnapshot", None, "(a{sv})")
            break
        except GLib.Error:
            time.sleep(0.2)
    else:
        raise SystemExit("FAIL  the sandboxed service did not start: " + subprocess.run(["journalctl", "--user", "-u", UNIT, "-n", "8", "--no-pager"], capture_output=True, text=True).stdout)
    gpus = [d for d in snapshot.get("devices", []) if d["gpu"]]
    print(("ok   " if gpus else "FAIL ") + " the sandboxed service sees the GPU: " + (f"{gpus[0]['description']} ({gpus[0]['memory_free'] // 2**20} MiB free)" if gpus else f"devices={snapshot.get('devices')}"))

    done = {}
    conn.signal_subscribe("org.helix.Inference", "org.helix.Inference1", "Completed", "/org/helix/Inference", None, Gio.DBusSignalFlags.NONE,
                          lambda c, s, p, i, n, params: done.update({params.unpack()[0]: params.unpack()[2]}))
    msg = GLib.Variant("av", [GLib.Variant("a{sv}", {"role": GLib.Variant("s", "user"), "content": GLib.Variant("s", "Reply with exactly the single word OK.")})])
    (op,) = call("Action", GLib.Variant("(sa{sv})", ("chat", {"messages": msg, "max_tokens": GLib.Variant("x", 8), "model": GLib.Variant("s", "qwen2.5-1.5b-instruct-q4_k_m.gguf")})), "(t)")
    ctx = GLib.MainContext.default()
    end = time.time() + 120
    while op not in done and time.time() < end:
        while ctx.iteration(False):
            pass
        time.sleep(0.05)
    reply = __import__("json").loads(done.get(op, '{"ok": false, "error": "timeout"}'))
    service_ok = bool(gpus) and reply.get("ok") and "OK" in reply.get("text", "")
    print(("ok   " if service_ok else "FAIL ") + f" the sandboxed service ran the model: {reply.get('text')!r} ({reply.get('error') or 'no error'})")
finally:
    subprocess.run(["systemctl", "--user", "stop", UNIT], capture_output=True)
results.append((service_ok, None))

probe_file.unlink(missing_ok=True)  # in case a probe wrote it
sys.exit(0 if all(ok for ok, _ in results) else 1)
