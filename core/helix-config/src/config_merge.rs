//! `mergeConfigWithIncludes` parity for `config.toml` trees (port of the former C++ `config_merge.cpp`).

use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};

use toml::map::Map;
use toml::Value as Toml;

type TomlTable = Map<String, Toml>;

pub fn deep_merge(into: &mut toml::map::Map<String, Toml>, from: toml::map::Map<String, Toml>) {
    for (key, value) in from {
        match (into.get_mut(&key), value) {
            (Some(Toml::Table(existing)), Toml::Table(incoming)) => deep_merge(existing, incoming),
            (_, value) => {
                into.insert(key, value);
            }
        }
    }
}

fn expand_env_vars(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let bytes = input.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] != b'$' {
            out.push(bytes[i] as char);
            i += 1;
            continue;
        }
        if i + 1 < bytes.len() && bytes[i + 1] == b'{' {
            if let Some(close) = input[i + 2..].find('}') {
                let name = &input[i + 2..i + 2 + close];
                if let Ok(val) = std::env::var(name) {
                    out.push_str(&val);
                }
                i += close + 3;
                continue;
            }
            out.push('$');
            i += 1;
            continue;
        }
        if i + 1 < bytes.len() && is_name_start(bytes[i + 1]) {
            let start = i + 1;
            let mut j = start;
            while j < bytes.len() && is_name_char(bytes[j]) {
                j += 1;
            }
            let name = &input[start..j];
            if let Ok(val) = std::env::var(name) {
                out.push_str(&val);
            }
            i = j;
            continue;
        }
        out.push('$');
        i += 1;
    }
    out
}

fn is_name_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_'
}

fn is_name_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

fn resolve_path(raw: &str, base_dir: &Path) -> PathBuf {
    let expanded = expand_env_vars(raw);
    let path = PathBuf::from(expanded);
    if path.is_absolute() {
        path
    } else {
        base_dir.join(path)
    }
}

fn canonical_key(path: &Path) -> PathBuf {
    fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf())
}

fn sorted_toml_in_dir(dir: &Path) -> Vec<PathBuf> {
    let mut files: Vec<PathBuf> = fs::read_dir(dir)
        .ok()
        .into_iter()
        .flatten()
        .filter_map(|entry| entry.ok().map(|e| e.path()))
        .filter(|path| path.is_file() && path.extension().and_then(|e| e.to_str()) == Some("toml"))
        .collect();
    files.sort();
    files
}

struct IncludeDirective {
    files: Vec<String>,
    autoload: bool,
    has_autoload: bool,
}

fn read_include(table: &toml::map::Map<String, Toml>) -> IncludeDirective {
    let mut directive = IncludeDirective {
        files: Vec::new(),
        autoload: true,
        has_autoload: false,
    };
    let Some(Toml::Table(inc)) = table.get("include") else {
        return directive;
    };
    if let Some(Toml::Boolean(v)) = inc.get("autoload") {
        directive.autoload = *v;
        directive.has_autoload = true;
    }
    if let Some(Toml::Array(arr)) = inc.get("files") {
        for item in arr {
            if let Toml::String(path) = item {
                directive.files.push(path.clone());
            }
        }
    }
    directive
}

fn parse_toml_file(path: &Path) -> Option<TomlTable> {
    let text = fs::read_to_string(path).ok()?;
    match text.parse::<Toml>().ok()? {
        Toml::Table(table) => Some(table),
        _ => None,
    }
}

fn expand_file(path: &Path, parsed: TomlTable, visited: &mut HashSet<PathBuf>) -> TomlTable {
    let key = canonical_key(path);
    if visited.contains(&key) {
        return TomlTable::new();
    }
    visited.insert(key);

    let including_dir = path.parent().unwrap_or_else(|| Path::new("."));
    let directive = read_include(&parsed);

    let mut base = TomlTable::new();
    for entry in &directive.files {
        let target = resolve_path(entry, including_dir);
        if target.is_dir() {
            for child in sorted_toml_in_dir(&target) {
                if let Some(child_table) = parse_toml_file(&child) {
                    let expanded = expand_file(&child, child_table, visited);
                    deep_merge(&mut base, expanded);
                }
            }
        } else if target.is_file() {
            if let Some(child_table) = parse_toml_file(&target) {
                let expanded = expand_file(&target, child_table, visited);
                deep_merge(&mut base, expanded);
            }
        }
    }

    let mut body = parsed;
    body.remove("include");
    deep_merge(&mut base, body);
    base
}

/// Merged config directory matching C++ `helix::config::mergeConfigWithIncludes`.
pub fn merge_config_with_includes(config_dir: &Path) -> TomlTable {
    let mut merged = TomlTable::new();
    if !config_dir.is_dir() {
        return merged;
    }

    struct Root {
        path: PathBuf,
        table: TomlTable,
        opt_out: bool,
    }

    let mut roots = Vec::new();
    let mut any_opt_out = false;
    for path in sorted_toml_in_dir(config_dir) {
        let Some(table) = parse_toml_file(&path) else {
            continue;
        };
        let directive = read_include(&table);
        let opt_out = directive.has_autoload && !directive.autoload;
        any_opt_out |= opt_out;
        roots.push(Root {
            path,
            table,
            opt_out,
        });
    }

    let mut visited = HashSet::new();
    for root in roots {
        if any_opt_out && !root.opt_out {
            continue;
        }
        let expanded = expand_file(&root.path, root.table, &mut visited);
        deep_merge(&mut merged, expanded);
    }
    merged
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_file_wins_over_include() {
        let root = std::env::temp_dir().join(format!(
            "helix-config-merge-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("base.toml"), "[theme]\nmode = \"dark\"\n").unwrap();
        fs::write(
            root.join("config.toml"),
            "include = { files = [\"base.toml\"] }\n\n[theme]\nmode = \"light\"\n",
        )
        .unwrap();

        let merged = merge_config_with_includes(&root);
        assert_eq!(
            merged
                .get("theme")
                .and_then(Toml::as_table)
                .and_then(|t| t.get("mode"))
                .and_then(Toml::as_str),
            Some("light")
        );
        let _ = fs::remove_dir_all(root);
    }
}
