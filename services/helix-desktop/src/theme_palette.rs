//! Custom palette mutations via C++ `helix-theme-palette` (ThemeService parity).

use std::process::Command;

use crate::assets_path::helix_theme_palette_bin;

fn run(args: &[&str]) -> Result<Option<String>, String> {
    if let Ok(cmd) = std::env::var("HELIX_THEME_PALETTE_CMD") {
        if !cmd.trim().is_empty() {
            let status = Command::new("sh")
                .arg("-lc")
                .arg(&cmd)
                .status()
                .map_err(|error| error.to_string())?;
            if !status.success() {
                return Err("Palette could not be saved".into());
            }
            return Ok(None);
        }
    }

    let bin = helix_theme_palette_bin()
        .ok_or_else(|| "Palette could not be saved".to_string())?;
    let output = Command::new(&bin)
        .args(args)
        .output()
        .map_err(|error| error.to_string())?;
    if output.status.success() {
        let name = String::from_utf8_lossy(&output.stdout).trim().to_string();
        return Ok(if name.is_empty() { None } else { Some(name) });
    }
    let stderr = String::from_utf8_lossy(&output.stderr);
    let message = stderr.trim();
    if message.is_empty() {
        Err("Palette could not be saved".into())
    } else {
        Err(message.lines().last().unwrap_or(message).to_string())
    }
}

pub fn save_palette(source: &str) -> Result<String, String> {
    if source != "current" && source != "wallpaper" {
        return Err("Unknown palette source".into());
    }
    run(&["save-palette", source])?
        .ok_or_else(|| "Palette could not be saved".into())
}

pub fn set_role(role: u32, hex_color: &str) -> Result<String, String> {
    run(&["set-role", &role.to_string(), hex_color])?
        .ok_or_else(|| "Palette could not be saved".into())
}

pub fn set_accent(hex_color: &str) -> Result<(), String> {
    run(&["set-accent", hex_color]).map(|_| ())
}
