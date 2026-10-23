//! Helix key binds: what Settings → Keyboard changes. Like the look and input settings they are *overrides* over `config.kdl`: the person's files are
//! never rewritten, the overrides live in `$XDG_STATE_HOME/helix/bind-overrides.json`, they are applied to the configuration every time it is loaded
//! (at start and at each reload), and they take effect at once when set over IPC. An override wins over a bind on the same chord in the config; giving
//! the chord back (`reset`) forgets it. The compositor also reports every bind in effect (`HelixBinds`) and where it came from, so nothing else has to
//! parse the config files to know.

use std::path::PathBuf;
use std::str::FromStr;

use niri_config::{Action, Bind, Config, Key, Modifiers, Trigger};
use niri_ipc::{HelixBind, HelixBindsChange};
use serde::{Deserialize, Serialize};
use smithay::input::keyboard::xkb::keysym_get_name;

use crate::niri::State;

const MAX_OVERRIDES: usize = 200;

#[derive(Serialize, Deserialize, Debug, Clone, Default, PartialEq, Eq)]
struct Overrides {
    binds: Vec<Entry>,
}

/// One override: a chord, and the action it now does (`None`: it is unbound).
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq, Eq)]
struct Entry {
    chord: String,
    action: Option<String>,
}

fn file() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/state")))
        .unwrap_or_default();
    base.join("helix/bind-overrides.json")
}

fn load() -> Overrides {
    std::fs::read_to_string(file())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn save(overrides: &Overrides) -> Result<(), String> {
    let path = file();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temporary = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(overrides).map_err(|e| e.to_string())?;
    std::fs::write(&temporary, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// A key as a config writes it, in one fixed order, so two spellings of a chord compare equal.
pub fn chord_of(key: &Key) -> String {
    let mut out = String::new();
    for (flag, name) in [
        (Modifiers::COMPOSITOR, "Mod"),
        (Modifiers::CTRL, "Ctrl"),
        (Modifiers::SHIFT, "Shift"),
        (Modifiers::ALT, "Alt"),
        (Modifiers::SUPER, "Super"),
        (Modifiers::ISO_LEVEL3_SHIFT, "ISO_Level3_Shift"),
        (Modifiers::ISO_LEVEL5_SHIFT, "ISO_Level5_Shift"),
    ] {
        if key.modifiers.contains(flag) {
            out.push_str(name);
            out.push('+');
        }
    }
    out.push_str(match key.trigger {
        Trigger::Keysym(keysym) => return out + &keysym_get_name(keysym),
        Trigger::MouseLeft => "MouseLeft",
        Trigger::MouseRight => "MouseRight",
        Trigger::MouseMiddle => "MouseMiddle",
        Trigger::MouseBack => "MouseBack",
        Trigger::MouseForward => "MouseForward",
        Trigger::WheelScrollDown => "WheelScrollDown",
        Trigger::WheelScrollUp => "WheelScrollUp",
        Trigger::WheelScrollLeft => "WheelScrollLeft",
        Trigger::WheelScrollRight => "WheelScrollRight",
        Trigger::TouchpadScrollDown => "TouchpadScrollDown",
        Trigger::TouchpadScrollUp => "TouchpadScrollUp",
        Trigger::TouchpadScrollLeft => "TouchpadScrollLeft",
        Trigger::TouchpadScrollRight => "TouchpadScrollRight",
        Trigger::TabletStylusButton1 => "TabletStylusButton1",
        Trigger::TabletStylusButton2 => "TabletStylusButton2",
        Trigger::TabletStylusButton3 => "TabletStylusButton3",
    });
    out
}

fn key_of(chord: &str) -> Result<Key, String> {
    let ok = !chord.is_empty()
        && chord.len() <= 64
        && chord
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '_' | '-'));
    if !ok {
        return Err("a shortcut is keys joined with +, like Mod+Shift+E".into());
    }
    Key::from_str(chord).map_err(|_| format!("{chord} is not a shortcut"))
}

/// The action as a config writes it: one action, no block, no second statement, nothing that starts a comment.
fn action_text_ok(action: &str) -> bool {
    let action = action.trim();
    !action.is_empty()
        && action.len() <= 512
        && !action.contains(['{', '}', ';', '\n', '\r', '\\'])
        && !action.contains("//")
        && !action.contains("/*")
}

fn parse_bind(chord: &str, action: &str) -> Result<Bind, String> {
    let key = key_of(chord)?;
    if !action_text_ok(action) {
        return Err("an action is one thing to do, like focus-column-left".into());
    }
    let text = format!(
        "binds {{\n{} {{ {}; }}\n}}\n",
        chord_of(&key),
        action.trim()
    );
    let config =
        Config::parse_mem(&text).map_err(|_| format!("{} is not an action", action.trim()))?;
    let mut binds = config.binds.0;
    if binds.len() != 1 || binds[0].key != key {
        return Err(format!("{} is not an action", action.trim()));
    }
    Ok(binds.remove(0))
}

/// What cannot be taken away, so the person can always get back to the shortcuts and out of a session.
fn protected(action: &Action) -> bool {
    matches!(
        action,
        Action::HelixSummon | Action::ShowHotkeyOverlay | Action::Quit(_)
    )
}

/// The variant's name as a config writes an action (`FocusColumnLeft` → `focus-column-left`) and what it was given.
fn describe(action: &Action) -> (String, String) {
    let debug = format!("{action:?}");
    let (name, rest) = match debug.split_once(['(', ' ', '{']) {
        Some((name, rest)) => (name, rest),
        None => (debug.as_str(), ""),
    };
    let mut kebab = String::new();
    for (i, c) in name.chars().enumerate() {
        if c.is_ascii_uppercase() {
            if i > 0 {
                kebab.push('-');
            }
            kebab.push(c.to_ascii_lowercase());
        } else {
            kebab.push(c);
        }
    }
    let detail: String = rest
        .trim_end_matches([')', '}', ' '])
        .chars()
        .filter(|c| !matches!(c, '"' | '[' | ']'))
        .collect::<String>()
        .replace(", ", " ");
    (kebab, detail.chars().take(200).collect())
}

/// Splits `a, b(c, d), e` at the commas that are not inside brackets.
fn top_level_args(text: &str) -> Vec<String> {
    let mut parts = Vec::new();
    let (mut depth, mut start, mut quoted) = (0i32, 0usize, false);
    for (i, c) in text.char_indices() {
        match c {
            '"' => quoted = !quoted,
            '(' | '[' | '{' if !quoted => depth += 1,
            ')' | ']' | '}' if !quoted => depth -= 1,
            ',' if !quoted && depth == 0 => {
                parts.push(text[start..i].trim().to_string());
                start = i + 1;
            }
            _ => {}
        }
    }
    let last = text[start..].trim();
    if !last.is_empty() {
        parts.push(last.to_string());
    }
    parts
}

fn number_text(n: &str) -> Option<String> {
    let f: f64 = n.parse().ok()?;
    Some(if f.fract() == 0.0 {
        format!("{}", f as i64)
    } else {
        format!("{f}")
    })
}

/// One printed argument as a config writes it, or `None` to leave it out. Booleans become properties, named in turn from `props`.
fn argument(token: &str, props: &mut std::slice::Iter<'_, &str>) -> Option<Option<String>> {
    if token == "None" {
        return Some(None);
    }
    if token == "true" || token == "false" {
        return Some(Some(format!("{}={token}", props.next()?)));
    }
    if let Some(inner) = token
        .strip_prefix("Some(")
        .and_then(|t| t.strip_suffix(')'))
    {
        return argument(inner, props);
    }
    for (wrapper, f) in [
        ("Index(", 0),
        ("Name(", 1),
        ("SetProportion(", 2),
        ("AdjustProportion(", 3),
        ("SetFixed(", 4),
        ("AdjustFixed(", 5),
    ] {
        if let Some(inner) = token
            .strip_prefix(wrapper)
            .and_then(|t| t.strip_suffix(')'))
        {
            let n = number_text(inner);
            return Some(Some(match f {
                0 => n?,
                1 => inner.to_string(),
                2 => format!("\"{}%\"", n?),
                3 => {
                    let n = n?;
                    format!("\"{}{n}%\"", if n.starts_with('-') { "" } else { "+" })
                }
                4 => n?,
                _ => {
                    let n = n?;
                    format!("\"{}{n}\"", if n.starts_with('-') { "" } else { "+" })
                }
            }));
        }
    }
    Some(Some(token.to_string()))
}

/// The action as a config writes it, worked out from how it prints (`Spawn([\"a\", \"b\"])` → `spawn \"a\" \"b\"`) and kept only if that text reads back
/// as the very same action: so what is shown can be put on another chord without losing anything. `None` for an action that does not come out right.
fn written(action: &Action) -> Option<String> {
    let debug = format!("{action:?}");
    let (name, _) = describe(action);
    let Some((_, rest)) = debug.split_once('(') else {
        return Some(name).filter(|n| verified(n, action));
    };
    let inner = rest.strip_suffix(')')?.trim();
    // One level of list becomes the arguments themselves.
    let inner = inner
        .strip_prefix('[')
        .and_then(|i| i.strip_suffix(']'))
        .unwrap_or(inner);
    let tokens = top_level_args(inner);
    // Which property each boolean is, tried in the orders a config could mean.
    let orders: &[&[&str]] = &[
        &["focus"],
        &["skip-confirmation"],
        &["show-pointer"],
        &["write-to-disk", "show-pointer"],
        &["write-to-disk"],
        &["show-pointer", "write-to-disk"],
    ];
    orders.iter().find_map(|order| {
        let mut props = order.iter();
        let mut words = Vec::new();
        for token in &tokens {
            words.extend(argument(token, &mut props)?);
        }
        // Properties go after the plain arguments.
        words.sort_by_key(|w| w.contains('=') && !w.starts_with('"'));
        let text = if words.is_empty() {
            name.clone()
        } else {
            format!("{name} {}", words.join(" "))
        };
        verified(&text, action).then_some(text)
    })
}

fn verified(text: &str, action: &Action) -> bool {
    action_text_ok(text) && parse_bind("F24", text).is_ok_and(|bind| bind.action == *action)
}

/// Puts the overrides over a configuration that was just loaded.
pub fn overlay(config: &mut Config, overrides: &Overrides) {
    for entry in &overrides.binds {
        let Ok(key) = key_of(&entry.chord) else {
            continue;
        };
        let bind = match &entry.action {
            Some(action) => match parse_bind(&entry.chord, action) {
                Ok(bind) => Some(bind),
                Err(err) => {
                    warn!("ignoring the saved bind {}: {err}", entry.chord);
                    continue;
                }
            },
            None => None,
        };
        config.binds.0.retain(|b| b.key != key);
        config.binds.0.extend(bind);
    }
}

pub fn apply_saved(config: &mut Config) {
    overlay(config, &load());
}

/// Every bind in config order, with the overrides marked.
fn report(config: &Config, overrides: &Overrides) -> Vec<HelixBind> {
    let mut out: Vec<HelixBind> = config
        .binds
        .0
        .iter()
        .map(|bind| {
            let chord = chord_of(&bind.key);
            let mine = overrides
                .binds
                .iter()
                .find(|e| key_of(&e.chord).is_ok_and(|k| k == bind.key));
            let (action, detail) = describe(&bind.action);
            HelixBind {
                chord,
                action,
                detail,
                title: bind.hotkey_overlay_title.clone().flatten(),
                source: if mine.is_some() { "helix" } else { "config" }.into(),
                action_kdl: mine
                    .and_then(|e| e.action.clone())
                    .or_else(|| written(&bind.action)),
            }
        })
        .collect();
    for entry in overrides.binds.iter().filter(|e| e.action.is_none()) {
        out.push(HelixBind {
            chord: entry.chord.clone(),
            action: "none".into(),
            source: "helix".into(),
            ..HelixBind::default()
        });
    }
    out
}

/// Refuses what cannot be applied, and works out the new overrides (without storing anything).
fn changed(
    config: &Config,
    stored: &Overrides,
    change: &HelixBindsChange,
) -> Result<Overrides, String> {
    let mut next = stored.clone();
    let mut touch =
        |chord: &str, action: Option<String>, next: &mut Overrides| -> Result<(), String> {
            let key = key_of(chord)?;
            // The base the person sees under this chord is the config's, unless an override already replaced it; either way it must not be a protected one.
            if config
                .binds
                .0
                .iter()
                .any(|b| b.key == key && protected(&b.action))
            {
                return Err(format!("{chord} is kept so you can always get back"));
            }
            let chord = chord_of(&key);
            next.binds
                .retain(|e| key_of(&e.chord).map_or(true, |k| k != key));
            next.binds.push(Entry { chord, action });
            Ok(())
        };
    for set in &change.set {
        parse_bind(&set.chord, &set.action)?;
        touch(&set.chord, Some(set.action.trim().to_string()), &mut next)?;
    }
    for chord in &change.unbind {
        touch(chord, None, &mut next)?;
    }
    for chord in &change.reset {
        let key = key_of(chord)?;
        next.binds
            .retain(|e| key_of(&e.chord).map_or(true, |k| k != key));
    }
    if next.binds.len() > MAX_OVERRIDES {
        return Err("too many shortcuts have been changed".into());
    }
    Ok(next)
}

impl State {
    pub fn helix_binds(&self) -> Vec<HelixBind> {
        report(&self.niri.config.borrow(), &load())
    }

    pub fn helix_set_binds(&mut self, change: HelixBindsChange) -> Result<(), String> {
        let stored = load();
        let next = changed(&self.niri.config.borrow(), &stored, &change)?;
        save(&next)?;
        if change.reset.is_empty() {
            let new_mod_key;
            {
                let mut config = self.niri.config.borrow_mut();
                overlay(&mut config, &next);
                new_mod_key = self.backend.mod_key(&config);
            }
            let config = self.niri.config.borrow();
            self.niri
                .hotkey_overlay
                .on_hotkey_config_updated(new_mod_key);
            self.niri.mods_with_mouse_binds =
                crate::input::mods_with_mouse_binds(new_mod_key, &config.binds);
            self.niri.mods_with_wheel_binds =
                crate::input::mods_with_wheel_binds(new_mod_key, &config.binds);
            self.niri.mods_with_tablet_stylus_binds =
                crate::input::mods_with_tablet_stylus_binds(new_mod_key, &config.binds);
            self.niri.mods_with_finger_scroll_binds =
                crate::input::mods_with_finger_scroll_binds(new_mod_key, &config.binds);
        } else if let Some(watcher) = &self.niri.config_file_watcher {
            // Giving a chord back means the config's bind has to come back: that is a reload, which applies what is still overridden.
            watcher.load_config(None);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config(text: &str) -> Config {
        Config::parse_mem(text).unwrap()
    }

    #[test]
    fn chords_are_written_one_way() {
        for (written, canonical) in [
            ("mod+shift+e", "Mod+Shift+E"),
            ("Shift+Mod+E", "Mod+Shift+e"),
            ("Ctrl+Alt+Delete", "Ctrl+Alt+Delete"),
            ("XF86AudioMute", "XF86AudioMute"),
            ("Mod+WheelScrollDown", "Mod+WheelScrollDown"),
        ] {
            let a = chord_of(&key_of(written).unwrap());
            let b = chord_of(&key_of(canonical).unwrap());
            assert_eq!(a, b, "{written}");
            assert_eq!(key_of(&a).unwrap(), key_of(written).unwrap());
        }
        assert!(key_of("Mod+").is_err());
        assert!(key_of("Mod+E; x").is_err());
    }

    #[test]
    fn an_override_replaces_the_config_and_a_reset_gives_it_back() {
        let base = "binds {\n Mod+D { spawn \"old\"; }\n Mod+Q { close-window; }\n}";
        let mut loaded = config(base);
        let change = HelixBindsChange {
            set: vec![niri_ipc::HelixBindSet {
                chord: "mod+d".into(),
                action: "helix-shell \"launcher\"".into(),
            }],
            unbind: vec!["Mod+Q".into()],
            reset: vec![],
        };
        let next = changed(&loaded, &Overrides::default(), &change).unwrap();
        overlay(&mut loaded, &next);
        assert_eq!(loaded.binds.0.len(), 1);
        assert!(matches!(&loaded.binds.0[0].action, Action::HelixShell(c) if c == "launcher"));
        let shown = report(&loaded, &next);
        assert_eq!(shown[0].source, "helix");
        assert_eq!(shown[0].action, "helix-shell");
        assert_eq!(
            shown[0].action_kdl.as_deref(),
            Some("helix-shell \"launcher\"")
        );
        assert_eq!(shown[1].action, "none");
        // Overlaying again does not pile up.
        overlay(&mut loaded, &next);
        assert_eq!(loaded.binds.0.len(), 1);
        // A reset forgets the override; the freshly loaded config is what shows.
        let back = changed(
            &loaded,
            &next,
            &HelixBindsChange {
                reset: vec!["Mod+D".into(), "Mod+Q".into()],
                ..Default::default()
            },
        )
        .unwrap();
        assert!(back.binds.is_empty());
        let mut fresh = config(base);
        overlay(&mut fresh, &back);
        assert_eq!(fresh.binds.0.len(), 2);
    }

    #[test]
    fn what_cannot_be_a_bind_is_refused() {
        let loaded = config(
            "binds {\n Mod+Shift+Space { helix-summon; }\n Mod+Slash { show-hotkey-overlay; }\n}",
        );
        let try_set = |chord: &str, action: &str| {
            changed(
                &loaded,
                &Overrides::default(),
                &HelixBindsChange {
                    set: vec![niri_ipc::HelixBindSet {
                        chord: chord.into(),
                        action: action.into(),
                    }],
                    ..Default::default()
                },
            )
        };
        assert!(try_set("Mod+E", "focus-column-left").is_ok());
        assert!(try_set("Mod+E", "not-a-thing").is_err());
        assert!(try_set("Mod+E", "focus-column-left; spawn \"x\"").is_err());
        assert!(try_set("Mod+E", "close-window } window-rule { open-floating true").is_err());
        assert!(try_set("Mod+E", "close-window // hide").is_err());
        assert!(try_set("Mod+E", "").is_err());
        assert!(
            try_set("Mod+Shift+Space", "close-window").is_err(),
            "the summon key is kept"
        );
        assert!(
            try_set("Mod+Slash", "close-window").is_err(),
            "the key that shows shortcuts is kept"
        );
        assert!(changed(
            &loaded,
            &Overrides::default(),
            &HelixBindsChange {
                unbind: vec!["Mod+Slash".into()],
                ..Default::default()
            }
        )
        .is_err());
    }

    #[test]
    fn actions_are_described_for_reading() {
        let loaded = config("binds {\n Mod+T { spawn \"foot\" \"-e\" \"htop\"; }\n Mod+H { focus-column-left; }\n Mod+L { helix-shell \"launcher\"; }\n}");
        let shown = report(&loaded, &Overrides::default());
        assert_eq!(
            (shown[0].action.as_str(), shown[0].detail.as_str()),
            ("spawn", "foot -e htop")
        );
        assert_eq!(
            (shown[1].action.as_str(), shown[1].detail.as_str()),
            ("focus-column-left", "")
        );
        assert_eq!(
            (shown[2].action.as_str(), shown[2].detail.as_str()),
            ("helix-shell", "launcher")
        );
        assert!(shown.iter().all(|b| b.source == "config"));
        assert_eq!(
            shown[0].action_kdl.as_deref(),
            Some("spawn \"foot\" \"-e\" \"htop\"")
        );
        assert_eq!(shown[1].action_kdl.as_deref(), Some("focus-column-left"));
        assert_eq!(
            shown[2].action_kdl.as_deref(),
            Some("helix-shell \"launcher\"")
        );
    }

    #[test]
    fn most_of_nirics_own_binds_can_be_moved() {
        // The default config ships with niri; where it is not installed there is nothing to check.
        let Ok(text) = std::fs::read_to_string("/usr/share/doc/niri/default-config.kdl") else {
            return;
        };
        let loaded = config(&text);
        let shown = report(&loaded, &Overrides::default());
        let movable = shown.iter().filter(|b| b.action_kdl.is_some()).count();
        let missing: Vec<_> = shown
            .iter()
            .filter(|b| b.action_kdl.is_none())
            .map(|b| format!("{} {} {}", b.chord, b.action, b.detail))
            .collect();
        eprintln!("{movable} of {} movable; not: {missing:?}", shown.len());
        assert!(movable * 10 >= shown.len() * 9, "{missing:?}");
    }
}
