#!/usr/bin/env python3
"""A scripted PackageKit on a private bus, for Helix Software's Fedora backend. It owns org.freedesktop.PackageKit and speaks the transaction API the
app uses (CreateTransaction, then GetPackages, GetUpdates, Resolve, InstallPackages, RemovePackages, UpdatePackages, RefreshCache, Cancel), emitting
Package, ItemProgress, ErrorCode and Finished like the real daemon. State: installed {helix-calc, helix-locked, bash}; helix-calc has an update; the
repositories also hold 'kernel' and 'glibc' updates (system software). Removing helix-locked fails as if another package needed it. Every call is
appended to <state dir>/fake-packagekit.log (`INSTALL helix-writer`).

  fake-packagekit.py <state dir> <bus address>
"""
import os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

state_dir, address = sys.argv[1], sys.argv[2]
log_path = os.path.join(state_dir, "fake-packagekit.log")
STEP_MS = int(os.environ.get("HELIX_FAKE_PK_STEP_MS", "900"))
INSTALLED = {"helix-calc": "1.0-1", "helix-locked": "1.0-1", "bash": "5.3-1"}
UPDATES = {"helix-calc": "2.0-1", "kernel": "7.2.11-1", "glibc": "2.43-1"}
AVAILABLE = {"helix-writer": "1.5-1", "helix-calc": "2.0-1", "helix-locked": "1.0-1"}
ARCH = "x86_64"
REPOS = [("fixture", "Fixture Repository 1 - x86_64", True), ("fixture-source", "Fixture Repository 1 - Source", False), ("fixture-extra", "Fixture Extras", True)]
transactions = {}
counter = [0]
connection = None

def log(line):
    with open(log_path, "a") as handle: handle.write(line + "\n")

def pid(name, version, data): return f"{name};{version};{ARCH};{data}"

ROOT_XML = """<node><interface name="org.freedesktop.PackageKit">
<method name="CreateTransaction"><arg type="o" direction="out"/></method></interface></node>"""
TX_XML = """<node><interface name="org.freedesktop.PackageKit.Transaction">
<method name="GetPackages"><arg type="t" direction="in"/></method>
<method name="GetUpdates"><arg type="t" direction="in"/></method>
<method name="Resolve"><arg type="t" direction="in"/><arg type="as" direction="in"/></method>
<method name="InstallPackages"><arg type="t" direction="in"/><arg type="as" direction="in"/></method>
<method name="RemovePackages"><arg type="t" direction="in"/><arg type="as" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="in"/></method>
<method name="UpdatePackages"><arg type="t" direction="in"/><arg type="as" direction="in"/></method>
<method name="RefreshCache"><arg type="b" direction="in"/></method>
<method name="GetRepoList"><arg type="t" direction="in"/></method>
<method name="Cancel"/>
<signal name="Package"><arg type="u"/><arg type="s"/><arg type="s"/></signal>
<signal name="RepoDetail"><arg type="s"/><arg type="s"/><arg type="b"/></signal>
<signal name="ItemProgress"><arg type="s"/><arg type="u"/><arg type="u"/></signal>
<signal name="ErrorCode"><arg type="u"/><arg type="s"/></signal>
<signal name="Finished"><arg type="u"/><arg type="u"/></signal>
</interface></node>"""

def emit(path, name, signature, values):
    connection.emit_signal(None, path, "org.freedesktop.PackageKit.Transaction", name, GLib.Variant(signature, values))

def finish(path, exit_code=1):
    emit(path, "Finished", "(uu)", (exit_code, 5))

def later(fn, delay=60):
    GLib.timeout_add(delay, lambda: (fn(), False)[1])

def names_of(ids): return [i.split(";")[0] for i in ids]

def steps(path, label, work, error=None):
    tx = transactions[path]
    tx["cancelled"] = False
    plan = [25, 50, 75, 100]
    def tick():
        if tx["cancelled"]:
            log(f"CANCELLED {label}")
            finish(path, 3)
            return False
        if not plan:
            if error:
                emit(path, "ErrorCode", "(us)", (9, error)); finish(path, 2)
            else:
                work(); finish(path, 1)
            return False
        emit(path, "ItemProgress", "(suu)", (label, 0, plan.pop(0)))
        return True
    GLib.timeout_add(STEP_MS, tick)

def on_tx(conn, sender, path, iface, method, params, invocation):
    log(f"{method} {params.unpack()}")
    args = params.unpack()
    invocation.return_value(None)
    if method == "GetPackages":
        def go():
            for name, version in INSTALLED.items():
                emit(path, "Package", "(uss)", (1, pid(name, version, "installed:fixture"), name))
            finish(path)
        later(go)
    elif method == "GetUpdates":
        def go():
            for name, version in UPDATES.items():
                emit(path, "Package", "(uss)", (8, pid(name, version, "updates"), name))
            finish(path)
        later(go)
    elif method == "Resolve":
        flt, names = args
        def go():
            for name in names:
                if flt & 4 and name in INSTALLED:
                    emit(path, "Package", "(uss)", (1, pid(name, INSTALLED[name], "installed:fixture"), name))
                if flt & 8 and name not in INSTALLED and name in AVAILABLE:
                    emit(path, "Package", "(uss)", (2, pid(name, AVAILABLE[name], "fixture"), name))
                # another architecture, which the app must not pick
                if flt & 8 and name in AVAILABLE and name not in INSTALLED:
                    emit(path, "Package", "(uss)", (2, f"{name};{AVAILABLE[name]};i686;fixture", name))
            finish(path)
        later(go)
    elif method == "InstallPackages":
        ids = args[1]
        log("INSTALL " + " ".join(names_of(ids)))
        steps(path, ids[0], lambda: [INSTALLED.__setitem__(n, AVAILABLE.get(n, "1.0-1")) for n in names_of(ids)])
    elif method == "RemovePackages":
        ids = args[1]
        log("REMOVE " + " ".join(names_of(ids)) + f" allow_deps={args[2]}")
        if "helix-locked" in names_of(ids):
            steps(path, ids[0], lambda: None, error="Package helix-locked is needed by helix-other")
        else:
            steps(path, ids[0], lambda: [INSTALLED.pop(n, None) for n in names_of(ids)])
    elif method == "UpdatePackages":
        ids = args[1]
        log("UPDATE " + " ".join(sorted(names_of(ids))))
        def apply():
            for n in names_of(ids):
                if n in UPDATES:
                    INSTALLED[n] = UPDATES.pop(n)
        steps(path, ids[0], apply)
    elif method == "GetRepoList":
        def go():
            for repo, description, enabled in REPOS:
                emit(path, "RepoDetail", "(ssb)", (repo, description, enabled))
            finish(path)
        later(go)
    elif method == "RefreshCache":
        log("REFRESH")
        later(lambda: finish(path))
    elif method == "Cancel":
        transactions[path]["cancelled"] = True

def on_root(conn, sender, path, iface, method, params, invocation):
    counter[0] += 1
    tx_path = f"/org/freedesktop/PackageKit/Transaction{counter[0]}"
    node = Gio.DBusNodeInfo.new_for_xml(TX_XML)
    conn.register_object(tx_path, node.interfaces[0], on_tx, None, None)
    transactions[tx_path] = {}
    invocation.return_value(GLib.Variant("(o)", (tx_path,)))

conn = Gio.DBusConnection.new_for_address_sync(address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
connection = conn
node = Gio.DBusNodeInfo.new_for_xml(ROOT_XML)
conn.register_object("/org/freedesktop/PackageKit", node.interfaces[0], on_root, None, None)
Gio.bus_own_name_on_connection(conn, "org.freedesktop.PackageKit", Gio.BusNameOwnerFlags.NONE, None, None)
print("fake packagekit ready", flush=True)
GLib.MainLoop().run()
