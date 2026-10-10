//! The commands an agent (or a key binding) may send to the shell over its command socket, in the wire format the desktop service already
//! speaks (`<cwd>\x1e<command> <argument>...`). Only the commands named in [`SUPPORTED`] exist here: everything else is refused with a plain
//! "not supported", so the service lists only what really works. Parsing is strict: a number is whole digits (and an optional `%`) inside its
//! range, a word is one of the listed words, an unexpected extra argument is an error, and nothing is guessed.

use crate::shell_surface::PANELS;

/// The most a command line may hold (the service sends short lines; a longer one is not from it).
pub const MAX_LINE: usize = 1024;

/// The commands this shell carries out, each also classified in `protocols/ipc/msg-risk.toml` (a test checks).
pub const SUPPORTED: &[&str] = &[
    "volume-up",
    "volume-down",
    "volume-mute",
    "volume-set",
    "brightness-up",
    "brightness-down",
    "brightness-set",
    "theme-mode-set",
    "theme-mode-toggle",
    "theme-mode-get",
    "nightlight-enable",
    "nightlight-disable",
    "nightlight-toggle",
    "notification-dnd-set",
    "notification-dnd-toggle",
    "notification-dnd-status",
    "panel-open",
    "panel-close",
    "panel-toggle",
    "settings-open",
    "status",
    "window-close",
    "workspace-switch",
    "app-launch",
    "clipboard-text",
    "clipboard-copy",
    "clipboard-clear",
];

/// How long free text on the wire may be (`app-launch` query, `clipboard-copy` payload).
const MAX_LITERAL_CHARS: usize = 800;

const VOLUME_STEP: i32 = 5;
const BRIGHTNESS_STEP: i32 = 10;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Theme {
    Dark,
    Light,
    Auto,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Command {
    /// Percentage points, negative for quieter.
    VolumeStep(i32),
    VolumeToggleMute,
    VolumeSet(f64),
    BrightnessStep(i32),
    BrightnessSet(f64),
    ThemeSet(Theme),
    ThemeToggle,
    ThemeGet,
    NightLight(Option<bool>),
    DndSet(bool),
    DndToggle,
    DndStatus,
    PanelOpen(String),
    PanelToggle(String),
    PanelClose(Option<String>),
    /// Open Helix Settings, optionally at a named section/page.
    SettingsOpen(Option<String>),
    /// Print the desktop's state (no titles or other program text).
    Status,
    /// Close the focused window, or the window with this niri id.
    WindowClose(Option<u64>),
    /// The next or previous workspace.
    WorkspaceSwitch(Direction),
    /// Launch an installed application by desktop id or name (rest of the wire line).
    AppLaunch(String),
    /// Read the current clipboard text (observe-private).
    ClipboardText,
    /// Put text on the clipboard (rest of the wire line; may contain spaces).
    ClipboardCopy(String),
    /// Clear clipboard history in the shell and the current selection.
    ClipboardClear,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    Next,
    Prev,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MsgError {
    Empty,
    TooLong,
    Unknown,
    Usage(&'static str),
}

impl MsgError {
    /// What goes back on the wire: the service treats a reply starting `error:` as a failure.
    pub fn wire(&self) -> String {
        match self {
            MsgError::Empty => "error: empty command\n".into(),
            MsgError::TooLong => "error: IPC command too large\n".into(),
            MsgError::Unknown => "error: unknown command (this shell does not support it)\n".into(),
            MsgError::Usage(text) => format!("error: {text}\n"),
        }
    }
}

/// A whole number of `lo..=hi`, written as digits with an optional trailing `%`.
fn whole(text: &str, lo: i32, hi: i32) -> Result<i32, MsgError> {
    let digits = text.strip_suffix('%').unwrap_or(text);
    if digits.is_empty() || digits.len() > 3 || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return Err(MsgError::Usage("expected a whole number"));
    }
    let value: i32 = digits.parse().map_err(|_| MsgError::Usage("expected a whole number"))?;
    if (lo..=hi).contains(&value) {
        Ok(value)
    } else {
        Err(MsgError::Usage("number out of range"))
    }
}

/// `[step]` or `[all step]` for the brightness commands (the optional first word names the display; only `all` is understood).
fn level_args<'a>(args: &'a [&'a str]) -> Result<Option<&'a str>, MsgError> {
    match args {
        [] => Ok(None),
        [one] if *one != "all" => Ok(Some(one)),
        ["all"] => Ok(None),
        ["all", value] => Ok(Some(value)),
        _ => Err(MsgError::Usage("unexpected arguments")),
    }
}

fn no_args(args: &[&str]) -> Result<(), MsgError> {
    args.is_empty().then_some(()).ok_or(MsgError::Usage("this command takes no arguments"))
}

fn panel(id: &str) -> Result<String, MsgError> {
    PANELS.contains(&id).then(|| id.to_string()).ok_or(MsgError::Usage("unknown panel"))
}

fn literal_payload(rest: &str) -> Result<String, MsgError> {
    // Agent Surface puts free text after the command name with spaces preserved (no quoting).
    // Newlines are allowed in clipboard payloads; the reserved wire separators are not.
    if rest.contains('\x1e') || rest.contains('\0') {
        return Err(MsgError::Usage("control characters are not allowed"));
    }
    let text = rest.trim_end_matches(['\n', '\r']);
    if text.chars().count() > MAX_LITERAL_CHARS {
        return Err(MsgError::Usage("text is too long"));
    }
    Ok(text.to_string())
}

/// One command line, with or without the `<cwd>\x1e` prefix the service puts in front.
pub fn parse(line: &str) -> Result<Command, MsgError> {
    if line.len() > MAX_LINE {
        return Err(MsgError::TooLong);
    }
    let line = line.split_once('\x1e').map_or(line, |(_, rest)| rest).trim_matches(|c| c == '\n' || c == '\r');
    if line.contains('\x1e') || line.contains('\0') {
        return Err(MsgError::Usage("control characters are not allowed"));
    }
    let (name, rest) = match line.split_once(' ') {
        Some((name, rest)) => (name, Some(rest)),
        None => (line, None),
    };
    if name.is_empty() {
        return Err(MsgError::Empty);
    }
    if !SUPPORTED.contains(&name) {
        return Err(MsgError::Unknown);
    }
    // Free-text verbs take the remainder of the line (spaces kept). Everything else stays word-oriented.
    if matches!(name, "app-launch" | "clipboard-copy") {
        let text = literal_payload(rest.unwrap_or(""))?;
        return Ok(match name {
            "app-launch" => {
                if text.is_empty() {
                    return Err(MsgError::Usage("app-launch needs a desktop id or name"));
                }
                if text.chars().any(|c| c.is_control()) {
                    return Err(MsgError::Usage("control characters are not allowed"));
                }
                Command::AppLaunch(text)
            }
            "clipboard-copy" => {
                if text.is_empty() {
                    return Err(MsgError::Usage("clipboard-copy needs text"));
                }
                Command::ClipboardCopy(text)
            }
            _ => unreachable!(),
        });
    }
    if line.chars().any(|c| c.is_control()) {
        return Err(MsgError::Usage("control characters are not allowed"));
    }
    let args: Vec<&str> = rest
        .map(|r| r.split(' ').filter(|w| !w.is_empty()).collect())
        .unwrap_or_default();
    Ok(match name {
        "volume-up" | "volume-down" => {
            let step = match args.as_slice() {
                [] => VOLUME_STEP,
                [one] => whole(one, 1, 100)?,
                _ => return Err(MsgError::Usage("unexpected arguments")),
            };
            Command::VolumeStep(if name == "volume-up" { step } else { -step })
        }
        "volume-mute" => {
            no_args(&args)?;
            Command::VolumeToggleMute
        }
        "volume-set" => match args.as_slice() {
            [one] => Command::VolumeSet(f64::from(whole(one, 0, 100)?)),
            _ => return Err(MsgError::Usage("volume-set needs a value from 0 to 100")),
        },
        "brightness-up" | "brightness-down" => {
            let step = level_args(&args)?.map_or(Ok(BRIGHTNESS_STEP), |v| whole(v, 1, 100))?;
            Command::BrightnessStep(if name == "brightness-up" { step } else { -step })
        }
        "brightness-set" => match level_args(&args)? {
            Some(value) => Command::BrightnessSet(f64::from(whole(value, 0, 100)?)),
            None => return Err(MsgError::Usage("brightness-set needs a value from 0 to 100")),
        },
        "theme-mode-set" => match args.as_slice() {
            ["dark"] => Command::ThemeSet(Theme::Dark),
            ["light"] => Command::ThemeSet(Theme::Light),
            ["auto"] => Command::ThemeSet(Theme::Auto),
            _ => return Err(MsgError::Usage("theme-mode-set takes dark, light or auto")),
        },
        "theme-mode-toggle" => {
            no_args(&args)?;
            Command::ThemeToggle
        }
        "theme-mode-get" => {
            no_args(&args)?;
            Command::ThemeGet
        }
        "nightlight-enable" | "nightlight-disable" | "nightlight-toggle" => {
            no_args(&args)?;
            Command::NightLight(match name {
                "nightlight-enable" => Some(true),
                "nightlight-disable" => Some(false),
                _ => None,
            })
        }
        "notification-dnd-set" => match args.as_slice() {
            ["on" | "true" | "1"] => Command::DndSet(true),
            ["off" | "false" | "0"] => Command::DndSet(false),
            _ => return Err(MsgError::Usage("notification-dnd-set takes on or off")),
        },
        "notification-dnd-toggle" => {
            no_args(&args)?;
            Command::DndToggle
        }
        "notification-dnd-status" => {
            no_args(&args)?;
            Command::DndStatus
        }
        "panel-open" | "panel-toggle" => match args.as_slice() {
            [id] => {
                let id = panel(id)?;
                if name == "panel-open" {
                    Command::PanelOpen(id)
                } else {
                    Command::PanelToggle(id)
                }
            }
            _ => return Err(MsgError::Usage("this command takes one panel name")),
        },
        "panel-close" => match args.as_slice() {
            [] => Command::PanelClose(None),
            [id] => Command::PanelClose(Some(panel(id)?)),
            _ => return Err(MsgError::Usage("unexpected arguments")),
        },
        "settings-open" => match args.as_slice() {
            [] => Command::SettingsOpen(None),
            [section]
                if section.len() <= 40
                    && section
                        .bytes()
                        .all(|b| b.is_ascii_lowercase() || b == b'-' || b == b'_') =>
            {
                Command::SettingsOpen(Some((*section).to_string()))
            }
            [_] => {
                return Err(MsgError::Usage(
                    "settings section is lowercase letters, hyphens or underscores",
                ))
            }
            _ => {
                return Err(MsgError::Usage(
                    "settings-open takes no arguments or one section name",
                ))
            }
        },
        "status" => {
            no_args(&args)?;
            Command::Status
        }
        "window-close" => match args.as_slice() {
            [] => Command::WindowClose(None),
            [id] if id.len() <= 10 && id.bytes().all(|b| b.is_ascii_digit()) => {
                Command::WindowClose(Some(id.parse().map_err(|_| MsgError::Usage("not a window id"))?))
            }
            [_] => return Err(MsgError::Usage("a window id is whole digits")),
            _ => return Err(MsgError::Usage("unexpected arguments")),
        },
        "workspace-switch" => match args.as_slice() {
            ["next"] => Command::WorkspaceSwitch(Direction::Next),
            ["prev"] => Command::WorkspaceSwitch(Direction::Prev),
            _ => return Err(MsgError::Usage("workspace-switch takes next or prev")),
        },
        "clipboard-text" => {
            no_args(&args)?;
            Command::ClipboardText
        }
        "clipboard-clear" => {
            no_args(&args)?;
            Command::ClipboardClear
        }
        _ => return Err(MsgError::Unknown),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wire(line: &str) -> String {
        format!("/\x1e{line}")
    }

    #[test]
    fn the_services_wire_format_parses_with_or_without_the_prefix() {
        assert_eq!(parse(&wire("volume-up")), Ok(Command::VolumeStep(5)));
        assert_eq!(parse("volume-up"), Ok(Command::VolumeStep(5)));
        assert_eq!(parse("/home/me/project\x1evolume-up 10\n"), Ok(Command::VolumeStep(10)));
        assert_eq!(parse(&wire("volume-up  10 ")), Ok(Command::VolumeStep(10)), "extra spaces do not matter");
    }

    #[test]
    fn volume_numbers_are_whole_and_in_range() {
        assert_eq!(parse("volume-down 100"), Ok(Command::VolumeStep(-100)));
        assert_eq!(parse("volume-up 1"), Ok(Command::VolumeStep(1)));
        assert_eq!(parse("volume-up 25%"), Ok(Command::VolumeStep(25)));
        assert_eq!(parse("volume-up 007"), Ok(Command::VolumeStep(7)));
        for bad in ["0", "101", "-5", "+5", "5.5", "five", "%", "1000", "5%%", "5 5", "0x10", "٣"] {
            assert!(parse(&format!("volume-up {bad}")).is_err(), "{bad:?}");
        }
        assert_eq!(parse("volume-set 0"), Ok(Command::VolumeSet(0.0)));
        assert_eq!(parse("volume-set 100"), Ok(Command::VolumeSet(100.0)));
        assert_eq!(parse("volume-set 40%"), Ok(Command::VolumeSet(40.0)));
        assert!(parse("volume-set 101").is_err() && parse("volume-set").is_err() && parse("volume-set 5 5").is_err());
    }

    #[test]
    fn brightness_takes_a_step_or_all_and_a_step() {
        assert_eq!(parse("brightness-up"), Ok(Command::BrightnessStep(10)));
        assert_eq!(parse("brightness-down 20"), Ok(Command::BrightnessStep(-20)));
        assert_eq!(parse("brightness-up all 15"), Ok(Command::BrightnessStep(15)));
        assert_eq!(parse("brightness-up all"), Ok(Command::BrightnessStep(10)));
        assert_eq!(parse("brightness-set 60"), Ok(Command::BrightnessSet(60.0)));
        assert_eq!(parse("brightness-set all 60"), Ok(Command::BrightnessSet(60.0)));
        assert_eq!(parse("brightness-set 0"), Ok(Command::BrightnessSet(0.0)));
        for bad in ["brightness-set", "brightness-set all", "brightness-set 101", "brightness-set DP-1 50", "brightness-up DP-1 5", "brightness-up all 5 5", "brightness-set all 5 5"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn themes_night_light_and_do_not_disturb() {
        assert_eq!(parse("theme-mode-set dark"), Ok(Command::ThemeSet(Theme::Dark)));
        assert_eq!(parse("theme-mode-set light"), Ok(Command::ThemeSet(Theme::Light)));
        assert_eq!(parse("theme-mode-set auto"), Ok(Command::ThemeSet(Theme::Auto)));
        for bad in ["theme-mode-set", "theme-mode-set Dark", "theme-mode-set sepia", "theme-mode-set dark dark", "theme-mode-toggle now", "theme-mode-get x"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
        assert_eq!(parse("theme-mode-toggle"), Ok(Command::ThemeToggle));
        assert_eq!(parse("theme-mode-get"), Ok(Command::ThemeGet));
        assert_eq!(parse("nightlight-enable"), Ok(Command::NightLight(Some(true))));
        assert_eq!(parse("nightlight-disable"), Ok(Command::NightLight(Some(false))));
        assert_eq!(parse("nightlight-toggle"), Ok(Command::NightLight(None)));
        assert!(parse("nightlight-enable now").is_err());
        for word in ["on", "true", "1"] {
            assert_eq!(parse(&format!("notification-dnd-set {word}")), Ok(Command::DndSet(true)));
        }
        for word in ["off", "false", "0"] {
            assert_eq!(parse(&format!("notification-dnd-set {word}")), Ok(Command::DndSet(false)));
        }
        for bad in ["notification-dnd-set", "notification-dnd-set maybe", "notification-dnd-set on off", "notification-dnd-set ON"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
        assert_eq!(parse("notification-dnd-toggle"), Ok(Command::DndToggle));
        assert_eq!(parse("notification-dnd-status"), Ok(Command::DndStatus));
        assert!(parse("volume-mute x").is_err() && parse("volume-mute") == Ok(Command::VolumeToggleMute));
    }

    #[test]
    fn panels_are_only_the_shells_own() {
        assert_eq!(parse("panel-open launcher"), Ok(Command::PanelOpen("launcher".into())));
        assert_eq!(parse("panel-toggle control-center"), Ok(Command::PanelToggle("control-center".into())));
        assert_eq!(parse("panel-close"), Ok(Command::PanelClose(None)));
        assert_eq!(parse("panel-close launcher"), Ok(Command::PanelClose(Some("launcher".into()))));
        for bad in ["panel-open", "panel-open nope", "panel-open launcher context", "panel-open Launcher", "panel-open ../x", "panel-close a b", "panel-toggle"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn settings_open_takes_an_optional_section() {
        assert_eq!(parse("settings-open"), Ok(Command::SettingsOpen(None)));
        assert_eq!(parse("settings-open network"), Ok(Command::SettingsOpen(Some("network".into()))));
        assert_eq!(parse("settings-open control-center"), Ok(Command::SettingsOpen(Some("control-center".into()))));
        for bad in ["settings-open Network", "settings-open a b", "settings-open ../x", "settings-open weird!"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
        assert!(parse(&format!("settings-open {}", "a".repeat(41))).is_err());
    }

    #[test]
    fn status_windows_and_workspaces() {
        assert_eq!(parse("status"), Ok(Command::Status));
        assert!(parse("status now").is_err());
        assert_eq!(parse("window-close"), Ok(Command::WindowClose(None)));
        assert_eq!(parse("window-close 12"), Ok(Command::WindowClose(Some(12))));
        assert_eq!(parse("window-close 0"), Ok(Command::WindowClose(Some(0))));
        for bad in ["window-close -1", "window-close 1.5", "window-close abc", "window-close 12 13", "window-close 99999999999", "window-close +3", "window-close 0x10"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
        assert_eq!(parse("workspace-switch next"), Ok(Command::WorkspaceSwitch(Direction::Next)));
        assert_eq!(parse("workspace-switch prev"), Ok(Command::WorkspaceSwitch(Direction::Prev)));
        for bad in ["workspace-switch", "workspace-switch up", "workspace-switch Next", "workspace-switch next prev", "workspace-switch 2"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn app_launch_and_clipboard_keep_spaces_in_free_text() {
        assert_eq!(
            parse("app-launch Helix Settings"),
            Ok(Command::AppLaunch("Helix Settings".into()))
        );
        assert_eq!(
            parse("app-launch org.helix.Settings.desktop"),
            Ok(Command::AppLaunch("org.helix.Settings.desktop".into()))
        );
        assert_eq!(
            parse(&wire("clipboard-copy hello world")),
            Ok(Command::ClipboardCopy("hello world".into()))
        );
        assert_eq!(
            parse("clipboard-copy line1\nline2"),
            Ok(Command::ClipboardCopy("line1\nline2".into()))
        );
        assert_eq!(parse("clipboard-text"), Ok(Command::ClipboardText));
        assert_eq!(parse("clipboard-clear"), Ok(Command::ClipboardClear));
        for bad in [
            "app-launch",
            "app-launch\x07x",
            "clipboard-copy",
            "clipboard-text now",
            "clipboard-clear now",
            "clipboard-copy \x1esecret",
        ] {
            assert!(parse(bad).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn anything_else_is_not_supported_and_odd_lines_are_refused() {
        for unknown in ["bar-toggle", "session", "wallpaper-next", "VOLUME-UP", "volume-up;ls", "session", "window-switcher"] {
            assert_eq!(parse(unknown), Err(MsgError::Unknown), "{unknown}");
        }
        assert_eq!(parse(""), Err(MsgError::Empty));
        assert_eq!(parse("/\x1e"), Err(MsgError::Empty));
        assert_eq!(parse("   "), Err(MsgError::Empty));
        assert!(parse("volume-up\0").is_err() && parse("volume-up\x07").is_err() && parse("volume-up 5\t5").is_err());
        assert_eq!(parse(&"a".repeat(MAX_LINE + 1)), Err(MsgError::TooLong));
        assert!(parse(&format!("volume-up {}", "1".repeat(MAX_LINE - 15))).is_err());
    }

    #[test]
    fn errors_go_back_as_error_lines() {
        for error in [MsgError::Empty, MsgError::TooLong, MsgError::Unknown, MsgError::Usage("x")] {
            assert!(error.wire().starts_with("error: ") && error.wire().ends_with('\n'));
        }
    }

    #[test]
    fn every_supported_command_is_classified_and_none_is_blocked() {
        let policy = include_str!("../../../../protocols/ipc/msg-risk.toml");
        for name in SUPPORTED {
            let line = policy.lines().find(|l| l.starts_with(&format!("{name} = "))).unwrap_or_else(|| panic!("{name} is not classified"));
            assert!(!line.contains("blocked"), "{line}");
        }
        let observe = ["theme-mode-get", "notification-dnd-status", "status"];
        // Closing a window ends someone's work: it needs a person's say-so.
        assert!(policy.contains("window-close = \"confirm\""));
        assert!(policy.contains("app-launch = \"confirm\""));
        assert!(policy.contains("clipboard-copy = \"confirm\""));
        assert!(policy.contains("clipboard-clear = \"confirm\""));
        assert!(policy.contains("clipboard-text = \"observe-private\""));
        for name in observe {
            assert!(policy.contains(&format!("{name} = \"observe\"")), "{name}");
        }
        let mut sorted: Vec<&&str> = SUPPORTED.iter().collect();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), SUPPORTED.len(), "no duplicates");
    }

    #[test]
    fn every_supported_command_parses_with_valid_arguments() {
        // A command that is listed but cannot be parsed would be offered to the model and always fail.
        let examples = [
            "volume-up", "volume-down", "volume-mute", "volume-set 50", "brightness-up", "brightness-down", "brightness-set 50", "theme-mode-set dark",
            "theme-mode-toggle", "theme-mode-get", "nightlight-enable", "nightlight-disable", "nightlight-toggle", "notification-dnd-set on",
            "notification-dnd-toggle", "notification-dnd-status", "panel-open launcher", "panel-close", "panel-toggle launcher", "settings-open", "status", "window-close", "workspace-switch next",
            "app-launch Helix Settings", "clipboard-text", "clipboard-copy hello world", "clipboard-clear",
        ];
        assert_eq!(examples.len(), SUPPORTED.len());
        for (example, name) in examples.iter().zip(SUPPORTED) {
            assert!(example.starts_with(name), "{example} vs {name}");
            assert!(parse(example).is_ok(), "{example}");
        }
    }
}
