//! The control center's audio model. It turns the service's Audio1 snapshot into what the page shows, and it is the only place that
//! can produce a write request. Nothing here talks to D-Bus: the shell sends what [`Request`] says, and the service checks it again.
//!
//! The page asks for "volume 40%" or "mute"; it never names a device. The target is always the current output, or (for
//! [`Audio::set_default`]) an id that is one of the outputs in the snapshot just read, so a stale or forged id cannot reach the service.

use serde::Serialize;
use serde_json::{json, Value};

use crate::display::sanitize_for_display;

const MAX_NAME_CHARS: usize = 60;
/// The slider's range. The service accepts up to 150%, but this panel does not offer amplification.
pub const MAX_PERCENT: f64 = 100.0;

#[derive(Clone, Debug, PartialEq)]
pub struct Device {
    pub id: u32,
    pub name: String,
    /// 0.0..=1.5 as the service reports it.
    pub volume: f64,
    pub muted: bool,
    pub is_default: bool,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Audio {
    available: bool,
    outputs: Vec<Device>,
    inputs: Vec<Device>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    /// There is no audio server, or the service says so.
    Unavailable,
    /// Audio works but no output device is available.
    NoOutput,
    OutOfRange,
    /// The id is not one of the current outputs.
    UnknownDevice,
}

/// What to send to Audio1.Action.
#[derive(Clone, Debug, PartialEq)]
pub struct Request {
    pub action: &'static str,
    pub args: Value,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct OutputView {
    pub id: u32,
    pub name: String,
    pub is_default: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct AudioView {
    pub available: bool,
    /// The device the slider controls. `None` when there is no output.
    pub output: Option<OutputView>,
    pub percent: u8,
    pub muted: bool,
    pub outputs: Vec<OutputView>,
}

fn devices(snapshot: &Value, key: &str) -> Vec<Device> {
    let Some(list) = snapshot.get(key).and_then(Value::as_array) else {
        return Vec::new();
    };
    list.iter()
        .filter_map(|entry| {
            if entry.get("available").and_then(Value::as_bool) != Some(true) {
                return None;
            }
            let id = u32::try_from(entry.get("id")?.as_u64()?).ok()?;
            let label = ["description", "name"]
                .iter()
                .filter_map(|field| entry.get(*field).and_then(Value::as_str))
                .map(|text| sanitize_for_display(text, MAX_NAME_CHARS))
                .find(|text| !text.is_empty())?;
            let volume = entry
                .get("volume")
                .and_then(Value::as_f64)
                .filter(|v| v.is_finite())
                .unwrap_or(0.0)
                .clamp(0.0, 1.5);
            Some(Device {
                id,
                name: label,
                volume,
                muted: entry.get("muted").and_then(Value::as_bool).unwrap_or(false),
                is_default: entry
                    .get("default")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
            })
        })
        .collect()
}

impl Audio {
    /// A snapshot that is missing, malformed or marked unavailable is simply no audio.
    pub fn from_snapshot(snapshot: &Value) -> Self {
        if snapshot.get("available").and_then(Value::as_bool) != Some(true) {
            return Self::default();
        }
        Self {
            available: true,
            outputs: devices(snapshot, "sinks"),
            inputs: devices(snapshot, "sources"),
        }
    }

    /// The device the volume controls apply to: the default output, else the first one.
    pub fn output(&self) -> Option<&Device> {
        self.outputs
            .iter()
            .find(|device| device.is_default)
            .or_else(|| self.outputs.first())
    }

    pub fn inputs(&self) -> &[Device] {
        &self.inputs
    }

    pub fn view(&self) -> AudioView {
        let show = |device: &Device| OutputView {
            id: device.id,
            name: device.name.clone(),
            is_default: device.is_default,
        };
        let current = self.output();
        AudioView {
            available: self.available,
            output: current.map(show),
            percent: current.map_or(0, |device| {
                (device.volume * 100.0).round().clamp(0.0, 150.0) as u8
            }),
            muted: current.is_some_and(|device| device.muted),
            outputs: self.outputs.iter().map(show).collect(),
        }
    }

    fn target(&self) -> Result<&Device, RequestError> {
        if !self.available {
            return Err(RequestError::Unavailable);
        }
        self.output().ok_or(RequestError::NoOutput)
    }

    /// Sets the current output's volume to `percent` (0 to 100).
    pub fn set_volume(&self, percent: f64) -> Result<Request, RequestError> {
        let target = self.target()?;
        if !percent.is_finite() || !(0.0..=MAX_PERCENT).contains(&percent) {
            return Err(RequestError::OutOfRange);
        }
        Ok(Request {
            action: "volume",
            args: json!({"id": target.id, "value": percent / 100.0}),
        })
    }

    pub fn set_muted(&self, muted: bool) -> Result<Request, RequestError> {
        let target = self.target()?;
        Ok(Request {
            action: "mute",
            args: json!({"id": target.id, "value": muted}),
        })
    }

    /// Makes `id` the default output. It must be one of the outputs in this snapshot.
    pub fn set_default(&self, id: u32) -> Result<Request, RequestError> {
        if !self.available {
            return Err(RequestError::Unavailable);
        }
        if !self.outputs.iter().any(|device| device.id == id) {
            return Err(RequestError::UnknownDevice);
        }
        Ok(Request {
            action: "default",
            args: json!({"id": id}),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sink(
        id: u64,
        description: &str,
        volume: f64,
        muted: bool,
        default: bool,
        available: bool,
    ) -> Value {
        json!({"id": id, "name": format!("node.{id}"), "description": description, "volume": volume, "muted": muted, "default": default, "available": available})
    }
    fn snapshot() -> Value {
        json!({
            "available": true,
            "sinks": [
                sink(48, "EVO4 Headphone / Line Out", 0.44, false, true, true),
                sink(51, "HomePod Mini", 1.0, false, false, true),
                sink(60, "Unplugged", 0.5, false, false, false),
            ],
            "sources": [sink(70, "Microphone", 0.8, true, true, true)],
            "programs": [sink(110, "Some Stream", 1.0, false, false, true)]
        })
    }

    #[test]
    fn the_view_shows_the_default_output_and_lists_only_available_ones() {
        let view = Audio::from_snapshot(&snapshot()).view();
        assert!(view.available);
        assert_eq!(view.output.as_ref().unwrap().id, 48);
        assert_eq!(
            view.output.as_ref().unwrap().name,
            "EVO4 Headphone / Line Out"
        );
        assert_eq!(view.percent, 44);
        assert!(!view.muted);
        assert_eq!(
            view.outputs.iter().map(|o| o.id).collect::<Vec<_>>(),
            [48, 51],
            "the unplugged sink is not offered"
        );
    }

    #[test]
    fn without_a_default_the_first_output_is_used() {
        let mut data = snapshot();
        data["sinks"][0]["default"] = json!(false);
        assert_eq!(Audio::from_snapshot(&data).output().unwrap().id, 48);
        data["sinks"][0]["available"] = json!(false);
        assert_eq!(Audio::from_snapshot(&data).output().unwrap().id, 51);
    }

    #[test]
    fn unavailable_or_malformed_snapshots_are_no_audio() {
        for data in [
            json!({}),
            json!({"available": false, "sinks": [sink(1, "x", 1.0, false, true, true)]}),
            json!("text"),
            json!(null),
            json!({"available": "yes"}),
        ] {
            let audio = Audio::from_snapshot(&data);
            assert!(!audio.view().available, "{data}");
            assert_eq!(audio.set_volume(10.0), Err(RequestError::Unavailable));
            assert_eq!(audio.set_muted(true), Err(RequestError::Unavailable));
            assert_eq!(audio.set_default(1), Err(RequestError::Unavailable));
        }
    }

    #[test]
    fn available_audio_with_no_output_cannot_be_written_to() {
        let audio = Audio::from_snapshot(&json!({"available": true, "sinks": [], "sources": []}));
        assert!(audio.view().available && audio.view().output.is_none());
        assert_eq!(audio.set_volume(10.0), Err(RequestError::NoOutput));
        assert_eq!(audio.set_muted(true), Err(RequestError::NoOutput));
    }

    #[test]
    fn volume_requests_target_the_current_output_and_are_range_checked() {
        let audio = Audio::from_snapshot(&snapshot());
        let request = audio.set_volume(40.0).unwrap();
        assert_eq!(request.action, "volume");
        assert_eq!(request.args["id"], 48);
        assert!((request.args["value"].as_f64().unwrap() - 0.4).abs() < 1e-12);
        assert!(audio.set_volume(0.0).is_ok() && audio.set_volume(100.0).is_ok());
        for bad in [
            -0.1,
            100.01,
            150.0,
            f64::NAN,
            f64::INFINITY,
            f64::NEG_INFINITY,
        ] {
            assert_eq!(
                audio.set_volume(bad),
                Err(RequestError::OutOfRange),
                "{bad}"
            );
        }
    }

    #[test]
    fn mute_requests_target_the_current_output() {
        let audio = Audio::from_snapshot(&snapshot());
        assert_eq!(
            audio.set_muted(true).unwrap(),
            Request {
                action: "mute",
                args: json!({"id": 48, "value": true})
            }
        );
        assert_eq!(audio.set_muted(false).unwrap().args["value"], false);
    }

    #[test]
    fn only_a_current_output_can_become_the_default() {
        let audio = Audio::from_snapshot(&snapshot());
        assert_eq!(
            audio.set_default(51).unwrap(),
            Request {
                action: "default",
                args: json!({"id": 51})
            }
        );
        assert_eq!(
            audio.set_default(60),
            Err(RequestError::UnknownDevice),
            "unplugged"
        );
        assert_eq!(
            audio.set_default(70),
            Err(RequestError::UnknownDevice),
            "a microphone is not an output"
        );
        assert_eq!(
            audio.set_default(110),
            Err(RequestError::UnknownDevice),
            "a program stream is not an output"
        );
        assert_eq!(audio.set_default(9999), Err(RequestError::UnknownDevice));
    }

    #[test]
    fn device_names_are_display_safe_and_odd_values_are_tamed() {
        let data = json!({"available": true, "sinks": [
            {"id": 1, "name": "n", "description": "Evil\u{202e}Name\u{7}\nline", "volume": 9.0, "available": true},
            {"id": 2, "name": "fallback-name", "description": "", "volume": -3.0, "available": true},
            {"id": 3, "description": "no volume", "available": true},
            {"id": 4294967296u64, "description": "id too large", "available": true},
            {"description": "no id", "available": true},
            {"id": 5, "description": "", "name": "", "available": true},
        ]});
        let audio = Audio::from_snapshot(&data);
        let view = audio.view();
        assert_eq!(
            view.outputs.iter().map(|o| o.id).collect::<Vec<_>>(),
            [1, 2, 3]
        );
        assert!(
            !view.outputs[0].name.contains('\u{202e}')
                && !view.outputs[0].name.contains('\n')
                && !view.outputs[0].name.contains('\u{7}')
        );
        assert_eq!(view.outputs[1].name, "fallback-name");
        assert_eq!(view.percent, 150, "volume is clamped, not trusted");
        let volumes: Vec<f64> = [1, 2, 3]
            .iter()
            .map(|id| audio.outputs.iter().find(|d| d.id == *id).unwrap().volume)
            .collect();
        assert_eq!(volumes, [1.5, 0.0, 0.0], "the stored volume is clamped to what the service accepts, and a missing one is silence");
    }

    #[test]
    fn the_default_output_wins_even_when_it_is_not_listed_first() {
        let data = json!({"available": true, "sinks": [
            sink(51, "HomePod Mini", 1.0, false, false, true),
            sink(48, "Headphones", 0.3, false, true, true),
        ]});
        let audio = Audio::from_snapshot(&data);
        assert_eq!(audio.output().unwrap().id, 48);
        assert_eq!(audio.view().percent, 30);
        assert_eq!(
            audio.set_volume(10.0).unwrap().args["id"],
            48,
            "writes go to the default, not the first"
        );
    }
}
