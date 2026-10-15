#!/usr/bin/env python3
"""A scripted shell for Mobius's task card. It owns org.helix.Shell (AssistantSubmit, OpenPanel) and org.helix.Shell.Act
(GetState, Stop, Changed) the way the shell's assistant session does, and walks a handed-over request through
thinking -> confirm -> acting -> result. Approving is something only the real shell does, so the test stands in for the person by
creating <state>/approve; a request containing "fail" ends as a failure. Every call is appended to <state>/fake-shell-act.log.

  fake-shell-act.py <state dir>
"""
import json, os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
state_dir = sys.argv[1]
log_path = os.path.join(state_dir, "fake-shell-act.log")
approve_path = os.path.join(state_dir, "approve")
PATH_SHELL, PATH_ACT = "/org/helix/Shell", "/org/helix/Shell/Act"
state = {"task": 0, "phase": "idle", "request": "", "text": "", "description": "", "reason": "", "failed": False}
timers = []
connection = None

def log(line):
    with open(log_path, "a") as handle: handle.write(line + "\n")

def variant(value):
    if isinstance(value, bool): return GLib.Variant("b", value)
    if isinstance(value, int): return GLib.Variant("t", value)
    return GLib.Variant("s", value)

def changed():
    connection.emit_signal(None, PATH_ACT, "org.helix.Shell.Act1", "Changed", None)

def cancel_timers():
    while timers: GLib.source_remove(timers.pop())

def later(ms, fn):
    def run():
        if fn(): return True
        return False
    timers.append(GLib.timeout_add(ms, run))

def move(**fields):
    state.update(fields); changed()

def poll_approval():
    if state["phase"] != "confirm": return False
    if not os.path.exists(approve_path): return True
    os.remove(approve_path)
    log("APPROVED")
    move(phase="acting")
    later(600, finish)
    return False

def finish():
    if "fail" in state["request"].lower():
        move(phase="result", failed=True, text="The desktop service did not answer.")
    else:
        move(phase="result", failed=False, text="Done. The screen is dimmer.")
    return False

def begin_confirm():
    move(phase="confirm", description="Make the screen dimmer")
    later(150, poll_approval)
    return False

def on_method(conn, sender, path, interface, method, params, invocation):
    short = interface.split(".")[-1]
    try:
        if short == "Shell1" and method == "AssistantSubmit":
            (request,) = params.unpack()
            log(f"SUBMIT {json.dumps(request)}")
            cancel_timers()
            if os.path.exists(approve_path): os.remove(approve_path)
            state.update(task=state["task"] + 1, phase="thinking", request=request, text="", description="", reason="", failed=False)
            changed()
            later(900, begin_confirm)
            invocation.return_value(None)
        elif short == "Shell1" and method == "OpenPanel":
            (panel,) = params.unpack()
            log(f"OPEN {panel}")
            invocation.return_value(None)
        elif short == "Act1" and method == "GetState":
            invocation.return_value(GLib.Variant("(a{sv})", ({k: variant(v) for k, v in state.items()},)))
        elif short == "Act1" and method == "Stop":
            log("STOP")
            cancel_timers()
            if state["phase"] in ("thinking", "confirm", "acting"):
                move(phase="idle", text="", description="")
            invocation.return_value(None)
        else:
            invocation.return_dbus_error("org.helix.Desktop.Error.Failed", f"fake shell: {short}.{method} is not implemented")
    except Exception as error:
        invocation.return_dbus_error("org.helix.Desktop.Error.Failed", f"fake shell error: {error}")

def register(conn, xml_file, path):
    node = Gio.DBusNodeInfo.new_for_xml(open(os.path.join(ROOT, "protocols/dbus", xml_file)).read())
    for iface in node.interfaces:
        conn.register_object(path, iface, on_method, None, None)

def on_bus(conn, name):
    global connection
    connection = conn
    register(conn, "org.helix.Shell.xml", PATH_SHELL)
    register(conn, "org.helix.Shell.Act.xml", PATH_ACT)
    Gio.bus_own_name_on_connection(conn, "org.helix.Shell.Act", Gio.BusNameOwnerFlags.NONE, None, None)
    print("fake shell ready", flush=True)

Gio.bus_own_name(Gio.BusType.SESSION, "org.helix.Shell", Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
GLib.MainLoop().run()
