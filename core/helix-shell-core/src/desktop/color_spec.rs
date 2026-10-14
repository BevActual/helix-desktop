//! Config color strings: palette role tokens or `#RRGGBB` (C++ `colorSpecFromConfigString` subset).

const ROLE_TOKENS: &[&str] = &[
    "primary",
    "on_primary",
    "secondary",
    "on_secondary",
    "tertiary",
    "on_tertiary",
    "error",
    "on_error",
    "surface",
    "on_surface",
    "surface_variant",
    "on_surface_variant",
    "outline",
    "shadow",
    "hover",
    "on_hover",
];

pub fn validate_color_config_string(text: &str) -> Result<(), String> {
    if text.is_empty() {
        return Ok(());
    }
    if text.len() > 64 {
        return Err("Color values must be short".into());
    }
    if ROLE_TOKENS.contains(&text) {
        return Ok(());
    }
    if text.len() == 7
        && text.starts_with('#')
        && text[1..].chars().all(|c| c.is_ascii_hexdigit())
    {
        return Ok(());
    }
    Err("Choose a palette role or hex color".into())
}

/// `#RRGGBB` accent colors for `Appearance1.SetAccent` / `SetRole` (C++ `hex()`).
pub fn validate_accent_hex(text: &str) -> Result<(), String> {
    if text.len() == 7
        && text.starts_with('#')
        && text[1..].chars().all(|c| c.is_ascii_hexdigit())
    {
        Ok(())
    } else {
        Err("Choose a palette role or hex color".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_roles_and_hex() {
        validate_color_config_string("").unwrap();
        validate_color_config_string("primary").unwrap();
        validate_color_config_string("#aabbcc").unwrap();
        assert!(validate_color_config_string("not-a-color").is_err());
    }
}
