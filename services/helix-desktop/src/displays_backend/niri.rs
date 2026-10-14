//! Live display inventory via `niri msg --json outputs` (same as C++ `DisplayInventory`).

use std::process::Command;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use super::mock;

const REFRESH_INTERVAL: Duration = Duration::from_secs(3);

#[derive(Clone, Debug)]
struct ParsedOutput {
    name: String,
    label: String,
    enabled: bool,
    width: i64,
    height: i64,
    refresh_millihertz: u32,
    scale: f64,
    x: i64,
    y: i64,
    logical_width: i64,
    logical_height: i64,
    transform: String,
    vrr_supported: bool,
    vrr_enabled: bool,
}

#[derive(Clone, Debug)]
pub struct NiriBackend {
    snapshot: Value,
    next_allowed: Instant,
    query_in_flight: bool,
}

pub fn probe() -> bool {
    if mock::force_mock() {
        return false;
    }
    niri_socket_set() && Command::new("niri").arg("--version").output().is_ok()
}

fn niri_socket_set() -> bool {
    std::env::var("NIRI_SOCKET")
        .map(|value| !value.is_empty())
        .unwrap_or(false)
}

impl NiriBackend {
    pub fn new() -> Self {
        Self {
            snapshot: mock::unavailable_snapshot(),
            next_allowed: Instant::now(),
            query_in_flight: false,
        }
    }

    pub fn snapshot(&self) -> Value {
        self.snapshot.clone()
    }

    pub fn refresh(&mut self, desktop_registered: bool) -> bool {
        let permitted = desktop_registered && niri_socket_set();
        if !permitted {
            let next = mock::unavailable_snapshot();
            let changed = self.snapshot != next;
            self.snapshot = next;
            self.query_in_flight = false;
            return changed;
        }

        let now = Instant::now();
        if self.query_in_flight || now < self.next_allowed {
            return false;
        }

        self.next_allowed = now + REFRESH_INTERVAL;
        self.query_in_flight = true;
        let result = query_outputs();
        self.query_in_flight = false;

        let next = match result {
            Ok(outputs) if !outputs.is_empty() => snapshot_from_outputs(outputs),
            Ok(_) => json!({
                "available": false,
                "outputs": [],
                "capabilities": [],
                "reason": "The display backend is unavailable.",
            }),
            Err(_) => json!({
                "available": false,
                "outputs": [],
                "capabilities": [],
                "reason": "The display backend is unavailable. Settings will retry automatically.",
            }),
        };
        let changed = self.snapshot != next;
        self.snapshot = next;
        changed
    }
}

fn snapshot_from_outputs(outputs: Vec<ParsedOutput>) -> Value {
    json!({
        "available": true,
        "outputs": outputs.iter().map(|output| json!({
            "name": output.name,
            "label": output.label,
            "enabled": output.enabled,
            "width": output.width,
            "height": output.height,
            "refresh-millihertz": output.refresh_millihertz,
            "scale": output.scale,
            "x": output.x,
            "y": output.y,
            "logical-width": output.logical_width,
            "logical-height": output.logical_height,
            "transform": output.transform,
            "vrr-supported": output.vrr_supported,
            "vrr-enabled": output.vrr_enabled,
        })).collect::<Vec<_>>(),
        "capabilities": ["inventory"],
    })
}

fn query_outputs() -> Result<Vec<ParsedOutput>, String> {
    let output = Command::new("niri")
        .args(["msg", "--json", "outputs"])
        .output()
        .map_err(|error| error.to_string())?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).into_owned());
    }
    let text = String::from_utf8_lossy(&output.stdout);
    parse_outputs_map(&text)
}

fn parse_outputs_map(text: &str) -> Result<Vec<ParsedOutput>, String> {
    let raw: Value = serde_json::from_str(text).map_err(|error| error.to_string())?;
    let envelope = json!({ "Outputs": raw });
    let Some(outputs) = envelope.get("Outputs").and_then(Value::as_object) else {
        return Ok(Vec::new());
    };

    let mut parsed = Vec::new();
    for (name, entry) in outputs {
        let Some(object) = entry.as_object() else {
            continue;
        };
        let output_name = object
            .get("name")
            .and_then(Value::as_str)
            .filter(|text| !text.is_empty())
            .unwrap_or(name.as_str())
            .to_string();
        let make = object
            .get("make")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        let model = object
            .get("model")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        let modes = object
            .get("modes")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        let current_mode = object
            .get("current_mode")
            .and_then(Value::as_i64)
            .and_then(|index| usize::try_from(index).ok())
            .and_then(|index| modes.get(index));
        let (width, height, refresh_millihertz) = current_mode
            .map(|mode| {
                (
                    mode.get("width").and_then(Value::as_i64).unwrap_or(0),
                    mode.get("height").and_then(Value::as_i64).unwrap_or(0),
                    mode.get("refresh_rate")
                        .and_then(Value::as_u64)
                        .and_then(|value| u32::try_from(value).ok())
                        .unwrap_or(0),
                )
            })
            .unwrap_or((0, 0, 0));
        let logical = object.get("logical").and_then(Value::as_object);
        let enabled = logical.is_some();
        let (x, y, logical_width, logical_height, scale, transform) = if let Some(logical) = logical {
            (
                logical.get("x").and_then(Value::as_i64).unwrap_or(0),
                logical.get("y").and_then(Value::as_i64).unwrap_or(0),
                logical.get("width").and_then(Value::as_i64).unwrap_or(0),
                logical.get("height").and_then(Value::as_i64).unwrap_or(0),
                logical.get("scale").and_then(Value::as_f64).unwrap_or(1.0),
                logical
                    .get("transform")
                    .and_then(Value::as_str)
                    .map(transform_json_name)
                    .unwrap_or_else(|| "Normal".to_string()),
            )
        } else {
            (0, 0, 0, 0, 1.0, "Normal".to_string())
        };
        let label = display_label(&make, &model, &output_name);
        parsed.push(ParsedOutput {
            name: output_name,
            label,
            enabled,
            width,
            height,
            refresh_millihertz,
            scale,
            x,
            y,
            logical_width,
            logical_height,
            transform,
            vrr_supported: object
                .get("vrr_supported")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            vrr_enabled: object
                .get("vrr_enabled")
                .and_then(Value::as_bool)
                .unwrap_or(false),
        });
    }

    parsed.sort_by(|a, b| {
        b.enabled
            .cmp(&a.enabled)
            .then_with(|| a.x.cmp(&b.x))
            .then_with(|| a.y.cmp(&b.y))
            .then_with(|| a.name.cmp(&b.name))
    });
    Ok(parsed)
}

fn display_label(make: &str, model: &str, name: &str) -> String {
    if !model.is_empty() && model != "Unknown" {
        if !make.is_empty() && make != "Unknown" {
            return format!("{make} {model}");
        }
        return model.to_string();
    }
    if !make.is_empty() && make != "Unknown" {
        return make.to_string();
    }
    name.to_string()
}

fn transform_json_name(raw: &str) -> String {
    match raw.trim() {
        "normal" | "Normal" => "Normal".to_string(),
        "90" => "90".to_string(),
        "180" => "180".to_string(),
        "270" => "270".to_string(),
        "flipped" | "Flipped" => "Flipped".to_string(),
        "flipped-90" | "Flipped90" => "Flipped90".to_string(),
        "flipped-180" | "Flipped180" => "Flipped180".to_string(),
        "flipped-270" | "Flipped270" => "Flipped270".to_string(),
        other => other.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_niri_outputs_map() {
        let sample = r#"{
            "eDP-1": {
                "name": "eDP-1",
                "make": "BOE",
                "model": "Panel",
                "modes": [{"width": 1920, "height": 1080, "refresh_rate": 60000}],
                "current_mode": 0,
                "vrr_supported": true,
                "vrr_enabled": false,
                "logical": {"x": 0, "y": 0, "width": 1920, "height": 1080, "scale": 1.0, "transform": "Normal"}
            }
        }"#;
        let outputs = parse_outputs_map(sample).unwrap();
        assert_eq!(outputs.len(), 1);
        assert_eq!(outputs[0].name, "eDP-1");
        assert_eq!(outputs[0].label, "BOE Panel");
        assert!(outputs[0].enabled);
        assert_eq!(outputs[0].refresh_millihertz, 60000);
    }
}
