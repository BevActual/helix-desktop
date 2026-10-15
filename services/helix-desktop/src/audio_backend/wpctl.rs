//! PipeWire audio via WirePlumber's `wpctl` CLI (same stack as C++ `WirePlumberMixer`).

use std::process::Command;
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use super::Node;

#[derive(Clone, Debug)]
pub struct WpctlBackend {
    pub sinks: Vec<Node>,
    pub sources: Vec<Node>,
    pub programs: Vec<Node>,
    available: bool,
    last_snapshot: Value,
    /// Raises a flag when PipeWire reports a change; see `watch.rs`.
    watch: Arc<super::watch::Watch>,
    last_refresh: Instant,
}

/// Safety net in case an event is missed.
const SAFETY_REFRESH: Duration = Duration::from_secs(30);
/// Event-driven refreshes are spaced at least this far apart (a burst becomes one read).
const MIN_EVENT_INTERVAL: Duration = Duration::from_millis(750);
/// While PipeWire is unreachable, probing happens at the old polling cadence.
const UNAVAILABLE_RETRY: Duration = Duration::from_secs(3);

pub fn probe() -> bool {
    wpctl_output(&["status"]).is_ok()
}

impl WpctlBackend {
    pub fn new() -> Self {
        let mut backend = Self {
            sinks: Vec::new(),
            sources: Vec::new(),
            programs: Vec::new(),
            available: false,
            last_snapshot: json!({"available": false}),
            watch: Arc::new(super::watch::Watch::start()),
            last_refresh: Instant::now(),
        };
        backend.refresh();
        backend
    }

    /// Refresh only when PipeWire said something changed (or on the safety/retry timers).
    pub fn refresh_if_needed(&mut self) -> bool {
        let since = self.last_refresh.elapsed();
        // Leave the flag set while rate limited so the change is read on a later tick.
        let dirty = since >= MIN_EVENT_INTERVAL && self.watch.take_dirty();
        let interval = if self.available {
            SAFETY_REFRESH
        } else {
            UNAVAILABLE_RETRY
        };
        if dirty || since >= interval {
            self.refresh()
        } else {
            false
        }
    }

    pub fn is_available(&self) -> bool {
        self.available
    }

    pub fn snapshot(&self) -> Value {
        json!({
            "available": self.available,
            "sinks": self.sinks.iter().map(Node::to_json).collect::<Vec<_>>(),
            "sources": self.sources.iter().map(Node::to_json).collect::<Vec<_>>(),
            "programs": self.programs.iter().map(Node::to_json).collect::<Vec<_>>(),
        })
    }

    pub fn find_mut(&mut self, id: u32) -> Option<&mut Node> {
        self.sinks
            .iter_mut()
            .chain(self.sources.iter_mut())
            .chain(self.programs.iter_mut())
            .find(|node| node.id == id)
    }

    pub fn refresh(&mut self) -> bool {
        self.last_refresh = Instant::now();
        match refresh_from_wpctl() {
            Ok(next) => {
                let snap = json!({
                    "available": true,
                    "sinks": next.sinks.iter().map(Node::to_json).collect::<Vec<_>>(),
                    "sources": next.sources.iter().map(Node::to_json).collect::<Vec<_>>(),
                    "programs": next.programs.iter().map(Node::to_json).collect::<Vec<_>>(),
                });
                let changed = self.last_snapshot != snap;
                self.sinks = next.sinks;
                self.sources = next.sources;
                self.programs = next.programs;
                self.available = true;
                self.last_snapshot = snap;
                changed
            }
            Err(error) => {
                tracing::debug!(%error, "audio wpctl refresh failed");
                let snap = json!({"available": false});
                let changed = self.last_snapshot != snap;
                self.sinks.clear();
                self.sources.clear();
                self.programs.clear();
                self.available = false;
                self.last_snapshot = snap;
                changed
            }
        }
    }

    pub fn set_volume(&mut self, id: u32, volume: f64) -> Result<(), String> {
        if self.find_mut(id).is_none() {
            return Err("Audio device is no longer available".into());
        }
        let arg = format!("{volume:.4}");
        wpctl_output(&["set-volume", &id.to_string(), &arg])?;
        self.refresh();
        Ok(())
    }

    pub fn set_muted(&mut self, id: u32, muted: bool) -> Result<(), String> {
        if self.find_mut(id).is_none() {
            return Err("Audio device is no longer available".into());
        }
        wpctl_output(&["set-mute", &id.to_string(), if muted { "1" } else { "0" }])?;
        self.refresh();
        Ok(())
    }

    pub fn set_default(&mut self, id: u32, input: bool) -> Result<(), String> {
        let list = if input { &self.sources } else { &self.sinks };
        if !list.iter().any(|node| node.id == id) {
            return Err("Audio device is no longer available".into());
        }
        wpctl_output(&["set-default", &id.to_string()])?;
        self.refresh();
        Ok(())
    }
}

struct ParsedAudio {
    sinks: Vec<Node>,
    sources: Vec<Node>,
    programs: Vec<Node>,
}

fn refresh_from_wpctl() -> Result<ParsedAudio, String> {
    let status = wpctl_output(&["status"])?;
    let default_sink = default_node_id("@DEFAULT_AUDIO_SINK@");
    let default_source = default_node_id("@DEFAULT_AUDIO_SOURCE@");
    let sinks = parse_device_section(&status, "Sinks:", default_sink);
    let sources = parse_device_section(&status, "Sources:", default_source);
    let programs = parse_program_streams(&status)
        .into_iter()
        .map(|mut node| {
            if let Ok(volume) = read_volume(node.id) {
                node.volume = volume.volume;
                node.muted = volume.muted;
            }
            node
        })
        .collect();
    Ok(ParsedAudio {
        sinks,
        sources,
        programs,
    })
}

fn default_node_id(token: &str) -> Option<u32> {
    let text = wpctl_output(&["inspect", token]).ok()?;
    let first = text.lines().next()?;
    let rest = first.strip_prefix("id ")?;
    let id = rest.split(',').next()?.trim();
    id.parse().ok()
}

#[derive(Clone, Copy, Debug)]
struct VolumeLine {
    volume: f64,
    muted: bool,
}

fn read_volume(id: u32) -> Result<VolumeLine, String> {
    let text = wpctl_output(&["get-volume", &id.to_string()])?;
    parse_get_volume(&text).ok_or_else(|| "could not parse wpctl get-volume".into())
}

fn parse_get_volume(text: &str) -> Option<VolumeLine> {
    let line = text.lines().find(|line| line.starts_with("Volume:"))?;
    let after = line.strip_prefix("Volume:")?.trim();
    let muted = after.contains("[MUTED]");
    let number = after.split_whitespace().next()?.parse::<f64>().ok()?;
    Some(VolumeLine {
        volume: number,
        muted,
    })
}

/// Parse `wpctl status` sink/source lines under the Audio tree.
fn parse_device_section(status: &str, heading: &str, default_id: Option<u32>) -> Vec<Node> {
    let mut nodes = Vec::new();
    let mut in_audio = false;
    let mut in_section = false;
    for line in status.lines() {
        if line.starts_with("Audio") {
            in_audio = true;
            continue;
        }
        if !in_audio {
            continue;
        }
        if line.starts_with("Video") || line.starts_with("Settings") {
            break;
        }
        if line.contains(heading) {
            in_section = true;
            continue;
        }
        if in_section {
            if line.contains("├─") || line.contains("└─") {
                in_section = false;
                continue;
            }
            if let Some(node) = parse_volume_line(line, default_id) {
                nodes.push(node);
            }
        }
    }
    nodes.sort_by_key(|node| node.id);
    nodes
}

fn parse_volume_line(line: &str, default_id: Option<u32>) -> Option<Node> {
    let trimmed = line.trim();
    if !trimmed.contains("[vol:") {
        return None;
    }
    let after_pipe = trimmed.rsplit('│').next()?.trim();
    let after_pipe = after_pipe.strip_prefix('*').unwrap_or(after_pipe).trim();
    let (id_part, rest) = after_pipe.split_once('.')?;
    let id = id_part.trim().parse::<u32>().ok()?;
    let vol_start = rest.find("[vol:")?;
    let description = rest[..vol_start].trim();
    let vol_section = rest[vol_start..].trim();
    let inner = vol_section
        .strip_prefix("[vol:")
        .and_then(|s| s.strip_suffix(']'))?
        .trim();
    let muted = inner.contains("MUTED");
    let volume = inner.split_whitespace().next()?.parse::<f64>().ok()?;
    Some(Node {
        id,
        name: description.to_string(),
        description: description.to_string(),
        volume,
        muted,
        is_default: default_id == Some(id),
        available: true,
    })
}

fn parse_program_streams(status: &str) -> Vec<Node> {
    let mut nodes = Vec::new();
    let mut in_audio = false;
    let mut in_streams = false;
    for line in status.lines() {
        if line.starts_with("Audio") {
            in_audio = true;
            continue;
        }
        if !in_audio {
            continue;
        }
        if line.starts_with("Video") {
            break;
        }
        if line.contains("Streams:") {
            in_streams = true;
            continue;
        }
        if !in_streams {
            continue;
        }
        if line.contains("└─") {
            break;
        }
        let trimmed = line.trim();
        if trimmed.is_empty()
            || trimmed.starts_with("output_")
            || trimmed.contains('>')
            || trimmed.contains("[active]")
        {
            continue;
        }
        let Some(after_pipe) = trimmed.rsplit('│').next() else {
            continue;
        };
        let Some((id_part, rest)) = after_pipe.trim().split_once('.') else {
            continue;
        };
        let Ok(id) = id_part.trim().parse::<u32>() else {
            continue;
        };
        let description = rest.trim();
        if description.is_empty() {
            continue;
        }
        nodes.push(Node {
            id,
            name: description.to_string(),
            description: description.to_string(),
            volume: 1.0,
            muted: false,
            is_default: false,
            available: true,
        });
    }
    nodes.sort_by_key(|node| node.id);
    nodes
}

fn wpctl_output(args: &[&str]) -> Result<String, String> {
    let output = Command::new("wpctl")
        .args(args)
        .output()
        .map_err(|error| format!("wpctl spawn failed: {error}"))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(format!(
            "wpctl {} failed: {}",
            args.join(" "),
            stderr.trim()
        ));
    }
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURE: &str = r"
Audio
 ├─ Devices:
 │      53. Example Card [alsa]
 ├─ Sinks:
 │      47. Living Room Stereo System           [vol: 1.00]
 │      92. Built-in Audio Analog Stereo        [vol: 0.72 MUTED]
 ├─ Sources:
 │  *   93. Built-in Audio Analog Stereo        [vol: 1.00]
 └─ Streams:
       171. PipeWire ALSA [blip]
            172. output_FL       > EVO4:playback_FL	[active]
       177. Another App
Video
";

    #[test]
    fn parses_sinks_and_sources() {
        let sinks = parse_device_section(FIXTURE, "Sinks:", Some(47));
        assert_eq!(sinks.len(), 2);
        assert_eq!(sinks[0].id, 47);
        assert!(sinks[0].is_default);
        assert_eq!(sinks[1].id, 92);
        assert!(sinks[1].muted);
        assert!((sinks[1].volume - 0.72).abs() < f64::EPSILON);

        let sources = parse_device_section(FIXTURE, "Sources:", Some(93));
        assert_eq!(sources.len(), 1);
        assert_eq!(sources[0].id, 93);
        assert!(sources[0].is_default);
    }

    #[test]
    fn parses_program_streams() {
        let programs = parse_program_streams(FIXTURE);
        assert_eq!(programs.len(), 2);
        assert_eq!(programs[0].id, 171);
        assert_eq!(programs[1].description, "Another App");
    }

    #[test]
    fn parses_get_volume() {
        let parsed = parse_get_volume("Volume: 0.72 [MUTED]\n").unwrap();
        assert!(parsed.muted);
        assert!((parsed.volume - 0.72).abs() < f64::EPSILON);
    }
}
