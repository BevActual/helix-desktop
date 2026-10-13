//! Helix-owned niri snippets and session prefs. Settings writes `helix-setup.kdl`; the compositor
//! still owns `config.kdl`. Helix merges via `include "helix-setup.kdl"` (same pattern as layout/blur includes).

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::agent_desk::DEFAULT_NAME;

pub const SETUP_INCLUDE: &str = "helix-setup.kdl";
const SETUP_HEADER: &str = "// Managed by Helix Settings. Safe to edit by hand; Settings may rewrite this file.\n";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompositorPrefs {
    #[serde(default = "default_placement")]
    pub agent_desk_placement: bool,
}

fn default_placement() -> bool {
    true
}

impl Default for CompositorPrefs {
    fn default() -> Self {
        Self {
            agent_desk_placement: default_placement(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct HelixSnippet {
    pub agent_desk_name: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompositorIncludeView {
    pub filename: String,
    pub label: String,
    pub about: String,
    pub included: bool,
    pub exists: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompositorConfigView {
    pub niri_dir: String,
    pub main_config: String,
    pub setup_file: String,
    pub setup_included: bool,
    pub snippet: HelixSnippet,
    pub prefs: CompositorPrefs,
    pub includes: Vec<CompositorIncludeView>,
}

pub fn niri_config_dir() -> Option<PathBuf> {
    config_home().map(|home| home.join("niri"))
}

fn config_home() -> Option<PathBuf> {
    std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
}

fn helix_config_dir() -> Option<PathBuf> {
    config_home().map(|h| h.join("helix"))
}

pub fn prefs_path() -> Option<PathBuf> {
    helix_config_dir().map(|d| d.join("compositor-prefs.toml"))
}

pub fn load_prefs() -> CompositorPrefs {
    let path = prefs_path().filter(|p| p.is_file());
    let Some(path) = path else {
        return CompositorPrefs::default();
    };
    let text = fs::read_to_string(&path).unwrap_or_default();
    toml::from_str(&text).unwrap_or_default()
}

pub fn save_prefs(prefs: &CompositorPrefs) -> Result<(), String> {
    let path = prefs_path().ok_or("no config home")?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let text = toml::to_string(prefs).map_err(|e| e.to_string())?;
    write_atomic(&path, text.as_bytes())
}

pub fn agent_desk_placement_enabled() -> bool {
    if matches!(
        std::env::var("HELIX_AGENT_DESK").as_deref(),
        Ok("0") | Ok("off") | Ok("false")
    ) {
        return false;
    }
    load_prefs().agent_desk_placement
}

pub fn read_view() -> CompositorConfigView {
    let niri_dir = niri_config_dir();
    let main_config = niri_dir
        .as_ref()
        .map(|d| d.join("config.kdl"))
        .filter(|p| p.is_file())
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_default();
    let setup_file = niri_dir
        .as_ref()
        .map(|d| d.join(SETUP_INCLUDE))
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_default();
    let setup_included = niri_dir
        .as_ref()
        .and_then(|d| read_include_present(&d.join("config.kdl"), SETUP_INCLUDE))
        .unwrap_or(false);
    let snippet = read_snippet(niri_dir.as_deref());
    let includes = niri_dir
        .as_deref()
        .map(|dir| list_includes(dir))
        .unwrap_or_default();
    CompositorConfigView {
        niri_dir: niri_dir
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default(),
        main_config,
        setup_file,
        setup_included,
        snippet,
        prefs: load_prefs(),
        includes,
    }
}

const KNOWN_INCLUDES: &[(&str, &str, &str)] = &[
    (
        SETUP_INCLUDE,
        "Helix setup",
        "Agent desk name and other Helix-only compositor options (Settings).",
    ),
    (
        "helix-layout.kdl",
        "Layout & decorations",
        "Control Center theme, gaps, struts, and focus ring.",
    ),
    (
        "helix-blur.kdl",
        "Blur & liquid glass",
        "Compositor blur and chrome refraction on helix-niri.",
    ),
    (
        "helix-outputs.kdl",
        "Outputs",
        "Monitor arrangement from Control Center.",
    ),
    (
        "helix-overview.kdl",
        "Overview",
        "Overview animation tuning.",
    ),
    (
        "helix-window-rules.kdl",
        "Window rules",
        "Border and background rules for Helix chrome.",
    ),
    (
        "helix.kdl",
        "Helix theme snippet",
        "Legacy palette include; layout colors live in helix-layout.kdl.",
    ),
];

pub fn list_includes(niri_dir: &Path) -> Vec<CompositorIncludeView> {
    let config = niri_dir.join("config.kdl");
    let included = |name: &str| read_include_present(&config, name).unwrap_or(false);
    KNOWN_INCLUDES
        .iter()
        .map(|(filename, label, about)| {
            let path = niri_dir.join(filename);
            CompositorIncludeView {
                filename: (*filename).into(),
                label: (*label).into(),
                about: (*about).into(),
                included: included(filename),
                exists: path.is_file(),
            }
        })
        .collect()
}

pub fn resolve_include_path(niri_dir: &Path, which: &str) -> Option<PathBuf> {
    let filename = match which {
        "setup" => SETUP_INCLUDE,
        "main" => return niri_dir.join("config.kdl").is_file().then(|| niri_dir.join("config.kdl")),
        other if other.ends_with(".kdl") => other,
        other => KNOWN_INCLUDES
            .iter()
            .find(|(name, _, _)| *name == other || name.strip_suffix(".kdl") == Some(other))
            .map(|(name, _, _)| *name)
            .unwrap_or(other),
    };
    let path = niri_dir.join(filename);
    path.is_file().then_some(path)
}

pub fn save_snippet(snippet: &HelixSnippet) -> Result<(), String> {
    let name = snippet.agent_desk_name.trim();
    if name.is_empty() || name.chars().count() > 64 {
        return Err("agent desk name must be 1–64 characters".into());
    }
    if name.contains('"') || name.contains('\n') {
        return Err("agent desk name cannot contain quotes or line breaks".into());
    }
    let dir = niri_config_dir().ok_or("no config home")?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let setup_path = dir.join(SETUP_INCLUDE);
    let body = format!(
        "{SETUP_HEADER}\nhelix {{\n    agent-desk-name \"{name}\"\n}}\n",
        name = name.replace('\\', "\\\\")
    );
    write_atomic(&setup_path, body.as_bytes())?;
    ensure_include(&dir.join("config.kdl"), SETUP_INCLUDE)?;
    Ok(())
}

fn read_snippet(niri_dir: Option<&Path>) -> HelixSnippet {
    let mut name = None;
    if let Some(dir) = niri_dir {
        let setup = dir.join(SETUP_INCLUDE);
        if setup.is_file() {
            name = parse_agent_desk_name(&fs::read_to_string(setup).unwrap_or_default());
        }
        if name.is_none() {
            let main = dir.join("config.kdl");
            if main.is_file() {
                name = parse_agent_desk_name(&fs::read_to_string(main).unwrap_or_default());
            }
        }
    }
    HelixSnippet {
        agent_desk_name: name.unwrap_or_else(|| DEFAULT_NAME.into()),
    }
}

fn parse_agent_desk_name(text: &str) -> Option<String> {
    for line in text.lines() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("agent-desk-name") {
            let rest = rest.trim();
            if let Some(quoted) = rest.strip_prefix('"').and_then(|s| s.strip_suffix('"')) {
                if !quoted.is_empty() && quoted.chars().count() <= 64 {
                    return Some(quoted.to_string());
                }
            }
        }
    }
    None
}

fn read_include_present(config: &Path, filename: &str) -> Option<bool> {
    let text = fs::read_to_string(config).ok()?;
    Some(text.lines().any(|line| include_line_matches(line, filename)))
}

pub fn ensure_include(config: &Path, filename: &str) -> Result<(), String> {
    let include_line = format!("include \"{filename}\"");
    if config.is_file() {
        let text = fs::read_to_string(config).map_err(|e| e.to_string())?;
        if text.lines().any(|line| include_line_matches(line, filename)) {
            return Ok(());
        }
        let mut next = text;
        if !next.ends_with('\n') {
            next.push('\n');
        }
        next.push('\n');
        next.push_str(&include_line);
        next.push('\n');
        return write_atomic(config, next.as_bytes());
    }
    fs::create_dir_all(config.parent().ok_or("no config path")?).map_err(|e| e.to_string())?;
    write_atomic(config, format!("{include_line}\n").as_bytes())
}

fn include_line_matches(line: &str, filename: &str) -> bool {
    let trimmed = line.trim();
    if !trimmed.starts_with("include") {
        return false;
    }
    trimmed.contains(&format!("\"{filename}\""))
        || trimmed.contains(&format!("\"./{filename}\""))
        || trimmed.contains(&format!("\"niri/{filename}\""))
}

fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let parent = path.parent().ok_or("no parent directory")?;
    let tmp = parent.join(format!(
        ".{}.tmp",
        path.file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("helix")
    ));
    fs::write(&tmp, bytes).map_err(|e| e.to_string())?;
    fs::rename(&tmp, path).map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_agent_desk_name() {
        let text = "helix {\n  agent-desk-name \"Mobius desk\"\n}\n";
        assert_eq!(
            parse_agent_desk_name(text).as_deref(),
            Some("Mobius desk")
        );
    }

    #[test]
    fn ensure_include_appends_once() {
        let dir = std::env::temp_dir().join(format!("helix-setup-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let config = dir.join("config.kdl");
        fs::write(&config, "input {}\n").unwrap();
        ensure_include(&config, SETUP_INCLUDE).unwrap();
        let text = fs::read_to_string(&config).unwrap();
        assert_eq!(text.matches("helix-setup.kdl").count(), 1);
        ensure_include(&config, SETUP_INCLUDE).unwrap();
        assert_eq!(
            fs::read_to_string(&config).unwrap().matches("helix-setup.kdl").count(),
            1
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn include_detection() {
        assert!(include_line_matches("include \"helix-setup.kdl\"", SETUP_INCLUDE));
        assert!(!include_line_matches("include \"helix.kdl\"", SETUP_INCLUDE));
    }

    #[test]
    fn lists_known_includes() {
        let dir = std::env::temp_dir().join(format!("helix-includes-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("config.kdl"), "include \"helix-setup.kdl\"\n").unwrap();
        fs::write(dir.join(SETUP_INCLUDE), "helix {}\n").unwrap();
        let views = list_includes(&dir);
        assert!(views.iter().any(|v| v.filename == SETUP_INCLUDE && v.included && v.exists));
        let _ = fs::remove_dir_all(&dir);
    }
}
