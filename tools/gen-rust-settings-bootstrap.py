#!/usr/bin/env python3
"""Generate settings_bootstrap_extra.rs from src/desktop/settings_catalog.cpp."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/desktop/settings_catalog.cpp"
RUST_SETTINGS = ROOT / "core/helix-shell-core/src/desktop/settings.rs"
KEYBINDS = ROOT / "core/helix-shell-core/src/desktop/keybinds.rs"
OUT = ROOT / "core/helix-shell-core/src/desktop/settings_bootstrap_extra.rs"

ORDERED_TYPES = {
    "key-chords",
    "shortcut-list",
    "notification-rules",
    "application-list",
    "widget-list",
    "display-list",
    "tab-list",
    "template-list",
    "transition-list",
}

TYPE_TO_KIND = {
    "boolean": "Boolean",
    "number": "Number",
    "integer": "Integer",
    "string": "String",
    "mode": "Mode",
    "material": "Material",
    "shell-layer": "ShellLayer",
    "screen-position": "ScreenPosition",
    "orientation": "Orientation",
    "position": "Position",
    "dock-launcher": "DockLauncher",
    "hot-corner-action": "HotCornerAction",
    "wallpaper-fit": "WallpaperFit",
    "wallpaper-source": "WallpaperSource",
    "wallpaper-order": "WallpaperOrder",
    "daily-provider": "DailyProvider",
    "video-pause": "VideoPause",
    "clipboard-paste": "ClipboardPaste",
    "password-mask": "PasswordMask",
    "panel-placement": "PanelPlacement",
    "panel-position": "PanelPosition",
    "shell-mode": "ShellMode",
    "wallpaper-scheme": "WallpaperScheme",
    "niri-overview": "NiriOverview",
    "color": "Color",
    "directory": "Directory",
    "weather-unit": "WeatherUnit",
    "shadow-direction": "ShadowDirection",
    "sidebar-mode": "SidebarMode",
    "storage-key": "StorageKey",
    "wifi-band": "WifiBand",
    "language": "String",
    "bar-anchor": "BarAnchor",
}


def parse_cpp_settings(text: str) -> list[dict]:
    start = text.index("constexpr Setting settings[] = {")
    end = text.index("};", start)
    block = text[start:end]
    pattern = re.compile(
        r'\{"([^"]+)",\s*"([^"]+)",\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]+)"(?:,\s*(true|false))?',
    )
    out: list[dict] = []
    for m in pattern.finditer(block):
        sid, page, title, group, typ = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        desktop = m.group(6) != "false"
        tail = block[m.end() : m.end() + 80]
        minimum, maximum = 0.0, 0.0
        nums = re.match(r"\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)", tail)
        if nums:
            minimum, maximum = float(nums.group(1)), float(nums.group(2))
        out.append(
            {
                "id": sid,
                "page": page,
                "title": title.replace('"', '\\"'),
                "group": group.replace('"', '\\"'),
                "type": typ,
                "desktop": desktop,
                "minimum": minimum,
                "maximum": maximum,
            }
        )
    return out


def existing_ids() -> set[str]:
    rust = RUST_SETTINGS.read_text()
    ids = set(re.findall(r'id: "([^"]+)"', rust))
    ids |= {x.strip('"') for x in re.findall(r'"keybinds\.[^"]+"', KEYBINDS.read_text())}
    return ids


def default_for(entry: dict) -> str:
    typ = entry["type"]
    if typ == "boolean":
        if entry["id"].endswith("_enabled") or entry["id"].endswith(".enabled"):
            return "DefaultValue::Bool(true)" if "wizard" not in entry["id"] else "DefaultValue::Bool(true)"
        return "DefaultValue::Bool(false)"
    if typ in ("number", "integer"):
        if entry["minimum"] != 0.0 or entry["maximum"] != 0.0:
            mid = entry["minimum"] if entry["minimum"] != 0.0 else entry["maximum"]
            if entry["id"] == "idle.pre_action_fade_seconds":
                return "DefaultValue::Number(2.0)"
            if entry["id"] == "shell.session.grid_columns":
                return "DefaultValue::Number(3.0)"
            if entry["id"] == "calendar.refresh_minutes":
                return "DefaultValue::Number(15.0)"
            if entry["id"] == "weather.refresh_minutes":
                return "DefaultValue::Number(30.0)"
            if entry["id"] == "tailscale.refresh_interval_seconds":
                return "DefaultValue::Number(60.0)"
            if entry["id"] == "shell.screen_corners.size":
                return "DefaultValue::Number(12.0)"
            if entry["id"] == "shell.screen_record.replay_duration":
                return "DefaultValue::Number(30.0)"
            return f"DefaultValue::Number({mid})"
        return "DefaultValue::Number(0.0)"
    if typ == "storage-key":
        return 'DefaultValue::Str("secret-service")'
    if typ == "weather-unit":
        return 'DefaultValue::Str("metric")'
    if typ == "shadow-direction":
        return 'DefaultValue::Str("down")'
    if typ == "sidebar-mode":
        return 'DefaultValue::Str("full")'
    if typ == "wifi-band":
        return 'DefaultValue::Str("all")'
    if typ == "shell.launcher.provider_prefix":
        return 'DefaultValue::Str("/")'
    return 'DefaultValue::Str("")'


def emit_def(entry: dict) -> str:
    kind = TYPE_TO_KIND.get(entry["type"])
    if not kind:
        return ""
    default = default_for(entry)
    desktop = "true" if entry["desktop"] else "false"
    return f"""    SettingDef {{
        id: "{entry['id']}",
        page: "{entry['page']}",
        title: "{entry['title']}",
        group: "{entry['group']}",
        kind: SettingKind::{kind},
        desktop: {desktop},
        minimum: {entry['minimum']},
        maximum: {entry['maximum']},
        default: {default},
    }},
"""


def render_extra(missing_scalars: list[dict]) -> str:
    lines = [
        "// Generated by tools/gen-rust-settings-bootstrap.py — do not edit by hand.",
        "use super::{DefaultValue, SettingDef, SettingKind};",
        "",
        "pub const BOOTSTRAP_EXTRA: &[SettingDef] = &[",
    ]
    for entry in missing_scalars:
        chunk = emit_def(entry)
        if chunk:
            lines.append(chunk)
    lines.append("];")
    lines.append("")
    return "\n".join(lines)


def collect_missing_scalars() -> list[dict]:
    cpp = CPP.read_text()
    have = existing_ids()
    missing_scalars: list[dict] = []
    for entry in parse_cpp_settings(cpp):
        if entry["id"] in have:
            continue
        if entry["type"] in ORDERED_TYPES:
            continue
        if entry["type"] not in TYPE_TO_KIND:
            print(f"skip unsupported type {entry['type']} for {entry['id']}", file=sys.stderr)
            continue
        missing_scalars.append(entry)
    return missing_scalars


def main() -> int:
    missing_scalars = collect_missing_scalars()
    body = render_extra(missing_scalars)
    if "--check" in sys.argv:
        if not OUT.exists():
            print(f"missing {OUT}; run without --check to generate", file=sys.stderr)
            return 1
        if OUT.read_text() != body:
            print(
                f"{OUT} is out of date with {CPP.name}; run: python3 tools/gen-rust-settings-bootstrap.py",
                file=sys.stderr,
            )
            return 1
        print(f"settings bootstrap extra OK ({len(missing_scalars)} defs)")
        return 0
    OUT.write_text(body)
    print(f"wrote {len(missing_scalars)} defs to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
