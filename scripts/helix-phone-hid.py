#!/usr/bin/env python3
"""Advertise Helix as a BLE HID mouse so an iPhone can pair it in AssistiveTouch."""

import json
import os
import signal
import socket
import sys
import threading
import time

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

NAME = "Helix Phone"
ADAPTER = "/org/bluez/hci0"
APP_PATH = "/org/helix/phone"
AD_PATH = "/org/helix/phone/advertisement0"
AGENT_PATH = "/org/helix/phone/agent"
SERVICE_PATH = "/org/helix/phone/service0"
HID_UUID = "00001812-0000-1000-8000-00805f9b34fb"
BATTERY_UUID = "0000180f-0000-1000-8000-00805f9b34fb"
INFO_UUID = "0000180a-0000-1000-8000-00805f9b34fb"

# Composite HID: relative mouse (1), keyboard (2), consumer keys (3), absolute
# pointer (4). iOS AssistiveTouch subscribes to every input report after pairing.
REPORT_MAP = bytes([
    # Report 1: relative mouse, 3 buttons, X, Y, wheel. Used for scrolling.
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
    # Report 4: absolute pointer, 3 buttons, X/Y 0..32767 across the screen.
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x04,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0xC0, 0xC0,
    # Report 2: boot keyboard, modifiers + 6 keys.
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    # Report 3: consumer control, one 16-bit usage (Home, Search, App Switcher).
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,
    0x15, 0x00, 0x26, 0xA0, 0x02, 0x19, 0x00, 0x2A, 0xA0, 0x02,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
])

ABSOLUTE_MAX = 32767

# X11 keysym -> HID keyboard usage (unshifted). Shift/Ctrl/Alt/Super travel as modifiers.
KEYSYMS = {
    **{0x61 + i: 0x04 + i for i in range(26)},          # a-z
    **{0x31 + i: 0x1E + i for i in range(9)},           # 1-9
    0x30: 0x27,                                          # 0
    0xFF0D: 0x28, 0xFF1B: 0x29, 0xFF08: 0x2A, 0xFF09: 0x2B, 0x20: 0x2C,
    0x2D: 0x2D, 0x3D: 0x2E, 0x5B: 0x2F, 0x5D: 0x30, 0x5C: 0x31,
    0x3B: 0x33, 0x27: 0x34, 0x60: 0x35, 0x2C: 0x36, 0x2E: 0x37, 0x2F: 0x38,
    0xFFE5: 0x39,                                        # Caps Lock
    **{0xFFBE + i: 0x3A + i for i in range(12)},        # F1-F12
    0xFF63: 0x49, 0xFF50: 0x4A, 0xFF55: 0x4B, 0xFFFF: 0x4C, 0xFF57: 0x4D, 0xFF56: 0x4E,
    0xFF53: 0x4F, 0xFF51: 0x50, 0xFF54: 0x51, 0xFF52: 0x52,
    0xFF8D: 0x28,                                        # KP Enter
}
# Modifier keysyms map straight to the modifier bitmask.
MODIFIER_KEYSYMS = {
    0xFFE3: 0x01, 0xFFE1: 0x02, 0xFFE9: 0x04, 0xFFEB: 0x08,   # L Ctrl, Shift, Alt, Super
    0xFFE4: 0x10, 0xFFE2: 0x20, 0xFFEA: 0x40, 0xFFEC: 0x80,   # R Ctrl, Shift, Alt, Super
}
# Consumer usages iOS acts on (verified on iOS 26): Menu (0x40) goes Home, AC Search
# opens Spotlight, and AC Next Keyboard Layout Select (0x29D) is the Globe modifier,
# which iOS combines with keyboard usages (Globe+H Home, Globe+A App Library, ...).
# AC Home (0x223) and Cmd+Tab are ignored on iPhone.
CONSUMER_HOME = 0x0040
CONSUMER_SEARCH = 0x0221
CONSUMER_GLOBE = 0x029D

SOCKET_PATH = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "helix-phone-hid.sock")
BLUETOOTH_CONF = "/etc/bluetooth/main.conf"
# bluetoothd settings the iPhone bond depends on. Privacy gives the HID device its own
# resolvable address so iOS does not fold it into the laptop's identity; JustWorksRepairing
# lets iOS re-pair after it forgets the device without a confirmation prompt on our side.
REQUIRED_BLUETOOTH_CONF = {"General": {"Privacy": "on", "JustWorksRepairing": "always"}}

# Shortcuts the shell can trigger by name. home, search, library verified on iOS 26 (iPhone);
# the Globe letter combos follow Apple's hardware keyboard shortcuts.
SHORTCUTS = {
    "home": ("consumer", 0x0040),
    "search": ("consumer", 0x0221),
    "library": ("globe", 0x52),        # Globe+Up opens App Library on iPhone
    "siri": ("globe", 0x16),           # Globe+S
    "control_center": ("globe", 0x06), # Globe+C
    "notifications": ("globe", 0x11),  # Globe+N
}


class BondedDevices:
    """Paired BlueZ devices on this adapter, kept current from ObjectManager signals.

    Status requests arrive on control-socket threads once a second while the shell
    mirrors; reading a cached dict there is free, where a GetManagedObjects round-trip
    per request was a blocking D-Bus call from a thread that does not own the main loop.
    """

    def __init__(self, bus):
        self.lock = threading.Lock()
        self.devices = {}
        try:
            manager = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
            for path, interfaces in manager.GetManagedObjects().items():
                self._added(path, interfaces)
        except dbus.exceptions.DBusException:
            pass
        bus.add_signal_receiver(self._added, dbus_interface="org.freedesktop.DBus.ObjectManager",
                                signal_name="InterfacesAdded")
        bus.add_signal_receiver(self._removed, dbus_interface="org.freedesktop.DBus.ObjectManager",
                                signal_name="InterfacesRemoved")
        bus.add_signal_receiver(self._changed, dbus_interface="org.freedesktop.DBus.Properties",
                                signal_name="PropertiesChanged", path_keyword="path")

    def _added(self, path, interfaces):
        device = interfaces.get("org.bluez.Device1")
        if device is None or not str(path).startswith(ADAPTER):
            return
        with self.lock:
            self.devices[str(path)] = {
                "name": str(device.get("Alias", device.get("Name", ""))),
                "paired": bool(device.get("Paired", False)),
                "connected": bool(device.get("Connected", False)),
            }

    def _removed(self, path, interfaces):
        if "org.bluez.Device1" in interfaces:
            with self.lock:
                self.devices.pop(str(path), None)

    def _changed(self, interface, changed, invalidated, path=None):
        if interface != "org.bluez.Device1" or path is None:
            return
        with self.lock:
            entry = self.devices.get(str(path))
            if entry is None:
                return
            if "Paired" in changed:
                entry["paired"] = bool(changed["Paired"])
            if "Connected" in changed:
                entry["connected"] = bool(changed["Connected"])
            if "Alias" in changed:
                entry["name"] = str(changed["Alias"])

    def paired(self):
        with self.lock:
            return [{"name": d["name"], "connected": d["connected"]} for d in self.devices.values() if d["paired"]]


_CONF_CACHE = {"mtime": None, "missing": []}


def bluetooth_conf_missing():
    """Return the [Section] Key=value entries that are not active in main.conf."""
    try:
        mtime = os.stat(BLUETOOTH_CONF).st_mtime_ns
    except OSError:
        mtime = None
    if mtime is not None and _CONF_CACHE["mtime"] == mtime:
        return list(_CONF_CACHE["missing"])
    missing = _bluetooth_conf_missing_uncached()
    _CONF_CACHE["mtime"] = mtime
    _CONF_CACHE["missing"] = list(missing)
    return missing


def _bluetooth_conf_missing_uncached():
    active = {}
    section = None
    try:
        with open(BLUETOOTH_CONF, encoding="utf-8") as handle:
            for raw in handle:
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("[") and line.endswith("]"):
                    section = line[1:-1]
                    continue
                if "=" in line and section:
                    key, _, value = line.partition("=")
                    active[(section, key.strip())] = value.strip().lower()
    except OSError:
        pass
    return [
        f"[{section}] {key} = {value}"
        for section, keys in REQUIRED_BLUETOOTH_CONF.items()
        for key, value in keys.items()
        if active.get((section, key)) != value
    ]

# ASCII -> (modifier, usage) for typing text.
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\", ":": ";", '"': "'", "~": "`", "<": ",", ">": ".", "?": "/",
}


def encode_char(char: str):
    if len(char) != 1:
        return None
    if char == "\n":
        return 0, 0x28
    if char == "\t":
        return 0, 0x2B
    if "A" <= char <= "Z":
        return 0x02, KEYSYMS[ord(char.lower())]
    if char in SHIFTED:
        return 0x02, KEYSYMS[ord(SHIFTED[char])]
    usage = KEYSYMS.get(ord(char))
    return (0, usage) if usage is not None else None


class Rejected(dbus.exceptions.DBusException):
    _dbus_error_name = "org.bluez.Error.Rejected"


class Application(dbus.service.Object):
    def __init__(self, bus):
        self.path = APP_PATH
        self.services = []
        dbus.service.Object.__init__(self, bus, self.path)

    def add(self, service):
        self.services.append(service)

    @dbus.service.method("org.freedesktop.DBus.ObjectManager", out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        objects = {}
        for service in self.services:
            objects[service.get_path()] = service.properties()
            for char in service.characteristics:
                objects[char.get_path()] = char.properties()
                for desc in char.descriptors:
                    objects[desc.get_path()] = desc.properties()
        return objects


class Service(dbus.service.Object):
    def __init__(self, bus, index, uuid, primary=True):
        self.path = f"{APP_PATH}/service{index}"
        self.uuid = uuid
        self.primary = primary
        self.characteristics = []
        dbus.service.Object.__init__(self, bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def add(self, characteristic):
        self.characteristics.append(characteristic)

    def properties(self):
        return {
            "org.bluez.GattService1": {
                "UUID": dbus.String(self.uuid),
                "Primary": dbus.Boolean(self.primary),
                "Characteristics": dbus.Array(
                    [char.get_path() for char in self.characteristics], signature="o"
                ),
            }
        }


class Characteristic(dbus.service.Object):
    def __init__(self, bus, index, uuid, flags, service, value=b""):
        self.path = f"{service.path}/char{index}"
        self.uuid = uuid
        self.flags = flags
        self.service = service
        self.value = bytearray(value)
        self.descriptors = []
        self.notifying = False
        dbus.service.Object.__init__(self, bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def add(self, descriptor):
        self.descriptors.append(descriptor)

    def properties(self):
        return {
            "org.bluez.GattCharacteristic1": {
                "Service": self.service.get_path(),
                "UUID": dbus.String(self.uuid),
                "Flags": dbus.Array(self.flags, signature="s"),
                "Descriptors": dbus.Array([desc.get_path() for desc in self.descriptors], signature="o"),
            }
        }

    @dbus.service.method("org.bluez.GattCharacteristic1", in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, _options):
        return dbus.Array(self.value, signature="y")

    @dbus.service.method("org.bluez.GattCharacteristic1", in_signature="aya{sv}")
    def WriteValue(self, value, _options):
        self.value = bytearray(value)

    @dbus.service.method("org.bluez.GattCharacteristic1")
    def StartNotify(self):
        self.notifying = True
        print(f"iPhone subscribed to {getattr(self, 'label', self.uuid)}", flush=True)

    @dbus.service.method("org.bluez.GattCharacteristic1")
    def StopNotify(self):
        self.notifying = False
        print(f"iPhone unsubscribed from {getattr(self, 'label', self.uuid)}", flush=True)

    def notify_value(self, data: bytes):
        self.value = bytearray(data)
        if not self.notifying:
            return
        self.sent = getattr(self, "sent", 0) + 1
        if self.sent == 1 or self.sent % 500 == 0:
            print(f"sent {self.sent} {getattr(self, 'label', 'input')} reports, last {data.hex()}", flush=True)
        self.PropertiesChanged(
            "org.bluez.GattCharacteristic1",
            {"Value": dbus.Array([dbus.Byte(byte) for byte in self.value], signature="y")},
            [],
        )

    @dbus.service.signal("org.freedesktop.DBus.Properties", signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass


class Descriptor(dbus.service.Object):
    def __init__(self, bus, index, uuid, flags, characteristic, value):
        self.path = f"{characteristic.path}/desc{index}"
        self.uuid = uuid
        self.flags = flags
        self.characteristic = characteristic
        self.value = bytearray(value)
        dbus.service.Object.__init__(self, bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def properties(self):
        return {
            "org.bluez.GattDescriptor1": {
                "Characteristic": self.characteristic.get_path(),
                "UUID": dbus.String(self.uuid),
                "Flags": dbus.Array(self.flags, signature="s"),
            }
        }

    @dbus.service.method("org.bluez.GattDescriptor1", in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, _options):
        return dbus.Array(self.value, signature="y")


class Advertisement(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, AD_PATH)

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != "org.bluez.LEAdvertisement1":
            raise Rejected()
        # Keep this inside a 31-byte legacy advert. A 128-bit UUID forces
        # Bluetooth 5 extended advertising, which AssistiveTouch does not list.
        # 0x03C0 is Generic HID, the appearance iOS scans for.
        print(f"advertising {NAME} as a HID mouse", flush=True)
        return {
            "Type": dbus.String("peripheral"),
            "ServiceUUIDs": dbus.Array(["1812"], signature="s"),
            "LocalName": dbus.String(NAME),
            "Appearance": dbus.UInt16(0x03C0),
            "Discoverable": dbus.Boolean(True),
        }

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        return self.GetAll(interface)[prop]

    @dbus.service.method("org.bluez.LEAdvertisement1")
    def Release(self):
        pass


class Agent(dbus.service.Object):
    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, AGENT_PATH)

    @dbus.service.method("org.bluez.Agent1")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        print(f"pair pin requested for {device}", flush=True)
        return "0000"

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="u")
    def RequestPasskey(self, device):
        print(f"pair passkey requested for {device}", flush=True)
        return dbus.UInt32(0)

    @dbus.service.method("org.bluez.Agent1", in_signature="ouq")
    def DisplayPasskey(self, device, passkey, entered):
        print(f"display passkey {passkey} for {device} entered={entered}", flush=True)

    @dbus.service.method("org.bluez.Agent1", in_signature="os")
    def DisplayPinCode(self, device, pincode):
        print(f"display pin {pincode} for {device}", flush=True)

    @dbus.service.method("org.bluez.Agent1", in_signature="ou")
    def RequestConfirmation(self, device, passkey):
        print(f"confirm {passkey} for {device}", flush=True)
        return

    @dbus.service.method("org.bluez.Agent1", in_signature="o")
    def RequestAuthorization(self, device):
        print(f"authorize {device}", flush=True)
        try:
            props = dbus.Interface(dbus.SystemBus().get_object("org.bluez", device), "org.freedesktop.DBus.Properties")
            props.Set("org.bluez.Device1", "Trusted", dbus.Boolean(True))
            print(f"trusted {device}", flush=True)
        except dbus.exceptions.DBusException as error:
            print(f"could not trust {device}: {error}", flush=True)
        return

    @dbus.service.method("org.bluez.Agent1", in_signature="os")
    def AuthorizeService(self, device, uuid):
        print(f"authorize service {uuid} for {device}", flush=True)
        return

    @dbus.service.method("org.bluez.Agent1")
    def Cancel(self):
        print("pairing canceled", flush=True)


class KeyboardState:
    """Keyboard reports built from the keysyms the shell forwards from the mirror window.

    Super is the iOS Globe key: Super+Space is Spotlight and Super+letter combos go out
    as Globe (consumer 0x29D) held around the keyboard usage, never as Cmd.
    """

    def __init__(self, emit):
        self._emit = emit
        self.pressed = []
        self.modifiers = 0
        self.active = False

    def key(self, keyboard, consumer, keysym, pressed, mask=0):
        modifier = MODIFIER_KEYSYMS.get(keysym)
        if modifier is not None:
            self.modifiers = self.modifiers | modifier if pressed else self.modifiers & ~modifier
            self._send(keyboard)
            return
        # `mask` is the compositor's modifier state for this key; it covers modifiers set
        # through wl_keyboard.modifiers alone (virtual keyboards) that never sent a key.
        held = self.modifiers | (mask & 0x0F)
        # The shell hands over the keysym with the layout's shift level applied ("S", "!").
        # HID wants the base key; Shift already travels in the modifier byte.
        if 0x41 <= keysym <= 0x5A:
            keysym += 0x20
        elif keysym < 0x80 and chr(keysym) in SHIFTED:
            keysym = ord(SHIFTED[chr(keysym)])
        usage = KEYSYMS.get(keysym)
        if held & 0x88 and (keysym == 0x20 or usage is not None):
            if pressed:
                if keysym == 0x20:
                    self._emit(consumer, bytes((CONSUMER_SEARCH & 0xFF, CONSUMER_SEARCH >> 8)))
                    self._emit(consumer, b"\x00\x00", 80)
                else:
                    other = held & ~0x88
                    self._emit(consumer, bytes((CONSUMER_GLOBE & 0xFF, CONSUMER_GLOBE >> 8)))
                    self._emit(keyboard, bytes([other, 0, usage, 0, 0, 0, 0, 0]), 40)
                    self._emit(keyboard, bytes([other, 0, 0, 0, 0, 0, 0, 0]), 40)
                    self._emit(consumer, b"\x00\x00", 40)
            return
        if usage is None:
            return
        if pressed:
            if usage not in self.pressed:
                self.pressed.append(usage)
        elif usage in self.pressed:
            self.pressed.remove(usage)
        self._send(keyboard, held)

    def release(self, keyboard):
        if self.pressed or self.modifiers:
            self.pressed.clear()
            self.modifiers = 0
            self._emit(keyboard, bytes(8))

    def _send(self, keyboard, held=None):
        held = self.modifiers if held is None else held
        keys = (self.pressed + [0] * 6)[:6]
        self._emit(keyboard, bytes([held & ~0x88, 0, *keys]))
        self.active = bool(self.pressed or self.modifiers)


class ControlSocket:
    """Line-delimited JSON on a Unix socket so the shell (and tests) can drive the phone.

    {"type":"absolute","x":0.5,"y":0.5,"buttons":0}   fractions of the phone screen
    {"type":"tap","x":0.5,"y":0.5}                    press + release at a point
    {"type":"scroll","amount":-1}                     wheel steps
    {"type":"key","usage":40,"modifier":0}            one HID key press
    {"type":"text","value":"hello"}                   type a string
    {"type":"consumer","name":"home"|"search"}
    {"type":"globe","usage":11}                       Globe + key (11 = H)
    {"type":"shortcut","name":"home"|"search"|"library"|"siri"|"control_center"|"notifications"}
    {"type":"drag","x0":..,"y0":..,"x1":..,"y1":..}   press, move, release
    {"type":"pointer","x":0.5,"y":0.5,"buttons":0,"scroll":0}  live stream: coalesced, no reply
    {"type":"keysym","keysym":104,"pressed":true,"modifiers":2}  key from the mirror window (X keysym + HID mods)
    {"type":"release"}                                lift every held key and button
    {"type":"status"}

    The shell's Helix Phone window streams "absolute" (with buttons) and "keysym" as the
    user moves, clicks, and types over the mirror; the rest is for chips and tests.
    """

    def __init__(self, mouse, absolute, keyboard, consumer, bonded):
        self.mouse = mouse
        self.absolute = absolute
        self.keyboard = keyboard
        self.consumer = consumer
        self.bonded = bonded
        self.keys = KeyboardState(self._emit)
        self.buttons = 0
        self.lock = threading.Lock()
        # Coalesced live pointer stream from the mirror window: one entry per run of
        # identical button state, holding the latest position and the summed wheel travel.
        self.pointer_queue = []
        self.pointer_scheduled = False
        self.pointer_lock = threading.Lock()
        self.thread = threading.Thread(target=self._serve, name="helix-phone-control", daemon=True)

    def start(self):
        self.thread.start()

    def _emit(self, characteristic, payload, delay_ms=0):
        done = threading.Event()

        def emit():
            characteristic.notify_value(payload)
            done.set()
            return False

        if delay_ms:
            GLib.timeout_add(delay_ms, emit)
        else:
            GLib.idle_add(emit)
        done.wait(1.0)

    def _absolute(self, x, y, buttons=0):
        self.buttons = buttons & 0x07
        px = max(0, min(ABSOLUTE_MAX, int(float(x) * ABSOLUTE_MAX)))
        py = max(0, min(ABSOLUTE_MAX, int(float(y) * ABSOLUTE_MAX)))
        self._emit(self.absolute, bytes((buttons & 0x07, px & 0xFF, px >> 8, py & 0xFF, py >> 8)))

    def _globe(self, usage):
        # Hold Globe while pressing a keyboard usage.
        self._emit(self.consumer, bytes((CONSUMER_GLOBE & 0xFF, CONSUMER_GLOBE >> 8)))
        self._emit(self.keyboard, bytes([0, 0, usage, 0, 0, 0, 0, 0]), 40)
        self._emit(self.keyboard, bytes(8), 40)
        self._emit(self.consumer, b"\x00\x00", 40)

    def _key(self, modifier, usage, hold_ms=30):
        self._emit(self.keyboard, bytes([modifier, 0, usage, 0, 0, 0, 0, 0]))
        if hold_ms > 30:
            # Release the key but keep the modifier held, so iOS shows the switcher.
            self._emit(self.keyboard, bytes([modifier, 0, 0, 0, 0, 0, 0, 0]), 30)
        self._emit(self.keyboard, bytes(8), hold_ms)

    def _queue_pointer(self, message):
        buttons = int(message.get("buttons", self.buttons)) & 0x07
        has_point = "x" in message and "y" in message
        point = (float(message["x"]), float(message["y"])) if has_point else None
        scroll = int(message.get("scroll", 0))
        with self.pointer_lock:
            if self.pointer_queue and self.pointer_queue[-1]["buttons"] == buttons:
                last = self.pointer_queue[-1]
                if point is not None:
                    last["point"] = point
                last["scroll"] += scroll
            else:
                self.pointer_queue.append({"buttons": buttons, "point": point, "scroll": scroll})
            schedule = not self.pointer_scheduled
            self.pointer_scheduled = True
        if schedule:
            GLib.idle_add(self._flush_pointer, priority=GLib.PRIORITY_HIGH)

    def _flush_pointer(self):
        with self.pointer_lock:
            queue, self.pointer_queue = self.pointer_queue, []
            self.pointer_scheduled = False
        scroll = 0
        last_point = None
        for entry in queue:
            point = entry["point"] or last_point
            scroll += entry["scroll"]
            if point is None:
                if entry["buttons"] != self.buttons:
                    # Button edge without a position: reuse the phone's current spot.
                    self.absolute.notify_value(bytes((entry["buttons"], *self.absolute.value[1:5])))
                    self.buttons = entry["buttons"]
                continue
            last_point = point
            px = max(0, min(ABSOLUTE_MAX, int(point[0] * ABSOLUTE_MAX)))
            py = max(0, min(ABSOLUTE_MAX, int(point[1] * ABSOLUTE_MAX)))
            self.absolute.notify_value(bytes((entry["buttons"], px & 0xFF, px >> 8, py & 0xFF, py >> 8)))
            self.buttons = entry["buttons"]
        if scroll:
            self.mouse.notify_value(bytes((0, 0, 0, max(-127, min(127, scroll)) & 0xFF)))
        return False

    def handle(self, message):
        kind = message.get("type")
        if kind == "absolute":
            self._absolute(message.get("x", 0.5), message.get("y", 0.5), int(message.get("buttons", 0)))
        elif kind == "tap":
            self._absolute(message["x"], message["y"], 0)
            self._absolute(message["x"], message["y"], 1)
            self._absolute(message["x"], message["y"], 0)
        elif kind == "drag":
            # Press at (x0,y0), move to (x1,y1) over `steps`, hold, release.
            x0, y0, x1, y1 = (float(message[k]) for k in ("x0", "y0", "x1", "y1"))
            steps = max(1, int(message.get("steps", 12)))
            self._absolute(x0, y0, 0)
            self._absolute(x0, y0, 1)
            for i in range(1, steps + 1):
                t = i / steps
                self._absolute(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 1)
                time.sleep(float(message.get("step_ms", 16)) / 1000)
            time.sleep(float(message.get("hold_ms", 0)) / 1000)
            self._absolute(x1, y1, 0)
        elif kind == "scroll":
            self._emit(self.mouse, bytes((0, 0, 0, int(message.get("amount", 0)) & 0xFF)))
        elif kind == "key":
            self._key(int(message.get("modifier", 0)), int(message["usage"]), int(message.get("hold_ms", 30)))
        elif kind == "globe":
            self._globe(int(message["usage"]))
        elif kind == "shortcut":
            channel, usage = SHORTCUTS[message["name"]]
            if channel == "globe":
                self._globe(usage)
            else:
                self._emit(self.consumer, bytes((usage & 0xFF, usage >> 8)))
                self._emit(self.consumer, b"\x00\x00", 80)
        elif kind == "text":
            for char in str(message.get("value", "")):
                encoded = encode_char(char)
                if encoded:
                    self._key(*encoded)
        elif kind == "consumer":
            usage = {"home": CONSUMER_HOME, "search": CONSUMER_SEARCH}[message.get("name", "home")]
            self._emit(self.consumer, bytes((usage & 0xFF, usage >> 8)))
            self._emit(self.consumer, b"\x00\x00", 80)
        elif kind == "keysym":
            self.keys.key(
                self.keyboard, self.consumer, int(message["keysym"]), bool(message.get("pressed", True)),
                int(message.get("modifiers", 0)),
            )
        elif kind == "release":
            self.keys.release(self.keyboard)
            if self.buttons:
                self.buttons = 0
                self._emit(self.absolute, bytes(5))
        elif kind == "status":
            missing = bluetooth_conf_missing()
            bonded = self.bonded.paired()
            return {
                "connected": self.absolute.notifying,
                "bonded": bool(bonded),
                "bonded_devices": bonded,
                "keyboard_captured": self.keys.active,
                "pointer_inside": self.buttons != 0,
                "bluetooth_config_ok": not missing,
                "bluetooth_config_missing": missing,
                "reports": {
                    "absolute": self.absolute.notifying,
                    "mouse": self.mouse.notifying,
                    "keyboard": self.keyboard.notifying,
                    "consumer": self.consumer.notifying,
                },
            }
        else:
            return {"error": f"unknown type {kind!r}"}
        return {"ok": True}

    def _serve(self):
        try:
            os.unlink(SOCKET_PATH)
        except FileNotFoundError:
            pass
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(SOCKET_PATH)
        os.chmod(SOCKET_PATH, 0o600)
        server.listen(4)
        print(f"control socket at {SOCKET_PATH}", flush=True)
        # The shell keeps one connection open for live pointer/keyboard traffic while
        # status polls and chips arrive on short-lived ones, so each gets its own thread.
        while True:
            conn, _ = server.accept()
            threading.Thread(target=self._client, args=(conn,), name="helix-phone-client", daemon=True).start()

    def _client(self, conn):
        with conn, conn.makefile("rwb", buffering=0) as stream:
            for line in stream:
                try:
                    message = json.loads(line)
                    if message.get("type") == "pointer":
                        # Live stream: never waits on the main loop and never replies.
                        self._queue_pointer(message)
                        continue
                    with self.lock:
                        response = self.handle(message)
                except (KeyError, TypeError, ValueError) as error:
                    response = {"error": str(error)}
                try:
                    stream.write((json.dumps(response) + "\n").encode())
                except (BrokenPipeError, ConnectionResetError):
                    return


def main() -> int:
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    app = Application(bus)

    hid = Service(bus, 0, HID_UUID)
    info = Characteristic(bus, 0, "00002a4a-0000-1000-8000-00805f9b34fb", ["read"], hid, b"\x11\x01\x00\x02")
    report_map = Characteristic(bus, 1, "00002a4b-0000-1000-8000-00805f9b34fb", ["read"], hid, REPORT_MAP)
    protocol = Characteristic(bus, 2, "00002a4e-0000-1000-8000-00805f9b34fb", ["read", "write-without-response"], hid, b"\x01")
    control = Characteristic(bus, 3, "00002a4c-0000-1000-8000-00805f9b34fb", ["write-without-response"], hid, b"")
    report_uuid = "00002a4d-0000-1000-8000-00805f9b34fb"
    reference_uuid = "00002908-0000-1000-8000-00805f9b34fb"
    mouse = Characteristic(bus, 4, report_uuid, ["read", "notify"], hid, b"\x00\x00\x00\x00")
    mouse.add(Descriptor(bus, 0, reference_uuid, ["read"], mouse, b"\x01\x01"))
    keyboard = Characteristic(bus, 5, report_uuid, ["read", "notify"], hid, bytes(8))
    keyboard.add(Descriptor(bus, 0, reference_uuid, ["read"], keyboard, b"\x02\x01"))
    consumer = Characteristic(bus, 6, report_uuid, ["read", "notify"], hid, b"\x00\x00")
    consumer.add(Descriptor(bus, 0, reference_uuid, ["read"], consumer, b"\x03\x01"))
    absolute = Characteristic(bus, 7, report_uuid, ["read", "notify"], hid, bytes(5))
    absolute.add(Descriptor(bus, 0, reference_uuid, ["read"], absolute, b"\x04\x01"))
    absolute.label = "absolute pointer"
    for char in (info, report_map, protocol, control, mouse, keyboard, consumer, absolute):
        hid.add(char)

    battery = Service(bus, 1, BATTERY_UUID)
    battery.add(Characteristic(bus, 0, "00002a19-0000-1000-8000-00805f9b34fb", ["read"], battery, b"\x64"))

    device_info = Service(bus, 2, INFO_UUID)
    device_info.add(Characteristic(bus, 0, "00002a29-0000-1000-8000-00805f9b34fb", ["read"], device_info, b"Helix"))
    # USB VID 0x1D6B (Linux Foundation), PID 0x0001.
    device_info.add(Characteristic(bus, 1, "00002a50-0000-1000-8000-00805f9b34fb", ["read"], device_info, b"\x02\x6b\x1d\x01\x00\x01\x00"))

    app.add(hid)
    app.add(battery)
    app.add(device_info)
    Advertisement(bus)
    Agent(bus)

    adapter = dbus.Interface(bus.get_object("org.bluez", ADAPTER), "org.bluez.GattManager1")
    advertiser = dbus.Interface(bus.get_object("org.bluez", ADAPTER), "org.bluez.LEAdvertisingManager1")
    manager = dbus.Interface(bus.get_object("org.bluez", "/org/bluez"), "org.bluez.AgentManager1")
    adapter_props = dbus.Interface(bus.get_object("org.bluez", ADAPTER), "org.freedesktop.DBus.Properties")
    adapter_props.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(True))
    adapter_props.Set("org.bluez.Adapter1", "Pairable", dbus.Boolean(True))
    # The iPhone already knows this radio as the laptop. Leaving the adapter
    # discoverable puts "fedora" in the normal Bluetooth list beside it.
    adapter_props.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(False))
    previous_alias = str(adapter_props.Get("org.bluez.Adapter1", "Alias"))
    adapter_props.Set("org.bluez.Adapter1", "Alias", dbus.String(NAME))

    loop = GLib.MainLoop()

    def shutdown():
        try:
            adapter_props.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(False))
            adapter_props.Set("org.bluez.Adapter1", "Alias", dbus.String(previous_alias))
        except dbus.exceptions.DBusException:
            pass
        loop.quit()
        return False

    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, shutdown)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGINT, shutdown)

    def registered_app():
        print(f"GATT HID ready, advertising {NAME}", flush=True)

    def failed(error):
        print(f"Bluetooth HID failed: {error}", flush=True)
        loop.quit()

    adapter.RegisterApplication(
        APP_PATH, {},
        reply_handler=registered_app,
        error_handler=failed,
    )
    advertiser.RegisterAdvertisement(
        AD_PATH, {},
        reply_handler=lambda: print("advertisement registered", flush=True),
        error_handler=failed,
    )
    def claim_agent():
        try:
            manager.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
        except dbus.exceptions.DBusException:
            pass
        try:
            manager.RequestDefaultAgent(AGENT_PATH)
        except dbus.exceptions.DBusException as error:
            print(f"pairing agent not registered: {error}", flush=True)
        return True

    claim_agent()
    # The shell registers its own agent at startup and would otherwise force a
    # numeric comparison, which AssistiveTouch rejects.
    GLib.timeout_add_seconds(3, claim_agent)

    missing_conf = bluetooth_conf_missing()
    if missing_conf:
        print("bluetooth config needs: " + "; ".join(missing_conf) + f" (in {BLUETOOTH_CONF})", flush=True)
    ControlSocket(mouse, absolute, keyboard, consumer, BondedDevices(bus)).start()
    print(f"{NAME} is waiting for the iPhone", flush=True)
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
