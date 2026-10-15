//! `load_and_resize` against C++ `loadAndResize` (`tests/golden/image_loader.json`, four PNGs x triangle and box filters).

use helix_theme::wallpaper::{load_and_resize, Scheme};
use serde_json::Value;
use std::path::Path;

#[test]
fn resized_buffers_match_cpp() {
    let golden: Vec<Value> = serde_json::from_str(include_str!("golden/image_loader.json")).unwrap();
    assert_eq!(golden.len(), 8);
    for entry in &golden {
        let file = Path::new(entry["file"].as_str().unwrap()).file_name().unwrap().to_owned();
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/golden/images").join(file);
        let scheme = Scheme::parse(entry["scheme"].as_str().unwrap()).unwrap();
        let rgb = load_and_resize(&path, scheme).unwrap();
        let hex: String = rgb.iter().map(|b| format!("{b:02x}")).collect();
        let want = entry["rgb"].as_str().unwrap();
        let differing = hex.as_bytes().chunks(2).zip(want.as_bytes().chunks(2)).filter(|(a, b)| a != b).count();
        assert_eq!(differing, 0, "{} {}: {differing} channel values differ", path.display(), entry["scheme"]);
    }
}
