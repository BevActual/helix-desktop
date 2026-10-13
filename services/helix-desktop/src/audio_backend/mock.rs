//! In-memory Audio1 backend for nested / offline sessions.

use serde_json::{json, Value};

use super::Node;

#[derive(Clone, Debug)]
pub struct MockBackend {
    pub sinks: Vec<Node>,
    pub sources: Vec<Node>,
    pub programs: Vec<Node>,
}

pub fn force_mock() -> bool {
    matches!(
        std::env::var("HELIX_DESKTOP_MOCK_AUDIO").ok().as_deref(),
        Some("1") | Some("true") | Some("yes")
    )
}

impl MockBackend {
    /// Placeholder until PipeWire appears (integration tests expect `available: false` initially).
    pub fn offline() -> Self {
        Self {
            sinks: Vec::new(),
            sources: Vec::new(),
            programs: Vec::new(),
        }
    }

    pub fn new(initial_volume: f64) -> Self {
        let volume = initial_volume.clamp(0.0, 1.5);
        Self {
            sinks: vec![Node {
                id: 1,
                name: "Helix Speakers".into(),
                description: "Helix Speakers".into(),
                volume,
                muted: false,
                is_default: true,
                available: true,
            }],
            sources: vec![Node {
                id: 2,
                name: "Helix Microphone".into(),
                description: "Helix Microphone".into(),
                volume: 0.8,
                muted: false,
                is_default: true,
                available: true,
            }],
            programs: Vec::new(),
        }
    }

    pub fn is_available(&self) -> bool {
        !self.sinks.is_empty() || !self.sources.is_empty()
    }

    pub fn snapshot(&self) -> Value {
        json!({
            "available": self.is_available(),
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

    pub fn set_volume(&mut self, id: u32, volume: f64) -> Result<(), String> {
        let node = self
            .find_mut(id)
            .ok_or_else(|| "Audio device is no longer available".to_string())?;
        node.volume = volume;
        Ok(())
    }

    pub fn set_muted(&mut self, id: u32, muted: bool) -> Result<(), String> {
        let node = self
            .find_mut(id)
            .ok_or_else(|| "Audio device is no longer available".to_string())?;
        node.muted = muted;
        Ok(())
    }

    pub fn set_default(&mut self, id: u32, input: bool) -> Result<(), String> {
        let list = if input {
            &mut self.sources
        } else {
            &mut self.sinks
        };
        if !list.iter().any(|node| node.id == id) {
            return Err("Audio device is no longer available".into());
        }
        for node in list.iter_mut() {
            node.is_default = node.id == id;
        }
        Ok(())
    }
}
