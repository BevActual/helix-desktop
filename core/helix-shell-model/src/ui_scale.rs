//! The interface scale as the shell's chrome sees it. Applications zoom their own windows; the shell's bar, dock and panels are one layer
//! surface, so what has to follow the scale besides its page zoom is every size the compositor is told in pixels: the bar's exclusive zone, the
//! dock's, and the region that takes the pointer. The shell writes the scale it applies to a small file in the runtime directory, and
//! `helix-shell-strut` (a separate process that holds the exclusive zones) reads it.

use std::path::PathBuf;

/// The range Settings offers (`accessibility.ui_scale`).
pub const MIN: f64 = 0.8;
pub const MAX: f64 = 1.5;

const FILE: &str = "helix-ui-scale";

/// Within range; 1.0 for anything that is not a number.
pub fn clamp(scale: f64) -> f64 {
    if scale.is_finite() {
        scale.clamp(MIN, MAX)
    } else {
        1.0
    }
}

/// `px` at `scale`, rounded to a whole pixel and never below zero.
pub fn scaled(px: i32, scale: f64) -> i32 {
    ((f64::from(px) * clamp(scale)).round() as i32).max(0)
}

pub fn state_path() -> Option<PathBuf> {
    std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .map(|dir| dir.join(FILE))
}

/// The scale the shell last applied; 1.0 when it has not said.
pub fn read() -> f64 {
    state_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .and_then(|text| text.trim().parse::<f64>().ok())
        .map_or(1.0, clamp)
}

pub fn write(scale: f64) -> Result<(), String> {
    let path = state_path().ok_or("no runtime directory")?;
    let temporary = path.with_extension("tmp");
    std::fs::write(&temporary, format!("{}\n", clamp(scale))).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scale_is_kept_in_range_and_sizes_round_to_pixels() {
        assert_eq!(clamp(3.0), 1.5);
        assert_eq!(clamp(0.1), 0.8);
        assert_eq!(clamp(f64::NAN), 1.0);
        assert_eq!(scaled(42, 1.0), 42);
        assert_eq!(scaled(42, 1.5), 63);
        assert_eq!(scaled(42, 1.25), 53, "52.5 rounds up");
        assert_eq!(scaled(42, 0.8), 34);
        assert_eq!(scaled(0, 1.5), 0);
        assert_eq!(scaled(-5, 1.0), 0);
    }

    #[test]
    fn the_file_round_trips_and_missing_means_one() {
        let dir = std::env::temp_dir().join(format!("helix-ui-scale-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::env::set_var("XDG_RUNTIME_DIR", &dir);
        assert_eq!(read(), 1.0);
        write(1.25).unwrap();
        assert_eq!(read(), 1.25);
        write(9.0).unwrap();
        assert_eq!(read(), 1.5);
        std::fs::write(dir.join(FILE), "nonsense").unwrap();
        assert_eq!(read(), 1.0);
        std::fs::remove_dir_all(dir).ok();
    }
}
