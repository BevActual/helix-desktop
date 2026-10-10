//! Ensures the long-lived exclusive-zone process is up so the bar does not hold the strut itself.

use std::process::Stdio;
use std::time::Duration;

pub fn strut_unit_active() -> bool {
    std::process::Command::new("systemctl")
        .args(["--user", "is-active", "--quiet", "helix-shell-strut.service"])
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn strut_unit_installed() -> bool {
    std::process::Command::new("systemctl")
        .args(["--user", "cat", "helix-shell-strut.service"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn start_strut_unit() -> bool {
    std::process::Command::new("systemctl")
        .args(["--user", "start", "helix-shell-strut.service"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

/// For the bar role: prefer the external strut unit. Returns true when the bar must
/// **not** call [`crate::layer::reserve_bar_space`] (strut is up or was started).
/// Returns false when the bar should fall back to an in-process reserve (dev / no unit).
pub fn prefer_external_strut() -> bool {
    if strut_unit_active() {
        return true;
    }
    if !strut_unit_installed() {
        eprintln!("STRUT unit missing — bar will reserve in-process (dev fallback)");
        return false;
    }
    if !start_strut_unit() {
        eprintln!("STRUT systemctl start failed — bar will reserve in-process");
        return false;
    }
    for _ in 0..40 {
        if strut_unit_active() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    eprintln!("STRUT unit did not become active — bar will reserve in-process");
    false
}
