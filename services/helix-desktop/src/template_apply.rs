//! Theme template reapply for Rust `helix-desktop` (`TemplateApplyService` parity).

use std::process::Command;

use helix_shell_core::desktop::settings::active_icon_theme_name;
use helix_theme::apply::{self, ApplyInputs, CommunityTemplate, Outcome};
use helix_theme::custom::{resolve_palette, PaletteSource, ThemeSelection};
use helix_theme::template::make_theme_data;
use helix_theme::transform::apply_resolved_transforms;

use crate::assets_path::{builtin_templates_root, helix_assets_dir};
use crate::state::State;

const STATE_OWNER: &str = "theme_templates";
const STATE_APPLIED_IDS: &str = "applied_builtin_ids";

/// `org.kde.KGlobalSettings.notifyChange(0, 0)` on the session bus, like the C++ helper.
fn notify_kde_scheme_changed() -> Option<String> {
    let send = || -> zbus::Result<()> {
        let connection = zbus::blocking::Connection::session()?;
        connection.emit_signal(
            None::<&str>,
            "/KGlobalSettings",
            "org.kde.KGlobalSettings",
            "notifyChange",
            &(0i32, 0i32),
        )
    };
    crate::blocking::run(send).err().map(|error| error.to_string())
}

/// Applies templates in-process when every input is something `helix-theme` reproduces
/// exactly. `None` means "use the C++ helper"; nothing has been written or run in that case.
fn apply_in_process(state: &mut State) -> Option<Result<(), String>> {
    let config = &state.config;
    let text = |id: &str| config.get(id).as_str().map(str::to_string);
    let flag = |id: &str| config.get(id).as_bool().unwrap_or(false);

    let source = match text("theme.source").as_deref() {
        Some("builtin") | None => PaletteSource::Builtin,
        Some("custom") => PaletteSource::Custom,
        Some("community") => PaletteSource::Community,
        Some("wallpaper") => PaletteSource::Wallpaper,
        _ => return None,
    };
    let session_wallpaper = state.wallpaper.clone();
    // A wallpaper that cannot be turned into a palette is the helper's to handle.
    let wallpaper = if source == PaletteSource::Wallpaper {
        Some(crate::wallpaper_source::wallpaper_palette(config, &session_wallpaper)?)
    } else {
        None
    };
    let mode = crate::day_night::resolved_app_mode(config)?;
    let shell_light = crate::day_night::shell_palette_light(config)?;

    let assets = helix_assets_dir()?;
    let builtin_config = builtin_templates_root(&assets).join("builtin.toml");
    let paths = config.paths();
    let palettes_dir = paths.config_dir.join("palettes");
    let state_dir = paths.settings_toml.parent()?.to_path_buf();
    let community_name = text("theme.community_palette").unwrap_or_default();
    let offline = flag("shell.offline_mode");
    if source == PaletteSource::Community {
        if !crate::palette_catalog::ensure_community_palette_cached(&state_dir, &community_name, offline) {
            return None;
        }
    }
    let selection = ThemeSelection {
        source,
        builtin: text("theme.builtin").unwrap_or_else(|| "Helix".into()),
        custom_palette: text("theme.custom_palette").unwrap_or_default(),
        community_cache: (source == PaletteSource::Community && !community_name.is_empty())
            .then(|| helix_theme::custom::community_palette_cache_path(&state_dir, &community_name)),
        shell_light,
        wallpaper,
    };
    let mut generated = resolve_palette(&palettes_dir, &selection)?;
    apply_resolved_transforms(
        &mut generated.dark,
        &mut generated.light,
        flag("theme.pure_black_dark"),
        flag("accessibility.high_contrast"),
    );
    let scheme = text("theme.wallpaper_scheme").unwrap_or_else(|| "m3-content".into());
    let scheme_type = scheme.strip_prefix("m3-").unwrap_or(&scheme).to_string();

    let list = |id: &str| -> Vec<String> {
        config
            .get_raw_list(id)
            .and_then(|v| v.as_array().cloned())
            .map(|items| items.iter().filter_map(|i| i.as_str().map(str::to_string)).collect())
            .unwrap_or_default()
    };

    // User templates / custom colors, mapped exactly like C++ `buildUserTemplateRoot`.
    let mut templates_cfg = toml::Table::new();
    for key in ["user", "custom_colors"] {
        if let Some(value) = config.get_json_value(&format!("theme.templates.{key}")) {
            if let Ok(toml::Value::Table(table)) = toml::Value::try_from(&value) {
                templates_cfg.insert(key.to_string(), toml::Value::Table(table));
            }
        }
    }
    let user_root = apply::build_user_template_root(&templates_cfg);
    let user_root = (!user_root.is_empty()).then(|| (user_root, paths.config_dir.join("config.toml")));

    let applied_before = state
        .runtime
        .get_string(STATE_OWNER, STATE_APPLIED_IDS)
        .map(|s| s.split_whitespace().map(str::to_string).collect())
        .unwrap_or_default();

    let inputs = ApplyInputs {
        theme_data: make_theme_data(&generated.dark, &generated.light),
        default_mode: mode.to_string(),
        scheme_type,
        image_path: {
            let path = crate::wallpaper_source::palette_wallpaper_path(config, &session_wallpaper);
            if path.is_empty() { None } else { Some(path) }
        },
        config_table: Some(crate::config_export::template_config_json(config)),
        icon_theme: active_icon_theme_name(),
        builtin_config,
        enable_builtin: config.get("theme.templates.enable_builtin_templates").as_bool().unwrap_or(true),
        builtin_ids: list("theme.templates.builtin_ids"),
        enable_community: config.get("theme.templates.enable_community_templates").as_bool().unwrap_or(true),
        community: list("theme.templates.community_ids")
            .into_iter()
            .map(|id| CommunityTemplate {
                config_path: state_dir.join("community-templates").join(&id).join("template.toml"),
                id,
            })
            .collect(),
        applied_before,
        user_root,
        notify_kde: Some(notify_kde_scheme_changed),
    };

    match apply::apply(&inputs) {
        Outcome::Unsupported(why) => {
            tracing::info!(%why, "template apply: in-process unsupported; using helix-template-apply");
            None
        }
        Outcome::Applied(report) => {
            if let Err(error) = state.runtime.set_string(
                STATE_OWNER,
                STATE_APPLIED_IDS,
                &report.applied_builtin_ids.join(" "),
            ) {
                tracing::warn!(%error, "failed to persist applied built-in template ids");
            }
            if let Some((id, error)) = report.failures.iter().next_back() {
                return Some(Err(format!("template failure: {id}: {error}")));
            }
            Some(Ok(()))
        }
    }
}

/// Shared path for `Appearance1.Reapply` and `templates-apply` msg IPC.
pub fn reapply_templates(state: &mut State) -> Result<(), String> {
    if let Ok(cmd) = std::env::var("HELIX_TEMPLATE_APPLY_CMD") {
        if !cmd.trim().is_empty() {
            let status = Command::new("sh")
                .arg("-lc")
                .arg(&cmd)
                .status()
                .map_err(|error| error.to_string())?;
            if !status.success() {
                return Err("template apply command failed".into());
            }
            return Ok(());
        }
    }

    match apply_in_process(state) {
        Some(result) => result,
        None => Err(
            "Template apply is not supported for this theme configuration in-process".into(),
        ),
    }
}

#[cfg(test)]
mod apply_integration_tests {
    use std::fs;
    use std::path::Path;
    use std::time::{SystemTime, UNIX_EPOCH};

    use super::reapply_templates;
    use crate::state::State;

    fn copy_tree(from: &Path, to: &Path) {
        for entry in fs::read_dir(from).unwrap().flatten() {
            let dest = to.join(entry.file_name());
            if entry.file_type().unwrap().is_dir() {
                fs::create_dir_all(&dest).unwrap();
                copy_tree(&entry.path(), &dest);
            } else {
                if let Some(parent) = dest.parent() {
                    fs::create_dir_all(parent).unwrap();
                }
                fs::copy(entry.path(), dest).unwrap();
            }
        }
    }

    fn temp_home(tag: &str) -> std::path::PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let home = std::env::temp_dir().join(format!("helix-desktop-{tag}-{nanos}"));
        let _ = fs::remove_dir_all(&home);
        for sub in ["cfg/helix", "state/helix", "data", "cache"] {
            fs::create_dir_all(home.join(sub)).unwrap();
        }
        home
    }

    fn apply_test_env(home: &Path, assets: &Path) {
        unsafe {
            std::env::remove_var("HELIX_CONFIG_HOME");
            std::env::remove_var("HELIX_STATE_HOME");
            std::env::remove_var("HELIX_TEMPLATE_APPLY_CMD");
            std::env::set_var("HOME", home);
            std::env::set_var("XDG_CONFIG_HOME", home.join("cfg"));
            std::env::set_var("XDG_STATE_HOME", home.join("state"));
            std::env::set_var("XDG_DATA_HOME", home.join("data"));
            std::env::set_var("XDG_CACHE_HOME", home.join("cache"));
            std::env::set_var("HELIX_ASSETS_DIR", assets);
        }
    }

    fn repo_root() -> std::path::PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
    }

    #[test]
    fn wallpaper_source_applies_in_process_without_cpp_helper() {
        let _guard = crate::testing::ENV_LOCK.lock().unwrap();
        let repo = repo_root();
        let scenario = repo.join("core/helix-theme/tests/golden/apply/wallpaper-content");
        let assets = repo.join("assets");
        assert!(
            assets.join("templates/builtin.toml").is_file(),
            "missing shipped assets for template apply"
        );

        let home = temp_home("wallpaper-apply");
        fs::write(
            home.join("cfg/helix/config.toml"),
            r#"[theme]
source = "wallpaper"
wallpaper_scheme = "m3-content"
mode = "dark"

[theme.templates]
builtin_ids = ["helix", "qt"]
"#,
        )
        .unwrap();
        if scenario.join("files").is_dir() {
            copy_tree(&scenario.join("files"), &home.join("cfg"));
        }
        if scenario.join("statefiles").is_dir() {
            copy_tree(&scenario.join("statefiles"), &home.join("state/helix"));
        }

        apply_test_env(&home, &assets);

        let mut state = State::new(crate::agents::new_hub());
        reapply_templates(&mut state).expect("wallpaper-source apply should stay in-process");
        let theme_out = home.join("cfg/helix/themes/helix.toml");
        assert!(
            theme_out.is_file() && fs::metadata(&theme_out).unwrap().len() > 0,
            "expected helix theme output at {}",
            theme_out.display()
        );
    }

    #[test]
    fn community_source_applies_in_process_without_cpp_helper() {
        let _guard = crate::testing::ENV_LOCK.lock().unwrap();
        let repo = repo_root();
        let scenario = repo.join("core/helix-theme/tests/golden/apply/community-source");
        let assets = repo.join("assets");
        assert!(assets.join("templates/builtin.toml").is_file());

        let home = temp_home("community-apply");
        fs::write(
            home.join("cfg/helix/config.toml"),
            r#"[theme]
source = "community"
community_palette = "Oxo lab"
mode = "dark"

[theme.templates]
builtin_ids = ["helix", "qt"]
"#,
        )
        .unwrap();
        let cache_src = scenario.join("statefiles/community-palettes");
        if cache_src.is_dir() {
            copy_tree(&cache_src, &home.join("state/helix/community-palettes"));
        }

        apply_test_env(&home, &assets);

        let mut state = State::new(crate::agents::new_hub());
        reapply_templates(&mut state).expect("community-source apply should stay in-process");
        let theme_out = home.join("cfg/helix/themes/helix.toml");
        assert!(theme_out.is_file() && fs::metadata(&theme_out).unwrap().len() > 0);
    }
}
