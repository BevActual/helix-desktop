//! Settings → Top bar: which parts of the bar are shown. The choices are `~/.config/helix/bar-settings.json` (`helix_config::bar_prefs`), which the
//! shell watches and applies at once. The two buttons that open the control center and the notifications are not on the list, so the bar can never be
//! left with no way to reach them.

use helix_config::bar_prefs::{BarPrefs, ITEMS};
use helix_config::catalog::{Control, RowView};
use serde_json::Value;

pub fn file() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config"))
        })?;
    Some(base.join("helix/bar-settings.json"))
}

pub fn read() -> BarPrefs {
    file()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .map(|text| BarPrefs::parse(&text))
        .unwrap_or_default()
}

pub fn write(prefs: &BarPrefs) -> Result<(), String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let path = file().ok_or("no folder for the settings")?;
    std::fs::create_dir_all(path.parent().ok_or("no folder")?).map_err(|e| e.to_string())?;
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    {
        let mut out = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|e| e.to_string())?;
        out.write_all(prefs.to_json().as_bytes())
            .map_err(|e| e.to_string())?;
    }
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

pub fn rows(prefs: &BarPrefs) -> Vec<RowView> {
    ITEMS
        .iter()
        .map(|item| RowView {
            id: format!("bar:{}", item.key),
            title: item.title.into(),
            group: item.group.into(),
            description: item.about.into(),
            overridden: false,
            control: Control::Switch {
                on: prefs.shown(item.key),
            },
        })
        .collect()
}

/// The choices after one change (`bar:<item>` and a switch).
pub fn change(prefs: &BarPrefs, id: &str, change: &Value) -> Result<BarPrefs, String> {
    let key = id
        .strip_prefix("bar:")
        .filter(|key| ITEMS.iter().any(|item| item.key == *key))
        .ok_or("unknown change")?;
    if change.get("kind").and_then(Value::as_str) != Some("switch") {
        return Err("unknown change".into());
    }
    let on = change
        .get("value")
        .and_then(Value::as_bool)
        .ok_or("not a boolean")?;
    Ok(prefs.with(key, on))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn a_row_per_item_and_a_change_is_one_switch() {
        let prefs = BarPrefs::default();
        let rows = rows(&prefs);
        assert_eq!(rows.len(), ITEMS.len());
        assert!(matches!(rows[0].control, Control::Switch { on: true }));
        let next = change(
            &prefs,
            "bar:date",
            &json!({"kind": "switch", "value": false}),
        )
        .unwrap();
        assert!(!next.shown("date") && next.shown("apps"));
        assert!(change(
            &prefs,
            "bar:nothing",
            &json!({"kind": "switch", "value": false})
        )
        .is_err());
        assert!(change(&prefs, "bar:date", &json!({"kind": "number", "value": 1})).is_err());
        assert!(change(
            &prefs,
            "bar:date",
            &json!({"kind": "switch", "value": "no"})
        )
        .is_err());
    }
}
