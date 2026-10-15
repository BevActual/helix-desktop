//! Per-caller microphone level capture (C++ `InputMeter` + `Service::meters`).

mod pw_capture;

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use pw_capture::PipeWireInputCapture;

const LEASE: Duration = Duration::from_secs(8);

struct Slot {
    source: u32,
    expires: Instant,
    capture: Option<PipeWireInputCapture>,
}

pub struct AudioMeterManager {
    slots: Mutex<HashMap<String, Slot>>,
}

impl AudioMeterManager {
    pub fn new() -> Self {
        Self {
            slots: Mutex::new(HashMap::new()),
        }
    }

    pub fn start(&self, owner: &str, source: u32, live_capture: bool) {
        let mut slots = self.slots.lock().expect("audio meter slots");
        let expires = Instant::now() + LEASE;
        if let Some(slot) = slots.get_mut(owner) {
            if slot.source == source {
                slot.expires = expires;
                return;
            }
            slot.capture = None;
        }
        let capture = if live_capture {
            PipeWireInputCapture::start(source)
        } else {
            None
        };
        slots.insert(
            owner.to_string(),
            Slot {
                source,
                expires,
                capture,
            },
        );
    }

    pub fn stop(&self, owner: &str) {
        self.slots.lock().expect("audio meter slots").remove(owner);
    }

    pub fn remove_owner(&self, owner: &str) {
        self.stop(owner);
    }

    /// Returns `(owner, source, peak, available)` for active leases after expiry cleanup.
    pub fn poll(&self) -> Vec<(String, u32, f64, bool)> {
        let now = Instant::now();
        let mut slots = self.slots.lock().expect("audio meter slots");
        slots.retain(|_, slot| now < slot.expires);
        slots
            .iter()
            .map(|(owner, slot)| {
                let (peak, available) = match &slot.capture {
                    Some(cap) => (cap.peak(), cap.available()),
                    None => (0.0, false),
                };
                (owner.clone(), slot.source, peak, available)
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::pw_capture::input_peak_from_bytes;

    #[test]
    fn peak_from_f32_samples() {
        let samples = [0.0f32, 0.25f32, -0.5f32, 1.5f32];
        let bytes: Vec<u8> = samples
            .iter()
            .flat_map(|s| s.to_le_bytes())
            .collect();
        let peak = input_peak_from_bytes(&bytes);
        assert!((peak - 1.0).abs() < 1e-6);
    }
}
