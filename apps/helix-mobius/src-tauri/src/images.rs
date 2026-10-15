//! The picture store: images a person attaches to a chat live here, beside the transcripts, readable only by them.
//!
//! A picture is staged the moment it is added (so the composer can show it) and referred to by an id from then on. The bytes are
//! checked by their content, never by a name or a claimed type, and are never passed to anything that does not decode them
//! strictly: the inference service is the only consumer, and it applies its own limits again.

use std::io::{Read, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

pub const MAX_BYTES: usize = 8 * 1024 * 1024;

/// What the bytes are, from their first bytes; `None` for anything the service would not read (SVG, HTML, text...).
pub fn sniff(bytes: &[u8]) -> Option<&'static str> {
    if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        Some("image/png")
    } else if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) {
        Some("image/jpeg")
    } else if bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a") {
        Some("image/gif")
    } else if bytes.len() > 12 && &bytes[0..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        Some("image/webp")
    } else if bytes.starts_with(b"BM") && bytes.len() > 26 {
        Some("image/bmp")
    } else {
        None
    }
}

fn new_id() -> Result<String, String> {
    let mut raw = [0u8; 8];
    std::fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut raw))
        .map_err(|_| "Could not make an id for the picture.".to_string())?;
    Ok(raw.iter().map(|b| format!("{b:02x}")).collect())
}

pub fn store(dir: &Path, bytes: &[u8]) -> Result<String, String> {
    if bytes.is_empty() || bytes.len() > MAX_BYTES {
        return Err("Pictures can be up to 8 MB.".into());
    }
    if sniff(bytes).is_none() {
        return Err("That is not a picture Mobius can read (PNG, JPEG, GIF, WebP or BMP).".into());
    }
    std::fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(dir)
        .map_err(|_| "Could not save the picture.".to_string())?;
    let id = new_id()?;
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(dir.join(&id))
        .map_err(|_| "Could not save the picture.".to_string())?;
    file.write_all(bytes).map_err(|_| "Could not save the picture.".to_string())?;
    Ok(id)
}

pub fn path_of(dir: &Path, id: &str) -> Option<PathBuf> {
    helix_shell_core::chat::valid_id(id).then(|| dir.join(id))
}

pub fn load(dir: &Path, id: &str) -> Result<Vec<u8>, String> {
    let path = path_of(dir, id).ok_or("not a picture")?;
    let bytes = std::fs::read(path).map_err(|_| "That picture is no longer on this computer.".to_string())?;
    if bytes.len() > MAX_BYTES || sniff(&bytes).is_none() {
        return Err("That picture cannot be read.".into());
    }
    Ok(bytes)
}

pub fn remove(dir: &Path, ids: &[String]) {
    for id in ids {
        if let Some(path) = path_of(dir, id) {
            let _ = std::fs::remove_file(path);
            let _ = std::fs::remove_file(dir.join(format!("{id}.json")));
        }
    }
}

/// Pictures nobody refers to (staged and never sent, or left by a deleted chat) and that are over a day old.
pub fn sweep(dir: &Path, referenced: &std::collections::HashSet<String>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    let cutoff = std::time::SystemTime::now() - std::time::Duration::from_secs(24 * 3600);
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if !helix_shell_core::chat::valid_id(&name) || referenced.contains(&name) {
            continue;
        }
        let old = entry.metadata().and_then(|m| m.modified()).is_ok_and(|t| t < cutoff);
        if old {
            let _ = std::fs::remove_file(entry.path());
            let _ = std::fs::remove_file(dir.join(format!("{name}.json")));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PNG: &[u8] = b"\x89PNG\r\n\x1a\n0000000000000000000000000";

    #[test]
    fn only_pictures_are_kept_and_only_by_id() {
        let dir = std::env::temp_dir().join(format!("helix-mobius-images-{}", std::process::id()));
        let id = store(&dir, PNG).unwrap();
        assert_eq!(load(&dir, &id).unwrap(), PNG);
        assert!(store(&dir, b"<svg onload=alert(1)/>").is_err());
        assert!(store(&dir, b"").is_err());
        assert!(load(&dir, "../../etc/passwd").is_err());
        assert!(load(&dir, "0000000000000000").is_err());
        remove(&dir, std::slice::from_ref(&id));
        assert!(load(&dir, &id).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
