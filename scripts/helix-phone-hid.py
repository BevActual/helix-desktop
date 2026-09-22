#!/usr/bin/env python3
"""Advertise Helix as a BLE HID mouse so an iPhone can pair it in AssistiveTouch."""

import json
import os
import signal
import socket
import struct
import sys
import threading
import time

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

NAME = "Helix Phone"
VERBOSE = os.environ.get("HELIX_PHONE_VERBOSE", "") not in ("", "0")
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
        # Keep this inside a 31-byte legacy advert: flags (3) + HID 16-bit UUID (4) +
        # ANCS solicitation (18) = 25; the kernel moves appearance and name into the scan
        # response. A 128-bit *service* UUID would force extended advertising, which
        # AssistiveTouch does not list, but a solicitation UUID is what tells iOS this
        # accessory consumes notifications, and it is the only way the phone offers
        # "Share System Notifications" for the bond. 0x03C0 is Generic HID, the
        # appearance iOS scans for.
        print(f"advertising {NAME} as a HID mouse", flush=True)
        return {
            "Type": dbus.String("peripheral"),
            "ServiceUUIDs": dbus.Array(["1812"], signature="s"),
            "SolicitUUIDs": dbus.Array([ANCS_SERVICE_UUID], signature="s"),
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


ANCS_SERVICE_UUID = "7905f431-b5ce-4e99-a40f-4b1e122d00d0"
ANCS_NOTIFICATION_SOURCE = "9fbf120d-6301-42d9-8c58-25e699a21dbd"
ANCS_CONTROL_POINT = "69d1d8f3-45e1-49a8-9821-9bbdfdaad9d9"
ANCS_DATA_SOURCE = "22eac6e9-24d6-4bb5-be44-b36ace7c7bfb"

ANCS_EVENT_ADDED, ANCS_EVENT_MODIFIED, ANCS_EVENT_REMOVED = 0, 1, 2
ANCS_FLAG_SILENT, ANCS_FLAG_IMPORTANT, ANCS_FLAG_PREEXISTING = 0x01, 0x02, 0x04
ANCS_FLAG_POSITIVE_ACTION, ANCS_FLAG_NEGATIVE_ACTION = 0x08, 0x10
ANCS_CMD_NOTIFICATION_ATTRIBUTES, ANCS_CMD_APP_ATTRIBUTES, ANCS_CMD_PERFORM_ACTION = 0, 1, 2
ANCS_ATTR_APP_ID, ANCS_ATTR_TITLE, ANCS_ATTR_SUBTITLE, ANCS_ATTR_MESSAGE = 0, 1, 2, 3
ANCS_ATTR_DATE, ANCS_ATTR_POSITIVE_LABEL, ANCS_ATTR_NEGATIVE_LABEL = 5, 6, 7
ANCS_ACTION_POSITIVE, ANCS_ACTION_NEGATIVE = 0, 1

# CategoryID -> (freedesktop category hint, icon name). Icons come from the desktop icon
# theme so the toast reads like any other desktop notification, not a phone screenshot.
ANCS_CATEGORIES = {
    1: ("call.incoming", "call-start-symbolic"),
    2: ("call.missed", "call-missed-symbolic"),
    3: ("call.voicemail", "media-record-symbolic"),
    4: ("im.received", "user-available-symbolic"),
    5: ("schedule", "x-office-calendar-symbolic"),
    6: ("email.arrived", "mail-unread-symbolic"),
    7: ("news", "application-rss+xml-symbolic"),
    8: ("health", "emblem-favorite-symbolic"),
    9: ("finance", "emblem-money-symbolic"),
    10: ("location", "mark-location-symbolic"),
    11: ("entertainment", "applications-multimedia-symbolic"),
}
ANCS_DEFAULT_ICON = "phone-symbolic"

NOTIFICATIONS_BUS = "org.freedesktop.Notifications"
NOTIFICATIONS_PATH = "/org/freedesktop/Notifications"


class AncsClient:
    """Mirror iPhone notifications onto the desktop through Apple Notification Center Service.

    The iPhone exposes ANCS on the same encrypted link AssistiveTouch uses for the HID
    pointer, so once it is paired BlueZ already has the remote GATT database under the
    device path. We subscribe to Notification Source and Data Source, fetch attributes
    over Control Point one command at a time (ANCS answers strictly in order), and post
    each notification to org.freedesktop.Notifications, which the shell serves. Actions on
    the toast go back as PerformNotificationAction; dismissing it performs the negative
    action (Clear) so the phone stays in step, the way KDE Connect does for Android.

    Everything here runs on the GLib main loop; control-socket threads only read the
    small status dict.
    """

    REQUEST_TIMEOUT_MS = 4000

    def __init__(self, bus, bonded):
        self.bus = bus
        self.bonded = bonded
        self.enabled = True
        self.session = None
        self.device_path = None
        self.device_name = ""
        self.chars = {}
        self.attached = False
        self.queue = []
        self.inflight = None
        self.timeout_id = None
        self.buffer = bytearray()
        self.app_names = {}
        self.pending = {}          # uid -> parsed notification waiting on an app name
        self.notifications = {}    # uid -> {"id": desktop id, "flags": .., "app": ..}
        self.desktop_ids = {}      # desktop id -> uid
        self.posted = 0
        self.attached_at = 0.0
        self.status_lock = threading.Lock()
        bus.add_signal_receiver(self._device_changed, dbus_interface="org.freedesktop.DBus.Properties",
                                signal_name="PropertiesChanged", path_keyword="path")
        GLib.idle_add(self._scan)

    # ── status / config ────────────────────────────────────────────────────────

    def set_enabled(self, enabled):
        enabled = bool(enabled)
        if enabled == self.enabled:
            return
        self.enabled = enabled
        print(f"ancs {'enabled' if enabled else 'disabled'}", flush=True)
        if not enabled:
            GLib.idle_add(self._close_all)

    def status(self):
        with self.status_lock:
            return {
                "enabled": self.enabled,
                "attached": self.attached,
                "device": self.device_name,
                "active": len(self.notifications),
                "posted": self.posted,
            }

    # ── attach / detach ────────────────────────────────────────────────────────

    def _scan(self):
        try:
            manager = dbus.Interface(self.bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
            objects = manager.GetManagedObjects()
        except dbus.exceptions.DBusException:
            return False
        for path, interfaces in objects.items():
            device = interfaces.get("org.bluez.Device1")
            if device is None or not str(path).startswith(ADAPTER):
                continue
            if bool(device.get("Connected", False)) and bool(device.get("ServicesResolved", False)):
                self._attach(str(path), objects)
                if self.attached:
                    break
        return False

    def _device_changed(self, interface, changed, invalidated, path=None):
        if path is None:
            return
        if interface == "org.bluez.GattCharacteristic1":
            if "Value" in changed:
                self.on_value(str(path), bytes(changed["Value"]))
            return
        if interface != "org.bluez.Device1":
            return
        path = str(path)
        if not path.startswith(ADAPTER):
            return
        if "ServicesResolved" in changed and bool(changed["ServicesResolved"]):
            GLib.idle_add(self._attach, path, None)
        elif path == self.device_path:
            if ("Connected" in changed and not bool(changed["Connected"])) or (
                    "ServicesResolved" in changed and not bool(changed["ServicesResolved"])):
                self._detach()

    def _attach(self, device_path, objects):
        if self.attached and self.device_path == device_path:
            return False
        if objects is None:
            try:
                manager = dbus.Interface(self.bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
                objects = manager.GetManagedObjects()
            except dbus.exceptions.DBusException:
                return False
        chars = {}
        for path, interfaces in objects.items():
            char = interfaces.get("org.bluez.GattCharacteristic1")
            if char is None or not str(path).startswith(device_path + "/"):
                continue
            uuid = str(char.get("UUID", "")).lower()
            if uuid in (ANCS_NOTIFICATION_SOURCE, ANCS_CONTROL_POINT, ANCS_DATA_SOURCE):
                chars[uuid] = str(path)
        if len(chars) != 3:
            return False
        if self.attached:
            self._detach()
        device = objects.get(device_path, {}).get("org.bluez.Device1", {})
        with self.status_lock:
            self.device_path = device_path
            self.device_name = str(device.get("Alias", device.get("Name", "iPhone")))
            self.chars = chars
        # Data Source first so the reply channel is open before any event can arrive.
        self._start_notify(chars[ANCS_DATA_SOURCE], lambda: self._start_notify(
            chars[ANCS_NOTIFICATION_SOURCE], self._attached))
        return False

    def _start_notify(self, path, then):
        def ok():
            then()

        def failed(error):
            text = str(error)
            if "Already notifying" in text or "InProgress" in text:
                then()
                return
            print(f"ancs StartNotify failed on {path[-8:]}: {error}", flush=True)

        try:
            self._char(path).StartNotify(reply_handler=ok, error_handler=failed)
        except dbus.exceptions.DBusException as error:
            failed(error)

    def _attached(self):
        with self.status_lock:
            self.attached = True
        self.attached_at = time.time()
        print(f"ancs attached to {self.device_name}", flush=True)

    def _detach(self):
        if self.device_path is None:
            return
        print(f"ancs detached from {self.device_name}", flush=True)
        with self.status_lock:
            self.attached = False
            self.device_path = None
            self.chars = {}
        self._cancel_timeout()
        self.queue.clear()
        self.inflight = None
        self.buffer.clear()
        self.pending.clear()
        self._close_all()

    def _char(self, path):
        return dbus.Interface(self.bus.get_object("org.bluez", path), "org.bluez.GattCharacteristic1")

    # ── GATT traffic ───────────────────────────────────────────────────────────

    def on_value(self, path, data):
        """Called from the shared PropertiesChanged receiver for GattCharacteristic1 Value."""
        if path == self.chars.get(ANCS_NOTIFICATION_SOURCE):
            self._on_notification_source(data)
        elif path == self.chars.get(ANCS_DATA_SOURCE):
            self._on_data_source(data)

    def _on_notification_source(self, data):
        if len(data) < 8:
            return
        event, flags, category, count, uid = struct.unpack("<BBBBI", data[:8])
        if event == ANCS_EVENT_REMOVED:
            self._close(uid, from_phone=True)
            return
        if not self.enabled:
            return
        if flags & ANCS_FLAG_PREEXISTING and event == ANCS_EVENT_ADDED:
            # Already on the phone before we connected; the user has seen these.
            return
        if VERBOSE:
            print(f"ancs event={event} flags={flags:#04x} category={category} uid={uid}", flush=True)
        entry = self.notifications.setdefault(uid, {"id": 0})
        entry.update({"flags": flags, "category": category, "count": count})
        self._enqueue({
            "cmd": ANCS_CMD_NOTIFICATION_ATTRIBUTES,
            "uid": uid,
            "attrs": [ANCS_ATTR_APP_ID, ANCS_ATTR_TITLE, ANCS_ATTR_SUBTITLE, ANCS_ATTR_MESSAGE,
                      ANCS_ATTR_DATE, ANCS_ATTR_POSITIVE_LABEL, ANCS_ATTR_NEGATIVE_LABEL],
        })

    def _enqueue(self, request):
        self.queue.append(request)
        self._pump()

    def _pump(self):
        if self.inflight is not None or not self.queue or not self.attached:
            return
        request = self.queue.pop(0)
        payload = bytearray([request["cmd"]])
        if request["cmd"] == ANCS_CMD_NOTIFICATION_ATTRIBUTES:
            payload += struct.pack("<I", request["uid"])
            for attr in request["attrs"]:
                payload.append(attr)
                if attr in (ANCS_ATTR_TITLE, ANCS_ATTR_SUBTITLE):
                    payload += struct.pack("<H", 160)
                elif attr == ANCS_ATTR_MESSAGE:
                    payload += struct.pack("<H", 1024)
        elif request["cmd"] == ANCS_CMD_APP_ATTRIBUTES:
            payload += request["app"].encode("utf-8") + b"\x00" + bytes([0])
        elif request["cmd"] == ANCS_CMD_PERFORM_ACTION:
            payload += struct.pack("<IB", request["uid"], request["action"])
        self.inflight = request
        self.buffer.clear()

        def failed(error):
            print(f"ancs control point write failed: {error}", flush=True)
            if self.inflight is request:
                self.inflight = None
                self._cancel_timeout()
                self._pump()

        def wrote():
            if request["cmd"] == ANCS_CMD_PERFORM_ACTION and self.inflight is request:
                # No Data Source reply for actions.
                self.inflight = None
                self._cancel_timeout()
                self._pump()

        try:
            self._char(self.chars[ANCS_CONTROL_POINT]).WriteValue(
                dbus.ByteArray(bytes(payload)), {"type": "request"}, reply_handler=wrote, error_handler=failed)
        except (dbus.exceptions.DBusException, KeyError) as error:
            failed(error)
            return
        self._cancel_timeout()
        self.timeout_id = GLib.timeout_add(self.REQUEST_TIMEOUT_MS, self._timed_out, request)

    def _timed_out(self, request):
        self.timeout_id = None
        if self.inflight is request:
            print(f"ancs request {request['cmd']} timed out", flush=True)
            self.inflight = None
            self.buffer.clear()
            self._pump()
        return False

    def _cancel_timeout(self):
        if self.timeout_id is not None:
            GLib.source_remove(self.timeout_id)
            self.timeout_id = None

    def _on_data_source(self, data):
        request = self.inflight
        if request is None:
            return
        self.buffer.extend(data)
        buf = self.buffer
        if not buf or buf[0] != request["cmd"]:
            return
        if request["cmd"] == ANCS_CMD_NOTIFICATION_ATTRIBUTES:
            if len(buf) < 5:
                return
            offset = 5
            expected = len(request["attrs"])
        else:
            end = buf.find(b"\x00", 1)
            if end < 0:
                return
            offset = end + 1
            expected = 1
        attrs = {}
        while len(attrs) < expected:
            if offset + 3 > len(buf):
                return
            attr_id = buf[offset]
            length = struct.unpack_from("<H", buf, offset + 1)[0]
            if offset + 3 + length > len(buf):
                return
            attrs[attr_id] = bytes(buf[offset + 3:offset + 3 + length]).decode("utf-8", "replace")
            offset += 3 + length
        self.inflight = None
        self._cancel_timeout()
        self.buffer.clear()
        if request["cmd"] == ANCS_CMD_NOTIFICATION_ATTRIBUTES:
            self._got_notification(request["uid"], attrs)
        else:
            self._got_app_name(request["app"], attrs.get(0, ""))
        self._pump()

    # ── desktop notifications ──────────────────────────────────────────────────

    def _got_notification(self, uid, attrs):
        entry = self.notifications.get(uid)
        if entry is None:
            return
        if not entry.get("id") and self._dated_before_attach(attrs.get(ANCS_ATTR_DATE, "")):
            # iOS replays Notification Center on every subscribe and does not always set
            # PreExisting; anything dated before we attached has been on the phone already.
            self.notifications.pop(uid, None)
            return
        entry["attrs"] = attrs
        app = attrs.get(ANCS_ATTR_APP_ID, "")
        entry["app"] = app
        if app and app not in self.app_names:
            self.pending[uid] = True
            if not any(r["cmd"] == ANCS_CMD_APP_ATTRIBUTES and r["app"] == app for r in self.queue):
                self._enqueue({"cmd": ANCS_CMD_APP_ATTRIBUTES, "app": app})
            return
        self._post(uid)

    def _dated_before_attach(self, date):
        # ANCS dates are the phone's local time as yyyyMMdd'T'HHmmSS.
        try:
            stamp = time.mktime(time.strptime(date, "%Y%m%dT%H%M%S"))
        except (ValueError, OverflowError):
            return False
        return stamp < self.attached_at - 90

    def _got_app_name(self, app, name):
        self.app_names[app] = name or app.rsplit(".", 1)[-1].capitalize()
        for uid in [u for u, e in self.notifications.items() if e.get("app") == app and u in self.pending]:
            self.pending.pop(uid, None)
            self._post(uid)

    def _session_bus(self):
        if self.session is None:
            self.session = dbus.SessionBus()
            self.session.add_signal_receiver(self._action_invoked, dbus_interface=NOTIFICATIONS_BUS,
                                             signal_name="ActionInvoked")
            self.session.add_signal_receiver(self._notification_closed, dbus_interface=NOTIFICATIONS_BUS,
                                             signal_name="NotificationClosed")
        return dbus.Interface(self.session.get_object(NOTIFICATIONS_BUS, NOTIFICATIONS_PATH), NOTIFICATIONS_BUS)

    def _post(self, uid):
        entry = self.notifications.get(uid)
        if entry is None or "attrs" not in entry or not self.enabled:
            return
        attrs = entry["attrs"]
        flags = entry.get("flags", 0)
        app_name = self.app_names.get(entry.get("app", ""), self.device_name or "iPhone")
        title = attrs.get(ANCS_ATTR_TITLE, "").strip() or app_name
        body_parts = [attrs.get(ANCS_ATTR_SUBTITLE, "").strip(), attrs.get(ANCS_ATTR_MESSAGE, "").strip()]
        body = "\n".join(part for part in body_parts if part)
        category, icon = ANCS_CATEGORIES.get(entry.get("category", 0), ("", ANCS_DEFAULT_ICON))
        actions = []
        if flags & ANCS_FLAG_POSITIVE_ACTION and attrs.get(ANCS_ATTR_POSITIVE_LABEL):
            actions += ["positive", attrs[ANCS_ATTR_POSITIVE_LABEL]]
        if flags & ANCS_FLAG_NEGATIVE_ACTION and attrs.get(ANCS_ATTR_NEGATIVE_LABEL):
            actions += ["negative", attrs[ANCS_ATTR_NEGATIVE_LABEL]]
        hints = {
            "x-helix-phone": dbus.String(self.device_name or "iPhone"),
            "x-helix-phone-app": dbus.String(entry.get("app", "")),
            "urgency": dbus.Byte(2 if entry.get("category") == 1 else (1 if flags & ANCS_FLAG_IMPORTANT else 0)),
            "suppress-sound": dbus.Boolean(bool(flags & ANCS_FLAG_SILENT)),
        }
        if category:
            hints["category"] = dbus.String(category)

        def posted(desktop_id):
            desktop_id = int(desktop_id)
            current = self.notifications.get(uid)
            if current is None:
                # Removed on the phone while we were posting.
                self._close_desktop(desktop_id)
                return
            if current.get("id") and current["id"] != desktop_id:
                self.desktop_ids.pop(current["id"], None)
            current["id"] = desktop_id
            self.desktop_ids[desktop_id] = uid
            with self.status_lock:
                self.posted += 1

        def failed(error):
            print(f"ancs could not post notification: {error}", flush=True)

        try:
            self._session_bus().Notify(
                app_name, dbus.UInt32(entry.get("id", 0)), icon, title, body,
                dbus.Array(actions, signature="s"), dbus.Dictionary(hints, signature="sv"), dbus.Int32(-1),
                reply_handler=posted, error_handler=failed)
        except dbus.exceptions.DBusException as error:
            failed(error)

    def _action_invoked(self, desktop_id, action_key):
        uid = self.desktop_ids.get(int(desktop_id))
        if uid is None:
            return
        action = {"positive": ANCS_ACTION_POSITIVE, "negative": ANCS_ACTION_NEGATIVE}.get(str(action_key))
        if action is None:
            return
        self._enqueue({"cmd": ANCS_CMD_PERFORM_ACTION, "uid": uid, "action": action})

    def _notification_closed(self, desktop_id, reason):
        uid = self.desktop_ids.pop(int(desktop_id), None)
        if uid is None:
            return
        entry = self.notifications.pop(uid, None)
        self.pending.pop(uid, None)
        # Reason 2 is "dismissed by the user": clear it on the phone too.
        if int(reason) == 2 and entry is not None and entry.get("flags", 0) & ANCS_FLAG_NEGATIVE_ACTION:
            self._enqueue({"cmd": ANCS_CMD_PERFORM_ACTION, "uid": uid, "action": ANCS_ACTION_NEGATIVE})

    def _close(self, uid, from_phone=False):
        entry = self.notifications.pop(uid, None)
        self.pending.pop(uid, None)
        if entry is None:
            return
        desktop_id = entry.get("id", 0)
        if desktop_id:
            self.desktop_ids.pop(desktop_id, None)
            self._close_desktop(desktop_id)

    def _close_desktop(self, desktop_id):
        try:
            self._session_bus().CloseNotification(dbus.UInt32(desktop_id), reply_handler=lambda: None,
                                                  error_handler=lambda _e: None)
        except dbus.exceptions.DBusException:
            pass

    def _close_all(self):
        for uid in list(self.notifications):
            self._close(uid)
        return False


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
    {"type":"ancs","enabled":true}                    mirror iPhone notifications to the desktop
    {"type":"status"}

    The shell's Helix Phone window streams "absolute" (with buttons) and "keysym" as the
    user moves, clicks, and types over the mirror; the rest is for chips and tests.
    """

    def __init__(self, mouse, absolute, keyboard, consumer, bonded, ancs):
        self.mouse = mouse
        self.absolute = absolute
        self.keyboard = keyboard
        self.consumer = consumer
        self.bonded = bonded
        self.ancs = ancs
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
        elif kind == "ancs":
            self.ancs.set_enabled(message.get("enabled", True))
        elif kind == "status":
            missing = bluetooth_conf_missing()
            bonded = self.bonded.paired()
            return {
                "connected": self.absolute.notifying,
                "ancs": self.ancs.status(),
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
    bonded = BondedDevices(bus)
    ControlSocket(mouse, absolute, keyboard, consumer, bonded, AncsClient(bus, bonded)).start()
    print(f"{NAME} is waiting for the iPhone", flush=True)
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
