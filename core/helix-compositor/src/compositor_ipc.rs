//! One-shot questions to the compositor (`$NIRI_SOCKET`).

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct AgentDeskStatus {
    pub name: String,
    pub workspace_id: Option<u64>,
    pub window_count: u32,
    pub is_focused: bool,
    pub placements_pending: u32,
    pub available: bool,
}

/// The pointer and keyboard settings the compositor lets Settings change (`HelixInput`). In what the compositor reports every field is set; in a
/// change only the set fields are applied.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct InputSettings {
    pub touchpad_tap: Option<bool>,
    pub touchpad_natural_scroll: Option<bool>,
    pub touchpad_speed: Option<f64>,
    pub touchpad_disable_while_typing: Option<bool>,
    pub touchpad_click_method: Option<String>,
    pub mouse_natural_scroll: Option<bool>,
    pub mouse_speed: Option<f64>,
    pub mouse_accel_profile: Option<String>,
    pub mouse_left_handed: Option<bool>,
    pub keyboard_repeat_delay: Option<u16>,
    pub keyboard_repeat_rate: Option<u8>,
    pub keyboard_layout: Option<String>,
}

/// What had the keyboard when Mobius was summoned (`helix-summon`): only what the compositor lists about every window.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SummonContext {
    pub title: Option<String>,
    pub app_id: Option<String>,
    pub pid: Option<i32>,
    /// Milliseconds since the summon.
    pub age_ms: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct HelixCaps {
    pub liquid_glass: bool,
    pub agent_desk: bool,
    /// The compositor remembers display setups per set of connected displays and applies them itself.
    #[serde(default)]
    pub display_profiles: bool,
    /// `helix-summon` and the note it leaves for Mobius.
    #[serde(default)]
    pub summon: bool,
    /// `HelixInput` and `HelixSetInput`.
    #[serde(default)]
    pub input_settings: bool,
}

pub fn request_json(line: &str) -> Option<Value> {
    let path = std::env::var_os("NIRI_SOCKET")?;
    let mut stream = UnixStream::connect(path).ok()?;
    stream.set_read_timeout(Some(Duration::from_secs(2))).ok()?;
    stream
        .set_write_timeout(Some(Duration::from_secs(2)))
        .ok()?;
    stream.write_all(format!("{line}\n").as_bytes()).ok()?;
    let mut reply = String::new();
    BufReader::new(stream).read_line(&mut reply).ok()?;
    let value: Value = serde_json::from_str(&reply).ok()?;
    value.get("Ok").cloned()
}

pub fn helix_capabilities() -> HelixCaps {
    // The reply is `{"HelixCapabilities": {…}}`: the response variant wraps the fields.
    let Some(ok) = request_json("\"HelixCapabilities\"") else {
        return HelixCaps::default();
    };
    parse_caps(&ok)
}

fn parse_caps(ok: &Value) -> HelixCaps {
    ok.get("HelixCapabilities")
        .cloned()
        .and_then(|caps| serde_json::from_value(caps).ok())
        .unwrap_or_default()
}

/// Like `request_json`, but keeps the compositor's own words when it refuses.
fn request_result(line: &str) -> Result<Value, String> {
    let path = std::env::var_os("NIRI_SOCKET").ok_or("not running under the compositor")?;
    let mut stream = UnixStream::connect(path).map_err(|e| e.to_string())?;
    let timeout = Some(Duration::from_secs(5));
    stream
        .set_read_timeout(timeout)
        .map_err(|e| e.to_string())?;
    stream
        .set_write_timeout(timeout)
        .map_err(|e| e.to_string())?;
    stream
        .write_all(format!("{line}\n").as_bytes())
        .map_err(|e| e.to_string())?;
    let mut reply = String::new();
    BufReader::new(stream)
        .read_line(&mut reply)
        .map_err(|e| e.to_string())?;
    let value: Value = serde_json::from_str(&reply).map_err(|e| e.to_string())?;
    match (value.get("Ok"), value.get("Err")) {
        (Some(ok), _) => Ok(ok.clone()),
        (_, Some(err)) => Err(err.as_str().unwrap_or("the compositor refused").to_string()),
        _ => Err("unexpected reply".into()),
    }
}

/// The pointer and keyboard settings in effect (`None` when the compositor cannot say, as stock niri cannot).
pub fn input_settings() -> Option<InputSettings> {
    let ok = request_json("\"HelixInput\"")?;
    serde_json::from_value(ok.get("HelixInput")?.clone()).ok()
}

/// Changes the set fields now; the compositor remembers them and applies them again whenever its configuration is loaded.
pub fn set_input_settings(change: &InputSettings) -> Result<(), String> {
    let line = serde_json::json!({ "HelixSetInput": change }).to_string();
    request_result(&line).map(|_| ())
}

/// The window that had the keyboard when the person last pressed the summon key, once (the compositor forgets it as it hands it over, and after
/// a minute). `None` when there is none.
pub fn take_summon_context() -> Option<SummonContext> {
    let ok = request_json("\"HelixSummonContext\"")?;
    parse_summon(&ok)
}

fn parse_summon(ok: &Value) -> Option<SummonContext> {
    let inner = ok.get("HelixSummonContext")?;
    if inner.is_null() {
        return None;
    }
    serde_json::from_value(inner.clone()).ok()
}

/// Asks the compositor to remember how the displays connected now are set up, and to apply it again whenever this same set is connected.
pub fn save_display_profile() -> Result<(), String> {
    request_result("\"HelixSaveDisplayProfile\"").map(|_| ())
}

/// Whether a setup is remembered for the displays connected now (`None` when the compositor cannot say).
pub fn display_profile_saved() -> Option<bool> {
    request_json("\"HelixDisplayProfile\"")?
        .get("HelixDisplayProfile")?
        .get("saved")?
        .as_bool()
}

/// True when the compositor answers `HelixCapabilities` (helix-niri, not stock niri).
pub fn is_helix_compositor() -> bool {
    request_json("\"HelixCapabilities\"").is_some()
}

pub fn agent_desk_status() -> AgentDeskStatus {
    let Some(ok) = request_json("\"HelixAgentDesk\"") else {
        return AgentDeskStatus::default();
    };
    let mut status: AgentDeskStatus = ok
        .get("HelixAgentDesk")
        .cloned()
        .and_then(|desk| serde_json::from_value(desk).ok())
        .unwrap_or_default();
    status.available = true;
    status
}

/// Ask the compositor to reload its config (picks up `helix-setup.kdl` changes).
pub fn reload_config() {
    let line = serde_json::json!({"Action": {"LoadConfigFile": {}}}).to_string();
    let _ = request_json(&line);
}

/// Queue the next opened window(s) on the Helix agent desk without stealing focus.
pub fn place_on_agent_desk(count: u8) -> Result<(), String> {
    if !crate::compositor_config::agent_desk_placement_enabled() {
        return Ok(());
    }
    if !helix_capabilities().agent_desk {
        return Err("agent desk is not available on this compositor".into());
    }
    let line = serde_json::json!({"Action": {"HelixPlaceOnAgentDesk": {"count": count.max(1)}}})
        .to_string();
    if request_json(&line).is_none() {
        return Err("the compositor did not accept agent desk placement".into());
    }
    Ok(())
}

/// Scales the Displays page may ask for. Anything else is refused before it reaches the compositor.
pub const OUTPUT_SCALES: [f64; 5] = [1.0, 1.25, 1.5, 1.75, 2.0];

/// One mode the monitor advertised.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OutputMode {
    pub width: u32,
    pub height: u32,
    pub refresh_millihertz: u32,
    pub preferred: bool,
}

/// One connected output, as the compositor reported it.
#[derive(Debug, Clone, PartialEq)]
pub struct OutputSnapshot {
    pub name: String,
    pub make: String,
    pub model: String,
    pub serial: String,
    pub modes: Vec<OutputMode>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub refresh_millihertz: Option<u32>,
    pub scale: Option<f64>,
    pub transform: Option<String>,
    pub x: Option<i32>,
    pub y: Option<i32>,
    pub logical_width: Option<u32>,
    pub logical_height: Option<u32>,
    pub vrr_supported: bool,
    pub vrr_enabled: bool,
}

/// A change the compositor applies immediately. Settings also records it in `helix-setup.kdl`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum OutputAdjust {
    Scale(f64),
    /// Wire value: `Normal`, `90`, `180`, or `270`.
    Turn(&'static str),
    Mode {
        width: u16,
        height: u16,
        refresh_hz: f64,
    },
    /// `None` asks the compositor to place the output itself.
    Position(Option<(i32, i32)>),
    Vrr(bool),
    Power(bool),
}

/// Where one display sits relative to another.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Side {
    Right,
    Left,
    Above,
    Below,
}

/// Logical top-left of `this` when it sits on `side` of `other` (`x`, `y`, width, height).
pub fn place_beside(side: Side, other: (i32, i32, u32, u32), this_size: (u32, u32)) -> (i32, i32) {
    let (x, y, width, height) = other;
    match side {
        Side::Right => (x.saturating_add(width as i32), y),
        Side::Left => (x.saturating_sub(this_size.0 as i32), y),
        Side::Below => (x, y.saturating_add(height as i32)),
        Side::Above => (x, y.saturating_sub(this_size.1 as i32)),
    }
}

/// A name written in `helix-setup.kdl`: a connector, or "make model serial". Anything that could break out of a KDL string is refused.
pub fn config_name_ok(name: &str) -> bool {
    (1..=160).contains(&name.chars().count())
        && !name.trim().is_empty()
        && name
            .chars()
            .all(|c| !c.is_control() && c != '"' && c != '\\')
}

impl OutputSnapshot {
    /// The name this output's settings are saved under. The compositor matches `output "Make Model Serial"` as well as a connector, and a
    /// monitor keeps its identity when it moves to another port (a dock, a different cable), so it is saved by identity whenever it has one.
    /// Two monitors that report the same identity, or a monitor that reports none, are saved by connector instead.
    pub fn config_name(&self, all: &[OutputSnapshot]) -> String {
        let unknown = |text: &str| text.trim().is_empty() || text.eq_ignore_ascii_case("unknown");
        if unknown(&self.make) || unknown(&self.model) || unknown(&self.serial) {
            return self.name.clone();
        }
        let identity = format!("{} {} {}", self.make, self.model, self.serial);
        let twins = all
            .iter()
            .filter(|other| {
                format!("{} {} {}", other.make, other.model, other.serial)
                    .eq_ignore_ascii_case(&identity)
            })
            .count();
        if twins > 1 || !config_name_ok(&identity) {
            self.name.clone()
        } else {
            identity
        }
    }
}

/// Connector names the page is willing to send. A name that was not in the last output list is refused by the caller.
pub fn output_name_ok(name: &str) -> bool {
    (1..=64).contains(&name.len())
        && name
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
}

pub fn scale_from_token(value: &str) -> Option<f64> {
    OUTPUT_SCALES
        .into_iter()
        .find(|scale| scale_token(*scale) == Some(value))
}

pub fn scale_token(scale: f64) -> Option<&'static str> {
    const TABLE: [(f64, &str); 5] = [
        (1.0, "1"),
        (1.25, "1.25"),
        (1.5, "1.5"),
        (1.75, "1.75"),
        (2.0, "2"),
    ];
    TABLE
        .iter()
        .find(|(known, _)| (known - scale).abs() < 0.02)
        .map(|(_, token)| *token)
}

/// `normal`, `90`, `180`, `270` become the compositor's transform names.
pub fn turn_wire(value: &str) -> Option<&'static str> {
    match value {
        "normal" => Some("Normal"),
        "90" => Some("90"),
        "180" => Some("180"),
        "270" => Some("270"),
        _ => None,
    }
}

/// `1920x1080@59998` is one advertised mode. The number is the refresh rate in millihertz.
pub fn mode_from_token(value: &str) -> Option<(u16, u16, u32)> {
    let (size, refresh) = value.split_once('@')?;
    let (width, height) = size.split_once('x')?;
    let width = width.parse().ok()?;
    let height = height.parse().ok()?;
    let refresh = refresh.parse().ok()?;
    if width == 0 || height == 0 || refresh == 0 {
        return None;
    }
    Some((width, height, refresh))
}

pub fn mode_token(width: u32, height: u32, refresh_millihertz: u32) -> String {
    format!("{width}x{height}@{refresh_millihertz}")
}

/// KDL transform name (`normal`, `90`, …) for a compositor wire value.
pub fn kdl_transform(wire: &str) -> Option<&'static str> {
    match wire {
        "Normal" => Some("normal"),
        "90" => Some("90"),
        "180" => Some("180"),
        "270" => Some("270"),
        "Flipped" => Some("flipped"),
        "Flipped90" => Some("flipped-90"),
        "Flipped180" => Some("flipped-180"),
        "Flipped270" => Some("flipped-270"),
        _ => None,
    }
}

/// The JSON line for one output change. Refuses a name or a value the page does not offer.
pub fn output_request(name: &str, adjust: OutputAdjust) -> Result<String, &'static str> {
    if !output_name_ok(name) {
        return Err("unknown display");
    }
    let action = match adjust {
        OutputAdjust::Scale(scale) => {
            if scale_token(scale).is_none() {
                return Err("pick a scale");
            }
            serde_json::json!({"Scale": {"scale": {"Specific": scale}}})
        }
        OutputAdjust::Turn(turn) => {
            if !matches!(turn, "Normal" | "90" | "180" | "270") {
                return Err("pick an orientation");
            }
            serde_json::json!({"Transform": {"transform": turn}})
        }
        OutputAdjust::Mode {
            width,
            height,
            refresh_hz,
        } => {
            if width == 0 || height == 0 || !(1.0..=1000.0).contains(&refresh_hz) {
                return Err("pick a resolution");
            }
            serde_json::json!({"Mode": {"mode": {"Specific": {"width": width, "height": height, "refresh": refresh_hz}}}})
        }
        OutputAdjust::Position(Some((x, y))) => {
            if !(-100_000..=100_000).contains(&x) || !(-100_000..=100_000).contains(&y) {
                return Err("that arrangement is off the desktop");
            }
            serde_json::json!({"Position": {"position": {"Specific": {"x": x, "y": y}}}})
        }
        OutputAdjust::Position(None) => serde_json::json!({"Position": {"position": "Automatic"}}),
        OutputAdjust::Vrr(on) => {
            serde_json::json!({"Vrr": {"vrr": {"vrr": on, "on_demand": false}}})
        }
        OutputAdjust::Power(true) => serde_json::json!("On"),
        OutputAdjust::Power(false) => serde_json::json!("Off"),
    };
    Ok(serde_json::json!({"Output": {"output": name, "action": action}}).to_string())
}

/// Sends one output change. `Ok` only when the compositor says it applied the change.
pub fn apply_output(line: &str) -> Result<(), String> {
    match request_json(line) {
        Some(value)
            if value.get("OutputConfigChanged").and_then(Value::as_str) == Some("Applied") =>
        {
            Ok(())
        }
        Some(_) => Err("that display is not connected".into()),
        None => Err("the compositor did not apply the change".into()),
    }
}

pub fn connected_outputs() -> Vec<OutputSnapshot> {
    let Some(ok) = request_json("\"Outputs\"") else {
        return Vec::new();
    };
    parse_outputs(&ok)
}

pub fn parse_outputs(ok: &Value) -> Vec<OutputSnapshot> {
    let Some(outputs) = ok.get("Outputs").and_then(Value::as_object) else {
        return Vec::new();
    };
    let mut rows: Vec<OutputSnapshot> = outputs
        .iter()
        .filter_map(|(key, output)| snapshot(key, output))
        .collect();
    rows.sort_by(|a, b| a.name.cmp(&b.name));
    rows
}

fn snapshot(key: &str, output: &Value) -> Option<OutputSnapshot> {
    let name = output
        .get("name")
        .and_then(Value::as_str)
        .filter(|name| output_name_ok(name))
        .unwrap_or(key);
    if !output_name_ok(name) {
        return None;
    }
    let text = |field: &str| {
        output
            .get(field)
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string()
    };
    let mode = output
        .get("current_mode")
        .and_then(Value::as_u64)
        .and_then(|index| output.pointer(&format!("/modes/{index}")));
    let logical = output.get("logical").filter(|value| !value.is_null());
    let modes = output
        .get("modes")
        .and_then(Value::as_array)
        .map(|modes| {
            modes
                .iter()
                .filter_map(|mode| {
                    Some(OutputMode {
                        width: mode.get("width")?.as_u64()? as u32,
                        height: mode.get("height")?.as_u64()? as u32,
                        refresh_millihertz: mode.get("refresh_rate")?.as_u64()? as u32,
                        preferred: mode
                            .get("is_preferred")
                            .and_then(Value::as_bool)
                            .unwrap_or(false),
                    })
                })
                .collect()
        })
        .unwrap_or_default();
    Some(OutputSnapshot {
        name: name.to_string(),
        make: text("make"),
        model: text("model"),
        serial: text("serial"),
        modes,
        width: mode
            .and_then(|mode| mode.get("width"))
            .and_then(Value::as_u64)
            .map(|n| n as u32),
        height: mode
            .and_then(|mode| mode.get("height"))
            .and_then(Value::as_u64)
            .map(|n| n as u32),
        refresh_millihertz: mode
            .and_then(|mode| mode.get("refresh_rate"))
            .and_then(Value::as_u64)
            .map(|n| n as u32),
        scale: logical
            .and_then(|logical| logical.get("scale"))
            .and_then(Value::as_f64),
        transform: logical
            .and_then(|logical| logical.get("transform"))
            .and_then(Value::as_str)
            .map(str::to_string),
        x: logical
            .and_then(|logical| logical.get("x"))
            .and_then(Value::as_i64)
            .map(|n| n as i32),
        y: logical
            .and_then(|logical| logical.get("y"))
            .and_then(Value::as_i64)
            .map(|n| n as i32),
        logical_width: logical
            .and_then(|logical| logical.get("width"))
            .and_then(Value::as_u64)
            .map(|n| n as u32),
        logical_height: logical
            .and_then(|logical| logical.get("height"))
            .and_then(Value::as_u64)
            .map(|n| n as u32),
        vrr_supported: output
            .get("vrr_supported")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        vrr_enabled: output
            .get("vrr_enabled")
            .and_then(Value::as_bool)
            .unwrap_or(false),
    })
}

/// Switch keyboard focus to the agent desk workspace.
pub fn focus_agent_desk() -> Result<(), String> {
    if !helix_capabilities().agent_desk {
        return Err("agent desk is not available on this compositor".into());
    }
    let line = serde_json::json!({"Action": {"HelixFocusAgentDesk": {}}}).to_string();
    if request_json(&line).is_none() {
        return Err("the compositor did not focus the agent desk".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn focus_fails_when_compositor_does_not_report_agent_desk() {
        std::env::remove_var("NIRI_SOCKET");
        let err = focus_agent_desk().unwrap_err();
        assert!(err.contains("agent desk"));
    }

    #[test]
    fn place_is_noop_when_placement_pref_is_off() {
        std::env::remove_var("NIRI_SOCKET");
        let root = std::env::temp_dir().join(format!("helix-place-off-{}", std::process::id()));
        let helix = root.join("helix");
        std::fs::create_dir_all(&helix).unwrap();
        std::fs::write(
            helix.join("compositor-prefs.toml"),
            "agent_desk_placement = false\n",
        )
        .unwrap();
        let prev_cfg = std::env::var("XDG_CONFIG_HOME").ok();
        let prev_env = std::env::var("HELIX_AGENT_DESK").ok();
        std::env::set_var("XDG_CONFIG_HOME", root.to_string_lossy().as_ref());
        std::env::remove_var("HELIX_AGENT_DESK");
        assert!(place_on_agent_desk(1).is_ok());
        if let Some(prev) = prev_cfg {
            std::env::set_var("XDG_CONFIG_HOME", prev);
        } else {
            std::env::remove_var("XDG_CONFIG_HOME");
        }
        if let Some(prev) = prev_env {
            std::env::set_var("HELIX_AGENT_DESK", prev);
        }
        std::fs::remove_dir_all(root).ok();
    }

    #[test]
    fn a_display_is_saved_by_identity_unless_that_is_missing_or_shared() {
        let out = |name: &str, make: &str, model: &str, serial: &str| OutputSnapshot {
            name: name.into(),
            make: make.into(),
            model: model.into(),
            serial: serial.into(),
            modes: Vec::new(),
            width: None,
            height: None,
            refresh_millihertz: None,
            scale: None,
            transform: None,
            x: None,
            y: None,
            logical_width: None,
            logical_height: None,
            vrr_supported: false,
            vrr_enabled: false,
        };
        let dell = out("DP-3", "Dell Inc.", "U2720Q", "ABC123");
        let laptop = out("eDP-1", "BOE", "0x0A1B", "");
        let all = [dell.clone(), laptop.clone()];
        assert_eq!(dell.config_name(&all), "Dell Inc. U2720Q ABC123");
        assert_eq!(laptop.config_name(&all), "eDP-1", "no serial: by connector");
        let twin = out("DP-4", "Dell Inc.", "U2720Q", "ABC123");
        assert_eq!(
            dell.config_name(&[dell.clone(), twin]),
            "DP-3",
            "the same identity twice: by connector"
        );
        let quoted = out("DP-5", "Evil\"Co", "M", "1");
        assert_eq!(quoted.config_name(std::slice::from_ref(&quoted)), "DP-5");
        assert!(
            config_name_ok("Dell Inc. U2720Q ABC123")
                && !config_name_ok("a\"b")
                && !config_name_ok("a\nb")
        );
    }

    #[test]
    fn capabilities_are_read_from_inside_the_response_variant() {
        let ok = serde_json::json!({"HelixCapabilities": {"liquid_glass": true, "agent_desk": true, "display_profiles": true, "trusted_surfaces": true}});
        let caps = parse_caps(&ok);
        assert!(caps.liquid_glass && caps.agent_desk && caps.display_profiles);
        assert!(!parse_caps(&serde_json::json!({"Something": 1})).agent_desk);
        // An older compositor that does not know display profiles still reads as false.
        let old =
            serde_json::json!({"HelixCapabilities": {"liquid_glass": true, "agent_desk": true}});
        assert!(!parse_caps(&old).display_profiles && parse_caps(&old).agent_desk);
    }

    #[test]
    fn input_changes_name_only_the_fields_they_change() {
        let change = InputSettings {
            touchpad_tap: Some(true),
            ..InputSettings::default()
        };
        let line = serde_json::json!({ "HelixSetInput": change }).to_string();
        assert!(
            line.contains("\"touchpad_tap\":true") && line.contains("\"mouse_speed\":null"),
            "{line}"
        );
        let back: InputSettings =
            serde_json::from_str(r#"{"touchpad_speed": 0.25, "keyboard_layout": "us"}"#).unwrap();
        assert_eq!(
            (
                back.touchpad_speed,
                back.keyboard_layout.as_deref(),
                back.mouse_left_handed
            ),
            (Some(0.25), Some("us"), None)
        );
    }

    #[test]
    fn the_summon_note_is_read_when_there_is_one() {
        let some = serde_json::json!({"HelixSummonContext": {"title": "notes.md", "app_id": "code", "pid": 42, "age_ms": 80}});
        let note = parse_summon(&some).unwrap();
        assert_eq!(note.title.as_deref(), Some("notes.md"));
        assert_eq!(note.pid, Some(42));
        assert!(parse_summon(&serde_json::json!({"HelixSummonContext": null})).is_none());
        assert!(parse_summon(&serde_json::json!("Handled")).is_none());
    }

    #[test]
    fn output_request_is_a_session_scale() {
        let line = output_request("eDP-1", OutputAdjust::Scale(1.5)).unwrap();
        assert_eq!(
            line,
            r#"{"Output":{"action":{"Scale":{"scale":{"Specific":1.5}}},"output":"eDP-1"}}"#
        );
        let turned = output_request("HDMI-A-1", OutputAdjust::Turn("90")).unwrap();
        assert_eq!(
            turned,
            r#"{"Output":{"action":{"Transform":{"transform":"90"}},"output":"HDMI-A-1"}}"#
        );
    }

    #[test]
    fn output_request_refuses_anything_the_page_does_not_offer() {
        assert!(output_request("eDP-1", OutputAdjust::Scale(3.0)).is_err());
        assert!(output_request("eDP-1", OutputAdjust::Turn("Flipped")).is_err());
        assert!(output_request("Dell Monitor", OutputAdjust::Scale(1.0)).is_err());
        assert!(output_request("eDP-1\n", OutputAdjust::Turn("Normal")).is_err());
    }

    #[test]
    fn parse_outputs_keeps_mode_scale_and_orientation() {
        let raw = serde_json::json!({"Outputs": {
            "DP-2": {"name": "DP-2", "make": "Unknown", "model": "Unknown"},
            "eDP-1": {
                "name": "eDP-1",
                "make": "Dell",
                "model": "U2720Q",
                "modes": [{"width": 1920, "height": 1080, "refresh_rate": 59998}],
                "current_mode": 0,
                "logical": {"x": 0, "y": 40, "width": 1536, "height": 864, "scale": 1.25, "transform": "Normal"}
            }
        }});
        let outputs = parse_outputs(&raw);
        assert_eq!(outputs.len(), 2);
        assert_eq!(outputs[0].name, "DP-2");
        assert!(outputs[0].scale.is_none());
        assert_eq!(outputs[1].width, Some(1920));
        assert_eq!(outputs[1].refresh_millihertz, Some(59998));
        assert_eq!(outputs[1].scale, Some(1.25));
        assert_eq!(outputs[1].transform.as_deref(), Some("Normal"));
        assert_eq!(outputs[1].y, Some(40));
        assert_eq!(outputs[1].modes.len(), 1);
        assert!(!outputs[1].vrr_supported);
    }

    #[test]
    fn output_request_covers_resolution_place_refresh_and_power() {
        let mode = output_request(
            "eDP-1",
            OutputAdjust::Mode {
                width: 1920,
                height: 1080,
                refresh_hz: 60.0,
            },
        )
        .unwrap();
        let mode: Value = serde_json::from_str(&mode).unwrap();
        assert_eq!(
            mode["Output"]["action"]["Mode"]["mode"]["Specific"]["width"],
            1920
        );
        assert_eq!(
            mode["Output"]["action"]["Mode"]["mode"]["Specific"]["refresh"],
            60.0
        );

        let place = output_request("DP-2", OutputAdjust::Position(Some((1280, 0)))).unwrap();
        let place: Value = serde_json::from_str(&place).unwrap();
        assert_eq!(
            place["Output"]["action"]["Position"]["position"]["Specific"]["x"],
            1280
        );

        let auto = output_request("DP-2", OutputAdjust::Position(None)).unwrap();
        let auto: Value = serde_json::from_str(&auto).unwrap();
        assert_eq!(
            auto["Output"]["action"]["Position"]["position"],
            "Automatic"
        );

        let vrr = output_request("eDP-1", OutputAdjust::Vrr(true)).unwrap();
        let vrr: Value = serde_json::from_str(&vrr).unwrap();
        assert_eq!(vrr["Output"]["action"]["Vrr"]["vrr"]["vrr"], true);

        let off = output_request("HDMI-A-1", OutputAdjust::Power(false)).unwrap();
        let off: Value = serde_json::from_str(&off).unwrap();
        assert_eq!(off["Output"]["action"], "Off");
        assert!(output_request(
            "eDP-1",
            OutputAdjust::Mode {
                width: 0,
                height: 1080,
                refresh_hz: 60.0
            }
        )
        .is_err());
    }

    #[test]
    fn place_beside_uses_logical_size() {
        let other = (0, 0, 1920, 1080);
        assert_eq!(place_beside(Side::Right, other, (1280, 720)), (1920, 0));
        assert_eq!(place_beside(Side::Left, other, (1280, 720)), (-1280, 0));
        assert_eq!(place_beside(Side::Below, other, (1280, 720)), (0, 1080));
        assert_eq!(place_beside(Side::Above, other, (1280, 720)), (0, -720));
    }

    #[test]
    fn mode_token_round_trips() {
        assert_eq!(
            mode_from_token("1920x1080@59998"),
            Some((1920, 1080, 59998))
        );
        assert_eq!(mode_token(1920, 1080, 59998), "1920x1080@59998");
        assert!(mode_from_token("1920x1080").is_none());
        assert_eq!(kdl_transform("Normal"), Some("normal"));
        assert_eq!(kdl_transform("90"), Some("90"));
    }
}
