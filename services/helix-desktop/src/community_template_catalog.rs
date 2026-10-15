//! Community app-template catalog download (C++ `CommunityTemplateService::refreshCatalog`).
//! Opt-in via `HELIX_COMMUNITY_TEMPLATE_CATALOG_URL`; cache at `state/community-templates/catalog.json`.

use std::fs;
use std::path::{Path, PathBuf};

use serde_json::Value;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CatalogSync {
    Skipped,
    Fetched,
}

fn state_dir_from_settings_toml(settings_toml: &Path) -> PathBuf {
    settings_toml
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."))
}

pub fn catalog_cache_path(state_dir: &Path) -> PathBuf {
    state_dir.join("community-templates").join("catalog.json")
}

fn catalog_has_templates(body: &str) -> Result<(), String> {
    let root: Value = serde_json::from_str(body)
        .map_err(|error| format!("community template catalog is not valid JSON: {error}"))?;
    let entries = root
        .as_array()
        .or_else(|| root.get("templates").and_then(Value::as_array));
    let Some(items) = entries else {
        return Err("community template catalog has no templates array".into());
    };
    if items.is_empty() {
        return Err("community template catalog contained no templates".into());
    }
    for item in items {
        let id = item
            .get("id")
            .or_else(|| item.get("name"))
            .and_then(Value::as_str)
            .filter(|text| !text.is_empty());
        if id.is_none() {
            return Err("community template catalog entry missing id".into());
        }
    }
    Ok(())
}

/// Download the community template catalog when `HELIX_COMMUNITY_TEMPLATE_CATALOG_URL` is set.
pub fn sync_community_template_catalog(
    settings_toml: &Path,
    offline: bool,
) -> Result<CatalogSync, String> {
    if offline {
        tracing::info!("community template catalog sync skipped (offline mode)");
        return Ok(CatalogSync::Skipped);
    }
    let Ok(url) = std::env::var("HELIX_COMMUNITY_TEMPLATE_CATALOG_URL") else {
        return Ok(CatalogSync::Skipped);
    };
    if url.trim().is_empty() {
        return Ok(CatalogSync::Skipped);
    }
    let state_dir = state_dir_from_settings_toml(settings_toml);
    let path = catalog_cache_path(&state_dir);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    tracing::info!(%url, "fetching community template catalog");
    let response = ureq::get(&url)
        .timeout(std::time::Duration::from_secs(30))
        .call()
        .map_err(|error| format!("community template catalog download failed: {error}"))?;
    let body = response
        .into_string()
        .map_err(|error| format!("community template catalog read failed: {error}"))?;
    catalog_has_templates(&body)?;
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, body.as_bytes()).map_err(|error| error.to_string())?;
    fs::rename(&tmp, &path).map_err(|error| error.to_string())?;
    Ok(CatalogSync::Fetched)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn catalog_has_templates_rejects_empty() {
        assert!(catalog_has_templates("[]").is_err());
        assert!(catalog_has_templates(r#"{"templates":[]}"#).is_err());
    }

    #[test]
    fn catalog_has_templates_accepts_object_list() {
        catalog_has_templates(r#"[{"id":"firefox","display_name":"Firefox"}]"#).unwrap();
        catalog_has_templates(r#"{"templates":[{"name":"vscode"}]}"#).unwrap();
    }

    #[test]
    fn sync_skips_without_url() {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let settings = std::env::temp_dir()
            .join(format!("helix-template-catalog-{nanos}"))
            .join("settings.toml");
        std::fs::create_dir_all(settings.parent().unwrap()).unwrap();
        std::fs::write(&settings, "").unwrap();
        assert_eq!(
            sync_community_template_catalog(&settings, false).unwrap(),
            CatalogSync::Skipped
        );
    }
}
