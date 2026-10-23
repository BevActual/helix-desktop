//! The agent display: a second, headless Helix compositor that only the programs an agent launched run on, with its own runtime directory, its
//! own session bus and its own profile (home, settings, cookies). The person watches it, and helps, through a viewer; the agent acts on it and only
//! on it (`docs/design/computer-use.md`, option A). It is a different place from the person's desktop: nothing the person has open, no socket the shell
//! listens on, no secret service and no stored login of theirs exists there.
//!
//! This is the plain part: what the compositor is told, how its start-up is recognised, and what environment a program on the display gets. The
//! shell does the process handling.

use std::path::{Path, PathBuf};

/// Where the display's compositor listens, as it said in its log.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Started {
    pub ipc_socket: String,
    pub wayland_display: String,
}

/// Recognises the compositor's two start-up lines (`IPC listening on: <path>` and the `wayland-N` socket it opened).
pub fn parse_started(log: &str) -> Option<Started> {
    let ipc_socket = log.lines().rev().find_map(|line| {
        line.split_once("IPC listening on: ")
            .map(|(_, rest)| rest.trim().to_string())
    })?;
    if !ipc_socket.starts_with('/') || ipc_socket.contains(char::is_whitespace) {
        return None;
    }
    let wayland_display = log
        .split(|c: char| !c.is_ascii_alphanumeric() && c != '-')
        .rev()
        .find(|w| {
            w.strip_prefix("wayland-")
                .is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
        })?;
    Some(Started {
        ipc_socket,
        wayland_display: wayland_display.to_string(),
    })
}

/// The display's compositor configuration: no bar, no hotkey overlay, windows open filling the display (a computer-use screen is one big window at a
/// time, as on a phone or a kiosk), and screenshots go where the shell asks.
pub fn config_kdl(shots_dir: &str) -> String {
    format!(
        "hotkey-overlay {{ skip-at-startup; }}\nprefer-no-csd\nlayout {{ gaps 0; }}\nscreenshot-path \"{}/shot-%H%M%S.png\"\nwindow-rule {{ open-maximized true; }}\n",
        shots_dir.chars().filter(|c| c.is_ascii_alphanumeric() || matches!(c, '/' | '.' | '_' | '-')).collect::<String>()
    )
}

/// The folders of the display, all under one private runtime directory so a program on it cannot see the person's sockets (the shell's command socket,
/// the audio server, the session bus).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Places {
    /// Private `XDG_RUNTIME_DIR` of everything on the display; the compositor's sockets and the display's bus live here.
    pub runtime: PathBuf,
    /// The display's own home: settings, caches and logins made on the display stay on it.
    pub home: PathBuf,
    pub shots: PathBuf,
    pub bus: PathBuf,
}

pub fn places(person_runtime: &Path, data_home: &Path) -> Places {
    let runtime = person_runtime.join("helix-agent-display");
    Places {
        bus: runtime.join("bus"),
        shots: runtime.join("shots"),
        home: data_home.join("helix/agent-display/home"),
        runtime,
    }
}

/// Variables the display's programs must not inherit from the person's session: how to reach the person's compositor or X server, and anything
/// that points at the shell. (The shell also removes its own `SHELL_ONLY_VARS`.)
pub const REMOVE: &[&str] = &[
    "NIRI_SOCKET",
    "WAYLAND_SOCKET",
    "DISPLAY",
    "XAUTHORITY",
    "XDG_ACTIVATION_TOKEN",
    "DESKTOP_STARTUP_ID",
    "SSH_AUTH_SOCK",
    "GPG_AGENT_INFO",
    "DBUS_STARTER_ADDRESS",
    "DBUS_STARTER_BUS_TYPE",
    "HELIX_AGENT_IPC_DIR",
    "HELIX_SURFACE_CONTROL",
    "HELIX_ALLOW_UNPROTECTED_APPROVALS",
];

/// The environment a program started on the display gets, on top of the person's (after [`REMOVE`]).
pub fn program_env(places: &Places, wayland_display: &str) -> Vec<(String, String)> {
    let home = &places.home;
    let s = |p: &Path| p.to_string_lossy().into_owned();
    vec![
        ("WAYLAND_DISPLAY".into(), wayland_display.to_string()),
        ("XDG_RUNTIME_DIR".into(), s(&places.runtime)),
        ("XDG_SESSION_TYPE".into(), "wayland".into()),
        (
            "DBUS_SESSION_BUS_ADDRESS".into(),
            format!("unix:path={}", s(&places.bus)),
        ),
        ("HOME".into(), s(home)),
        ("XDG_CONFIG_HOME".into(), s(&home.join(".config"))),
        ("XDG_DATA_HOME".into(), s(&home.join(".local/share"))),
        ("XDG_CACHE_HOME".into(), s(&home.join(".cache"))),
        ("XDG_STATE_HOME".into(), s(&home.join(".local/state"))),
        ("GDK_BACKEND".into(), "wayland".into()),
        ("QT_QPA_PLATFORM".into(), "wayland".into()),
        ("MOZ_ENABLE_WAYLAND".into(), "1".into()),
        ("ELECTRON_OZONE_PLATFORM_HINT".into(), "wayland".into()),
    ]
}

/// The compositor's own environment: headless (no window of its own), and rooted in the private runtime directory.
pub fn compositor_env(places: &Places) -> Vec<(String, String)> {
    let mut env = program_env(places, "");
    env.retain(|(k, _)| {
        !matches!(
            k.as_str(),
            "WAYLAND_DISPLAY" | "GDK_BACKEND" | "QT_QPA_PLATFORM"
        )
    });
    env.push(("HELIX_NESTED_HEADLESS".into(), "1".into()));
    env
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_compositors_start_up_lines_are_recognised() {
        let log = "INFO niri starting\nINFO listening on Wayland socket: wayland-3\nINFO IPC listening on: /run/user/1000/helix-agent-display/niri.wayland-3.99.sock\n";
        assert_eq!(
            parse_started(log),
            Some(Started {
                ipc_socket: "/run/user/1000/helix-agent-display/niri.wayland-3.99.sock".into(),
                wayland_display: "wayland-3".into()
            })
        );
        assert_eq!(parse_started("nothing yet"), None);
        assert_eq!(
            parse_started("IPC listening on: relative.sock\nwayland-0"),
            None,
            "a socket path is absolute"
        );
        assert_eq!(
            parse_started("IPC listening on: /x.sock"),
            None,
            "and a display name is needed too"
        );
    }

    #[test]
    fn the_display_is_rooted_under_one_private_runtime_directory() {
        let p = places(
            Path::new("/run/user/1000"),
            Path::new("/home/a/.local/share"),
        );
        assert_eq!(p.runtime, Path::new("/run/user/1000/helix-agent-display"));
        assert_eq!(
            p.home,
            Path::new("/home/a/.local/share/helix/agent-display/home")
        );
        let env = program_env(&p, "wayland-0");
        let get = |k: &str| env.iter().find(|(n, _)| n == k).map(|(_, v)| v.as_str());
        assert_eq!(
            get("XDG_RUNTIME_DIR"),
            Some("/run/user/1000/helix-agent-display")
        );
        assert_eq!(
            get("DBUS_SESSION_BUS_ADDRESS"),
            Some("unix:path=/run/user/1000/helix-agent-display/bus")
        );
        assert_eq!(
            get("HOME"),
            Some("/home/a/.local/share/helix/agent-display/home")
        );
        assert_eq!(
            get("XDG_CONFIG_HOME"),
            Some("/home/a/.local/share/helix/agent-display/home/.config")
        );
        assert_eq!(get("WAYLAND_DISPLAY"), Some("wayland-0"));
        for variable in [
            "NIRI_SOCKET",
            "DISPLAY",
            "SSH_AUTH_SOCK",
            "HELIX_AGENT_IPC_DIR",
        ] {
            assert!(REMOVE.contains(&variable), "{variable}");
            assert!(get(variable).is_none(), "{variable} is never set");
        }
        let compositor = compositor_env(&p);
        assert!(compositor
            .iter()
            .any(|(k, v)| k == "HELIX_NESTED_HEADLESS" && v == "1"));
        assert!(
            !compositor.iter().any(|(k, _)| k == "WAYLAND_DISPLAY"),
            "the compositor does not nest in the person's display"
        );
    }

    #[test]
    fn the_configuration_cannot_be_broken_out_of() {
        let kdl = config_kdl("/run/x/shots\"; spawn \"rm\"");
        // Only path characters survive in the one place text goes, so there is no way to end the string and add a node.
        assert!(
            kdl.contains("screenshot-path \"/run/x/shotsspawnrm/shot-%H%M%S.png\""),
            "{kdl}"
        );
        assert_eq!(kdl.matches('"').count(), 2);
    }
}
