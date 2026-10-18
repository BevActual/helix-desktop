//! Will a model run on this computer, and how fast has it run here? Memory is read from the machine (the GPU's through `nvidia-smi` or
//! the kernel's AMD files, the rest from `/proc/meminfo`) and speeds are what Mobius measured on this computer's own replies.

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Memory {
    /// Dedicated graphics memory in GB, if a GPU with its own memory was found.
    pub gpu_gb: Option<f32>,
    /// Memory the system could give a model in GB (free plus what can be reclaimed).
    pub ram_gb: f32,
}

/// How a model of a given size sits on this machine.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fit {
    /// All of it fits in graphics memory with room for its context.
    Gpu,
    /// Nearly all of the graphics memory: it will run, but little else can share the GPU.
    Tight,
    /// Too big for the GPU (or no GPU): runs in system memory, slower.
    Cpu,
    /// Larger than the memory the computer can spare.
    No,
}

impl Fit {
    pub fn word(self) -> &'static str {
        match self {
            Fit::Gpu => "gpu",
            Fit::Tight => "tight",
            Fit::Cpu => "cpu",
            Fit::No => "no",
        }
    }
}

/// The memory a model needs in GB: the catalog's figure when it has one, else its file size and a margin for the context.
pub fn needed_gb(memory_gb: f32, bytes: u64) -> f32 {
    if memory_gb > 0.0 {
        memory_gb
    } else {
        bytes as f32 / 1e9 * 1.15 + 0.4
    }
}

pub fn fit(need_gb: f32, memory: Memory) -> Fit {
    // The service keeps one model in memory at a time, so what matters is the whole GPU, not what is free right now.
    if let Some(gpu) = memory.gpu_gb {
        if need_gb <= gpu * 0.85 {
            return Fit::Gpu;
        }
        if need_gb <= gpu * 0.97 {
            return Fit::Tight;
        }
    }
    if need_gb <= memory.ram_gb * 0.8 {
        Fit::Cpu
    } else {
        Fit::No
    }
}

fn gpu_gb() -> Option<f32> {
    if let Ok(out) = std::process::Command::new("nvidia-smi")
        .args(["--query-gpu=memory.total", "--format=csv,noheader,nounits"])
        .output()
    {
        if out.status.success() {
            let best = String::from_utf8_lossy(&out.stdout)
                .lines()
                .filter_map(|l| l.trim().parse::<f32>().ok())
                .fold(0.0f32, f32::max);
            if best > 0.0 {
                return Some(best / 1024.0);
            }
        }
    }
    let mut best = 0u64;
    for entry in std::fs::read_dir("/sys/class/drm")
        .into_iter()
        .flatten()
        .flatten()
    {
        let path = entry.path().join("device/mem_info_vram_total");
        if let Some(bytes) = std::fs::read_to_string(path)
            .ok()
            .and_then(|t| t.trim().parse::<u64>().ok())
        {
            best = best.max(bytes);
        }
    }
    (best > 0).then(|| best as f32 / 1e9)
}

fn ram_gb() -> f32 {
    std::fs::read_to_string("/proc/meminfo")
        .ok()
        .and_then(|text| {
            text.lines()
                .find_map(|l| l.strip_prefix("MemAvailable:"))
                .and_then(|v| {
                    v.split_whitespace()
                        .next()
                        .and_then(|n| n.parse::<f32>().ok())
                })
        })
        .map(|kb| kb / 1e6)
        .unwrap_or(0.0)
}

static CACHE: Mutex<Option<(Instant, Memory)>> = Mutex::new(None);

/// The machine's memory, read at most once every ten seconds (the library list is asked for often).
pub fn memory() -> Memory {
    let mut cache = CACHE.lock().expect("hardware cache");
    if let Some((at, memory)) = *cache {
        if at.elapsed() < Duration::from_secs(10) {
            return memory;
        }
    }
    let memory = Memory {
        gpu_gb: gpu_gb(),
        ram_gb: ram_gb(),
    };
    *cache = Some((Instant::now(), memory));
    memory
}

fn speeds_file() -> PathBuf {
    crate::data_home().join("helix/mobius/speeds.json")
}

pub fn speeds() -> BTreeMap<String, f32> {
    std::fs::read_to_string(speeds_file())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

/// Folds one reply's speed into the model's running figure (tokens a second, smoothed so one odd reply does not rule). Short replies are too
/// noisy to count, and the first part of a reply that had to load the model is not a speed, so only replies of 20 tokens or more are used.
pub fn note_speed(model: &str, tokens: u32, ms: u64) {
    if tokens < 20 || ms == 0 || model.is_empty() {
        return;
    }
    let now = tokens as f32 * 1000.0 / ms as f32;
    let mut all = speeds();
    let entry = all.entry(model.to_string()).or_insert(now);
    *entry = *entry * 0.7 + now * 0.3;
    if let Some(dir) = speeds_file().parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::write(
        speeds_file(),
        serde_json::to_string(&all).unwrap_or_default(),
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes_land_in_the_right_place() {
        let m = Memory {
            gpu_gb: Some(16.0),
            ram_gb: 40.0,
        };
        assert_eq!(fit(5.0, m), Fit::Gpu);
        assert_eq!(fit(14.0, m), Fit::Tight);
        assert_eq!(fit(20.0, m), Fit::Cpu);
        assert_eq!(fit(60.0, m), Fit::No);
        let none = Memory {
            gpu_gb: None,
            ram_gb: 8.0,
        };
        assert_eq!(fit(5.0, none), Fit::Cpu);
        assert_eq!(fit(7.0, none), Fit::No);
    }

    #[test]
    fn a_file_with_no_catalog_figure_gets_a_margin() {
        assert!((needed_gb(0.0, 4_000_000_000) - 5.0).abs() < 0.01);
        assert_eq!(needed_gb(3.0, 9), 3.0);
    }
}
