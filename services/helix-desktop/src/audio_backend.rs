//! In-memory Audio1 backend for the Rust service scaffold (no PipeWire yet).
//! Nested / offline sessions get a default sink + source so the shell audio page works.

use serde_json::{json, Value};

#[derive(Clone, Debug)]
pub struct Node {
    pub id: u32,
    pub name: String,
    pub description: String,
    pub volume: f64,
    pub muted: bool,
    pub is_default: bool,
    pub available: bool,
}

impl Node {
    fn to_json(&self) -> Value {
        json!({
            "id": self.id,
            "name": self.name,
            "description": self.description,
            "volume": self.volume,
            "muted": self.muted,
            "default": self.is_default,
            "available": self.available,
        })
    }
}

#[derive(Clone, Debug)]
pub struct AudioBackend {
    pub sinks: Vec<Node>,
    pub sources: Vec<Node>,
    pub programs: Vec<Node>,
}

impl AudioBackend {
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

    pub fn snapshot(&self) -> Value {
        json!({
            "available": true,
            "sinks": self.sinks.iter().map(Node::to_json).collect::<Vec<_>>(),
            "sources": self.sources.iter().map(Node::to_json).collect::<Vec<_>>(),
            "programs": self.programs.iter().map(Node::to_json).collect::<Vec<_>>(),
        })
    }

    fn find_mut(&mut self, id: u32) -> Option<&mut Node> {
        self.sinks
            .iter_mut()
            .chain(self.sources.iter_mut())
            .chain(self.programs.iter_mut())
            .find(|node| node.id == id)
    }

    pub fn set_volume(&mut self, id: u32, volume: f64) -> Result<(), String> {
        if !volume.is_finite() || !(0.0..=1.5).contains(&volume) {
            return Err("Volume out of range".into());
        }
        let node = self.find_mut(id).ok_or("Audio device is no longer available")?;
        node.volume = volume;
        Ok(())
    }

    pub fn set_muted(&mut self, id: u32, muted: bool) -> Result<(), String> {
        let node = self.find_mut(id).ok_or("Audio device is no longer available")?;
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
