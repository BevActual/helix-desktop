//! Template application (`TemplateEngine::processConfigTemplates`, `TemplateApplyService`
//! one-shot path, `HookRunner`).
//!
//! Work is split into a side-effect-free *plan* (render every template, resolve paths, run
//! `*_path_dynamic` queries) and an *execute* phase (write files, run hooks). Anything this
//! implementation cannot reproduce exactly is reported as [`Outcome::Unsupported`] during
//! planning, before a single file is written or hook run, so callers can fall back to the
//! C++ helper without duplicating side effects.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Condvar, Mutex};

use serde_json::Value as Json;
use toml::{Table, Value};

use crate::color::Color;
use crate::hct::{Hct, TonalPalette};
use crate::template::{render, Options, ThemeData};

/// `HookRunner::kDefaultMaxConcurrent`.
const MAX_CONCURRENT_HOOKS: usize = 4;

#[derive(Clone, Debug)]
pub struct CommunityTemplate {
    pub id: String,
    /// `communityTemplateConfigPath(id)`; the file may not exist yet.
    pub config_path: PathBuf,
}

#[derive(Clone, Debug)]
pub struct ApplyInputs {
    pub theme_data: ThemeData,
    pub default_mode: String,
    /// `[theme].wallpaper_scheme` minus any `m3-` prefix (`content`, `tonal-spot`, ...).
    pub scheme_type: String,
    /// Wallpaper path for `{{ image }}`; when `None`, templates that read it are unsupported.
    pub image_path: Option<String>,
    /// `config.*` table; when `None`, templates that read it make the apply unsupported.
    pub config_table: Option<Json>,
    pub icon_theme: String,
    /// `assets/templates/builtin.toml`.
    pub builtin_config: PathBuf,
    pub enable_builtin: bool,
    pub builtin_ids: Vec<String>,
    pub enable_community: bool,
    pub community: Vec<CommunityTemplate>,
    /// Ids persisted as applied by the previous run (`[theme_templates] applied_builtin_ids`).
    pub applied_before: Vec<String>,
    /// Pre-built user template root (`[config]` / `[templates]`) and the config path it came from.
    pub user_root: Option<(Table, PathBuf)>,
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct Report {
    /// Template id → short error (first line, as the shell shows it).
    pub failures: BTreeMap<String, String>,
    /// Value to persist as `applied_builtin_ids`.
    pub applied_builtin_ids: Vec<String>,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Outcome {
    Applied(Report),
    /// Planning hit a feature that is not ported; nothing was written or run.
    Unsupported(String),
}

// ---- helpers ------------------------------------------------------------------------

fn trim(s: &str) -> &str {
    s.trim_matches(|c| matches!(c, ' ' | '\t' | '\n' | '\r' | '\x0b' | '\x0c'))
}

pub fn is_safe_template_id(id: &str) -> bool {
    !id.is_empty() && id.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.'))
}

fn expand_xdg_base_dir(path: &str) -> String {
    const BASES: [(&str, &str, &str); 4] = [
        ("$XDG_CONFIG_HOME", "XDG_CONFIG_HOME", ".config"),
        ("$XDG_DATA_HOME", "XDG_DATA_HOME", ".local/share"),
        ("$XDG_STATE_HOME", "XDG_STATE_HOME", ".local/state"),
        ("$XDG_CACHE_HOME", "XDG_CACHE_HOME", ".cache"),
    ];
    for (token, env, home_default) in BASES {
        let Some(rest) = path.strip_prefix(token) else { continue };
        if !rest.is_empty() && !rest.starts_with('/') {
            continue;
        }
        let base = match std::env::var(env) {
            Ok(v) if !v.is_empty() => v,
            _ => match std::env::var("HOME") {
                Ok(h) if !h.is_empty() => format!("{h}/{home_default}"),
                _ => return path.to_string(),
            },
        };
        return format!("{base}{rest}");
    }
    path.to_string()
}

fn expand_user_path(path: &str) -> PathBuf {
    if !path.starts_with('~') {
        return PathBuf::from(path);
    }
    let Ok(home) = std::env::var("HOME") else { return PathBuf::from(path) };
    if home.is_empty() {
        return PathBuf::from(path);
    }
    if path.len() == 1 {
        return PathBuf::from(home);
    }
    match path.strip_prefix("~/") {
        Some(rest) => PathBuf::from(home).join(rest),
        None => PathBuf::from(path),
    }
}

fn resolve_config_path(config_path: &Path, path: &str) -> PathBuf {
    let expanded = expand_user_path(&expand_xdg_base_dir(path));
    if expanded.is_absolute() {
        return expanded;
    }
    match config_path.parent() {
        Some(base) if !base.as_os_str().is_empty() => base.join(expanded),
        _ => expanded,
    }
}

fn parent_string(path: &Path) -> String {
    path.parent().map(|p| p.to_string_lossy().into_owned()).unwrap_or_default()
}

// ---- Lab nearest colour (compare_to / colors_to_compare) -----------------------------

fn lab(c: Color) -> (f64, f64, f64) {
    fn linearize(c: i32) -> f64 {
        let n = f64::from(c) / 255.0;
        if n <= 0.04045 {
            n / 12.92
        } else {
            ((n + 0.055) / 1.055).powf(2.4)
        }
    }
    fn f(t: f64) -> f64 {
        if t > 0.008856 {
            t.cbrt()
        } else {
            (903.3 * t + 16.0) / 116.0
        }
    }
    let (lr, lg, lb) = (linearize(c.r), linearize(c.g), linearize(c.b));
    let x = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) * 100.0;
    let y = (0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb) * 100.0;
    let z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) * 100.0;
    let (fx, fy, fz) = (f(x / 95.047), f(y / 100.0), f(z / 108.883));
    (116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz))
}

fn find_closest_color(compare_to: &str, colors: &[(String, String)]) -> String {
    if colors.is_empty() {
        return String::new();
    }
    let Some(target) = Color::from_hex(compare_to).map(lab) else { return String::new() };
    let mut closest = String::new();
    let mut best = f64::INFINITY;
    for (name, color) in colors {
        let Some(c) = Color::from_hex(color).map(lab) else { continue };
        let d = ((target.0 - c.0).powi(2) + (target.1 - c.1).powi(2) + (target.2 - c.2).powi(2)).sqrt();
        if d < best {
            best = d;
            closest = name.clone();
        }
    }
    closest
}

// ---- client config root gating --------------------------------------------------------

fn infer_client_config_root(output: &Path) -> Option<PathBuf> {
    let parts: Vec<&std::ffi::OsStr> = output
        .components()
        .map(|c| c.as_os_str())
        .filter(|p| !p.is_empty() && *p != ".")
        .collect();
    let n = parts.len();
    if n >= 4 {
        for i in 0..n - 3 {
            if parts[i] == ".var" && parts[i + 1] == "app" && parts[i + 3] == "config" {
                let mut root = PathBuf::new();
                for p in &parts[..=i + 3] {
                    root.push(p);
                }
                return Some(root);
            }
        }
    }
    let mut current = output.parent().map(Path::to_path_buf);
    while let Some(dir) = current {
        if dir.as_os_str().is_empty() || dir.parent().is_none() {
            break;
        }
        if dir.file_name().is_some_and(|f| f == "themes") {
            let parent = dir.parent().map(Path::to_path_buf).unwrap_or_default();
            let grandparent = parent.parent().map(Path::to_path_buf).unwrap_or_default();
            let named = |p: &Path, n: &str| p.file_name().is_some_and(|f| f == n);
            if named(&parent, "extensions") || named(&grandparent, "extensions") {
                let mut walk = Some(dir.clone());
                while let Some(w) = &walk {
                    if w.as_os_str().is_empty() || named(w, "extensions") {
                        break;
                    }
                    walk = w.parent().map(Path::to_path_buf);
                }
                if let Some(w) = walk {
                    if named(&w, "extensions") {
                        return w.parent().map(Path::to_path_buf);
                    }
                }
            }
            return Some(parent);
        }
        current = dir.parent().map(Path::to_path_buf);
    }
    None
}

// ---- entry parsing ----------------------------------------------------------------------

#[derive(Debug, Clone)]
struct Entry {
    name: String,
    input_path: String,
    input_path_dynamic: String,
    output_paths: Vec<String>,
    output_path_dynamic: String,
    compare_to: String,
    colors_to_compare: Vec<(String, String)>,
    pre_hook: String,
    post_hook: String,
    post_action: String,
    requires_path: String,
    gate_by_client_root: bool,
    index: i64,
    hook_async: bool,
}

fn str_field(tpl: &Table, key: &str) -> Option<String> {
    tpl.get(key).and_then(Value::as_str).map(str::to_string)
}

fn parse_entry(config_path: &Path, name: &str, tpl: &Table, default_mode: &str) -> Option<Entry> {
    let mut input_path = String::new();
    let modes = tpl.get("input_path_modes").and_then(Value::as_table).and_then(|t| {
        Some((t.get("dark")?.as_str()?.to_string(), t.get("light")?.as_str()?.to_string()))
    });
    if let Some((dark, light)) = modes {
        input_path = if default_mode == "light" { light } else { dark };
    } else if let Some(p) = str_field(tpl, "input_path") {
        input_path = p;
    }
    let input_path_dynamic = str_field(tpl, "input_path_dynamic").unwrap_or_default();
    if input_path.is_empty() && input_path_dynamic.is_empty() {
        return None;
    }

    let resolve = |p: &str| resolve_config_path(config_path, p).to_string_lossy().into_owned();
    let output_paths = match tpl.get("output_path") {
        Some(Value::String(s)) => vec![resolve(s)],
        Some(Value::Array(a)) => a.iter().filter_map(Value::as_str).map(resolve).collect(),
        _ => vec![],
    };
    let colors_to_compare = tpl
        .get("colors_to_compare")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_table)
                .filter_map(|t| Some((t.get("name")?.as_str()?.to_string(), t.get("color")?.as_str()?.to_string())))
                .collect()
        })
        .unwrap_or_default();

    Some(Entry {
        name: name.to_string(),
        input_path: if input_path.is_empty() { String::new() } else { resolve(&input_path) },
        input_path_dynamic,
        output_paths,
        output_path_dynamic: str_field(tpl, "output_path_dynamic").unwrap_or_default(),
        compare_to: str_field(tpl, "compare_to").unwrap_or_default(),
        colors_to_compare,
        pre_hook: str_field(tpl, "pre_hook").unwrap_or_default(),
        post_hook: str_field(tpl, "post_hook").unwrap_or_default(),
        post_action: str_field(tpl, "post_action").unwrap_or_default(),
        requires_path: str_field(tpl, "requires_path").map(|p| resolve(&p)).unwrap_or_default(),
        gate_by_client_root: false,
        index: tpl.get("index").and_then(Value::as_integer).unwrap_or(0),
        hook_async: tpl.get("hook_async").and_then(Value::as_bool).unwrap_or(true),
    })
}

fn mark_multi_client_gated(entries: &mut [Entry]) {
    let mut roots: HashMap<String, HashSet<String>> = HashMap::new();
    for e in entries.iter() {
        if e.input_path.is_empty() {
            continue;
        }
        for out in &e.output_paths {
            if let Some(root) = infer_client_config_root(Path::new(out)) {
                roots.entry(e.input_path.clone()).or_default().insert(root.to_string_lossy().into_owned());
            }
        }
    }
    for e in entries.iter_mut() {
        if e.input_path.is_empty() {
            continue;
        }
        if roots.get(&e.input_path).is_some_and(|r| r.len() > 1) {
            e.gate_by_client_root = true;
        }
    }
}

fn should_skip_output(e: &Entry, output: &str) -> bool {
    if !e.requires_path.is_empty() {
        return !Path::new(&e.requires_path).exists();
    }
    if !e.gate_by_client_root {
        return false;
    }
    match infer_client_config_root(Path::new(output)) {
        Some(root) => !root.exists(),
        None => false,
    }
}

/// C++ `buildUserTemplateRoot` over a `[theme.templates]` table: only the fields the
/// config schema carries survive (`requires_path` is not one of them), disabled entries are
/// dropped and single outputs collapse to a string.
pub fn build_user_template_root(templates_cfg: &Table) -> Table {
    let mut root = Table::new();
    if let Some(colors) = templates_cfg.get("custom_colors").and_then(Value::as_table) {
        if !colors.is_empty() {
            let mut config = Table::new();
            config.insert("custom_colors".into(), Value::Table(colors.clone()));
            root.insert("config".into(), Value::Table(config));
        }
    }
    let mut user_templates = Table::new();
    if let Some(user) = templates_cfg.get("user").and_then(Value::as_table) {
        for (id, node) in user {
            let Some(src) = node.as_table() else { continue };
            if src.get("enabled").and_then(Value::as_bool) == Some(false) {
                continue;
            }
            let mut t = Table::new();
            let modes = src.get("input_path_modes").and_then(Value::as_table).and_then(|m| {
                Some((m.get("dark")?.as_str()?.to_string(), m.get("light")?.as_str()?.to_string()))
            });
            if let Some((dark, light)) = modes {
                let mut m = Table::new();
                m.insert("dark".into(), Value::String(dark));
                m.insert("light".into(), Value::String(light));
                t.insert("input_path_modes".into(), Value::Table(m));
            } else if let Some(p) = src.get("input_path").and_then(Value::as_str).filter(|p| !p.is_empty()) {
                t.insert("input_path".into(), Value::String(p.to_string()));
            }
            let outputs: Vec<String> = match src.get("output_path") {
                Some(Value::String(s)) => vec![s.clone()],
                Some(Value::Array(a)) => a.iter().filter_map(Value::as_str).map(str::to_string).collect(),
                _ => vec![],
            };
            match outputs.len() {
                0 => {}
                1 => {
                    t.insert("output_path".into(), Value::String(outputs[0].clone()));
                }
                _ => {
                    t.insert(
                        "output_path".into(),
                        Value::Array(outputs.into_iter().map(Value::String).collect()),
                    );
                }
            }
            for key in ["output_path_dynamic", "compare_to", "pre_hook", "post_hook", "post_action"] {
                if let Some(v) = src.get(key).and_then(Value::as_str).filter(|v| !v.is_empty()) {
                    t.insert(key.into(), Value::String(v.to_string()));
                }
            }
            if let Some(arr) = src.get("colors_to_compare").and_then(Value::as_array) {
                let colors: Vec<Value> = arr
                    .iter()
                    .filter_map(Value::as_table)
                    .filter_map(|c| {
                        let mut out = Table::new();
                        out.insert("name".into(), Value::String(c.get("name")?.as_str()?.to_string()));
                        out.insert("color".into(), Value::String(c.get("color")?.as_str()?.to_string()));
                        Some(Value::Table(out))
                    })
                    .collect();
                if !colors.is_empty() {
                    t.insert("colors_to_compare".into(), Value::Array(colors));
                }
            }
            if let Some(i) = src.get("index").and_then(Value::as_integer).filter(|i| *i != 0) {
                t.insert("index".into(), Value::Integer(i));
            }
            if src.get("hook_async").and_then(Value::as_bool) == Some(false) {
                t.insert("hook_async".into(), Value::Boolean(false));
            }
            user_templates.insert(id.clone(), Value::Table(t));
        }
    }
    if !user_templates.is_empty() {
        root.insert("templates".into(), Value::Table(user_templates));
    }
    root
}

// ---- custom colors ------------------------------------------------------------------------

fn harmonize_hex(src_hex: &str, target_hex: &str) -> Option<String> {
    let src = Hct::from_argb(Color::from_hex(src_hex)?.to_argb());
    let target = Hct::from_argb(Color::from_hex(target_hex)?.to_argb());
    let mut diff = target.hue() - src.hue();
    if diff > 180.0 {
        diff -= 360.0;
    } else if diff < -180.0 {
        diff += 360.0;
    }
    let mut rotation = (diff.abs() * 0.5).min(15.0);
    if diff < 0.0 {
        rotation = -rotation;
    }
    let result = Hct::new((src.hue() + rotation + 360.0) % 360.0, src.chroma(), src.tone());
    Some(Color::from_argb(result.to_argb()).to_hex())
}

fn scheme_primary_palette(scheme_type: &str, source: Hct) -> TonalPalette {
    let hue = source.hue();
    let sanitize = |d: f64| if d < 0.0 { (d % 360.0) + 360.0 } else if d >= 360.0 { d % 360.0 } else { d };
    match scheme_type {
        "tonal-spot" => TonalPalette::from_hue_chroma(hue, 36.0),
        "fruit-salad" => TonalPalette::from_hue_chroma(sanitize(hue - 50.0), 48.0),
        "rainbow" => TonalPalette::from_hue_chroma(hue, 48.0),
        "monochrome" => TonalPalette::from_hue_chroma(hue, 0.0),
        _ => TonalPalette::from_hue_chroma(hue, source.chroma()), // content
    }
}

/// C++ `TemplateEngine::applyCustomColors`. `Err` when a colour is not a valid hex string
/// (the C++ code throws there), so the caller falls back.
fn apply_custom_colors(root: &Table, data: &mut ThemeData, default_mode: &str, scheme_type: &str) -> Result<(), String> {
    let Some(custom) = root.get("config").and_then(Value::as_table).and_then(|c| c.get("custom_colors")).and_then(Value::as_table)
    else {
        return Ok(());
    };
    let source_hex = data
        .get(default_mode)
        .and_then(|m| m.get("primary").or_else(|| m.get("source_color")))
        .cloned()
        .unwrap_or_default();

    for (name, node) in custom {
        let (dark_hex, light_hex, blend) = match node {
            Value::String(s) => (s.clone(), s.clone(), true),
            Value::Table(t) => {
                let get = |k: &str| t.get(k).and_then(Value::as_str).unwrap_or("").to_string();
                let (base, dark_o, light_o) = (get("color"), get("color_dark"), get("color_light"));
                let pick = |cands: [&String; 3]| cands.into_iter().find(|c| !c.is_empty()).cloned().unwrap_or_default();
                (
                    pick([&dark_o, &base, &light_o]),
                    pick([&light_o, &base, &dark_o]),
                    t.get("blend").and_then(Value::as_bool).unwrap_or(true),
                )
            }
            _ => continue,
        };
        if dark_hex.is_empty() {
            continue;
        }
        for mode in ["dark", "light"] {
            let color_hex = if mode == "dark" { &dark_hex } else { &light_hex };
            let palette_hex = if blend && !source_hex.is_empty() {
                harmonize_hex(color_hex, &source_hex).ok_or("invalid custom color")?
            } else {
                color_hex.clone()
            };
            let seed = Color::from_hex(&palette_hex).ok_or("invalid custom color")?;
            let palette = scheme_primary_palette(scheme_type, Hct::from_argb(seed.to_argb()));
            let at = |tone: f64| Color::from_argb(palette.get(tone)).to_hex();

            let mode_data = data.entry(mode.to_string()).or_default();
            mode_data.insert(format!("{name}_source"), color_hex.clone());
            mode_data.insert(format!("{name}_value"), color_hex.clone());
            let (base, on_base, container, on_container) =
                if mode == "dark" { (80.0, 20.0, 30.0, 90.0) } else { (40.0, 100.0, 90.0, 10.0) };
            mode_data.insert(name.clone(), at(base));
            mode_data.insert(format!("on_{name}"), at(on_base));
            mode_data.insert(format!("{name}_container"), at(container));
            mode_data.insert(format!("on_{name}_container"), at(on_container));
        }
    }
    Ok(())
}

// ---- planning ---------------------------------------------------------------------------

struct OutputPlan {
    path: PathBuf,
    /// `Some(text)` rendered successfully; `None` = render or input failure.
    text: Option<String>,
    error: Option<&'static str>,
}

struct EntryPlan {
    name: String,
    pre_hook: Option<String>,
    has_outputs: bool,
    outputs: Vec<OutputPlan>,
    post_action: String,
    post_hook: Option<String>,
    hook_async: bool,
    hook_render_failed: Vec<&'static str>,
    pre_hook_render_failed: bool,
}

struct Planner<'a> {
    inputs: &'a ApplyInputs,
    /// Theme data for the config root being planned (custom colors are per engine in C++).
    data: ThemeData,
    unsupported: Option<String>,
}

impl Planner<'_> {
    fn options(&self, config_path: &Path, closest: &str) -> Options {
        Options {
            default_mode: self.inputs.default_mode.clone(),
            image_path: self.inputs.image_path.clone().unwrap_or_default(),
            closest_color: closest.to_string(),
            config_dir: parent_string(config_path),
            config_file: config_path.to_string_lossy().into_owned(),
            config_table: self.inputs.config_table.clone(),
            icon_theme: self.inputs.icon_theme.clone(),
        }
    }

    fn render(&mut self, options: &Options, text: &str) -> crate::template::RenderResult {
        let r = render(&self.data, options, text);
        if let Some(why) = r.unsupported {
            self.unsupported.get_or_insert_with(|| why.to_string());
        }
        if r.needs_config {
            self.unsupported.get_or_insert_with(|| "template reads config.* / icon_theme".to_string());
        }
        if r.needs_image && self.inputs.image_path.is_none() {
            self.unsupported.get_or_insert_with(|| "template reads the wallpaper path".to_string());
        }
        r
    }

    fn plan_config(&mut self, root: &Table, config_path: &Path, enabled: Option<&HashSet<String>>) -> Vec<EntryPlan> {
        self.data = self.inputs.theme_data.clone();
        if let Err(why) = apply_custom_colors(root, &mut self.data, &self.inputs.default_mode, &self.inputs.scheme_type)
        {
            self.unsupported.get_or_insert(why);
        }
        let Some(templates) = root.get("templates").and_then(Value::as_table) else { return vec![] };

        let mut entries = Vec::new();
        // toml++ iterates tables in key order; sort explicitly so feature flags cannot change it.
        let mut names: Vec<&String> = templates.keys().collect();
        names.sort();
        for name in names {
            if let Some(enabled) = enabled {
                if !enabled.is_empty() && !enabled.contains(name.as_str()) {
                    continue;
                }
            }
            let Some(tpl) = templates[name].as_table() else { continue };
            if let Some(entry) = parse_entry(config_path, name, tpl, &self.inputs.default_mode) {
                entries.push(entry);
            }
        }
        entries.sort_by_key(|e| e.index); // stable
        mark_multi_client_gated(&mut entries);

        let mut plans = Vec::new();
        for entry in &entries {
            if !entry.post_action.is_empty() {
                self.unsupported
                    .get_or_insert_with(|| format!("post_action '{}' (template {})", entry.post_action, entry.name));
            }

            let mut closest = String::new();
            if !entry.compare_to.is_empty() && !entry.colors_to_compare.is_empty() {
                let opts = self.options(config_path, "");
                let r = self.render(&opts, &entry.compare_to);
                if r.error_count == 0 {
                    closest = find_closest_color(&r.text, &entry.colors_to_compare);
                }
            }
            let opts = self.options(config_path, &closest);

            let dynamic = |planner: &mut Self, template: &str| -> Option<Vec<String>> {
                let r = planner.render(&opts, template);
                if r.error_count != 0 || r.text.is_empty() {
                    return None;
                }
                let out = Command::new("/bin/sh").arg("-lc").arg(&r.text).output().ok()?;
                if !out.status.success() {
                    return None;
                }
                Some(
                    String::from_utf8_lossy(&out.stdout)
                        .lines()
                        .map(trim)
                        .filter(|l| !l.is_empty() && !l.starts_with('#'))
                        .map(|l| resolve_config_path(config_path, l).to_string_lossy().into_owned())
                        .collect(),
                )
            };

            let mut effective_input = entry.input_path.clone();
            if !entry.input_path_dynamic.is_empty() {
                if let Some(paths) = dynamic(self, &entry.input_path_dynamic) {
                    if let Some(first) = paths.into_iter().next() {
                        effective_input = first;
                    }
                }
            }
            let mut outputs = entry.output_paths.clone();
            if !entry.output_path_dynamic.is_empty() {
                if let Some(paths) = dynamic(self, &entry.output_path_dynamic) {
                    outputs.extend(paths);
                }
            }

            let render_hook = |planner: &mut Self, hook: &str| -> Result<Option<String>, ()> {
                if hook.is_empty() {
                    return Ok(None);
                }
                let r = planner.render(&opts, hook);
                if r.error_count != 0 || r.text.is_empty() {
                    Err(())
                } else {
                    Ok(Some(r.text))
                }
            };
            let has_outputs = !outputs.is_empty();
            let pre = if has_outputs { render_hook(self, &entry.pre_hook) } else { Ok(None) };
            let post = render_hook(self, &entry.post_hook);

            // Render the input once per output path set.
            let rendered = if effective_input.is_empty() {
                None
            } else {
                match std::fs::read_to_string(&effective_input) {
                    Ok(source) => {
                        let r = self.render(&opts, &source);
                        Some(if r.error_count == 0 { Some(r.text) } else { None })
                    }
                    Err(_) => Some(None),
                }
            };

            let mut out_plans = Vec::new();
            for output in &outputs {
                if should_skip_output(entry, output) {
                    continue;
                }
                let (text, error) = match (&rendered, effective_input.is_empty()) {
                    (_, true) => (None, Some("failed to resolve input path")),
                    (Some(Some(text)), _) => (Some(text.clone()), None),
                    _ => (None, Some("failed to render template")),
                };
                out_plans.push(OutputPlan { path: PathBuf::from(output), text, error });
            }

            plans.push(EntryPlan {
                name: entry.name.clone(),
                pre_hook: pre.clone().ok().flatten(),
                pre_hook_render_failed: pre.is_err(),
                has_outputs,
                outputs: out_plans,
                post_action: entry.post_action.clone(),
                post_hook: post.clone().ok().flatten(),
                hook_render_failed: if post.is_err() { vec!["failed to render hook"] } else { vec![] },
                hook_async: entry.hook_async,
            });
            let _ = &entry.post_hook;
        }
        plans
    }
}

// ---- execution ---------------------------------------------------------------------------

struct HookRunner {
    state: Arc<(Mutex<HookState>, Condvar)>,
}

#[derive(Default)]
struct HookState {
    running: usize,
    failures: BTreeMap<String, String>,
}

fn compact_hook_error(stderr: &str, stdout: &str, code: Option<i32>) -> String {
    let text = if stderr.is_empty() { stdout } else { stderr };
    let text = text.split('\n').next().unwrap_or("");
    let text = trim(text);
    if text.is_empty() {
        return format!("exit {}", code.unwrap_or(-1));
    }
    text.chars().take(160).collect()
}

impl HookRunner {
    fn new() -> Self {
        Self { state: Arc::new((Mutex::new(HookState::default()), Condvar::new())) }
    }

    fn enqueue(&self, command: String, template_id: String) {
        let (lock, cv) = &*self.state;
        let mut guard = lock.lock().unwrap();
        while guard.running >= MAX_CONCURRENT_HOOKS {
            guard = cv.wait(guard).unwrap();
        }
        guard.running += 1;
        drop(guard);
        let state = Arc::clone(&self.state);
        std::thread::spawn(move || {
            let result = Command::new("/bin/sh").arg("-lc").arg(&command).output();
            let (lock, cv) = &*state;
            let mut guard = lock.lock().unwrap();
            match result {
                Ok(out) if out.status.success() => {}
                Ok(out) => {
                    let msg = compact_hook_error(
                        &String::from_utf8_lossy(&out.stderr),
                        &String::from_utf8_lossy(&out.stdout),
                        out.status.code(),
                    );
                    if !template_id.is_empty() {
                        guard.failures.insert(template_id, msg);
                    }
                }
                Err(_) => {
                    if !template_id.is_empty() {
                        guard.failures.insert(template_id, "failed to start".into());
                    }
                }
            }
            guard.running -= 1;
            cv.notify_all();
        });
    }

    fn wait_idle(&self) {
        let (lock, cv) = &*self.state;
        let mut guard = lock.lock().unwrap();
        while guard.running > 0 {
            guard = cv.wait(guard).unwrap();
        }
    }

    fn take_failures(&self) -> BTreeMap<String, String> {
        std::mem::take(&mut self.state.0.lock().unwrap().failures)
    }
}

fn run_sync_hook(command: &str) -> Result<(), String> {
    match Command::new("/bin/sh").arg("-lc").arg(command).output() {
        Ok(out) if out.status.success() => Ok(()),
        Ok(out) => {
            // Sync hooks: first line, trimmed, no length cap (C++ runHook).
            let stderr = String::from_utf8_lossy(&out.stderr);
            let stdout = String::from_utf8_lossy(&out.stdout);
            let text = if stderr.is_empty() { stdout } else { stderr };
            let first = trim(text.split('\n').next().unwrap_or("")).to_string();
            Err(if first.is_empty() { format!("exit {}", out.status.code().unwrap_or(-1)) } else { first })
        }
        Err(_) => Err("exit -1".into()),
    }
}

fn write_if_changed(path: &Path, text: &str) -> bool {
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if std::fs::read_to_string(path).is_ok_and(|prev| prev == text) {
        return true;
    }
    std::fs::write(path, text).is_ok()
}

fn execute_entries(plans: Vec<EntryPlan>, hooks: &HookRunner, failures: &mut BTreeMap<String, String>) {
    for plan in plans {
        let mut report = |id: &str, msg: &str| {
            failures.insert(id.to_string(), msg.to_string());
        };
        if plan.pre_hook_render_failed {
            report(&plan.name, "failed to render hook");
        } else if let Some(cmd) = &plan.pre_hook {
            if let Err(msg) = run_sync_hook(cmd) {
                report(&plan.name, &msg);
            }
        }

        let mut outputs_ok = true;
        for out in &plan.outputs {
            match (&out.text, out.error) {
                (Some(text), _) => {
                    if !write_if_changed(&out.path, text) {
                        outputs_ok = false;
                        report(&plan.name, "failed to render template");
                    }
                }
                (None, err) => {
                    outputs_ok = false;
                    report(&plan.name, err.unwrap_or("failed to render template"));
                }
            }
        }

        let wants_post = (plan.has_outputs && outputs_ok)
            || (!plan.has_outputs && (plan.post_hook.is_some() || !plan.post_action.is_empty()));
        if wants_post {
            // post_action is rejected during planning; nothing to run here.
            if !plan.hook_async && plan.post_hook.is_some() {
                hooks.wait_idle();
            }
            if !plan.hook_render_failed.is_empty() {
                report(&plan.name, plan.hook_render_failed[0]);
            } else if let Some(cmd) = &plan.post_hook {
                if plan.hook_async {
                    hooks.enqueue(cmd.clone(), plan.name.clone());
                } else if let Err(msg) = run_sync_hook(cmd) {
                    report(&plan.name, &msg);
                }
            }
        }
    }
}

// ---- entry point ---------------------------------------------------------------------------

/// One-shot application: undo hooks for built-ins that were turned off, then built-in,
/// community and user templates, then wait for background hooks.
pub fn apply(inputs: &ApplyInputs) -> Outcome {
    let mut planner = Planner { inputs, data: inputs.theme_data.clone(), unsupported: None };

    let enabled: BTreeSet<String> = if inputs.enable_builtin {
        inputs.builtin_ids.iter().cloned().collect()
    } else {
        BTreeSet::new()
    };
    let owed: Vec<String> = inputs.applied_before.iter().filter(|id| !enabled.contains(*id)).cloned().collect();

    let builtin_root: Option<Table> =
        std::fs::read_to_string(&inputs.builtin_config).ok().and_then(|t| t.parse::<Table>().ok());

    // Undo hooks (planning: render only).
    let mut undo: Vec<(String, Option<String>)> = Vec::new(); // (id, command)
    if !owed.is_empty() {
        let hook_options = Options {
            default_mode: inputs.default_mode.clone(),
            image_path: inputs.image_path.clone().unwrap_or_default(),
            closest_color: String::new(),
            config_dir: parent_string(&inputs.builtin_config),
            config_file: inputs.builtin_config.to_string_lossy().into_owned(),
            config_table: inputs.config_table.clone(),
            icon_theme: inputs.icon_theme.clone(),
        };
        let templates = builtin_root.as_ref().and_then(|r| r.get("templates")).and_then(Value::as_table);
        if let Some(templates) = templates {
            for id in &owed {
                let hook = templates.get(id).and_then(Value::as_table).and_then(|t| t.get("undo_hook")).and_then(Value::as_str);
                match hook {
                    None => undo.push((id.clone(), None)),
                    Some(h) => {
                        let r = planner.render(&hook_options, h);
                        if r.error_count != 0 || r.text.is_empty() {
                            undo.push((id.clone(), None));
                        } else {
                            undo.push((id.clone(), Some(r.text)));
                        }
                    }
                }
            }
        }
        // Without a parseable builtin config the C++ returns no resolved ids: they stay owed.
    }

    // Template plans.
    let mut plans: Vec<(String, Vec<EntryPlan>)> = Vec::new();
    if inputs.enable_builtin && !inputs.builtin_ids.is_empty() {
        match &builtin_root {
            Some(root) => {
                let enabled_set: HashSet<String> = inputs.builtin_ids.iter().cloned().collect();
                let p = planner.plan_config(root, &inputs.builtin_config, Some(&enabled_set));
                plans.push(("builtin".into(), p));
            }
            None => plans.push(("builtin".into(), vec![])),
        }
    }
    let mut missing_failures: BTreeMap<String, String> = BTreeMap::new();
    if inputs.enable_community {
        for t in &inputs.community {
            if !is_safe_template_id(&t.id) {
                continue;
            }
            if !t.config_path.exists() {
                missing_failures.insert(t.id.clone(), "community template is not downloaded yet".into());
                continue;
            }
            match std::fs::read_to_string(&t.config_path).ok().and_then(|s| s.parse::<Table>().ok()) {
                Some(root) => {
                    let p = planner.plan_config(&root, &t.config_path, None);
                    plans.push((t.id.clone(), p));
                }
                None => {
                    missing_failures.insert(t.id.clone(), "failed to apply community template".into());
                }
            }
        }
    }
    if let Some((root, path)) = &inputs.user_root {
        let p = planner.plan_config(root, path, None);
        plans.push(("user".into(), p));
    }

    if let Some(why) = planner.unsupported {
        return Outcome::Unsupported(why);
    }

    // Execute.
    let mut failures = missing_failures;
    let mut still_owed: BTreeSet<String> = BTreeSet::new();
    let templates_known = builtin_root.is_some();
    for (id, command) in &undo {
        match command {
            None => {}
            Some(cmd) => {
                if let Err(msg) = run_sync_hook(cmd) {
                    tracing_warn(id, &msg);
                    still_owed.insert(id.clone());
                }
            }
        }
    }
    if !owed.is_empty() && !templates_known {
        still_owed.extend(owed.iter().cloned());
    }

    let hooks = HookRunner::new();
    for (_label, entry_plans) in plans {
        execute_entries(entry_plans, &hooks, &mut failures);
    }
    hooks.wait_idle();
    failures.extend(hooks.take_failures());

    let mut applied: BTreeSet<String> = enabled;
    applied.extend(still_owed);
    Outcome::Applied(Report { failures, applied_builtin_ids: applied.into_iter().collect() })
}

fn tracing_warn(_id: &str, _msg: &str) {}
