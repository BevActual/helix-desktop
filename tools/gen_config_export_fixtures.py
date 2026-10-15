#!/usr/bin/env python3
"""Capture effective config export per parity fixture (Rust exporter default; C++ optional)."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "core/helix-config-export/tests/golden/export_fixtures"

CASES: list[tuple[str, str, str, dict[str, str] | None]] = [
    ("bar_thickness_override", "[bar.default]\nthickness = 40\n", "\n", None),
    ("legacy_negative_bar_radius", "[bar.default]\nradius = -16\n", "\n", None),
    ("legacy_sysmon_show_icon", "[widget.cpu]\nshow_icon = false\n", "\n", None),
    ("legacy_workspaces_widget", '[widget.workspaces]\ndisplay = "none"\n', "\n", None),
    (
        "custom_named_sysmon_widget",
        '[widget.my-cpu]\ntype = "sysmon"\nstat = "cpu_usage"\n',
        "\n",
        None,
    ),
    (
        "legacy_location_schedule",
        "[location]\nsunset = \"18:00\"\nsunrise = \"06:00\"\n",
        "\n",
        None,
    ),
    ("plugins_auto_update_bool", "[plugins]\nauto_update = false\n", "\n", None),
    ('theme_builtin_override', '[theme]\nbuiltin = "Qt"\n', "\n", None),
    ("wallpaper_runtime_state", '[wallpaper]\ndefault = "/tmp/w.png"\n', "\n", None),
    (
        "calendar_account",
        '[calendar.account.work]\ntype = "ics"\nserver_url = "https://caldav.example.com"\n',
        "\n",
        None,
    ),
    (
        "bar_monitor_override",
        "[bar.default.monitor.DP-1]\nthickness = 28\n",
        "\n",
        None,
    ),
    (
        "legacy_bar_dead_zone_command",
        '[bar.default.dead_zone]\ncommand = "notify-send hi"\n',
        "\n",
        None,
    ),
    ("desktop_widgets_disabled", "[desktop_widgets]\nenabled = false\n", "\n", None),
    (
        "desktop_widget_placement",
        '[desktop_widgets]\nwidget_order = ["clock_main"]\n\n[desktop_widgets.widget.clock_main]\ntype = "clock"\ncx = 960.0\ncy = 540.0\n\n[desktop_widgets.widget.clock_main.settings]\nformat = "{:%H:%M}"\n',
        "\n",
        None,
    ),
    (
        "lockscreen_widget_placement",
        '[lockscreen_widgets]\nwidget_order = ["clock_main"]\n\n[lockscreen_widgets.widget.clock_main]\ntype = "clock"\ncx = 100.0\ncy = 200.0\n\n[lockscreen_widgets.widget.clock_main.settings]\nformat = "{:%H:%M}"\n',
        "\n",
        None,
    ),
    (
        "bar_monitor_match_override",
        '[bar.default.monitor.DP-1]\nmatch = "DP-1"\nthickness = 28\n',
        "\n",
        None,
    ),
    (
        "two_desktop_widget_placements",
        '[desktop_widgets]\nwidget_order = ["a", "b"]\n\n[desktop_widgets.widget.a]\ntype = "clock"\n\n[desktop_widgets.widget.b]\ntype = "date"\n',
        "\n",
        None,
    ),
    (
        "idle_lock_behavior_only",
        '[idle.behavior.lock]\ntimeout = 300\nenabled = true\naction = "lock"\n',
        "\n",
        None,
    ),
    ("nightlight_enabled", "[nightlight]\nenabled = true\n", "\n", None),
    (
        "two_calendar_accounts",
        '[calendar.account.zed]\ntype = "ics"\nserver_url = "https://a"\n\n[calendar.account.alpha]\ntype = "ics"\nserver_url = "https://b"\n',
        "\n",
        None,
    ),
    (
        "placement_widget_table_sort_order",
        '[desktop_widgets]\nwidget_order = ["b", "a"]\n\n[desktop_widgets.widget.b]\ntype = "clock"\n\n[desktop_widgets.widget.a]\ntype = "date"\n',
        "\n",
        None,
    ),
    (
        "idle_behavior_order_override",
        '[idle]\nbehavior_order = ["screen-off", "lock"]\n\n[idle.behavior.screen-off]\ntimeout = 200\nenabled = true\naction = "screen_off"\n\n[idle.behavior.lock]\ntimeout = 100\nenabled = true\naction = "lock"\n',
        "\n",
        None,
    ),
    (
        "desktop_widget_flip_x",
        '[desktop_widgets]\nwidget_order = ["clock_main"]\n\n[desktop_widgets.widget.clock_main]\ntype = "clock"\nflip_x = true\n',
        "\n",
        None,
    ),
    ("bar_thickness_float_literal", "[bar.default]\nthickness = 40.0\n", "\n", None),
    ("theme_pure_black_dark", "[theme]\npure_black_dark = true\n", "\n", None),
    ("dock_disabled", "[dock]\nenabled = false\n", "\n", None),
    ("settings_theme_override", "", "[theme]\nmode = \"light\"\n", None),
    (
        "migrated_settings_sidecar",
        "",
        "config_version = 14\n[shell]\nlaunch_apps_as_systemd_services = false\n",
        None,
    ),
    ("notification_max_visible", "[notification]\nmax_visible = 5\n", "\n", None),
    ("shell_polkit_agent", "[shell]\npolkit_agent = true\n", "\n", None),
    (
        "bar_start_widgets",
        '[bar.default]\nstart = ["clock", "date"]\n',
        "\n",
        None,
    ),
    (
        "secondary_bar",
        "[bar.secondary]\nenabled = true\nthickness = 28\n",
        "\n",
        None,
    ),
    (
        "default_and_secondary_bars",
        "[bar.default]\nthickness = 40\n\n[bar.secondary]\nenabled = true\nthickness = 28\n",
        "\n",
        None,
    ),
    (
        "explicit_bar_order",
        '[bar]\norder = ["zed", "alpha"]\n\n[bar.zed]\nenabled = true\n\n[bar.alpha]\nenabled = true\n',
        "\n",
        None,
    ),
    (
        "desktop_config_smoke",
        '[bar.main]\nenabled = true\nthickness = 34\n\n[idle.behavior.lock]\ntimeout = 420\nenabled = true\naction = "lock"\n\n[theme.templates]\nbuiltin_ids = ["helix", "qt"]\n',
        "\n",
        None,
    ),
    (
        "minimal_fixture",
        '[theme]\nmode = "dark"\n\n[bar.default]\nenabled = true\nthickness = 34\n',
        "\n",
        None,
    ),
    (
        "launcher_providers",
        '[shell.launcher.providers.calculator]\nprefix = "calc"\nglobal = true\n\n[shell.launcher.providers.emoji]\nprefix = "emo"\n\n[shell.launcher.providers.session]\nprefix = "session"\nglobal = false\n',
        "\n",
        None,
    ),
    (
        "shell_panel_glass",
        "[shell.panel]\nglass = 0.25\n",
        "\n",
        None,
    ),
    ("hooks_empty_section", "[hooks]\n", "\n", None),
    ('light_theme', '[theme]\nmode = "light"\n', "\n", None),
    (
        "config_include",
        'include = { files = ["base.toml"] }\n\n[theme]\nmode = "light"\n',
        "\n",
        {"base.toml": '[theme]\nmode = "dark"\n'},
    ),
]


def find_bin() -> Path:
    for key in ("HELIX_RUST_CONFIG_EXPORT_BIN", "HELIX_CONFIG_EXPORT_BIN"):
        env = os.environ.get(key, "")
        if env:
            candidate = Path(env)
            if candidate.is_file():
                return candidate
    for candidate in (
        ROOT / "target/debug/helix-rust-config-export",
        ROOT / "target/fast/helix-rust-config-export",
    ):
        if candidate.is_file():
            return candidate
    sys.exit(
        "error: build helix-rust-config-export "
        "(cargo build -p helix-shell-core --bin helix-rust-config-export) "
        "or set HELIX_RUST_CONFIG_EXPORT_BIN"
    )


def export_case(bin_path: Path, config: str, settings: str, extra: dict[str, str] | None) -> str:
    tmp_cfg = Path(tempfile.mkdtemp())
    tmp_state = Path(tempfile.mkdtemp())
    try:
        helix_cfg = tmp_cfg / "helix"
        helix_state = tmp_state / "helix"
        helix_cfg.mkdir(parents=True)
        helix_state.mkdir(parents=True)
        (helix_cfg / "config.toml").write_text(config)
        (helix_state / "settings.toml").write_text(settings)
        if extra:
            for name, body in extra.items():
                (helix_cfg / name).write_text(body)
        env = os.environ.copy()
        env["XDG_CONFIG_HOME"] = str(tmp_cfg)
        env["XDG_STATE_HOME"] = str(tmp_state)
        env["HELIX_CONFIG_HOME"] = str(tmp_cfg)
        env["HELIX_STATE_HOME"] = str(tmp_state)
        result = subprocess.run(
            [str(bin_path)],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr or "export failed")
        return result.stdout
    finally:
        shutil.rmtree(tmp_cfg, ignore_errors=True)
        shutil.rmtree(tmp_state, ignore_errors=True)


def main() -> None:
    bin_path = find_bin()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for case_id, config, settings, extra in CASES:
        out = OUT_DIR / f"{case_id}.toml"
        out.write_text(export_case(bin_path, config, settings, extra))
        print(f"wrote {out.relative_to(ROOT)}")
    print(f"ok: {len(CASES)} fixtures")


if __name__ == "__main__":
    main()
