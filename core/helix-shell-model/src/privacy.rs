//! Which programs are using the microphone or a camera right now, for the indicator in the bar. Plain data and parsing, no I/O beyond reading
//! `/proc`: the shell feeds it the output of `pw-dump --monitor` (PipeWire's own change stream, one client, nothing polled) and asks it about
//! programs that hold a camera device open without going through PipeWire.
//!
//! What counts: an audio capture stream (`Stream/Input/Audio`) or a video capture stream (`Stream/Input/Video`) whose node is **running**. A stream
//! that exists but is idle or paused is not recording. Internal device nodes (`.../Internal`) are the system's plumbing, not a program.

use std::collections::{BTreeSet, HashMap};
use std::path::{Path, PathBuf};

use serde_json::Value;

#[derive(Clone, Debug, Default)]
struct NodeInfo {
    class: String,
    state: String,
    name: String,
}

/// The PipeWire nodes seen so far, kept up to date from the monitor's objects.
#[derive(Default)]
pub struct Nodes {
    nodes: HashMap<u64, NodeInfo>,
}

/// Who is using what.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Capture {
    pub microphone: Vec<String>,
    pub camera: Vec<String>,
}

impl Capture {
    pub fn any(&self) -> bool {
        !self.microphone.is_empty() || !self.camera.is_empty()
    }
}

fn clean(text: &str) -> String {
    text.chars()
        .filter(|c| !c.is_control())
        .take(40)
        .collect::<String>()
        .trim()
        .to_string()
}

impl Nodes {
    /// Applies one array of objects from `pw-dump`. Returns whether anything changed that could matter.
    pub fn apply(&mut self, objects: &[Value]) -> bool {
        let before = self.capture();
        for object in objects {
            let Some(id) = object.get("id").and_then(Value::as_u64) else {
                continue;
            };
            // A removed object is reported with no info at all.
            if object.get("info").is_some_and(Value::is_null) {
                self.nodes.remove(&id);
                continue;
            }
            if object.get("type").and_then(Value::as_str) != Some("PipeWire:Interface:Node") {
                continue;
            }
            let Some(info) = object.get("info") else {
                continue;
            };
            let node = self.nodes.entry(id).or_default();
            if let Some(state) = info.get("state").and_then(Value::as_str) {
                node.state = state.to_string();
            }
            // Properties are only sent when they change, so they are merged, never replaced.
            if let Some(props) = info.get("props") {
                if let Some(class) = props.get("media.class").and_then(Value::as_str) {
                    node.class = class.to_string();
                }
                let name = [
                    "application.name",
                    "application.process.binary",
                    "node.name",
                ]
                .iter()
                .find_map(|key| {
                    props
                        .get(*key)
                        .and_then(Value::as_str)
                        .map(clean)
                        .filter(|n| !n.is_empty())
                });
                if let Some(name) = name {
                    node.name = name;
                }
            }
        }
        self.capture() != before
    }

    pub fn capture(&self) -> Capture {
        let mut microphone = BTreeSet::new();
        let mut camera = BTreeSet::new();
        for node in self.nodes.values().filter(|n| n.state == "running") {
            let name = if node.name.is_empty() {
                "A program".to_string()
            } else {
                node.name.clone()
            };
            match node.class.as_str() {
                "Stream/Input/Audio" => {
                    microphone.insert(name);
                }
                "Stream/Input/Video" => {
                    camera.insert(name);
                }
                _ => {}
            }
        }
        Capture {
            microphone: microphone.into_iter().collect(),
            camera: camera.into_iter().collect(),
        }
    }
}

/// Programs that hold one of the camera devices open (`/dev/video*`), found through `/proc/<pid>/fd`. PipeWire's own processes are left out: they
/// hold a device while they list it, and the programs that really use the camera through PipeWire show up as video streams.
pub fn video_users(proc_dir: &Path, devices: &[PathBuf], own_pid: u32) -> Vec<String> {
    const PLUMBING: &[&str] = &[
        "wireplumber",
        "pipewire",
        "pipewire-media-",
        "pipewire-pulse",
        "systemd",
        "(sd-pam)",
    ];
    let mut found = BTreeSet::new();
    let Ok(entries) = std::fs::read_dir(proc_dir) else {
        return Vec::new();
    };
    for entry in entries.flatten() {
        let Some(pid) = entry
            .file_name()
            .to_str()
            .and_then(|n| n.parse::<u32>().ok())
        else {
            continue;
        };
        if pid == own_pid {
            continue;
        }
        let Ok(fds) = std::fs::read_dir(entry.path().join("fd")) else {
            continue;
        };
        let uses_camera = fds
            .flatten()
            .any(|fd| std::fs::read_link(fd.path()).is_ok_and(|target| devices.contains(&target)));
        if !uses_camera {
            continue;
        }
        let name = std::fs::read_to_string(entry.path().join("comm"))
            .map(|c| clean(&c))
            .unwrap_or_default();
        if !name.is_empty() && !PLUMBING.contains(&name.as_str()) {
            found.insert(name);
        }
    }
    found.into_iter().collect()
}

/// The camera devices that exist: `/dev/video0`, `/dev/video1`, ….
pub fn camera_devices(dev_dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dev_dir) else {
        return Vec::new();
    };
    let mut list: Vec<PathBuf> = entries
        .flatten()
        .filter(|e| {
            e.file_name().to_str().is_some_and(|n| {
                n.strip_prefix("video").is_some_and(|rest| {
                    !rest.is_empty() && rest.bytes().all(|b| b.is_ascii_digit())
                })
            })
        })
        .map(|e| e.path())
        .collect();
    list.sort();
    list
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn node(id: u64, class: &str, state: &str, app: &str) -> Value {
        json!({"id": id, "type": "PipeWire:Interface:Node", "info": {"state": state, "props": {"media.class": class, "application.name": app}}})
    }

    #[test]
    fn only_running_capture_streams_count_and_they_are_named_once() {
        let mut nodes = Nodes::default();
        assert!(
            !nodes.apply(&[node(1, "Stream/Output/Audio", "running", "Music")]),
            "playing is not recording"
        );
        assert!(nodes.apply(&[
            node(2, "Stream/Input/Audio", "running", "Firefox"),
            node(3, "Stream/Input/Audio", "running", "Firefox")
        ]));
        assert!(
            !nodes.apply(&[node(4, "Stream/Input/Audio", "idle", "Zoom")]),
            "an idle stream is not recording"
        );
        assert!(
            !nodes.apply(&[node(5, "Stream/Input/Audio/Internal", "running", "alsa")]),
            "device plumbing is not a program"
        );
        let seen = nodes.capture();
        assert_eq!(seen.microphone, ["Firefox"]);
        assert!(seen.camera.is_empty() && seen.any());
        assert!(nodes.apply(&[node(6, "Stream/Input/Video", "running", "Cheese")]));
        assert_eq!(nodes.capture().camera, ["Cheese"]);
    }

    #[test]
    fn a_state_change_without_properties_and_a_removal_are_followed() {
        let mut nodes = Nodes::default();
        nodes.apply(&[node(2, "Stream/Input/Audio", "idle", "Zoom")]);
        assert!(
            nodes.apply(&[
                json!({"id": 2, "type": "PipeWire:Interface:Node", "info": {"state": "running"}})
            ]),
            "the class and name were remembered"
        );
        assert_eq!(nodes.capture().microphone, ["Zoom"]);
        assert!(nodes.apply(&[json!({"id": 2, "info": null})]));
        assert!(!nodes.capture().any());
    }

    #[test]
    fn a_program_holding_a_camera_open_is_found_and_plumbing_is_not() {
        let root = std::env::temp_dir().join(format!("helix-privacy-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let dev = root.join("dev");
        let proc = root.join("proc");
        std::fs::create_dir_all(&dev).unwrap();
        std::fs::write(dev.join("video0"), "").unwrap();
        std::fs::write(dev.join("video0-meta"), "").unwrap();
        std::fs::write(dev.join("null"), "").unwrap();
        let devices = camera_devices(&dev);
        assert_eq!(devices, [dev.join("video0")]);
        for (pid, comm, holds) in [
            (100u32, "obs", true),
            (101, "wireplumber", true),
            (102, "editor", false),
            (103, "me", true),
        ] {
            std::fs::create_dir_all(proc.join(pid.to_string()).join("fd")).unwrap();
            std::fs::write(proc.join(pid.to_string()).join("comm"), format!("{comm}\n")).unwrap();
            if holds {
                std::os::unix::fs::symlink(
                    dev.join("video0"),
                    proc.join(pid.to_string()).join("fd/5"),
                )
                .unwrap();
            }
        }
        assert_eq!(
            video_users(&proc, &devices, 103),
            ["obs"],
            "wireplumber is plumbing, the editor has no camera open, this process is left out"
        );
        let _ = std::fs::remove_dir_all(&root);
    }
}
