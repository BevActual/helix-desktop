//! Community + custom palette previews for Appearance1.
//! Reads the same paths as C++ (`community_palettes` / `custom_palettes`).
//! Optional catalog download when `HELIX_COMMUNITY_PALETTE_CATALOG_URL` is set
//! and shell offline mode is off.

use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use helix_theme::custom::{community_palette_cache_path, parse_palette_json, url_encode};

use crate::palette_previews::builtin_palette_previews;

const MAX_COMMUNITY: usize = 100;
const MAX_CUSTOM: usize = 100;

#[derive(Clone, Debug, Default)]
struct ModePreview {
    surface: String,
    accents: Vec<String>,
}

#[derive(Clone, Debug)]
struct AvailablePalette {
    name: String,
    dark: ModePreview,
    light: ModePreview,
}

fn state_dir_from_settings_toml(settings_toml: &Path) -> PathBuf {
    settings_toml
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("helix"))
}

fn community_catalog_path(state_dir: &Path) -> PathBuf {
    state_dir
        .join("community-palettes")
        .join(".catalog")
        .join("palettes.json")
}

fn custom_palette_dir(config_dir: &Path) -> PathBuf {
    config_dir.join("palettes")
}

fn string_field(obj: &serde_json::Map<String, Value>, key: &str) -> String {
    obj.get(key)
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string()
}

fn parse_hex(text: &str) -> Option<u32> {
    let text = text.trim();
    if text.len() != 7 || !text.starts_with('#') {
        return None;
    }
    u32::from_str_radix(&text[1..], 16).ok()
}

fn mode_preview_from_catalog(item: &Value, mode: &str) -> ModePreview {
    let Some(obj) = item.as_object() else {
        return ModePreview::default();
    };
    let Some(mode_obj) = obj.get(mode).and_then(Value::as_object) else {
        return ModePreview::default();
    };
    let mut accents = Vec::new();
    for field in ["primary", "secondary", "tertiary", "error"] {
        let color = string_field(mode_obj, field);
        if !color.is_empty() {
            accents.push(color);
        }
    }
    ModePreview {
        surface: string_field(mode_obj, "surface"),
        accents,
    }
}

fn mode_color_field(obj: &serde_json::Map<String, Value>, camel: &str) -> String {
    // Custom files may use mPrimary / primary.
    let mut prefixed = String::from("m");
    let mut chars = camel.chars();
    if let Some(first) = chars.next() {
        prefixed.push(first.to_ascii_uppercase());
        prefixed.extend(chars);
    }
    let from_prefixed = string_field(obj, &prefixed);
    if !from_prefixed.is_empty() {
        return from_prefixed;
    }
    string_field(obj, camel)
}

fn mode_preview_from_palette_obj(mode_json: &Value) -> ModePreview {
    let Some(obj) = mode_json.as_object() else {
        return ModePreview::default();
    };
    let mut accents = Vec::new();
    for field in ["primary", "secondary", "tertiary", "error"] {
        let color = mode_color_field(obj, field);
        if !color.is_empty() {
            accents.push(color);
        }
    }
    ModePreview {
        surface: mode_color_field(obj, "surface"),
        accents,
    }
}

fn parse_community_catalog(path: &Path) -> Vec<AvailablePalette> {
    let Ok(text) = fs::read_to_string(path) else {
        return Vec::new();
    };
    let Ok(root) = serde_json::from_str::<Value>(&text) else {
        return Vec::new();
    };
    let entries = if let Some(list) = root.as_array() {
        list
    } else if let Some(list) = root.get("palettes").and_then(Value::as_array) {
        list
    } else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for item in entries {
        let name = if let Some(text) = item.as_str() {
            text.to_string()
        } else {
            item.get("name")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string()
        };
        if name.is_empty() {
            continue;
        }
        out.push(AvailablePalette {
            name,
            dark: mode_preview_from_catalog(item, "dark"),
            light: mode_preview_from_catalog(item, "light"),
        });
    }
    out.sort_by_key(|a| a.name.to_lowercase());
    out.dedup_by(|a, b| a.name == b.name);
    out.truncate(MAX_COMMUNITY);
    out
}

fn load_custom(config_dir: &Path) -> Vec<AvailablePalette> {
    let dir = custom_palette_dir(config_dir);
    let Ok(entries) = fs::read_dir(&dir) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|ext| ext.to_str()) != Some("json") {
            continue;
        }
        let Some(name) = path.file_stem().and_then(|stem| stem.to_str()) else {
            continue;
        };
        let Ok(text) = fs::read_to_string(&path) else {
            continue;
        };
        let Ok(root) = serde_json::from_str::<Value>(&text) else {
            continue;
        };
        let dark = root
            .get("dark")
            .map(mode_preview_from_palette_obj)
            .unwrap_or_else(|| mode_preview_from_palette_obj(&root));
        let light = root
            .get("light")
            .map(mode_preview_from_palette_obj)
            .unwrap_or_else(|| dark.clone());
        out.push(AvailablePalette {
            name: name.to_string(),
            dark,
            light,
        });
    }
    out.sort_by_key(|a| a.name.to_lowercase());
    out.truncate(MAX_CUSTOM);
    out
}

fn mode_to_snapshot(mode: &ModePreview) -> Value {
    let surface = parse_hex(&mode.surface).unwrap_or(0);
    let accents: Vec<Value> = mode
        .accents
        .iter()
        .filter_map(|text| parse_hex(text).map(Value::from))
        .collect();
    json!({
        "surface": surface,
        "accents": accents,
    })
}

fn available_to_snapshot(source: &str, palette: &AvailablePalette) -> Value {
    json!({
        "source": source,
        "name": palette.name,
        "dark": mode_to_snapshot(&palette.dark),
        "light": mode_to_snapshot(&palette.light),
    })
}

/// Full `palette-previews` list: builtins, then community catalog, then custom files.
pub fn load_palette_previews(config_dir: &Path, settings_toml: &Path) -> Vec<Value> {
    let mut previews = builtin_palette_previews();
    let state_dir = state_dir_from_settings_toml(settings_toml);
    for palette in parse_community_catalog(&community_catalog_path(&state_dir)) {
        previews.push(available_to_snapshot("community", &palette));
    }
    for palette in load_custom(config_dir) {
        previews.push(available_to_snapshot("custom", &palette));
    }
    previews
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CatalogSync {
    /// No URL, offline mode, or nothing to do.
    Skipped,
    /// Catalog written (or overwritten) from the network.
    Fetched,
}

/// `communityPaletteDownloadUrl` — no default host (opt-in via env).
pub fn community_palette_download_url(name: &str) -> Option<String> {
    let base = std::env::var("HELIX_COMMUNITY_PALETTE_URL_BASE")
        .ok()
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())?;
    let base = base.trim_end_matches('/');
    Some(format!("{base}/{encoded}", encoded = url_encode(name)))
}

fn community_palette_cache_valid(path: &Path) -> bool {
    fs::read_to_string(path)
        .ok()
        .and_then(|text| parse_palette_json(&text))
        .is_some()
}

/// Ensure a community palette JSON exists under the state dir (download on miss when allowed).
/// Matches C++ `startCommunityDownload` for the blocking helper paths (`helix-theme-palette`, etc.).
pub fn ensure_community_palette_cached(state_dir: &Path, name: &str, offline: bool) -> bool {
    if name.is_empty() {
        return false;
    }
    let path = community_palette_cache_path(state_dir, name);
    if community_palette_cache_valid(&path) {
        return true;
    }
    if path.is_file() {
        let _ = fs::remove_file(&path);
    }
    if offline {
        tracing::info!(%name, "community palette cache miss; offline mode — not fetching");
        return false;
    }
    let url = match community_palette_download_url(name) {
        Some(url) => url,
        None => {
            tracing::warn!(
                %name,
                "community palette cache miss; HELIX_COMMUNITY_PALETTE_URL_BASE unset"
            );
            return false;
        }
    };
    if let Some(parent) = path.parent() {
        if let Err(error) = fs::create_dir_all(parent) {
            tracing::warn!(%error, %name, "community palette cache dir create failed");
            return false;
        }
    }
    tracing::info!(%url, %name, "fetching community palette");
    let response = match ureq::get(&url)
        .timeout(std::time::Duration::from_secs(30))
        .call()
    {
        Ok(response) => response,
        Err(error) => {
            tracing::warn!(%error, %name, "community palette download failed");
            return false;
        }
    };
    let body = match response.into_string() {
        Ok(body) => body,
        Err(error) => {
            tracing::warn!(%error, %name, "community palette read failed");
            return false;
        }
    };
    let tmp = path.with_extension("json.tmp");
    if fs::write(&tmp, body.as_bytes()).is_err() {
        return false;
    }
    if !community_palette_cache_valid(&tmp) {
        let _ = fs::remove_file(&tmp);
        tracing::warn!(%name, "community palette downloaded but failed to parse; removed cache");
        return false;
    }
    if fs::rename(&tmp, &path).is_err() {
        let _ = fs::remove_file(&tmp);
        return false;
    }
    true
}

/// Download the community catalog when `HELIX_COMMUNITY_PALETTE_CATALOG_URL` is set.
/// Matches C++: no default host; offline mode blocks the request.
pub fn sync_community_catalog(settings_toml: &Path, offline: bool) -> Result<CatalogSync, String> {
    if offline {
        tracing::info!("community palette catalog sync skipped (offline mode)");
        return Ok(CatalogSync::Skipped);
    }
    let Ok(url) = std::env::var("HELIX_COMMUNITY_PALETTE_CATALOG_URL") else {
        return Ok(CatalogSync::Skipped);
    };
    if url.trim().is_empty() {
        return Ok(CatalogSync::Skipped);
    }
    let state_dir = state_dir_from_settings_toml(settings_toml);
    let path = community_catalog_path(&state_dir);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    tracing::info!(%url, "fetching community palette catalog");
    let response = ureq::get(&url)
        .timeout(std::time::Duration::from_secs(15))
        .call()
        .map_err(|error| format!("community catalog download failed: {error}"))?;
    let body = response
        .into_string()
        .map_err(|error| format!("community catalog read failed: {error}"))?;
    let parsed: Value = serde_json::from_str(&body)
        .map_err(|error| format!("community catalog is not valid JSON: {error}"))?;
    let entries = if parsed.as_array().is_some() {
        Some(&parsed)
    } else {
        parsed.get("palettes")
    };
    if entries.and_then(Value::as_array).map(|list| list.is_empty()).unwrap_or(true) {
        return Err("community palette catalog contained no palettes".into());
    }
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, body.as_bytes()).map_err(|error| error.to_string())?;
    fs::rename(&tmp, &path).map_err(|error| error.to_string())?;
    Ok(CatalogSync::Fetched)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_root() -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("helix-palette-catalog-{nanos}"))
    }

    #[test]
    fn community_palette_download_url_encodes_name() {
        std::env::set_var("HELIX_COMMUNITY_PALETTE_URL_BASE", "https://example.com/palettes");
        let url = community_palette_download_url("Oxocarbon Dark").expect("url");
        assert_eq!(url, "https://example.com/palettes/Oxocarbon%20Dark");
        std::env::remove_var("HELIX_COMMUNITY_PALETTE_URL_BASE");
        assert!(community_palette_download_url("x").is_none());
    }

    #[test]
    fn ensure_community_palette_uses_valid_cache_without_fetch() {
        let root = temp_root();
        let state = root.join("state");
        fs::create_dir_all(state.join("community-palettes")).unwrap();
        let path = community_palette_cache_path(&state, "Cached");
        let json = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../core/helix-theme/tests/golden/apply/community-source/statefiles/community-palettes/Oxo%20lab.json"),
        )
        .expect("golden community palette");
        fs::write(&path, json).unwrap();
        assert!(ensure_community_palette_cached(&state, "Cached", false));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn community_catalog_and_custom_files_append_after_builtins() {
        let root = temp_root();
        let config = root.join("config");
        let state = root.join("state");
        fs::create_dir_all(config.join("palettes")).unwrap();
        fs::create_dir_all(state.join("community-palettes/.catalog")).unwrap();
        fs::write(
            state.join("community-palettes/.catalog/palettes.json"),
            r##"{"palettes":[{"name":"Oxocarbon","dark":{"surface":"#161616","primary":"#78a9ff","secondary":"#ee5396","tertiary":"#42be65"},"light":{"surface":"#ffffff","primary":"#0f62fe","secondary":"#da1e28","tertiary":"#198038"}}]}"##,
        )
        .unwrap();
        fs::write(
            config.join("palettes/MyLab.json"),
            r##"{"dark":{"mSurface":"#111111","mPrimary":"#ff0000","mSecondary":"#00ff00","mTertiary":"#0000ff"},"light":{"surface":"#eeeeee","primary":"#aa0000","secondary":"#00aa00","tertiary":"#0000aa"}}"##,
        )
        .unwrap();
        let settings = state.join("settings.toml");
        let previews = load_palette_previews(&config, &settings);
        assert!(previews.iter().any(|p| p["source"] == "builtin" && p["name"] == "Helix"));
        let community = previews
            .iter()
            .find(|p| p["source"] == "community" && p["name"] == "Oxocarbon")
            .expect("community");
        assert_eq!(community["dark"]["surface"], 0x161616);
        assert_eq!(community["dark"]["accents"][0], 0x78a9ff);
        let custom = previews
            .iter()
            .find(|p| p["source"] == "custom" && p["name"] == "MyLab")
            .expect("custom");
        assert_eq!(custom["dark"]["surface"], 0x111111);
        assert_eq!(custom["dark"]["accents"][0], 0xff0000);
        let _ = fs::remove_dir_all(root);
    }
}
