//! Helix-owned niri snippets and session prefs. Settings writes `helix-setup.kdl`; the compositor
//! still owns `config.kdl`. Helix merges via `include "helix-setup.kdl"` (same pattern as layout/blur includes).

use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::agent_desk::DEFAULT_NAME;

pub const SETUP_INCLUDE: &str = "helix-setup.kdl";
const SETUP_HEADER: &str =
    "// Managed by Helix Settings. Safe to edit by hand; Settings may rewrite this file.\n";

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
    let includes = niri_dir.as_deref().map(list_includes).unwrap_or_default();
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
        "main" => {
            return niri_dir
                .join("config.kdl")
                .is_file()
                .then(|| niri_dir.join("config.kdl"))
        }
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

/// One output written into `helix-setup.kdl`. Names are connector names the page already checked.
#[derive(Debug, Clone, PartialEq)]
pub struct DisplaySetup {
    pub name: String,
    pub off: bool,
    pub scale: Option<f64>,
    pub transform: Option<String>,
    pub mode: Option<String>,
    pub position: Option<(i32, i32)>,
    pub vrr: bool,
}

impl DisplaySetup {
    pub fn block(&self) -> Result<String, &'static str> {
        if !crate::compositor_ipc::config_name_ok(&self.name) {
            return Err("unknown display");
        }
        let mut lines = vec![format!("output \"{}\" {{", self.name)];
        if self.off {
            lines.push("    off".into());
        } else {
            if let Some(mode) = &self.mode {
                if mode.chars().any(|c| c.is_control() || c == '"') {
                    return Err("pick a resolution");
                }
                lines.push(format!("    mode \"{mode}\""));
            }
            if let Some(scale) = self.scale {
                let text = crate::compositor_ipc::scale_token(scale)
                    .map(str::to_string)
                    .unwrap_or_else(|| format!("{scale:.2}"));
                lines.push(format!("    scale {text}"));
            }
            if let Some(transform) = &self.transform {
                if transform != "normal"
                    && transform
                        .chars()
                        .all(|c| c.is_ascii_alphanumeric() || c == '-')
                {
                    lines.push(format!("    transform \"{transform}\""));
                }
            }
            if let Some((x, y)) = self.position {
                lines.push(format!("    position x={x} y={y}"));
            }
            if self.vrr {
                lines.push("    variable-refresh-rate".into());
            }
        }
        lines.push("}".into());
        Ok(lines.join("\n"))
    }
}

/// Rewrite the output blocks in `helix-setup.kdl` and make sure the main config includes that file.
pub fn save_displays(displays: &[DisplaySetup]) -> Result<(), String> {
    let blocks = displays
        .iter()
        .map(DisplaySetup::block)
        .collect::<Result<Vec<_>, _>>()
        .map_err(str::to_string)?;
    write_setup(|existing| replace_output_blocks(existing, &blocks))
}

/// Writes one output's block and leaves every other block, and everything else in the file, as it was. `replaces` are other names the same display
/// was saved under (its connector, now that it is saved by identity); their blocks are dropped so the display has just one.
pub fn save_display(display: &DisplaySetup, replaces: &[&str]) -> Result<(), String> {
    let block = display.block().map_err(str::to_string)?;
    write_setup(|existing| {
        let mut text = existing.to_string();
        for name in replaces.iter().filter(|name| **name != display.name) {
            text = upsert_output_block(&text, name, None);
        }
        upsert_output_block(&text, &display.name, Some(&block))
    })
}

/// Whether the saved block for any of `names` already has a line starting with `keyword` (`position`, `mode`, ...). Used so that changing one thing
/// about a display does not pin the things the person never set.
pub fn saved_output_has(names: &[&str], keyword: &str) -> bool {
    let Some(dir) = niri_config_dir() else {
        return false;
    };
    let text = fs::read_to_string(dir.join(SETUP_INCLUDE)).unwrap_or_default();
    find_output_blocks(&text)
        .into_iter()
        .filter(|(block_name, _)| names.contains(&block_name.as_str()))
        .any(|(_, range)| {
            text[range]
                .lines()
                .any(|line| line.split_whitespace().next() == Some(keyword))
        })
}

fn write_setup(edit: impl FnOnce(&str) -> String) -> Result<(), String> {
    let dir = niri_config_dir().ok_or("no config home")?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let setup_path = dir.join(SETUP_INCLUDE);
    let existing = fs::read_to_string(&setup_path).unwrap_or_default();
    let mut next = edit(&existing);
    if !next.contains("Managed by Helix Settings") {
        next = format!("{SETUP_HEADER}\n{next}");
    }
    if !next.ends_with('\n') {
        next.push('\n');
    }
    write_atomic(&setup_path, next.as_bytes())?;
    ensure_include(&dir.join("config.kdl"), SETUP_INCLUDE)
}

/// Every `output "name" { … }` block in a KDL text, with its byte range: on one line, over several, or with the brace on the next line.
fn find_output_blocks(text: &str) -> Vec<(String, std::ops::Range<usize>)> {
    let lines: Vec<(usize, &str)> = text
        .split_inclusive('\n')
        .scan(0usize, |offset, line| {
            let at = *offset;
            *offset += line.len();
            Some((at, line))
        })
        .collect();
    let mut found = Vec::new();
    let mut i = 0;
    while i < lines.len() {
        let (start, line) = lines[i];
        let Some(name) = output_header(line) else {
            i += 1;
            continue;
        };
        let mut depth = brace_delta(line);
        let mut opened = line.contains('{');
        let mut end_line = i;
        let mut j = i + 1;
        while j < lines.len() && (depth > 0 || !opened) {
            if !opened && !lines[j].1.trim().is_empty() && !lines[j].1.trim_start().starts_with('{')
            {
                break;
            }
            opened |= lines[j].1.contains('{');
            depth += brace_delta(lines[j].1);
            end_line = j;
            j += 1;
        }
        if opened && depth <= 0 {
            let end = lines[end_line].0 + lines[end_line].1.len();
            found.push((name.to_string(), start..end));
            i = end_line + 1;
        } else {
            i += 1;
        }
    }
    found
}

/// `existing` without any block for `name`, and with `block` (when given) added at the end.
pub fn upsert_output_block(existing: &str, name: &str, block: Option<&str>) -> String {
    remove_blocks(
        existing,
        |found| found == name,
        block.map(|b| vec![b.to_string()]).unwrap_or_default(),
    )
}

/// Drop every `output "…" { … }` block and append `blocks`, leaving the rest of the file alone.
pub fn replace_output_blocks(existing: &str, blocks: &[String]) -> String {
    remove_blocks(existing, |_| true, blocks.to_vec())
}

fn remove_blocks(existing: &str, drop: impl Fn(&str) -> bool, add: Vec<String>) -> String {
    let mut kept = existing.to_string();
    for (name, range) in find_output_blocks(existing).into_iter().rev() {
        if drop(&name) {
            kept.replace_range(range, "");
        }
    }
    let mut next = kept.trim_end().to_string();
    if !add.is_empty() {
        if !next.is_empty() {
            next.push_str("\n\n");
        }
        next.push_str(&add.join("\n\n"));
    }
    if !next.is_empty() && !next.ends_with('\n') {
        next.push('\n');
    }
    next
}

/// The connector name in a line that starts an output block (`output "eDP-1" {` or just `output "eDP-1"`).
fn output_header(line: &str) -> Option<&str> {
    let rest = line.trim().strip_prefix("output")?.trim_start();
    let quoted = rest.strip_prefix('"')?;
    let (name, after) = quoted.split_once('"')?;
    let after = after.trim_start();
    (crate::compositor_ipc::config_name_ok(name) && (after.is_empty() || after.starts_with('{')))
        .then_some(name)
}

fn brace_delta(line: &str) -> i32 {
    line.chars().fold(0, |depth, c| match c {
        '{' => depth + 1,
        '}' => depth - 1,
        _ => depth,
    })
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
    let existing = fs::read_to_string(&setup_path).unwrap_or_default();
    let helix = format!(
        "helix {{\n    agent-desk-name \"{name}\"\n}}",
        name = name.replace('\\', "\\\\")
    );
    let mut next = replace_helix_block(&existing, &helix);
    if !next.contains("Managed by Helix Settings") {
        next = format!("{SETUP_HEADER}\n{next}");
    }
    if !next.ends_with('\n') {
        next.push('\n');
    }
    write_atomic(&setup_path, next.as_bytes())?;
    ensure_include(&dir.join("config.kdl"), SETUP_INCLUDE)?;
    Ok(())
}

fn replace_helix_block(existing: &str, helix: &str) -> String {
    let mut kept = String::new();
    let mut lines = existing.lines().peekable();
    let mut replaced = false;
    while let Some(line) = lines.next() {
        if line.trim().starts_with("helix") && line.contains('{') {
            let mut depth = brace_delta(line);
            while depth > 0 {
                let Some(inner) = lines.next() else { break };
                depth += brace_delta(inner);
            }
            if !replaced {
                kept.push_str(helix);
                kept.push('\n');
                replaced = true;
            }
            continue;
        }
        if line.contains("Managed by Helix Settings") {
            continue;
        }
        kept.push_str(line);
        kept.push('\n');
    }
    let mut next = kept.trim().to_string();
    if !replaced {
        if !next.is_empty() {
            next = format!("{helix}\n\n{next}");
        } else {
            next = helix.to_string();
        }
    }
    next
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
    Some(
        text.lines()
            .any(|line| include_line_matches(line, filename)),
    )
}

pub fn ensure_include(config: &Path, filename: &str) -> Result<(), String> {
    let include_line = format!("include \"{filename}\"");
    if config.is_file() {
        let text = fs::read_to_string(config).map_err(|e| e.to_string())?;
        if text
            .lines()
            .any(|line| include_line_matches(line, filename))
        {
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
        path.file_name().and_then(|n| n.to_str()).unwrap_or("helix")
    ));
    fs::write(&tmp, bytes).map_err(|e| e.to_string())?;
    fs::rename(&tmp, path).map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn display_block_and_merge_keep_the_agent_desk() {
        let block = DisplaySetup {
            name: "eDP-1".into(),
            off: false,
            scale: Some(1.5),
            transform: Some("90".into()),
            mode: Some("1920x1080@60.000".into()),
            position: Some((1280, 0)),
            vrr: true,
        }
        .block()
        .unwrap();
        assert!(block.contains("scale 1.5"));
        assert!(block.contains("transform \"90\""));
        assert!(block.contains("mode \"1920x1080@60.000\""));
        assert!(block.contains("position x=1280 y=0"));
        assert!(block.contains("variable-refresh-rate"));
        let existing = "helix {\n    agent-desk-name \"Mobius desk\"\n}\n\noutput \"eDP-1\" {\n    scale 1\n}\n";
        let merged = replace_output_blocks(existing, &[block]);
        assert!(merged.contains("agent-desk-name \"Mobius desk\""));
        assert!(merged.contains("scale 1.5"));
        assert_eq!(merged.matches("output \"eDP-1\"").count(), 1);
        let kept = replace_helix_block(&merged, "helix {\n    agent-desk-name \"Lab\"\n}");
        assert!(kept.contains("agent-desk-name \"Lab\""));
        assert!(kept.contains("scale 1.5"));
        assert!(!kept.contains("Mobius desk"));
    }

    #[test]
    fn one_display_is_replaced_and_the_others_are_left_alone() {
        let text = "// Managed by Helix Settings.\noutput \"A\" { scale 2 }\noutput \"B\"\n{\n    scale 1\n}\noutput \"C\" {\n    off\n}\nhelix {\n}\n";
        let blocks = find_output_blocks(text);
        assert_eq!(
            blocks.iter().map(|b| b.0.as_str()).collect::<Vec<_>>(),
            ["A", "B", "C"]
        );
        let next = upsert_output_block(text, "A", Some("output \"A\" {\n    scale 1.5\n}"));
        assert_eq!(next.matches("output \"A\"").count(), 1, "{next}");
        assert!(next.contains("scale 1.5") && !next.contains("scale 2"));
        assert!(
            next.contains("output \"B\"\n{\n    scale 1\n}"),
            "B untouched: {next}"
        );
        assert!(next.contains("output \"C\" {\n    off\n}") && next.contains("helix {"));
        let gone = upsert_output_block(text, "B", None);
        assert!(!gone.contains("output \"B\"") && gone.contains("output \"A\""));
    }

    #[test]
    fn a_display_saved_by_identity_replaces_its_connector_block() {
        let text =
            "output \"DP-3\" {\n    scale 2\n}\noutput \"Dell U2720Q ABC123\" {\n    scale 1\n}\n";
        assert_eq!(
            find_output_blocks(text)
                .iter()
                .map(|b| b.0.as_str())
                .collect::<Vec<_>>(),
            ["DP-3", "Dell U2720Q ABC123"]
        );
        let mut next = upsert_output_block(text, "DP-3", None);
        next = upsert_output_block(
            &next,
            "Dell U2720Q ABC123",
            Some("output \"Dell U2720Q ABC123\" {\n    scale 1.5\n}"),
        );
        assert!(
            !next.contains("DP-3") && next.contains("scale 1.5") && !next.contains("scale 2"),
            "{next}"
        );
        let display = DisplaySetup {
            name: "Dell U2720Q ABC123".into(),
            off: false,
            scale: Some(1.5),
            transform: None,
            mode: None,
            position: None,
            vrr: false,
        };
        assert!(display
            .block()
            .unwrap()
            .starts_with("output \"Dell U2720Q ABC123\""));
        assert!(DisplaySetup {
            name: "bad\"name".into(),
            ..display
        }
        .block()
        .is_err());
    }

    #[test]
    fn parses_agent_desk_name() {
        let text = "helix {\n  agent-desk-name \"Mobius desk\"\n}\n";
        assert_eq!(parse_agent_desk_name(text).as_deref(), Some("Mobius desk"));
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
            fs::read_to_string(&config)
                .unwrap()
                .matches("helix-setup.kdl")
                .count(),
            1
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn include_detection() {
        assert!(include_line_matches(
            "include \"helix-setup.kdl\"",
            SETUP_INCLUDE
        ));
        assert!(!include_line_matches(
            "include \"helix.kdl\"",
            SETUP_INCLUDE
        ));
    }

    #[test]
    fn lists_known_includes() {
        let dir = std::env::temp_dir().join(format!("helix-includes-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("config.kdl"), "include \"helix-setup.kdl\"\n").unwrap();
        fs::write(dir.join(SETUP_INCLUDE), "helix {}\n").unwrap();
        let views = list_includes(&dir);
        assert!(views
            .iter()
            .any(|v| v.filename == SETUP_INCLUDE && v.included && v.exists));
        let _ = fs::remove_dir_all(&dir);
    }
}
