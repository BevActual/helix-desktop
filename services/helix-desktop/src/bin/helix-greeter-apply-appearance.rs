//! Stages and installs the login-screen appearance payload.
//!
//! `--stage DIR` reads the signed-in user's Helix config and writes `appearance.json` plus an
//! optional wallpaper image into `DIR` (mode `0700`). `--sync DIR --dest DEST` copies only that
//! payload. A privileged run (`pkexec`) must set `PKEXEC_UID` to the staging directory's owner,
//! so root never rebuilds the payload from root's config and never copies anything else.
//!
//! When it runs as root the destination is fixed to `/var/lib/helix-greeter` (a caller cannot aim
//! root at another directory), and staging files are opened relative to the verified staging
//! directory without following symlinks.

use std::ffi::CString;
use std::fs;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use helix_config::settings::{ConfigPaths, ConfigStore};
use helix_theme::color::Color;
use helix_theme::custom::find_builtin_palette;
use helix_theme::palette::{expand_fixed_palette_mode, TokenMap};
use helix_theme::transform::{apply_high_contrast, apply_pure_black_dark};
use helix_theme::wallpaper::{generate, load_and_resize, Scheme};
use serde::{Deserialize, Serialize};

const MAX_JSON: u64 = 64 * 1024;
const MAX_WALLPAPER: u64 = 20 * 1024 * 1024;
const DEFAULT_DEST: &str = "/var/lib/helix-greeter";

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
struct Colors {
    background: String,
    on_background: String,
    surface: String,
    on_surface: String,
    primary: String,
    on_primary: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
struct Payload {
    dark: bool,
    font: String,
    ui_scale: f64,
    corner_radius_scale: f64,
    transparency_mode: String,
    refraction: bool,
    high_contrast: bool,
    reduced_motion: bool,
    /// File name inside the greeter state directory, or empty when there is no image.
    wallpaper: String,
    colors: Colors,
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(error.code)
        }
    }
}

#[derive(Debug)]
struct Fail {
    code: u8,
    message: String,
}

impl Fail {
    fn new(code: u8, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

impl std::fmt::Display for Fail {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}

fn fail(message: impl Into<String>) -> Fail {
    Fail::new(1, message)
}

fn run() -> Result<(), Fail> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.as_slice() {
        [flag, dir] if flag == "--stage" => stage(Path::new(dir)),
        [flag, dir, dest_flag, dest] if flag == "--sync" && dest_flag == "--dest" => {
            let dest = resolve_dest(unsafe_euid(), Some(Path::new(dest)))?;
            install(Path::new(dir), &dest)
        }
        [flag, dir] if flag == "--sync" => {
            let dest = resolve_dest(unsafe_euid(), None)?;
            install(Path::new(dir), &dest)
        }
        _ => Err(fail(
            "usage: helix-greeter-apply-appearance --stage DIR | --sync DIR [--dest DIR]",
        )),
    }
}

/// Where the payload goes. Root only ever writes the fixed greeter state directory, so a local
/// user who gets this helper run through `pkexec` cannot point it (or `HELIX_GREETER_STATE`) at
/// another directory. Tests and unprivileged runs may choose their own.
fn resolve_dest(euid: u32, requested: Option<&Path>) -> Result<PathBuf, Fail> {
    if euid == 0 {
        return match requested {
            Some(path) if path != Path::new(DEFAULT_DEST) => Err(fail(
                "a privileged sync only writes /var/lib/helix-greeter; --dest is not accepted",
            )),
            _ => Ok(PathBuf::from(DEFAULT_DEST)),
        };
    }
    Ok(match requested {
        Some(path) => path.to_path_buf(),
        None => PathBuf::from(
            std::env::var("HELIX_GREETER_STATE").unwrap_or_else(|_| DEFAULT_DEST.to_string()),
        ),
    })
}

fn stage(dir: &Path) -> Result<(), Fail> {
    if !dir.is_absolute() {
        return Err(fail("staging directory must be absolute"));
    }
    fs::create_dir_all(dir).map_err(|error| {
        Fail::new(
            if error.kind() == io::ErrorKind::PermissionDenied {
                2
            } else {
                1
            },
            error.to_string(),
        )
    })?;
    set_mode(dir, 0o700)?;
    let store = ConfigStore::load(ConfigPaths::from_env());
    let payload = build_payload(&store);
    if !payload.wallpaper.is_empty() {
        if let Some(source) = wallpaper_file(&store) {
            copy_file(&source, &dir.join(&payload.wallpaper), 0o600)?;
        }
    }
    write_json(&dir.join("appearance.json"), &payload, 0o600)?;
    Ok(())
}

fn install(staging: &Path, dest: &Path) -> Result<(), Fail> {
    if !staging.is_absolute() || !dest.is_absolute() {
        return Err(fail("paths must be absolute"));
    }
    let staging = staging
        .canonicalize()
        .map_err(|error| fail(error.to_string()))?;
    let dir = fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(&staging)
        .map_err(|error| fail(error.to_string()))?;
    let meta = dir.metadata().map_err(|error| fail(error.to_string()))?;
    if !meta.is_dir() || meta.permissions().mode() & 0o077 != 0 {
        return Err(fail("staging directory must be a private directory"));
    }
    authorize_staging(meta.uid())?;
    let text = read_staged(&dir, "appearance.json", MAX_JSON)?;
    let text = String::from_utf8(text).map_err(|_| fail("appearance.json is not UTF-8 text"))?;
    let payload: Payload =
        serde_json::from_str(&text).map_err(|error| fail(format!("appearance.json: {error}")))?;
    let payload = sanitize(payload)?;
    let wallpaper = if payload.wallpaper.is_empty() {
        None
    } else {
        Some(read_staged(&dir, &payload.wallpaper, MAX_WALLPAPER)?)
    };
    fs::create_dir_all(dest).map_err(dest_error)?;
    set_mode(dest, 0o755)?;
    for name in [
        "wallpaper.png",
        "wallpaper.jpg",
        "wallpaper.jpeg",
        "wallpaper.webp",
    ] {
        let _ = fs::remove_file(dest.join(name));
    }
    if let Some(bytes) = wallpaper {
        write_atomic(&dest.join(&payload.wallpaper), &bytes, 0o644)?;
    }
    let json = serde_json::to_string_pretty(&payload).map_err(|error| fail(error.to_string()))?;
    write_atomic(&dest.join("appearance.json"), json.as_bytes(), 0o644)?;
    Ok(())
}

/// Reads one regular file out of the already-verified staging directory. The name is a single
/// path component, the open does not follow a symlink, and the size check runs on the open file,
/// so the caller cannot swap the file between the check and the read.
fn read_staged(dir: &fs::File, name: &str, max: u64) -> Result<Vec<u8>, Fail> {
    if !wallpaper_name_ok(name) && name != "appearance.json" {
        return Err(fail("staged file name is not allowed"));
    }
    let c_name = CString::new(name).map_err(|_| fail("staged file name is not allowed"))?;
    // SAFETY: `dir` is a valid open directory descriptor and `c_name` is a NUL-terminated string.
    let fd = unsafe {
        libc::openat(
            dir.as_raw_fd(),
            c_name.as_ptr(),
            libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC,
        )
    };
    if fd < 0 {
        return Err(fail(format!(
            "{name}: {}",
            io::Error::last_os_error()
        )));
    }
    // SAFETY: `fd` was just returned by openat and is owned by nothing else.
    let file = unsafe { fs::File::from_raw_fd(fd) };
    let meta = file.metadata().map_err(|error| fail(error.to_string()))?;
    if !meta.file_type().is_file() || meta.len() > max {
        return Err(fail(format!("{name} is missing, not a plain file, or too large")));
    }
    let mut bytes = Vec::new();
    file.take(max + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| fail(error.to_string()))?;
    if bytes.len() as u64 > max {
        return Err(fail(format!("{name} is too large")));
    }
    Ok(bytes)
}

/// Writes next to the target and renames, so the greeter never reads a half-written file.
fn write_atomic(path: &Path, bytes: &[u8], mode: u32) -> Result<(), Fail> {
    let name = path
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or_else(|| fail("destination has no file name"))?;
    let temp = path.with_file_name(format!(".{name}.tmp"));
    let _ = fs::remove_file(&temp);
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(mode)
        .custom_flags(libc::O_NOFOLLOW)
        .open(&temp)
        .map_err(dest_error)?;
    file.write_all(bytes).map_err(dest_error)?;
    file.sync_all().map_err(dest_error)?;
    drop(file);
    set_mode(&temp, mode)?;
    fs::rename(&temp, path).map_err(dest_error)
}

fn authorize_staging(owner: u32) -> Result<(), Fail> {
    let euid = unsafe_euid();
    if euid == owner {
        return Ok(());
    }
    if euid == 0 {
        let caller = std::env::var("PKEXEC_UID")
            .ok()
            .and_then(|text| text.parse::<u32>().ok());
        if caller == Some(owner) {
            return Ok(());
        }
        return Err(fail(
            "privileged sync requires PKEXEC_UID to own the staging directory",
        ));
    }
    Err(fail("staging directory is not owned by this user"))
}

fn unsafe_euid() -> u32 {
    // SAFETY: geteuid does not change process state.
    unsafe { libc::geteuid() }
}

fn dest_error(error: io::Error) -> Fail {
    let code = if error.kind() == io::ErrorKind::PermissionDenied {
        2
    } else {
        1
    };
    Fail::new(code, error.to_string())
}

fn build_payload(store: &ConfigStore) -> Payload {
    let mode = store
        .get("theme.mode")
        .as_str()
        .unwrap_or("dark")
        .to_string();
    let shell_mode = store
        .get("theme.shell_mode")
        .as_str()
        .unwrap_or("follow")
        .to_string();
    let dark = match mode.as_str() {
        "light" => false,
        "auto" => shell_mode != "light",
        _ => true,
    };
    let high_contrast = store
        .get("accessibility.high_contrast")
        .as_bool()
        .unwrap_or(false);
    let pure_black = store
        .get("theme.pure_black_dark")
        .as_bool()
        .unwrap_or(false);
    let mut tokens = palette_tokens(store, dark);
    if dark && pure_black {
        apply_pure_black_dark(&mut tokens);
    }
    if high_contrast {
        apply_high_contrast(&mut tokens, dark);
    }
    let wallpaper = wallpaper_file(store)
        .and_then(|path| {
            path.extension()
                .and_then(|ext| ext.to_str())
                .map(|ext| ext.to_ascii_lowercase())
        })
        .filter(|ext| matches!(ext.as_str(), "png" | "jpg" | "jpeg" | "webp"))
        .map(|ext| format!("wallpaper.{ext}"))
        .unwrap_or_default();
    let glass = store
        .get("shell.panel.transparency_mode")
        .as_str()
        .unwrap_or("soft")
        .to_string();
    Payload {
        dark,
        font: clean_font(
            store
                .get("shell.font_family")
                .as_str()
                .unwrap_or("sans-serif"),
        ),
        ui_scale: clamp(
            store.get("accessibility.ui_scale").as_f64().unwrap_or(1.0),
            0.5,
            3.0,
        ),
        corner_radius_scale: clamp(
            store
                .get("shell.corner_radius_scale")
                .as_f64()
                .unwrap_or(1.0),
            0.0,
            2.0,
        ),
        transparency_mode: if matches!(glass.as_str(), "solid" | "soft" | "glass") {
            glass
        } else {
            "soft".into()
        },
        refraction: store
            .get("shell.panel.refraction")
            .as_bool()
            .unwrap_or(true),
        high_contrast,
        reduced_motion: !store
            .get("shell.animation.enabled")
            .as_bool()
            .unwrap_or(true),
        wallpaper,
        colors: colors_from(&tokens),
    }
}

fn palette_tokens(store: &ConfigStore, dark: bool) -> TokenMap {
    let scheme_name = store.get("theme.wallpaper_scheme");
    let scheme_name = scheme_name.as_str().unwrap_or("m3-content");
    if let Some(path) = wallpaper_file(store) {
        if let Some(scheme) = Scheme::parse(scheme_name) {
            if let Ok(rgb) = load_and_resize(&path, scheme) {
                if let Ok(generated) = generate(&rgb, scheme) {
                    return if dark {
                        generated.dark
                    } else {
                        generated.light
                    };
                }
            }
        }
    }
    builtin_tokens(store.get("theme.builtin").as_str().unwrap_or("Helix"), dark)
}

fn builtin_tokens(name: &str, dark: bool) -> TokenMap {
    let palette = find_builtin_palette(name).or_else(|| find_builtin_palette("Helix"));
    match palette {
        Some(palette) => {
            let mode = if dark {
                &palette.palette.dark
            } else {
                &palette.palette.light
            };
            expand_fixed_palette_mode(&mode.palette, dark)
        }
        None => TokenMap::new(),
    }
}

fn colors_from(tokens: &TokenMap) -> Colors {
    Colors {
        background: token_hex(tokens, "background", "#0e1028"),
        on_background: token_hex(tokens, "on_background", "#f4f4f5"),
        surface: token_hex(tokens, "surface", "#1c1c20"),
        on_surface: token_hex(tokens, "on_surface", "#f4f4f5"),
        primary: token_hex(tokens, "primary", "#8ab4f8"),
        on_primary: token_hex(tokens, "on_primary", "#082043"),
    }
}

fn token_hex(tokens: &TokenMap, key: &str, fallback: &str) -> String {
    tokens
        .get(key)
        .map(|argb| Color::from_argb(*argb).to_hex())
        .filter(|hex| is_hex(hex))
        .unwrap_or_else(|| fallback.to_string())
}

fn wallpaper_file(store: &ConfigStore) -> Option<PathBuf> {
    let settings = fs::read_to_string(&store.paths().settings_toml).ok()?;
    let table: toml::Table = settings.parse().ok()?;
    let wallpaper = table.get("wallpaper").and_then(toml::Value::as_table)?;
    for key in ["last", "default"] {
        let path = wallpaper
            .get(key)
            .and_then(toml::Value::as_table)
            .and_then(|section| section.get("path"))
            .and_then(toml::Value::as_str)
            .map(expand_user);
        if let Some(path) = path.filter(|path| Path::new(path).is_file()) {
            return Some(PathBuf::from(path));
        }
    }
    None
}

fn expand_user(path: &str) -> String {
    match path.strip_prefix("~/") {
        Some(rest) => std::env::var("HOME")
            .map(|home| format!("{home}/{rest}"))
            .unwrap_or_else(|_| path.to_string()),
        None => path.to_string(),
    }
}

fn sanitize(mut payload: Payload) -> Result<Payload, Fail> {
    if !is_hex(&payload.colors.background)
        || !is_hex(&payload.colors.on_background)
        || !is_hex(&payload.colors.surface)
        || !is_hex(&payload.colors.on_surface)
        || !is_hex(&payload.colors.primary)
        || !is_hex(&payload.colors.on_primary)
    {
        return Err(fail("appearance colors must be #rrggbb"));
    }
    payload.font = clean_font(&payload.font);
    payload.ui_scale = clamp(payload.ui_scale, 0.5, 3.0);
    payload.corner_radius_scale = clamp(payload.corner_radius_scale, 0.0, 2.0);
    if !matches!(
        payload.transparency_mode.as_str(),
        "solid" | "soft" | "glass"
    ) {
        payload.transparency_mode = "soft".into();
    }
    if payload.wallpaper.is_empty() {
        return Ok(payload);
    }
    if !wallpaper_name_ok(&payload.wallpaper) {
        return Err(fail("wallpaper name is not a single image file"));
    }
    Ok(payload)
}

fn wallpaper_name_ok(name: &str) -> bool {
    let path = Path::new(name);
    path.components().count() == 1
        && matches!(
            path.extension().and_then(|ext| ext.to_str()),
            Some("png" | "jpg" | "jpeg" | "webp")
        )
}

fn is_hex(value: &str) -> bool {
    let Some(digits) = value.strip_prefix('#') else {
        return false;
    };
    digits.len() == 6 && digits.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn clean_font(font: &str) -> String {
    let ok = !font.is_empty()
        && font.len() <= 80
        && font
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, ' ' | ',' | '-' | '.'));
    if ok {
        font.to_string()
    } else {
        "sans-serif".into()
    }
}

fn clamp(value: f64, min: f64, max: f64) -> f64 {
    if !value.is_finite() {
        return 1.0;
    }
    value.clamp(min, max)
}

fn set_mode(path: &Path, mode: u32) -> Result<(), Fail> {
    fs::set_permissions(path, fs::Permissions::from_mode(mode)).map_err(dest_error)
}

fn write_json(path: &Path, payload: &Payload, mode: u32) -> Result<(), Fail> {
    let text = serde_json::to_string_pretty(payload).map_err(|error| fail(error.to_string()))?;
    let mut file = fs::File::create(path).map_err(dest_error)?;
    file.write_all(text.as_bytes())
        .map_err(|error| fail(error.to_string()))?;
    set_mode(path, mode)
}

fn copy_file(from: &Path, to: &Path, mode: u32) -> Result<(), Fail> {
    let meta = fs::symlink_metadata(from).map_err(|error| fail(error.to_string()))?;
    if !meta.file_type().is_file() || meta.len() > MAX_WALLPAPER {
        return Err(fail("wallpaper is missing or too large"));
    }
    fs::copy(from, to).map_err(dest_error)?;
    set_mode(to, mode)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;
    use std::sync::Mutex;

    static ENV: Mutex<()> = Mutex::new(());

    fn temp_dir(tag: &str) -> PathBuf {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("helix-greeter-sync-{tag}-{nanos}"));
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn stage_and_install_round_trip_light_mode() {
        let _guard = ENV.lock().unwrap();
        let root = temp_dir("round");
        let config = root.join("config");
        let state = root.join("state");
        fs::create_dir_all(config.join("helix")).unwrap();
        fs::create_dir_all(state.join("helix")).unwrap();
        fs::write(
            config.join("helix/config.toml"),
            "[theme]\nmode = \"light\"\n",
        )
        .unwrap();
        let png = root.join("wall.png");
        fs::write(&png, b"not-a-real-png").unwrap();
        fs::write(
            state.join("helix/settings.toml"),
            format!("[wallpaper.last]\npath = \"{}\"\n", png.display()),
        )
        .unwrap();
        // SAFETY: this test is single-threaded and restores nothing else in-process.
        unsafe {
            std::env::set_var("HELIX_CONFIG_HOME", &config);
            std::env::set_var("HELIX_STATE_HOME", &state);
        }
        let staging = root.join("stage");
        stage(&staging).unwrap();
        let payload: Payload =
            serde_json::from_str(&fs::read_to_string(staging.join("appearance.json")).unwrap())
                .unwrap();
        assert!(!payload.dark);
        assert_eq!(payload.wallpaper, "wallpaper.png");
        assert!(payload.colors.primary.starts_with('#'));
        assert!(staging.join("wallpaper.png").is_file());

        let dest = root.join("dest");
        install(&staging, &dest).unwrap();
        let installed: Payload =
            serde_json::from_str(&fs::read_to_string(dest.join("appearance.json")).unwrap())
                .unwrap();
        assert_eq!(installed, payload);
        let mode = fs::metadata(&staging).unwrap().permissions().mode() & 0o077;
        assert_eq!(mode, 0);
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn install_rejects_a_shared_staging_directory() {
        let root = temp_dir("open");
        let staging = root.join("stage");
        fs::create_dir_all(&staging).unwrap();
        fs::set_permissions(&staging, fs::Permissions::from_mode(0o755)).unwrap();
        fs::write(staging.join("appearance.json"), "{}").unwrap();
        let error = install(&staging, &root.join("dest")).unwrap_err();
        assert!(error.message.contains("private"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn install_rejects_a_wallpaper_path() {
        let _guard = ENV.lock().unwrap();
        let root = temp_dir("path");
        let staging = root.join("stage");
        stage_empty(&staging);
        let mut payload: Payload =
            serde_json::from_str(&fs::read_to_string(staging.join("appearance.json")).unwrap())
                .unwrap();
        payload.wallpaper = "../wall.png".into();
        fs::write(
            staging.join("appearance.json"),
            serde_json::to_string(&payload).unwrap(),
        )
        .unwrap();
        let error = install(&staging, &root.join("dest")).unwrap_err();
        assert!(error.message.contains("wallpaper"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn root_only_writes_the_fixed_greeter_directory() {
        assert_eq!(
            resolve_dest(0, None).unwrap(),
            PathBuf::from("/var/lib/helix-greeter")
        );
        assert_eq!(
            resolve_dest(0, Some(Path::new("/var/lib/helix-greeter"))).unwrap(),
            PathBuf::from("/var/lib/helix-greeter")
        );
        assert!(resolve_dest(0, Some(Path::new("/etc"))).is_err());
        assert_eq!(
            resolve_dest(1000, Some(Path::new("/tmp/x"))).unwrap(),
            PathBuf::from("/tmp/x")
        );
    }

    #[test]
    fn install_does_not_follow_a_staged_symlink() {
        let _guard = ENV.lock().unwrap();
        let root = temp_dir("link");
        let staging = root.join("stage");
        stage_empty(&staging);
        let secret = root.join("secret.png");
        fs::write(&secret, b"top secret").unwrap();
        let mut payload: Payload =
            serde_json::from_str(&fs::read_to_string(staging.join("appearance.json")).unwrap())
                .unwrap();
        payload.wallpaper = "wallpaper.png".into();
        fs::write(
            staging.join("appearance.json"),
            serde_json::to_string(&payload).unwrap(),
        )
        .unwrap();
        symlink(&secret, staging.join("wallpaper.png")).unwrap();
        let dest = root.join("dest");
        assert!(install(&staging, &dest).is_err());
        assert!(!dest.join("wallpaper.png").exists());
        let _ = fs::remove_dir_all(root);
    }

    fn stage_empty(staging: &Path) {
        let root = staging.parent().unwrap();
        let config = root.join("config");
        let state = root.join("state");
        fs::create_dir_all(config.join("helix")).unwrap();
        fs::create_dir_all(state.join("helix")).unwrap();
        fs::write(config.join("helix/config.toml"), "").unwrap();
        fs::write(state.join("helix/settings.toml"), "").unwrap();
        unsafe {
            std::env::set_var("HELIX_CONFIG_HOME", &config);
            std::env::set_var("HELIX_STATE_HOME", &state);
        }
        stage(staging).unwrap();
    }
}
