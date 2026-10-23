//! `helix-shell "<command>"`: a key bind that asks the Helix shell to do something (open the launcher, lock, change the volume). The compositor
//! speaks to the shell on the session bus itself, so a key press does not start a script and a process on the way. If the shell is not there (or is
//! not the one that answers these calls) the same request goes the old way, through `helix-panel`, which knows every shell.

use crate::niri::State;
use crate::utils::spawning::spawn;

/// What a command is, on the shell's bus interface.
#[derive(Debug, PartialEq, Eq)]
enum Call {
    Panel(&'static str),
    Plain(&'static str),
    Step(&'static str, i32),
}

const PANELS: &[&str] = &[
    "launcher",
    "control-center",
    "assistant",
    "agents",
    "notifications",
    "quick-insert",
];

fn call_for(command: &str) -> Option<Call> {
    if let Some(panel) = PANELS.iter().find(|p| **p == command) {
        return Some(Call::Panel(panel));
    }
    Some(match command {
        "dnd" => Call::Plain("ToggleDoNotDisturb"),
        "night-light" => Call::Plain("ToggleNightLight"),
        "volume-mute" => Call::Plain("ToggleMute"),
        "bar-show" => Call::Plain("BarShow"),
        "bar-hide" => Call::Plain("BarHide"),
        "bar-toggle" => Call::Plain("BarToggle"),
        "volume-up" => Call::Step("AdjustVolume", 5),
        "volume-down" => Call::Step("AdjustVolume", -5),
        "brightness-up" => Call::Step("AdjustBrightness", 5),
        "brightness-down" => Call::Step("AdjustBrightness", -5),
        _ => return None,
    })
}

/// A command is a short name: nothing else is passed on to a program.
fn command_ok(command: &str) -> bool {
    !command.is_empty()
        && command.len() <= 32
        && command
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
}

#[cfg(feature = "dbus")]
fn ask(call: &Call) -> bool {
    use std::sync::OnceLock;
    use zbus::blocking::Connection;

    static BUS: OnceLock<Option<Connection>> = OnceLock::new();
    let Some(bus) = BUS.get_or_init(|| Connection::session().ok()) else {
        return false;
    };
    let (name, path, interface) = ("org.helix.Shell", "/org/helix/Shell", "org.helix.Shell1");
    let reply = match call {
        Call::Panel(panel) => {
            bus.call_method(Some(name), path, Some(interface), "TogglePanel", &(*panel,))
        }
        Call::Plain(method) => bus.call_method(Some(name), path, Some(interface), *method, &()),
        Call::Step(method, by) => {
            bus.call_method(Some(name), path, Some(interface), *method, &(*by,))
        }
    };
    reply.is_ok()
}

#[cfg(not(feature = "dbus"))]
fn ask(_call: &Call) -> bool {
    false
}

fn helix_panel() -> String {
    std::env::var_os("HOME")
        .map(|home| std::path::PathBuf::from(home).join(".local/bin/helix-panel"))
        .filter(|path| path.exists())
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_else(|| "helix-panel".into())
}

impl State {
    pub fn helix_shell(&mut self, command: String) {
        if !command_ok(&command) {
            warn!("helix-shell: refusing the command {command:?}");
            return;
        }
        // The bus call can wait for the shell, so it is made off the compositor's thread; if it cannot be made the old way is used.
        let fallback = vec![helix_panel(), command.clone()];
        let call = call_for(&command);
        let started = std::thread::Builder::new()
            .name("Helix Shell Call".to_owned())
            .spawn(move || {
                if !call.as_ref().is_some_and(ask) {
                    spawn(fallback, None);
                }
            });
        if let Err(err) = started {
            warn!("helix-shell: could not start a thread: {err:?}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn commands_map_to_the_shells_methods() {
        assert_eq!(call_for("launcher"), Some(Call::Panel("launcher")));
        assert_eq!(
            call_for("volume-down"),
            Some(Call::Step("AdjustVolume", -5))
        );
        assert_eq!(
            call_for("night-light"),
            Some(Call::Plain("ToggleNightLight"))
        );
        // Things the shell does not take on the bus go the old way.
        assert_eq!(call_for("lock"), None);
    }

    #[test]
    fn only_short_names_are_accepted() {
        assert!(command_ok("control-center"));
        assert!(!command_ok(""));
        assert!(!command_ok("lock; rm"));
        assert!(!command_ok("../x"));
        assert!(!command_ok(&"a".repeat(40)));
    }
}
