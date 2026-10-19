//! Cloud/LAN API keys for Mobius backends.
//! Prefers the session Secret Service; falls back to a mode-0600 file under state.
//! Keys never appear in transcripts or `inference-routing.toml` (see inference-fabric.md).

use helix_config::inference_routing;
use std::fs;
use std::io::Write;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::path::PathBuf;

const SERVICE: &str = "org.helix.Mobius";

fn normalize_ref(api_key_ref: &str) -> Result<String, String> {
    let trimmed = api_key_ref.trim();
    if trimmed.is_empty() {
        return Err("Secret reference is empty.".into());
    }
    if trimmed.len() > 64
        || !trimmed
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_' || c == '.')
    {
        return Err("Secret reference must be a short id (letters, digits, ._-).".into());
    }
    Ok(trimmed.to_string())
}

fn file_path(api_key_ref: &str) -> PathBuf {
    inference_routing::state_dir()
        .join("secrets")
        .join(format!("backend-{api_key_ref}"))
}

fn keyring_entry(api_key_ref: &str) -> Result<keyring::Entry, String> {
    keyring::Entry::new(SERVICE, api_key_ref).map_err(|e| e.to_string())
}

fn write_file_secret(api_key_ref: &str, secret: &str) -> Result<(), String> {
    let dir = inference_routing::state_dir().join("secrets");
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    #[cfg(unix)]
    {
        let _ = fs::set_permissions(&dir, fs::Permissions::from_mode(0o700));
    }
    let path = file_path(api_key_ref);
    let temporary = path.with_extension("tmp");
    {
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|e| e.to_string())?;
        file.write_all(secret.as_bytes())
            .map_err(|e| e.to_string())?;
        file.sync_all().map_err(|e| e.to_string())?;
    }
    fs::rename(&temporary, &path).map_err(|e| e.to_string())?;
    Ok(())
}

fn read_file_secret(api_key_ref: &str) -> Option<String> {
    let text = fs::read_to_string(file_path(api_key_ref)).ok()?;
    let trimmed = text.trim();
    if trimmed.is_empty() {
        None
    } else {
        Some(trimmed.to_string())
    }
}

fn delete_file_secret(api_key_ref: &str) {
    let _ = fs::remove_file(file_path(api_key_ref));
}

/// Store `secret` for `api_key_ref`. Empty secret clears the entry.
pub fn set(api_key_ref: &str, secret: &str) -> Result<(), String> {
    let id = normalize_ref(api_key_ref)?;
    let secret = secret.trim();
    if secret.is_empty() {
        return clear(&id);
    }
    // A key is short; a sign-in (the ChatGPT plan's tokens) is a few kilobytes.
    if secret.len() > 16_384 {
        return Err("That secret is too long.".into());
    }
    match keyring_entry(&id) {
        Ok(entry) => {
            if let Err(error) = entry.set_password(secret) {
                eprintln!("MOBIUS keyring set failed ({error}); using file fallback");
                write_file_secret(&id, secret)?;
            } else if keyring_entry(&id)
                .and_then(|fresh| fresh.get_password().map_err(|e| e.to_string()))
                .is_ok_and(|read| read.trim() == secret)
            {
                // A real keyring has it: a later lookup, even after a restart, finds it, so the private file is not needed.
                delete_file_secret(&id);
            } else {
                // Without a real keyring behind the library, "saved" only means "held in memory for this one lookup". It is kept in the
                // private file instead, so it is still there next time.
                eprintln!("MOBIUS keyring did not keep the secret; using file fallback");
                write_file_secret(&id, secret)?;
            }
        }
        Err(error) => {
            eprintln!("MOBIUS keyring unavailable ({error}); using file fallback");
            write_file_secret(&id, secret)?;
        }
    }
    Ok(())
}

pub fn get(api_key_ref: &str) -> Option<String> {
    let id = normalize_ref(api_key_ref).ok()?;
    if let Ok(entry) = keyring_entry(&id) {
        if let Ok(password) = entry.get_password() {
            let trimmed = password.trim();
            if !trimmed.is_empty() {
                return Some(trimmed.to_string());
            }
        }
    }
    read_file_secret(&id)
}

pub fn has(api_key_ref: &str) -> bool {
    get(api_key_ref).is_some()
}

pub fn clear(api_key_ref: &str) -> Result<(), String> {
    let id = normalize_ref(api_key_ref)?;
    let mut failure: Option<String> = None;
    if let Ok(entry) = keyring_entry(&id) {
        match entry.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => {}
            Err(error) => {
                failure = Some(format!(
                    "The saved key could not be removed from the keyring: {error}"
                ))
            }
        }
    }
    // A key left in either place would be found and used again, so a failure to remove it is reported, not hidden.
    match fs::remove_file(file_path(&id)) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => failure = Some(format!("The saved key file could not be removed: {error}")),
    }
    match failure {
        Some(reason) => Err(reason),
        None => Ok(()),
    }
}

/// Resolve the Bearer token for a backend (`api_key_ref`, else backend id when non-local).
pub fn for_backend(backend_id: &str, api_key_ref: &str) -> Option<String> {
    let preferred = api_key_ref.trim();
    if !preferred.is_empty() {
        return get(preferred);
    }
    let id = backend_id.trim();
    if id.is_empty() || id == "local" {
        return None;
    }
    get(id)
}

#[cfg(test)]
mod persistence_tests {
    use super::*;

    /// What is saved can be read back by a later call, which is what a restart is. (With no real keyring behind `keyring`, a save used to
    /// vanish: the in-memory stand-in said yes and the private file was removed.)
    #[test]
    fn a_saved_secret_is_read_back_by_a_fresh_lookup() {
        let dir = std::env::temp_dir().join(format!("helix-secrets-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::env::set_var("HELIX_STATE_HOME", &dir);
        let id = "persist-check";
        set(id, "sk-test-value").unwrap();
        assert_eq!(get(id).as_deref(), Some("sk-test-value"));
        assert!(has(id));
        clear(id).unwrap();
        assert_eq!(get(id), None);
        std::env::remove_var("HELIX_STATE_HOME");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
