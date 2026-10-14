//! Theme template reapply for Rust `helix-desktop` (`TemplateApplyService` parity).

use std::path::Path;
use std::process::Command;

use helix_shell_core::desktop::compositor_ipc;
use helix_shell_core::ConfigStore;
use helix_theme::apply::{self, ApplyInputs, CommunityTemplate, Outcome};
use helix_theme::custom::{resolve_palette, PaletteSource, ThemeSelection};
use helix_theme::template::make_theme_data;
use helix_theme::transform::apply_resolved_transforms;

use crate::assets_path::{builtin_templates_root, helix_assets_dir, helix_template_apply_bin};
use crate::state::State;

const STATE_OWNER: &str = "theme_templates";
const STATE_APPLIED_IDS: &str = "applied_builtin_ids";

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
        _ => return None, // wallpaper palettes
    };
    let mode = crate::day_night::resolve_mode(config, text("theme.mode").as_deref().unwrap_or("dark"))?;

    let assets = helix_assets_dir()?;
    let builtin_config = builtin_templates_root(&assets).join("builtin.toml");
    let paths = config.paths();
    let palettes_dir = paths.config_dir.join("palettes");
    let state_dir = paths.settings_toml.parent()?.to_path_buf();
    let community_name = text("theme.community_palette").unwrap_or_default();
    let selection = ThemeSelection {
        source,
        builtin: text("theme.builtin").unwrap_or_else(|| "Helix".into()),
        custom_palette: text("theme.custom_palette").unwrap_or_default(),
        community_cache: (source == PaletteSource::Community && !community_name.is_empty())
            .then(|| helix_theme::custom::community_palette_cache_path(&state_dir, &community_name)),
        shell_light: false,
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
        image_path: None,
        config_table: None,
        icon_theme: String::new(),
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
    };

    match apply::apply(&inputs) {
        Outcome::Unsupported(why) => {
            tracing::debug!(%why, "in-process template apply unsupported; using helper");
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

    // Like a successful helper run, an in-process apply does not reload niri itself; the
    // templates' own hooks do that.
    if let Some(result) = apply_in_process(state) {
        return result;
    }

    let config = &state.config;
    if run_cpp_template_apply()? {
        state.runtime.reload();
        return Ok(());
    }

    if std::env::var_os("NIRI_SOCKET").is_some() {
        compositor_ipc::reload_config();
    }

    run_builtin_post_hooks(config)?;

    Ok(())
}

fn run_cpp_template_apply() -> Result<bool, String> {
    let Some(bin) = helix_template_apply_bin() else {
        return Ok(false);
    };
    let mut command = Command::new(&bin);
    if let Some(assets) = helix_assets_dir() {
        command.env("HELIX_ASSETS_DIR", assets);
    }
    let output = command.output().map_err(|error| error.to_string())?;
    if output.status.success() {
        return Ok(true);
    }
    let stderr = String::from_utf8_lossy(&output.stderr);
    let message = stderr.trim();
    if message.is_empty() {
        Err("template apply failed".into())
    } else {
        Err(message.lines().last().unwrap_or(message).to_string())
    }
}

fn enabled_builtin_template_ids(config: &ConfigStore) -> Vec<String> {
    if !config
        .get("theme.templates.enable_builtin_templates")
        .as_bool()
        .unwrap_or(true)
    {
        return Vec::new();
    }
    config
        .get_raw_list("theme.templates.builtin_ids")
        .and_then(|value| value.as_array().cloned())
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

fn run_script(apply_sh: &Path) -> Result<(), String> {
    let status = Command::new("bash")
        .arg(apply_sh)
        .arg("apply")
        .status()
        .map_err(|error| error.to_string())?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("template hook failed: {}", apply_sh.display()))
    }
}

/// Runs shipped `assets/templates/<id>/apply.sh apply` for enabled built-in template ids.
fn run_builtin_post_hooks(config: &ConfigStore) -> Result<(), String> {
    let Some(assets) = helix_assets_dir() else {
        return Ok(());
    };
    let root = builtin_templates_root(&assets);
    let mut failures = Vec::new();
    for id in enabled_builtin_template_ids(config) {
        let script = root.join(&id).join("apply.sh");
        if !script.is_file() {
            continue;
        }
        if let Err(error) = run_script(&script) {
            tracing::warn!(template = %id, %error, "builtin template post_hook failed");
            failures.push(id);
        }
    }
    if failures.is_empty() {
        Ok(())
    } else {
        Err(format!(
            "failed to apply built-in templates: {}",
            failures.join(", ")
        ))
    }
}
