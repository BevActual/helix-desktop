//! Audio1 device backend: WirePlumber (`wpctl`) when PipeWire is present, otherwise in-memory mock.
//!
//! Nested harnesses set `HELIX_DESKTOP_MOCK_AUDIO=1` so private-bus smoke tests stay deterministic.

mod mock;
mod watch;
mod wpctl;

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
    pub(crate) fn to_json(&self) -> Value {
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
enum Inner {
    Mock(mock::MockBackend),
    Wpctl(wpctl::WpctlBackend),
}

#[derive(Clone, Debug)]
pub struct AudioBackend {
    inner: Inner,
    /// When true, `refresh` may promote from offline mock to `wpctl` once PipeWire is up.
    recover_pipewire: bool,
    /// Last time PipeWire was probed for recovery (each probe spawns `wpctl`).
    last_probe: std::time::Instant,
}

impl AudioBackend {
    pub fn new(initial_volume: f64) -> Self {
        let (inner, recover_pipewire) = if mock::force_mock() {
            (Inner::Mock(mock::MockBackend::new(initial_volume)), false)
        } else if wpctl::probe() {
            (Inner::Wpctl(wpctl::WpctlBackend::new()), false)
        } else {
            tracing::info!("PipeWire unavailable; waiting for audio backend");
            (Inner::Mock(mock::MockBackend::offline()), true)
        };
        Self {
            inner,
            recover_pipewire,
            last_probe: std::time::Instant::now(),
        }
    }

    pub fn is_available(&self) -> bool {
        match &self.inner {
            Inner::Mock(backend) => backend.is_available(),
            Inner::Wpctl(backend) => backend.is_available(),
        }
    }

    /// Re-read device state from PipeWire. Returns `true` when the public snapshot changed.
    pub fn refresh(&mut self) -> bool {
        if self.recover_pipewire && self.last_probe.elapsed() >= std::time::Duration::from_secs(3) {
            self.last_probe = std::time::Instant::now();
            if wpctl::probe() {
                self.inner = Inner::Wpctl(wpctl::WpctlBackend::new());
                self.recover_pipewire = false;
                return true;
            }
        }
        match &mut self.inner {
            Inner::Mock(_) => false,
            // Event driven: only re-reads the device list when PipeWire reported a change.
            Inner::Wpctl(backend) => backend.refresh_if_needed(),
        }
    }

    pub fn snapshot(&self) -> Value {
        match &self.inner {
            Inner::Mock(backend) => backend.snapshot(),
            Inner::Wpctl(backend) => backend.snapshot(),
        }
    }

    pub fn sinks(&self) -> &[Node] {
        match &self.inner {
            Inner::Mock(backend) => &backend.sinks,
            Inner::Wpctl(backend) => &backend.sinks,
        }
    }

    pub fn sources(&self) -> &[Node] {
        match &self.inner {
            Inner::Mock(backend) => &backend.sources,
            Inner::Wpctl(backend) => &backend.sources,
        }
    }

    pub fn has_source(&self, id: u32) -> bool {
        self.sources().iter().any(|node| node.id == id)
    }

    pub fn uses_live_pipewire(&self) -> bool {
        matches!(&self.inner, Inner::Wpctl(_))
    }

    pub fn set_volume(&mut self, id: u32, volume: f64) -> Result<(), String> {
        if !volume.is_finite() || !(0.0..=1.5).contains(&volume) {
            return Err("Volume out of range".into());
        }
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_volume(id, volume),
            Inner::Wpctl(backend) => backend.set_volume(id, volume),
        }
    }

    pub fn set_muted(&mut self, id: u32, muted: bool) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_muted(id, muted),
            Inner::Wpctl(backend) => backend.set_muted(id, muted),
        }
    }

    pub fn set_default(&mut self, id: u32, input: bool) -> Result<(), String> {
        match &mut self.inner {
            Inner::Mock(backend) => backend.set_default(id, input),
            Inner::Wpctl(backend) => backend.set_default(id, input),
        }
    }
}
