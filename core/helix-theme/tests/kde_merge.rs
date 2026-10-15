//! Parity with `tests/kde_color_scheme_test.cpp` (`theme/kde_color_scheme.cpp`).

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

use helix_theme::kde::merge_color_scheme;

fn temp_dir() -> PathBuf {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let n = NEXT.fetch_add(1, Ordering::Relaxed);
    let root = std::env::temp_dir().join(format!("helix-kde-merge-{}-{}", std::process::id(), n));
    fs::create_dir_all(&root).unwrap();
    root
}

fn key_value(path: &Path, group: &str, key: &str) -> String {
    let text = fs::read_to_string(path).unwrap();
    let mut current = "";
    for line in text.lines() {
        if line.len() >= 3 && line.starts_with('[') && line.ends_with(']') {
            current = &line[1..line.len() - 1];
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            if current == group && k == key {
                return v.to_string();
            }
        }
    }
    String::new()
}

#[test]
fn merge_preserves_unrelated_settings() {
    let root = temp_dir();
    let scheme = root.join("helix.colors");
    let globals = root.join("kdeglobals");
    fs::write(
        &globals,
        "# keep this comment\n\
         [General]\n\
         ExistingKey=keep\n\
         ColorScheme=Old\n\
         \n\
         [Colors:Button]\n\
         ForegroundNormal=1,2,3\n\
         \n\
         [Unrelated]\n\
         MixedCase=Yes\n",
    )
    .unwrap();
    fs::write(
        &scheme,
        "[General]\n\
         ColorScheme=Helix\n\
         Name=helix\n\
         \n\
         [Colors:Button]\n\
         ForegroundNormal=9,8,7\n\
         BackgroundNormal=4,5,6\n\
         \n\
         [Colors:Header][Inactive]\n\
         ForegroundNormal=6,5,4\n",
    )
    .unwrap();
    fs::set_permissions(&globals, fs::Permissions::from_mode(0o600)).unwrap();

    merge_color_scheme(&scheme, &globals).expect("merge");

    assert_eq!(key_value(&globals, "General", "ExistingKey"), "keep");
    assert_eq!(key_value(&globals, "General", "ColorScheme"), "Helix");
    assert_eq!(key_value(&globals, "Colors:Button", "ForegroundNormal"), "9,8,7");
    assert_eq!(key_value(&globals, "Colors:Button", "BackgroundNormal"), "4,5,6");
    assert_eq!(key_value(&globals, "Unrelated", "MixedCase"), "Yes");
    assert_eq!(
        key_value(&globals, "Colors:Header][Inactive", "ForegroundNormal"),
        "6,5,4"
    );
    assert!(fs::read_to_string(&globals).unwrap().contains("# keep this comment"));
    let mode = fs::metadata(&globals).unwrap().permissions().mode() & 0o7777;
    assert_eq!(mode, 0o600);
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn invalid_scheme_does_not_overwrite_globals() {
    let root = temp_dir();
    let scheme = root.join("broken.colors");
    let globals = root.join("kdeglobals");
    let original = "[General]\nExistingKey=keep\n";
    fs::write(&scheme, "not a key file\n").unwrap();
    fs::write(&globals, original).unwrap();

    let err = merge_color_scheme(&scheme, &globals).unwrap_err();
    assert!(!err.is_empty());
    assert_eq!(fs::read_to_string(&globals).unwrap(), original);
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn missing_scheme_fails_without_creating_globals() {
    let root = temp_dir();
    let err = merge_color_scheme(&root.join("missing.colors"), &root.join("kdeglobals")).unwrap_err();
    assert!(!err.is_empty());
    assert!(!root.join("kdeglobals").exists());
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn empty_scheme_fails_without_creating_globals() {
    let root = temp_dir();
    let scheme = root.join("empty.colors");
    let globals = root.join("kdeglobals");
    fs::write(&scheme, "# no color groups\n").unwrap();

    let err = merge_color_scheme(&scheme, &globals).unwrap_err();
    assert!(!err.is_empty());
    assert!(!globals.exists());
    let _ = fs::remove_dir_all(&root);
}
