"""Pending confirmations expire when the confirmer does not answer in time.

Run under dbus-run-session with a short test deadline:
  HELIX_TEST_CONFIRMATION_SECS=1 desktop_agent_confirmation_expire.py <helix-desktop> <msg-risk.toml>
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

service_bin, policy = sys.argv[1], sys.argv[2]
NAME, PATH = "org.helix.Desktop", "/org/helix/Desktop"
AGENT = "org.helix.Desktop.AgentSurface1"
CFG = "org.helix.Desktop.Configuration1"

scratch = pathlib.Path(tempfile.mkdtemp(prefix="helix-agent-exp-"))
runtime = scratch / "run"
runtime.mkdir()
display = f"wayland-exp-{os.getpid()}"
sock_path = runtime / f"helix-{display}.sock"

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(str(sock_path))
server.listen(4)


def serve():
    while True:
        try:
            conn, _ = server.accept()
        except OSError:
            return
        with conn:
            conn.recv(4096)
            conn.sendall(b"ok\n")


threading.Thread(target=serve, daemon=True).start()

env = dict(
    os.environ,
    HELIX_AGENT_IPC_DIR=str(runtime),
    WAYLAND_DISPLAY=display,
    HELIX_RISK_POLICY=policy,
    HELIX_TEST_CONFIRMATION_SECS="1",
    XDG_STATE_HOME=str(scratch / "state"),
    XDG_CONFIG_HOME=str(scratch / "config"),
    XDG_DATA_HOME=str(scratch / "data"),
    XDG_CACHE_HOME=str(scratch / "cache"),
)
for key in ("HELIX_STATE_HOME", "HELIX_CONFIG_HOME", "HELIX_DATA_HOME", "HELIX_CACHE_HOME"):
    env.pop(key, None)


def agent(conn, method, signature=None, *values, reply="()"):
    params = GLib.Variant(signature, values) if signature else None
    return conn.call_sync(NAME, PATH, AGENT, method, params, GLib.VariantType(reply), Gio.DBusCallFlags.NONE, 5000, None).unpack()


process = subprocess.Popen([service_bin], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    conn = Gio.DBusConnection.new_for_address_sync(
        os.environ["DBUS_SESSION_BUS_ADDRESS"],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None,
        None,
    )
    for _ in range(100):
        if conn.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "NameHasOwner",
            GLib.Variant("(s)", (NAME,)),
            None,
            Gio.DBusCallFlags.NONE,
            1000,
            None,
        )[0]:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("service did not start")

    conn.call_sync(NAME, PATH, CFG, "RegisterDesktop", None, None, Gio.DBusCallFlags.NONE, 5000, None)
    agent(conn, "RegisterConfirmer")
    agent(conn, "Hello", "(ss)", "expire test", "1", reply="(a{sv})")

    grant_op = agent(conn, "RequestAccess", "(as)", ["observe", "confirm"], reply="(t)")[0]
    conn.call_sync(
        NAME,
        PATH,
        AGENT,
        "Confirm",
        GLib.Variant(
            "(tba{sv})",
            (
                grant_op,
                True,
                {"tiers": GLib.Variant("as", ["observe", "confirm"])},
            ),
        ),
        None,
        Gio.DBusCallFlags.NONE,
        5000,
        None,
    )

    completed = []

    def on_completed(_conn, _sender, _path, _iface, _name, params):
        completed.append(params.unpack())

    conn.signal_subscribe(NAME, AGENT, "Completed", PATH, None, Gio.DBusSignalFlags.NONE, on_completed)

    operation = agent(
        conn,
        "Action",
        "(sa{sv})",
        "app-launch",
        {
            "argv": GLib.Variant("as", []),
            "literal": GLib.Variant("as", ["Files"]),
            "reason": GLib.Variant("s", ""),
        },
        reply="(t)",
    )[0]

    time.sleep(2.5)
    ctx = GLib.MainContext.default()
    for _ in range(80):
        while ctx.iteration(False):
            pass
        if any(payload[0] == operation for payload in completed):
            break
        time.sleep(0.05)
    else:
        raise AssertionError(f"timed out waiting for Completed; saw {completed}")

    _, ok, text = next(payload for payload in completed if payload[0] == operation)
    result = json.loads(text)
    assert not ok and "timed out" in result["error"], (ok, result)
    print("confirmation expire ok")
finally:
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
    server.close()
    sock_path.unlink(missing_ok=True)
    shutil.rmtree(scratch, ignore_errors=True)
