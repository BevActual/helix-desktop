//! What to do when nobody is there, as plain tested data. Commands are argument lists, never shell strings, so nothing in a config
//! file is ever interpreted by a shell.

use std::time::Duration;

/// Locking sooner than this is more annoying than safe, and a screen-off before the lock would show the desktop to anyone who wakes it.
pub const MIN_LOCK_SECONDS: u64 = 15;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Config {
    pub enabled: bool,
    pub lock_after: Duration,
    /// `None`: never turn the screen off.
    pub screen_off_after: Option<Duration>,
    pub lock_command: Vec<String>,
    pub screen_off_command: Vec<String>,
    pub screen_on_command: Vec<String>,
}

fn words(list: &[&str]) -> Vec<String> {
    list.iter().map(|w| (*w).to_string()).collect()
}

impl Default for Config {
    fn default() -> Self {
        Self {
            enabled: true,
            lock_after: Duration::from_secs(300),
            screen_off_after: Some(Duration::from_secs(420)),
            lock_command: words(&["swaylock", "--daemonize", "--color", "0e1028"]),
            screen_off_command: words(&["niri", "msg", "action", "power-off-monitors"]),
            screen_on_command: words(&["niri", "msg", "action", "power-on-monitors"]),
        }
    }
}

fn command(table: &toml::Table, key: &str, fallback: Vec<String>) -> Vec<String> {
    let Some(list) = table.get(key).and_then(toml::Value::as_array) else {
        return fallback;
    };
    let words: Vec<String> = list
        .iter()
        .filter_map(|v| v.as_str().map(str::to_string))
        .collect();
    // A command is a program and its arguments, every one a string; anything else is not trusted to mean what it says.
    if words.is_empty() || words.len() != list.len() || words[0].is_empty() {
        fallback
    } else {
        words
    }
}

impl Config {
    /// Reads `idle.toml`. A missing, unreadable or odd file means the defaults; nothing in it can make the machine lock *less* than the
    /// minimum, and a screen-off can never come before the lock.
    pub fn parse(text: &str) -> Self {
        let defaults = Self::default();
        let Ok(table) = text.parse::<toml::Table>() else {
            return defaults;
        };
        let seconds = |key: &str| {
            table
                .get(key)
                .and_then(toml::Value::as_integer)
                .and_then(|n| u64::try_from(n).ok())
        };
        let lock = seconds("lock_after_seconds").map_or(defaults.lock_after, |s| {
            Duration::from_secs(s.max(MIN_LOCK_SECONDS))
        });
        let off = match table
            .get("screen_off_after_seconds")
            .and_then(toml::Value::as_integer)
        {
            Some(0) => None,
            Some(n) if n > 0 => Some(Duration::from_secs(n as u64)),
            _ => defaults.screen_off_after,
        }
        .map(|d| d.max(lock + Duration::from_secs(5)));
        Self {
            enabled: table
                .get("enabled")
                .and_then(toml::Value::as_bool)
                .unwrap_or(true),
            lock_after: lock,
            screen_off_after: off,
            lock_command: command(&table, "lock_command", defaults.lock_command),
            screen_off_command: command(&table, "screen_off_command", defaults.screen_off_command),
            screen_on_command: command(&table, "screen_on_command", defaults.screen_on_command),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_lock_at_five_minutes_and_turn_the_screen_off_after_seven() {
        let c = Config::default();
        assert!(c.enabled);
        assert_eq!(c.lock_after, Duration::from_secs(300));
        assert_eq!(c.screen_off_after, Some(Duration::from_secs(420)));
        assert_eq!(c.lock_command[0], "swaylock");
    }

    #[test]
    fn nothing_or_nonsense_means_the_defaults() {
        assert_eq!(Config::parse(""), Config::default());
        assert_eq!(Config::parse("this is [not toml"), Config::default());
        assert_eq!(
            Config::parse("lock_after_seconds = \"soon\""),
            Config::default()
        );
    }

    #[test]
    fn times_can_be_set_and_the_lock_has_a_floor() {
        let c = Config::parse("lock_after_seconds = 120\nscreen_off_after_seconds = 600");
        assert_eq!(
            (c.lock_after, c.screen_off_after),
            (Duration::from_secs(120), Some(Duration::from_secs(600)))
        );
        assert_eq!(
            Config::parse("lock_after_seconds = 1").lock_after,
            Duration::from_secs(MIN_LOCK_SECONDS)
        );
        assert_eq!(
            Config::parse("lock_after_seconds = -5").lock_after,
            Duration::from_secs(300),
            "a negative time is nonsense"
        );
    }

    #[test]
    fn the_screen_never_goes_off_before_the_lock() {
        let c = Config::parse("lock_after_seconds = 300\nscreen_off_after_seconds = 60");
        assert_eq!(c.screen_off_after, Some(Duration::from_secs(305)));
    }

    #[test]
    fn zero_means_never_turn_the_screen_off() {
        assert_eq!(
            Config::parse("screen_off_after_seconds = 0").screen_off_after,
            None
        );
    }

    #[test]
    fn it_can_be_switched_off() {
        assert!(!Config::parse("enabled = false").enabled);
        assert!(
            Config::parse("enabled = \"no\"").enabled,
            "only a real boolean counts"
        );
    }

    #[test]
    fn a_command_is_a_list_of_strings_or_it_is_ignored() {
        let c = Config::parse("lock_command = [\"my-locker\", \"--fast\"]");
        assert_eq!(c.lock_command, ["my-locker", "--fast"]);
        let d = Config::default().lock_command;
        assert_eq!(
            Config::parse("lock_command = \"swaylock; rm -rf ~\"").lock_command,
            d,
            "a string is not a command"
        );
        assert_eq!(Config::parse("lock_command = []").lock_command, d);
        assert_eq!(Config::parse("lock_command = [\"ok\", 5]").lock_command, d);
        assert_eq!(Config::parse("lock_command = [\"\"]").lock_command, d);
    }
}
