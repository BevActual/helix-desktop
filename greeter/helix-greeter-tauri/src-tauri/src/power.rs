//! Restart and shut down from the login screen. The greeter asks logind through `systemctl`, with fixed arguments, so the system's own policy decides
//! (the package installs a polkit rule that lets the greeter's account do these two things and nothing else). The page can only name one of two
//! actions; it never supplies a command.

use std::process::{Command, Stdio};

/// The command for an action the page may ask for.
pub fn command_for(action: &str) -> Option<&'static str> {
    match action {
        "poweroff" => Some("poweroff"),
        "reboot" => Some("reboot"),
        _ => None,
    }
}

/// Does it. A test can set `HELIX_GREETER_POWER_LOG` to a file, which gets the action's name instead of anything happening.
pub fn run(action: &str) -> Result<(), String> {
    let verb = command_for(action).ok_or("That is not something the login screen can do.")?;
    if let Some(log) = std::env::var_os("HELIX_GREETER_POWER_LOG") {
        return std::fs::write(log, verb).map_err(|e| e.to_string());
    }
    let output = Command::new("systemctl")
        .arg(verb)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .output()
        .map_err(|e| format!("Cannot ask the system: {e}"))?;
    if output.status.success() {
        Ok(())
    } else {
        let said = String::from_utf8_lossy(&output.stderr);
        let said = said.trim();
        Err(if said.is_empty() {
            "The system did not allow that.".into()
        } else {
            said.chars().take(160).collect()
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_two_actions_exist() {
        assert_eq!(command_for("poweroff"), Some("poweroff"));
        assert_eq!(command_for("reboot"), Some("reboot"));
        for bad in ["", "halt", "poweroff -f", "reboot; id", "suspend"] {
            assert_eq!(command_for(bad), None, "{bad}");
        }
    }
}
