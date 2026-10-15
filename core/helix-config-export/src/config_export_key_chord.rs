//! Key chord strings for config export (`keyChordToString` / `parseKeyChordSpec` parity).

fn canonical_export_key_name(raw: &str) -> String {
    match raw.to_ascii_lowercase().as_str() {
        "esc" | "escape" => "Escape".to_string(),
        "del" | "delete" => "Delete".to_string(),
        "enter" | "return" => "Return".to_string(),
        "kp_enter" => "KP_Enter".to_string(),
        "space" | "spacebar" => "space".to_string(),
        "left" => "Left".to_string(),
        "right" => "Right".to_string(),
        "up" => "Up".to_string(),
        "down" => "Down".to_string(),
        "tab" => "Tab".to_string(),
        "iso_left_tab" | "shift_tab" => "ISO_Left_Tab".to_string(),
        other if other.len() == 1 => other.to_string(),
        other => other.to_string(),
    }
}

fn export_modifier_name(raw: &str) -> Option<&'static str> {
    match raw.to_ascii_lowercase().as_str() {
        "ctrl" | "control" | "ctl" => Some("Ctrl"),
        "shift" => Some("Shift"),
        "alt" | "option" => Some("Alt"),
        _ => None,
    }
}

/// Serialize a user keybind chord the way C++ `keyChordToString` does after `parseKeyChordSpec`.
pub fn export_key_chord_spec(spec: &str) -> String {
    let spec = spec.trim();
    if spec.is_empty() {
        return String::new();
    }
    let tokens: Vec<&str> = spec
        .split('+')
        .map(str::trim)
        .filter(|part| !part.is_empty())
        .collect();
    if tokens.is_empty() {
        return String::new();
    }
    let mut out = String::new();
    for token in &tokens[..tokens.len() - 1] {
        let Some(name) = export_modifier_name(token) else {
            return spec.to_string();
        };
        out.push_str(name);
        out.push('+');
    }
    out.push_str(&canonical_export_key_name(tokens[tokens.len() - 1]));
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn export_matches_cpp_navigation_keybinds() {
        assert_eq!(export_key_chord_spec("escape"), "Escape");
        assert_eq!(export_key_chord_spec("return"), "Return");
        assert_eq!(export_key_chord_spec("shift+iso_left_tab"), "Shift+ISO_Left_Tab");
        assert_eq!(export_key_chord_spec("ctrl+c"), "Ctrl+c");
    }
}
