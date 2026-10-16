//! Template render goldens (Rust engine). Regenerate with `tools/regen_golden_template.sh`.
//! Bundled templates are compared by FNV-1a hash,
//! the synthetic template (every filter, loop and error path) by full text.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

use helix_theme::custom::{builtin_palettes, expand_fixed_palette};
use helix_theme::template::{make_theme_data, render, Options};

fn fnv1a(text: &str) -> u64 {
    let mut h: u64 = 1469598103934665603;
    for b in text.bytes() {
        h ^= u64::from(b);
        h = h.wrapping_mul(1099511628211);
    }
    h
}

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).to_path_buf()
}

#[test]
fn matches_cpp_engine() {
    let golden = root().join("tests/golden/template");
    let hashes = fs::read_to_string(golden.join("hashes.tsv")).unwrap();
    let synthetic = fs::read_to_string(golden.join("synthetic.tpl")).unwrap();
    let templates = root().join("../../assets/templates");

    let mut lines = hashes.lines();
    let icon_theme = lines
        .next()
        .unwrap()
        .split('\t')
        .nth(1)
        .unwrap()
        .to_string();

    let config = serde_json::json!({
        "shell": {
            "name": "helix_shell", "size": 12, "ratio": 1.5, "enabled": true,
            "list": ["a", "b", "c"], "nested": {"k": "v"}
        }
    });

    let mut data_by_palette = HashMap::new();
    for p in builtin_palettes() {
        let generated = expand_fixed_palette(&p.palette);
        data_by_palette.insert(
            p.name.clone(),
            make_theme_data(&generated.dark, &generated.light),
        );
    }

    let mut checked = 0;
    for line in lines {
        let f: Vec<&str> = line.split('\t').collect();
        let (palette, mode, name, errors, hash) = (f[0], f[1], f[2], f[3], f[4]);
        let options = Options {
            default_mode: mode.to_string(),
            image_path: "/img/wall paper.png".into(),
            closest_color: "#112233".into(),
            config_dir: "/home/u/.config/helix".into(),
            config_file: "/home/u/.config/helix/config.toml".into(),
            config_table: Some(config.clone()),
            icon_theme: icon_theme.clone(),
        };
        let source = match name.strip_prefix("t:") {
            Some(rel) => fs::read_to_string(templates.join(rel)).unwrap(),
            None => synthetic.clone(),
        };
        let result = render(&data_by_palette[palette], &options, &source);
        let label = format!("{palette}/{mode}/{name}");

        if name == "synthetic.tpl" {
            let want =
                fs::read_to_string(golden.join(format!("full/{palette}_{mode}_synthetic.tpl")))
                    .unwrap();
            assert_eq!(result.text, want, "{label} text");
        }
        assert_eq!(result.error_count.to_string(), errors, "{label} errors");
        assert_eq!(
            format!("{:016x}", fnv1a(&result.text)),
            hash,
            "{label} hash"
        );
        assert!(
            result.unsupported.is_none() || name.starts_with("t:"),
            "{label} unexpectedly unsupported"
        );
        checked += 1;
    }
    assert_eq!(checked, 1040);
}
