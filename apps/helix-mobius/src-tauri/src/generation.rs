//! Generated image provenance and explicit export/copy. Pixels use the existing private picture store.
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::io::{Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Info {
    pub prompt: String,
    pub model: String,
    pub width: u32,
    pub height: u32,
    pub seed: u32,
    pub seconds: f64,
}

fn metadata_path(dir: &Path, id: &str) -> Result<PathBuf, String> {
    if !helix_shell_core::chat::valid_id(id) {
        return Err("Invalid image id.".into());
    }
    Ok(dir.join(format!("{id}.json")))
}

pub fn store(dir: &Path, result: &Value, seconds: f64) -> Result<(String, Info), String> {
    let image = &result["image"];
    let encoded = image["png"]
        .as_str()
        .ok_or("The generator returned no picture.")?;
    if encoded.len() > 6 * 1024 * 1024 {
        return Err("Generated picture is too large.".into());
    }
    let bytes = STANDARD
        .decode(encoded)
        .map_err(|_| "Invalid generated picture.")?;
    let info: Info = serde_json::from_value(serde_json::json!({
        "prompt": image["prompt"], "model": image["model"], "width": image["width"], "height": image["height"], "seed": image["seed"], "seconds": seconds
    })).map_err(|_| "Invalid generated picture metadata.")?;
    if !bytes.starts_with(b"\x89PNG\r\n\x1a\n")
        || bytes.len() > 4 * 1024 * 1024
        || !(256..=768).contains(&info.width)
        || !(256..=768).contains(&info.height)
        || !info.width.is_multiple_of(64)
        || !info.height.is_multiple_of(64)
        || info.prompt.len() > 4000
        || info.model.len() > 200
    {
        return Err("Generated picture exceeds its limits.".into());
    }
    let id = crate::images::store(dir, &bytes)?;
    let metadata = metadata_path(dir, &id)?;
    let saved = (|| {
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(metadata)
            .map_err(|e| e.to_string())?;
        file.write_all(&serde_json::to_vec(&info).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())
    })();
    if let Err(error) = saved {
        crate::images::remove(dir, &[id]);
        return Err(error);
    }
    Ok((id, info))
}

pub fn info(dir: &Path, id: &str) -> Result<Option<Info>, String> {
    let path = metadata_path(dir, id)?;
    let file = match std::fs::File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error.to_string()),
    };
    let mut bytes = Vec::new();
    file.take(16385)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > 16384 {
        return Err("Image metadata is too large.".into());
    }
    serde_json::from_slice(&bytes)
        .map(Some)
        .map_err(|_| "Image metadata could not be read.".into())
}

pub fn copy(dir: &Path, id: &str) -> Result<(), String> {
    let bytes = crate::images::load(dir, id)?;
    let mut child = std::process::Command::new("wl-copy")
        .args(["--type", "image/png"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::null())
        .spawn()
        .map_err(|_| "Image copying requires wl-copy.")?;
    child
        .stdin
        .take()
        .ok_or("Could not copy the picture.")?
        .write_all(&bytes)
        .map_err(|e| e.to_string())?;
    if child.wait().map_err(|e| e.to_string())?.success() {
        Ok(())
    } else {
        Err("Could not copy the picture.".into())
    }
}

pub fn save(dir: &Path, id: &str) -> Result<Option<String>, String> {
    let bytes = crate::images::load(dir, id)?;
    let output = std::process::Command::new("zenity")
        .args([
            "--file-selection",
            "--save",
            "--title=Save generated image",
            "--filename=mobius-image.png",
            "--file-filter=PNG image | *.png",
        ])
        .output()
        .map_err(|_| "Saving pictures requires zenity.")?;
    if !output.status.success() {
        return Ok(None);
    }
    let path = String::from_utf8(output.stdout).map_err(|_| "Invalid save path.")?;
    let path = path.trim_end_matches(['\r', '\n']);
    if path.is_empty() {
        return Ok(None);
    }
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|e| format!("Could not save the picture (choose a new file name): {e}"))?;
    file.write_all(&bytes).map_err(|e| e.to_string())?;
    Ok(Some(path.to_string()))
}
