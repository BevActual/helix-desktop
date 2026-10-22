//! The plain part of the agent display's input helper: what a command may say, and how text and keys become keyboard events. Nothing here touches
//! Wayland. The helper takes one JSON object per line on stdin, and every command is checked here before anything is sent to the display.

use serde::Deserialize;

/// The longest text one command may type, and the most keys one combination may press.
pub const MAX_TEXT: usize = 2000;
pub const MAX_COMBO: usize = 6;

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(tag = "op", rename_all = "lowercase")]
pub enum Command {
    /// The display's size in pixels; absolute positions are relative to it.
    Size {
        w: u32,
        h: u32,
    },
    Move {
        x: f64,
        y: f64,
    },
    Button {
        button: String,
        down: bool,
    },
    Click {
        button: String,
        x: f64,
        y: f64,
    },
    Scroll {
        dx: f64,
        dy: f64,
    },
    /// One key by its keysym name (`Return`, `Tab`, `Left`, `a`): pressed and released, or only one of the two with `down`.
    Key {
        name: String,
        down: Option<bool>,
    },
    Text {
        text: String,
    },
    /// Keys pressed together in order and released in reverse (`["Control_L", "c"]`).
    Combo {
        keys: Vec<String>,
    },
}

pub fn parse_command(line: &str) -> Result<Command, String> {
    let command: Command =
        serde_json::from_str(line.trim()).map_err(|e| format!("not a command: {e}"))?;
    match &command {
        Command::Size { w, h } if !(160..=8192).contains(w) || !(120..=8192).contains(h) => {
            return Err("that is not a display size".into())
        }
        Command::Move { x, y } | Command::Click { x, y, .. }
            if !x.is_finite() || !y.is_finite() =>
        {
            return Err("that is not a position".into())
        }
        Command::Button { button, .. } | Command::Click { button, .. }
            if evdev_button(button).is_none() =>
        {
            return Err("buttons are left, right and middle".into())
        }
        Command::Scroll { dx, dy }
            if !dx.is_finite() || !dy.is_finite() || dx.abs() > 50.0 || dy.abs() > 50.0 =>
        {
            return Err("scroll by up to 50 steps".into())
        }
        Command::Key { name, .. } if named_keysym(name).is_none() => {
            return Err(format!("unknown key {name}"))
        }
        Command::Text { text } if text.chars().count() > MAX_TEXT => {
            return Err("that is too much text for one command".into())
        }
        Command::Combo { keys }
            if keys.is_empty()
                || keys.len() > MAX_COMBO
                || keys.iter().any(|k| named_keysym(k).is_none()) =>
        {
            return Err("a combination is up to six known keys".into())
        }
        _ => {}
    }
    Ok(command)
}

/// The Linux button codes of the three buttons.
pub fn evdev_button(name: &str) -> Option<u32> {
    match name {
        "left" => Some(0x110),
        "right" => Some(0x111),
        "middle" => Some(0x112),
        _ => None,
    }
}

/// A key's keysym as the keymap text names it: a named key as is, any single character as `U+XXXX`. `None` for anything else, so nothing a client sends can
/// reach the keymap text unchecked.
pub fn named_keysym(name: &str) -> Option<String> {
    const NAMED: &[&str] = &[
        "Return",
        "Tab",
        "Escape",
        "BackSpace",
        "Delete",
        "Home",
        "End",
        "Page_Up",
        "Page_Down",
        "Left",
        "Right",
        "Up",
        "Down",
        "space",
        "Insert",
        "Menu",
        "F1",
        "F2",
        "F3",
        "F4",
        "F5",
        "F6",
        "F7",
        "F8",
        "F9",
        "F10",
        "F11",
        "F12",
        "Control_L",
        "Shift_L",
        "Alt_L",
        "Super_L",
    ];
    if NAMED.contains(&name) {
        return Some(name.to_string());
    }
    let mut chars = name.chars();
    match (chars.next(), chars.next()) {
        (Some(c), None) if !c.is_control() => Some(format!("U{:04X}", c as u32)),
        _ => None,
    }
}

/// The keysym a character types: the newline is Return and the tab is Tab, anything printable is itself.
pub fn char_keysym(c: char) -> Option<String> {
    match c {
        '\n' | '\r' => Some("Return".into()),
        '\t' => Some("Tab".into()),
        c if !c.is_control() => Some(format!("U{:04X}", c as u32)),
        _ => None,
    }
}

/// A keymap whose keys are exactly `keysyms` (each on its own key, no layout, no shift), so any text can be typed whatever the display's layout is.
/// Key `i` is the evdev code `i + 1`. The four modifier keys are given their modifier, so a combination such as Control+c works.
pub fn keymap_text(keysyms: &[String]) -> String {
    let mut keycodes = String::new();
    let mut symbols = String::new();
    let mut modifiers = String::new();
    for (i, sym) in keysyms.iter().enumerate() {
        keycodes.push_str(&format!("<K{i}> = {};\n", i + 9));
        symbols.push_str(&format!("key <K{i}> {{ [ {sym} ] }};\n"));
        let modifier = match sym.as_str() {
            "Shift_L" => Some("Shift"),
            "Control_L" => Some("Control"),
            "Alt_L" => Some("Mod1"),
            "Super_L" => Some("Mod4"),
            _ => None,
        };
        if let Some(m) = modifier {
            modifiers.push_str(&format!("modifier_map {m} {{ <K{i}> }};\n"));
        }
    }
    format!(
        "xkb_keymap {{\nxkb_keycodes \"h\" {{\nminimum = 8;\nmaximum = {};\n{keycodes}}};\nxkb_types \"h\" {{ include \"complete\" }};\nxkb_compat \"h\" {{ include \"complete\" }};\nxkb_symbols \"h\" {{\n{symbols}{modifiers}}};\n}};\n",
        keysyms.len() + 9
    )
}

/// The distinct keysyms a command needs, in order of first use, and the evdev key codes to press for it (as `(code, down)` events).
/// The keysyms a command needs and the key events to send for it.
pub type KeyPlan = (Vec<String>, Vec<(u32, bool)>);

pub fn key_events(command: &Command) -> Option<KeyPlan> {
    let mut symbols: Vec<String> = Vec::new();
    let mut code = |sym: String| -> u32 {
        let at = symbols.iter().position(|s| *s == sym).unwrap_or_else(|| {
            symbols.push(sym);
            symbols.len() - 1
        });
        at as u32 + 1
    };
    let mut events = Vec::new();
    match command {
        Command::Key { name, down } => {
            let key = code(named_keysym(name)?);
            match down {
                Some(true) => events.push((key, true)),
                Some(false) => events.push((key, false)),
                None => events.extend([(key, true), (key, false)]),
            }
        }
        Command::Text { text } => {
            for c in text.chars() {
                let key = code(char_keysym(c)?);
                events.extend([(key, true), (key, false)]);
            }
        }
        Command::Combo { keys } => {
            let codes: Vec<u32> = keys
                .iter()
                .map(|k| named_keysym(k).map(&mut code))
                .collect::<Option<_>>()?;
            events.extend(codes.iter().map(|k| (*k, true)));
            events.extend(codes.iter().rev().map(|k| (*k, false)));
        }
        _ => return None,
    }
    Some((symbols, events))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn commands_are_parsed_and_checked() {
        assert_eq!(
            parse_command(r#"{"op":"click","button":"left","x":10,"y":20.5}"#),
            Ok(Command::Click {
                button: "left".into(),
                x: 10.0,
                y: 20.5
            })
        );
        assert_eq!(
            parse_command(r#"{"op":"key","name":"Return"}"#),
            Ok(Command::Key {
                name: "Return".into(),
                down: None
            })
        );
        for bad in [
            "nonsense",
            r#"{"op":"fly"}"#,
            r#"{"op":"button","button":"back","down":true}"#,
            r#"{"op":"key","name":"XF86Poweroff"}"#,
            r#"{"op":"key","name":"ab"}"#,
            r#"{"op":"scroll","dx":0,"dy":500}"#,
            r#"{"op":"size","w":1,"h":1}"#,
            r#"{"op":"combo","keys":[]}"#,
            r#"{"op":"combo","keys":["a","b","c","d","e","f","g"]}"#,
        ] {
            assert!(parse_command(bad).is_err(), "{bad}");
        }
        let long = format!(r#"{{"op":"text","text":"{}"}}"#, "x".repeat(MAX_TEXT + 1));
        assert!(parse_command(&long).is_err());
    }

    #[test]
    fn text_becomes_one_key_per_distinct_character() {
        let (symbols, events) = key_events(&Command::Text {
            text: "aba\n".into(),
        })
        .unwrap();
        assert_eq!(symbols, ["U0061", "U0062", "Return"]);
        assert_eq!(
            events,
            [
                (1, true),
                (1, false),
                (2, true),
                (2, false),
                (1, true),
                (1, false),
                (3, true),
                (3, false)
            ]
        );
        let (symbols, _) = key_events(&Command::Text {
            text: "é€".into()
        })
        .unwrap();
        assert_eq!(
            symbols,
            ["U00E9", "U20AC"],
            "any character, whatever the layout"
        );
        assert!(
            key_events(&Command::Text {
                text: "a\u{7}".into()
            })
            .is_none(),
            "a control character is not typed"
        );
    }

    #[test]
    fn a_combination_presses_in_order_and_releases_in_reverse() {
        let (symbols, events) = key_events(&Command::Combo {
            keys: vec!["Control_L".into(), "c".into()],
        })
        .unwrap();
        assert_eq!(symbols, ["Control_L", "U0063"]);
        assert_eq!(events, [(1, true), (2, true), (2, false), (1, false)]);
    }

    #[test]
    fn the_generated_keymap_is_one_the_toolkit_accepts_and_means_what_it_says() {
        use xkbcommon::xkb;
        let (symbols, _) = key_events(&Command::Combo {
            keys: vec![
                "Control_L".into(),
                "Shift_L".into(),
                "é".into(),
                "Return".into(),
            ],
        })
        .unwrap();
        let text = keymap_text(&symbols);
        let context = xkb::Context::new(xkb::CONTEXT_NO_FLAGS);
        let keymap = xkb::Keymap::new_from_string(
            &context,
            text,
            xkb::KEYMAP_FORMAT_TEXT_V1,
            xkb::KEYMAP_COMPILE_NO_FLAGS,
        )
        .expect("the keymap compiles");
        let mut state = xkb::State::new(&keymap);
        // Key i is evdev code i + 1, which is the xkb keycode i + 9.
        assert_eq!(
            state.key_get_one_sym(xkb::Keycode::new(3 + 8)).raw(),
            0xE9,
            "the third key types é"
        );
        assert_eq!(
            state.key_get_one_sym(xkb::Keycode::new(4 + 8)).raw(),
            0xFF0D,
            "and the fourth Return"
        );
        state.update_key(xkb::Keycode::new(1 + 8), xkb::KeyDirection::Down);
        assert!(
            state.mod_name_is_active(xkb::MOD_NAME_CTRL, xkb::STATE_MODS_EFFECTIVE),
            "the Control key sets the Control modifier"
        );
        assert!(!state.mod_name_is_active(xkb::MOD_NAME_SHIFT, xkb::STATE_MODS_EFFECTIVE));
    }
}
