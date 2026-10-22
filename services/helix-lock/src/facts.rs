//! The plain facts the lock screen shows: whose screen it is, the accent colour, the clock text, and which PAM service checks the password.

use std::path::Path;

/// The eight accents Settings offers, as the dark theme draws them.
pub const ACCENTS: &[(&str, [u8; 3])] = &[
    ("blue", [0x0a, 0x84, 0xff]),
    ("purple", [0xbf, 0x5a, 0xf2]),
    ("pink", [0xff, 0x37, 0x5f]),
    ("red", [0xff, 0x45, 0x3a]),
    ("orange", [0xff, 0x9f, 0x0a]),
    ("yellow", [0xff, 0xd6, 0x0a]),
    ("green", [0x30, 0xd1, 0x58]),
    ("graphite", [0x98, 0x98, 0x9d]),
];

pub fn accent_rgb(name: &str) -> [u8; 3] {
    ACCENTS
        .iter()
        .find(|(n, _)| *n == name)
        .map(|(_, rgb)| *rgb)
        .unwrap_or(ACCENTS[0].1)
}

/// `theme.accent` from the settings file (`[theme] accent = "purple"`); blue when there is none.
pub fn accent_from_settings(text: &str) -> String {
    text.parse::<toml::Table>()
        .ok()
        .and_then(|t| {
            t.get("theme")?
                .as_table()?
                .get("accent")?
                .as_str()
                .map(str::to_string)
        })
        .filter(|name| ACCENTS.iter().any(|(n, _)| n == name))
        .unwrap_or_else(|| "blue".into())
}

pub fn accent_name(settings: &Path) -> String {
    std::fs::read_to_string(settings)
        .map(|t| accent_from_settings(&t))
        .unwrap_or_else(|_| "blue".into())
}

/// The account's login name and its full name (the first field of the passwd comment), for the person who is locked in.
pub fn who() -> (String, String) {
    // SAFETY: `getpwuid` returns a pointer to static storage or null; the fields are read at once and copied.
    unsafe {
        let entry = libc::getpwuid(libc::getuid());
        if entry.is_null() {
            let login = std::env::var("USER").unwrap_or_default();
            return (login.clone(), login);
        }
        let text = |p: *const libc::c_char| {
            if p.is_null() {
                String::new()
            } else {
                std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned()
            }
        };
        let login = text((*entry).pw_name);
        let full = text((*entry).pw_gecos)
            .split(',')
            .next()
            .unwrap_or("")
            .trim()
            .to_string();
        let shown = if full.is_empty() { login.clone() } else { full };
        (login, shown)
    }
}

/// Which PAM service to ask: the one the package installs, else the one a locker the system already has uses, else `login`.
pub fn pam_service(exists: impl Fn(&str) -> bool) -> &'static str {
    if let Ok(service) = std::env::var("HELIX_LOCK_PAM_SERVICE") {
        if !service.is_empty() {
            return Box::leak(service.into_boxed_str());
        }
    }
    ["helix-lock", "swaylock", "login"]
        .into_iter()
        .find(|s| exists(s))
        .unwrap_or("login")
}

/// `(time, date)` for the clock: "14:05" and "Wednesday, October 21", from a broken-down local time.
pub fn clock_text(
    hour: u32,
    minute: u32,
    twelve: bool,
    weekday: &str,
    month: &str,
    day: u32,
) -> (String, String) {
    let time = if twelve {
        let h = if hour.is_multiple_of(12) {
            12
        } else {
            hour % 12
        };
        format!("{h}:{minute:02} {}", if hour < 12 { "AM" } else { "PM" })
    } else {
        format!("{hour:02}:{minute:02}")
    };
    (time, format!("{weekday}, {month} {day}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_accent_comes_from_settings_and_falls_back_to_blue() {
        assert_eq!(
            accent_from_settings("[theme]\naccent = \"purple\"\n"),
            "purple"
        );
        assert_eq!(accent_from_settings("[theme]\naccent = \"teal\"\n"), "blue");
        assert_eq!(accent_from_settings("nonsense"), "blue");
        assert_eq!(accent_rgb("purple"), [0xbf, 0x5a, 0xf2]);
        assert_eq!(accent_rgb("nothing"), [0x0a, 0x84, 0xff]);
    }

    #[test]
    fn the_clock_is_written_both_ways() {
        assert_eq!(
            clock_text(14, 5, false, "Wednesday", "October", 21),
            ("14:05".into(), "Wednesday, October 21".into())
        );
        assert_eq!(clock_text(0, 7, true, "Monday", "May", 3).0, "12:07 AM");
        assert_eq!(clock_text(15, 30, true, "Monday", "May", 3).0, "3:30 PM");
    }

    #[test]
    fn the_package_service_is_preferred() {
        assert_eq!(
            pam_service(|s| s == "helix-lock" || s == "login"),
            "helix-lock"
        );
        assert_eq!(pam_service(|s| s == "swaylock"), "swaylock");
        assert_eq!(pam_service(|_| false), "login");
    }
}
