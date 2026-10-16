//! KDE colour-scheme merge into `kdeglobals` (`theme/kde_color_scheme.cpp`).

use std::collections::HashMap;
use std::fs;
use std::path::Path;

#[derive(Default)]
struct Key {
    name: String,
    value: String,
    line_index: usize,
}

#[derive(Default)]
struct Section {
    name: String,
    header: String,
    lines: Vec<String>,
    keys: Vec<Key>,
    key_indices: HashMap<String, usize>,
}

#[derive(Default)]
struct Document {
    prefix_lines: Vec<String>,
    sections: Vec<Section>,
    section_indices: HashMap<String, usize>,
}

fn trim(s: &str) -> &str {
    s.trim_matches(|c| matches!(c, ' ' | '\t' | '\n' | '\r' | '\x0b' | '\x0c'))
}

fn is_comment_or_blank(line: &str) -> bool {
    let t = trim(line);
    t.is_empty() || t.starts_with('#') || t.starts_with(';')
}

fn parse(path: &Path, required: bool) -> Result<Document, String> {
    let mut doc = Document::default();
    let text = match fs::read_to_string(path) {
        Ok(t) => t,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return if required {
                Err(format!(
                    "failed to load {}: file does not exist",
                    path.display()
                ))
            } else {
                Ok(doc)
            };
        }
        Err(_) => return Err(format!("failed to load {}", path.display())),
    };

    let mut current: Option<usize> = None;
    // `getline` yields no trailing empty line for a final '\n'.
    let mut lines: Vec<&str> = text.split('\n').collect();
    if text.ends_with('\n') {
        lines.pop();
    }
    for (i, raw) in lines.iter().enumerate() {
        let line_number = i + 1;
        let mut line = (*raw).to_string();
        if line.ends_with('\r') {
            line.pop();
        }
        let trimmed = trim(&line).to_string();
        if is_comment_or_blank(&trimmed) {
            match current {
                Some(s) => doc.sections[s].lines.push(line),
                None => doc.prefix_lines.push(line),
            }
            continue;
        }
        if trimmed.starts_with('[') {
            if trimmed.len() < 3 || !trimmed.ends_with(']') {
                return Err(format!("invalid group at {}:{line_number}", path.display()));
            }
            let name = trimmed[1..trimmed.len() - 1].to_string();
            if name.is_empty() || doc.section_indices.contains_key(&name) {
                return Err(format!(
                    "duplicate or empty group at {}:{line_number}",
                    path.display()
                ));
            }
            doc.section_indices.insert(name.clone(), doc.sections.len());
            doc.sections.push(Section {
                name,
                header: line,
                ..Default::default()
            });
            current = Some(doc.sections.len() - 1);
            continue;
        }
        let equals = trimmed.find('=');
        let (Some(section), Some(equals)) = (current, equals) else {
            return Err(format!("invalid key at {}:{line_number}", path.display()));
        };
        let key_name = trim(&trimmed[..equals]).to_string();
        let sec = &mut doc.sections[section];
        if key_name.is_empty() || sec.key_indices.contains_key(&key_name) {
            return Err(format!(
                "duplicate or empty key at {}:{line_number}",
                path.display()
            ));
        }
        let value = trim(&trimmed[equals + 1..]).to_string();
        let line_index = sec.lines.len();
        sec.lines.push(line);
        sec.key_indices.insert(key_name.clone(), sec.keys.len());
        sec.keys.push(Key {
            name: key_name,
            value,
            line_index,
        });
    }
    Ok(doc)
}

fn append_key(section: &mut Section, key: &Key) {
    let mut insert_at = section.lines.len();
    while insert_at > 0 && is_comment_or_blank(&section.lines[insert_at - 1]) {
        insert_at -= 1;
    }
    section
        .lines
        .insert(insert_at, format!("{}={}", key.name, key.value));
}

fn merge(dest: &mut Document, source: &Document) {
    for src in &source.sections {
        let Some(&di) = dest.section_indices.get(&src.name) else {
            let mut section = Section {
                name: src.name.clone(),
                header: format!("[{}]", src.name),
                ..Default::default()
            };
            for key in &src.keys {
                append_key(&mut section, key);
            }
            dest.section_indices
                .insert(section.name.clone(), dest.sections.len());
            dest.sections.push(section);
            continue;
        };
        let section = &mut dest.sections[di];
        for key in &src.keys {
            match section.key_indices.get(&key.name) {
                None => append_key(section, key),
                Some(&ki) => {
                    let line_index = section.keys[ki].line_index;
                    section.lines[line_index] = format!("{}={}", key.name, key.value);
                }
            }
        }
    }
}

fn serialize(doc: &Document) -> String {
    let mut out = String::new();
    for line in &doc.prefix_lines {
        out.push_str(line);
        out.push('\n');
    }
    for (i, section) in doc.sections.iter().enumerate() {
        if i > 0 && !out.is_empty() {
            let prev = &doc.sections[i - 1];
            if prev.lines.is_empty() || !prev.lines.last().unwrap().is_empty() {
                out.push('\n');
            }
        }
        out.push_str(&section.header);
        out.push('\n');
        for line in &section.lines {
            out.push_str(line);
            out.push('\n');
        }
    }
    out
}

fn write_atomic(path: &Path, content: &str) -> bool {
    use std::os::unix::fs::PermissionsExt;
    // Follow a symlinked kdeglobals so the link itself survives.
    let target = fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    if let Some(parent) = target.parent() {
        if fs::create_dir_all(parent).is_err() {
            return false;
        }
    }
    let mode = fs::metadata(&target)
        .ok()
        .map(|m| m.permissions().mode() & 0o7777);
    let tmp = std::path::PathBuf::from(format!("{}.tmp", target.display()));
    if fs::write(&tmp, content).is_err() {
        return false;
    }
    if let Some(mode) = mode {
        if fs::set_permissions(&tmp, fs::Permissions::from_mode(mode)).is_err() {
            let _ = fs::remove_file(&tmp);
            return false;
        }
    }
    if fs::rename(&tmp, &target).is_err() {
        let _ = fs::remove_file(&tmp);
        return false;
    }
    true
}

/// `mergeKdeColorScheme`: layer the scheme's groups/keys over `kdeglobals`, preserving
/// everything else (comments, ordering, unrelated groups).
pub fn merge_color_scheme(scheme: &Path, kdeglobals: &Path) -> Result<(), String> {
    let scheme_doc = parse(scheme, true)?;
    if scheme_doc.sections.is_empty() {
        return Err(format!(
            "color scheme contains no groups: {}",
            scheme.display()
        ));
    }
    let mut globals = parse(kdeglobals, false)?;
    merge(&mut globals, &scheme_doc);
    if !write_atomic(kdeglobals, &serialize(&globals)) {
        return Err(format!("failed to write {}", kdeglobals.display()));
    }
    Ok(())
}
