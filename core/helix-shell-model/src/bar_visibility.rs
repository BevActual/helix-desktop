//! Session bar top strut (`helix-shell-reserve`) exclusive zone, shared between the bar process and `helix-shell-strut`.
//!
//! The painted bar window hides separately; this file only drives whether the compositor keeps the top gap.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::shell_surface::BAR_HEIGHT;

const FILE: &str = "helix-top-strut.json";

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TopStrutState {
    /// `BAR_HEIGHT` when the bar should reserve space, `0` when hidden.
    pub exclusive_zone: i32,
}

impl TopStrutState {
    pub fn visible() -> Self {
        Self {
            exclusive_zone: BAR_HEIGHT,
        }
    }

    pub fn hidden() -> Self {
        Self { exclusive_zone: 0 }
    }

    pub fn is_visible(&self) -> bool {
        self.exclusive_zone > 0
    }
}

pub fn state_path() -> Option<PathBuf> {
    std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .map(|dir| dir.join(FILE))
}

pub fn read() -> TopStrutState {
    let path = state_path();
    let Some(path) = path else {
        return TopStrutState::visible();
    };
    let text = std::fs::read_to_string(&path).unwrap_or_default();
    if text.trim().is_empty() {
        return TopStrutState::visible();
    }
    serde_json::from_str(&text).unwrap_or_else(|_| TopStrutState::visible())
}

pub fn write(state: &TopStrutState) -> Result<(), String> {
    let path = state_path().ok_or("no runtime directory")?;
    let text = serde_json::to_string(state).map_err(|e| e.to_string())?;
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, text).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_visible_hidden() {
        let dir = std::env::temp_dir().join(format!("helix-bar-vis-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::env::set_var("XDG_RUNTIME_DIR", &dir);
        write(&TopStrutState::hidden()).unwrap();
        assert!(!read().is_visible());
        write(&TopStrutState::visible()).unwrap();
        assert!(read().is_visible());
        std::fs::remove_dir_all(dir).ok();
    }
}
