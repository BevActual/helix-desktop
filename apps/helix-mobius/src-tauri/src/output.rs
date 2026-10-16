//! Save assistant output to a user-chosen file (preview happens in the UI first).
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;

use crate::terminal;

pub fn safe_filename(name: &str) -> Result<String, String> {
    let trimmed = name.trim();
    if trimmed.is_empty() || trimmed.len() > 120 {
        return Err("Choose a shorter file name.".into());
    }
    if trimmed.contains('/') || trimmed.contains('\\') || trimmed.contains("..") {
        return Err("The file name must not include paths.".into());
    }
    if !trimmed
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_' | ' '))
    {
        return Err("The file name uses unsupported characters.".into());
    }
    Ok(trimmed.to_string())
}

pub fn save_new_file(content: &str, default_filename: &str) -> Result<Option<String>, String> {
    terminal::safe_input(content, 512_000)?;
    let default = safe_filename(default_filename)?;
    let output = std::process::Command::new("zenity")
        .args([
            "--file-selection",
            "--save",
            "--title=Save Mobius output",
            &format!("--filename={default}"),
        ])
        .output()
        .map_err(|_| "Saving files requires zenity.")?;
    if !output.status.success() {
        return Ok(None);
    }
    let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if path.is_empty() {
        return Ok(None);
    }
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&path)
        .map_err(|e| format!("Could not save (choose a new file name): {e}"))?;
    file.write_all(content.as_bytes())
        .map_err(|e| e.to_string())?;
    Ok(Some(path))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn filenames_must_be_simple_basenames() {
        assert!(safe_filename("notes.md").is_ok());
        assert!(safe_filename("../x").is_err());
        assert!(safe_filename("bad/name").is_err());
    }
}
