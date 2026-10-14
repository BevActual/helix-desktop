//! Theme template reapply for Rust `helix-desktop` (`TemplateApplyService` parity).

use std::path::Path;
use std::process::Command;

use helix_shell_core::desktop::compositor_ipc;
use helix_shell_core::ConfigStore;

use crate::assets_path::{builtin_templates_root, helix_assets_dir, helix_template_apply_bin};

/// Shared path for `Appearance1.Reapply` and `templates-apply` msg IPC.
pub fn reapply_templates(config: &ConfigStore) -> Result<(), String> {
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

    if run_cpp_template_apply()? {
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
