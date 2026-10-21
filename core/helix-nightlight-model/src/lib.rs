//! Night light. A warm screen in the evening: the person's settings (`~/.config/helix/nightlight.json`, written by Settings and watched by the
//! `helix-nightlight` service), when the screen should be warm (an on-off switch, or a window of the day with a gentle fade at each end), and the
//! colour ramp that makes a screen that warm. Pure: no Wayland, no clock of its own (the caller says what time it is).

use serde_json::{json, Value};

pub const WARMTH_MIN: u32 = 2000;
pub const WARMTH_MAX: u32 = 6500;
pub const WARMTH_DEFAULT: u32 = 3800;
/// The temperature a screen shows with night light off: the white point of ordinary displays.
pub const NEUTRAL: u32 = 6500;
pub const START_DEFAULT: u32 = 20;
pub const END_DEFAULT: u32 = 7;
/// Minutes the screen takes to warm up before the window opens and to cool down after it closes.
pub const FADE_MINUTES: u32 = 30;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NightLight {
    pub enabled: bool,
    /// Colour temperature in kelvin the screen reaches (lower is warmer).
    pub warmth: u32,
    /// When off the screen is warm all the time; when on, only inside the window.
    pub scheduled: bool,
    /// Hour (0–23) the warm window opens.
    pub start: u32,
    /// Hour (0–23) the warm window closes.
    pub end: u32,
    /// With a schedule: follow sunset and sunrise where the person is (Settings → Date & time), instead of `start` and `end`.
    pub sun: bool,
}

/// Where the sun is, as the day and night schedule (`helix-theme`) works it out for the place the person set: whether it is night, and how long
/// since it became night or day (`since`) and until the next change (`until`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sky {
    pub night: bool,
    pub since_ms: i64,
    pub until_ms: i64,
}

impl Default for NightLight {
    fn default() -> Self {
        NightLight {
            enabled: false,
            warmth: WARMTH_DEFAULT,
            scheduled: true,
            start: START_DEFAULT,
            end: END_DEFAULT,
            sun: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NightLightError {
    UnknownSetting,
    BadValue,
}

impl NightLight {
    /// Reads the file's text. A bad or missing value falls back to its default, and numbers are clamped.
    pub fn parse(text: &str) -> NightLight {
        let mut night = NightLight::default();
        let Ok(value) = serde_json::from_str::<Value>(text) else {
            return night;
        };
        if let Some(on) = value.get("enabled").and_then(Value::as_bool) {
            night.enabled = on;
        }
        if let Some(n) = value.get("warmth").and_then(Value::as_u64) {
            night.warmth = n.clamp(WARMTH_MIN as u64, WARMTH_MAX as u64) as u32;
        }
        if let Some(on) = value.get("scheduled").and_then(Value::as_bool) {
            night.scheduled = on;
        }
        if let Some(n) = value.get("start").and_then(Value::as_u64) {
            night.start = (n % 24) as u32;
        }
        if let Some(n) = value.get("end").and_then(Value::as_u64) {
            night.end = (n % 24) as u32;
        }
        if let Some(on) = value.get("sun").and_then(Value::as_bool) {
            night.sun = on;
        }
        night
    }

    pub fn to_json(&self) -> String {
        json!({
            "version": 1,
            "enabled": self.enabled,
            "warmth": self.warmth,
            "scheduled": self.scheduled,
            "start": self.start,
            "end": self.end,
            "sun": self.sun,
        })
        .to_string()
    }

    /// Changes one setting by name. Values outside the range are refused, not clamped, so the page never shows what was not kept.
    pub fn set(&self, name: &str, value: &Value) -> Result<NightLight, NightLightError> {
        let mut next = self.clone();
        match name {
            "enabled" => next.enabled = value.as_bool().ok_or(NightLightError::BadValue)?,
            "scheduled" => next.scheduled = value.as_bool().ok_or(NightLightError::BadValue)?,
            "sun" => next.sun = value.as_bool().ok_or(NightLightError::BadValue)?,
            "warmth" => {
                let n = value.as_u64().ok_or(NightLightError::BadValue)?;
                if !(WARMTH_MIN as u64..=WARMTH_MAX as u64).contains(&n) {
                    return Err(NightLightError::BadValue);
                }
                next.warmth = n as u32;
            }
            "start" | "end" => {
                let n = value.as_u64().ok_or(NightLightError::BadValue)?;
                if n > 23 {
                    return Err(NightLightError::BadValue);
                }
                if name == "start" {
                    next.start = n as u32;
                } else {
                    next.end = n as u32;
                }
            }
            _ => return Err(NightLightError::UnknownSetting),
        }
        Ok(next)
    }

    /// How warm the screen should be at `minute` of the day (0–1439): 0 is the ordinary white point and 1 is the chosen warmth. Inside the window
    /// it is 1; the screen warms over `FADE_MINUTES` before the window opens and cools over as long after it closes. Always 1 when not scheduled.
    pub fn strength_at(&self, minute: u32) -> f64 {
        if !self.enabled {
            return 0.0;
        }
        if !self.scheduled {
            return 1.0;
        }
        let day = 24 * 60;
        let (start, end, now) = (self.start % 24 * 60, self.end % 24 * 60, minute % day);
        if start == end {
            return 0.0;
        }
        // Minutes from `from` forward to `to` around the clock.
        let forward = |from: u32, to: u32| (to + day - from) % day;
        let length = forward(start, end);
        let since_start = forward(start, now);
        if since_start < length {
            return 1.0;
        }
        let fade = FADE_MINUTES as f64;
        // Before the window: warming. After it: cooling.
        let until_start = forward(now, start) as f64;
        let since_end = forward(end, now) as f64;
        if until_start < fade {
            1.0 - until_start / fade
        } else if since_end < fade {
            1.0 - since_end / fade
        } else {
            0.0
        }
    }

    /// How warm the screen should be when it follows the sun: warm after dark with the same fade as the hours, and cooling over the last
    /// stretch of the night before sunrise. Always 1 when not scheduled (and 0 when off), the same as `strength_at`.
    pub fn strength_with_sky(&self, minute: u32, sky: Sky) -> f64 {
        if !self.enabled || !self.scheduled || !self.sun {
            return self.strength_at(minute);
        }
        if !sky.night {
            return 0.0;
        }
        let fade = f64::from(FADE_MINUTES) * 60_000.0;
        ((sky.since_ms as f64) / fade)
            .min((sky.until_ms as f64) / fade)
            .clamp(0.0, 1.0)
    }

    /// The temperature the screen shows at `minute`.
    pub fn temperature_at(&self, minute: u32) -> u32 {
        self.temperature_for(self.strength_at(minute))
    }

    /// The temperature for a strength from 0 (ordinary) to 1 (the chosen warmth).
    pub fn temperature_for(&self, strength: f64) -> u32 {
        (NEUTRAL as f64 + (self.warmth as f64 - NEUTRAL as f64) * strength).round() as u32
    }
}

/// `~/.config/helix/nightlight.json` (under `$XDG_CONFIG_HOME` when that is set).
pub fn config_path() -> std::path::PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config")))
        .unwrap_or_default();
    base.join("helix/nightlight.json")
}

/// The settings on disk; the defaults when the file is missing or unreadable.
pub fn load() -> NightLight {
    std::fs::read_to_string(config_path())
        .map(|text| NightLight::parse(&text))
        .unwrap_or_default()
}

/// Writes the settings the way the other Helix settings files are written: atomically, readable by the owner only, with a temporary name of
/// this process's own so two writers do not share a half-written file.
pub fn save(night: &NightLight) -> Result<(), String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let path = config_path();
    std::fs::create_dir_all(path.parent().ok_or("no folder for the settings")?)
        .map_err(|e| e.to_string())?;
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    {
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|e| e.to_string())?;
        file.write_all(night.to_json().as_bytes())
            .map_err(|e| e.to_string())?;
    }
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// Turns night light on or off, keeping everything else the person chose.
pub fn set_enabled(on: bool) -> Result<NightLight, String> {
    let next = load()
        .set("enabled", &json!(on))
        .map_err(|error| format!("{error:?}"))?;
    save(&next)?;
    Ok(next)
}

/// Red, green and blue multipliers (0–1) that turn a screen at the ordinary white point into one at `kelvin`. A blackbody approximation, scaled so
/// that 6500 K is exactly neutral.
pub fn white_point(kelvin: u32) -> [f64; 3] {
    fn raw(kelvin: f64) -> [f64; 3] {
        let t = kelvin.clamp(1000.0, 10_000.0) / 100.0;
        let red = if t <= 66.0 {
            255.0
        } else {
            329.698_727_446 * (t - 60.0).powf(-0.133_204_759_2)
        };
        let green = if t <= 66.0 {
            99.470_802_586_1 * t.ln() - 161.119_568_166_1
        } else {
            288.122_169_528_3 * (t - 60.0).powf(-0.075_514_849_2)
        };
        let blue = if t >= 66.0 {
            255.0
        } else if t <= 19.0 {
            0.0
        } else {
            138.517_731_223_1 * (t - 10.0).ln() - 305.044_792_730_7
        };
        [red, green, blue].map(|c| c.clamp(0.0, 255.0))
    }
    let neutral = raw(NEUTRAL as f64);
    let this = raw(kelvin as f64);
    [0, 1, 2].map(|i| (this[i] / neutral[i]).clamp(0.0, 1.0))
}

/// The gamma ramp for an output whose table has `size` entries per channel: all red, then all green, then all blue, each a straight line from
/// black to its multiplier of full brightness (`wlr-gamma-control` wants exactly this, in native byte order).
pub fn ramp(size: usize, multiplier: [f64; 3]) -> Vec<u16> {
    let mut out = Vec::with_capacity(size * 3);
    for channel in multiplier {
        for i in 0..size {
            let level = if size > 1 {
                i as f64 / (size - 1) as f64
            } else {
                1.0
            };
            out.push((level * channel * 65_535.0).round().clamp(0.0, 65_535.0) as u16);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn evening() -> NightLight {
        NightLight {
            enabled: true,
            ..NightLight::default()
        }
    }

    #[test]
    fn the_ordinary_white_point_changes_nothing() {
        assert_eq!(white_point(NEUTRAL), [1.0, 1.0, 1.0]);
        let ramp = ramp(256, white_point(NEUTRAL));
        assert_eq!(ramp[0], 0);
        assert_eq!(ramp[255], 65_535);
        assert_eq!(ramp[256 + 255], 65_535);
        assert_eq!(ramp.len(), 768);
    }

    #[test]
    fn warmer_means_less_blue_then_less_green_and_red_stays() {
        let warm = white_point(3400);
        assert!(warm[0] > 0.99, "{warm:?}");
        assert!(warm[2] < warm[1] && warm[1] < warm[0], "{warm:?}");
        let warmer = white_point(2200);
        assert!(
            warmer[2] < warm[2] && warmer[1] < warm[1],
            "{warmer:?} {warm:?}"
        );
        let cool = white_point(9000);
        assert!(cool[0] < 1.0 && cool[2] >= 0.99, "{cool:?}");
    }

    #[test]
    fn off_is_neutral_and_unscheduled_is_always_warm() {
        let off = NightLight::default();
        assert_eq!(off.strength_at(23 * 60), 0.0);
        let always = NightLight {
            enabled: true,
            scheduled: false,
            ..NightLight::default()
        };
        assert_eq!(always.strength_at(12 * 60), 1.0);
        assert_eq!(always.temperature_at(12 * 60), WARMTH_DEFAULT);
    }

    #[test]
    fn the_window_runs_through_midnight_and_fades_at_each_end() {
        let n = evening(); // 20:00 to 07:00
        assert_eq!(n.strength_at(22 * 60), 1.0);
        assert_eq!(n.strength_at(0), 1.0);
        assert_eq!(n.strength_at(6 * 60 + 59), 1.0);
        assert_eq!(n.strength_at(12 * 60), 0.0);
        // Warming over the half hour before 20:00.
        assert_eq!(n.strength_at(19 * 60 + 30), 0.0);
        let half = n.strength_at(19 * 60 + 45);
        assert!((half - 0.5).abs() < 1e-9, "{half}");
        assert!(n.strength_at(19 * 60 + 59) > 0.95);
        // Cooling over the half hour after 07:00.
        let cooling = n.strength_at(7 * 60 + 15);
        assert!((cooling - 0.5).abs() < 1e-9, "{cooling}");
        assert_eq!(n.strength_at(7 * 60 + 30), 0.0);
        // The same hour twice is no window.
        let none = NightLight {
            start: 9,
            end: 9,
            ..evening()
        };
        assert_eq!(none.strength_at(9 * 60), 0.0);
    }

    #[test]
    fn following_the_sun_is_warm_in_the_dark_and_fades_at_each_end() {
        let n = NightLight {
            sun: true,
            ..evening()
        };
        let sky = |night, since_min: i64, until_min: i64| Sky {
            night,
            since_ms: since_min * 60_000,
            until_ms: until_min * 60_000,
        };
        assert_eq!(
            n.strength_with_sky(12 * 60, sky(false, 300, 200)),
            0.0,
            "daytime"
        );
        assert_eq!(
            n.strength_with_sky(23 * 60, sky(true, 240, 400)),
            1.0,
            "deep night"
        );
        assert!(
            (n.strength_with_sky(20 * 60, sky(true, 15, 600)) - 0.5).abs() < 1e-9,
            "warming after sunset"
        );
        assert!(
            (n.strength_with_sky(6 * 60, sky(true, 500, 15)) - 0.5).abs() < 1e-9,
            "cooling before sunrise"
        );
        // Without `sun`, or without a schedule, the hours (or always-on) rule.
        let hours = NightLight {
            sun: false,
            ..evening()
        };
        assert_eq!(
            hours.strength_with_sky(22 * 60, sky(false, 0, 0)),
            hours.strength_at(22 * 60)
        );
        let always = NightLight {
            scheduled: false,
            sun: true,
            ..evening()
        };
        assert_eq!(always.strength_with_sky(12 * 60, sky(false, 0, 0)), 1.0);
        assert_eq!(
            NightLight {
                sun: true,
                ..NightLight::default()
            }
            .strength_with_sky(0, sky(true, 999, 999)),
            0.0,
            "off is off"
        );
        assert_eq!(n.temperature_for(0.0), NEUTRAL);
        assert_eq!(n.temperature_for(1.0), n.warmth);
    }

    #[test]
    fn a_daytime_window_works_too() {
        let n = NightLight {
            start: 9,
            end: 17,
            ..evening()
        };
        assert_eq!(n.strength_at(12 * 60), 1.0);
        assert_eq!(n.strength_at(17 * 60 + 30), 0.0);
        assert_eq!(n.strength_at(3 * 60), 0.0);
        assert_eq!(n.temperature_at(3 * 60), NEUTRAL);
    }

    #[test]
    fn the_file_round_trips_is_lenient_and_set_refuses_out_of_range() {
        let n = NightLight {
            enabled: true,
            warmth: 3000,
            scheduled: false,
            start: 21,
            end: 6,
            sun: true,
        };
        assert_eq!(NightLight::parse(&n.to_json()), n);
        assert_eq!(NightLight::parse("not json"), NightLight::default());
        assert_eq!(
            NightLight::parse(r#"{"warmth":1,"start":99}"#).warmth,
            WARMTH_MIN
        );
        let d = NightLight::default();
        assert!(d.set("enabled", &json!(true)).unwrap().enabled);
        assert_eq!(
            d.set("warmth", &json!(1000)),
            Err(NightLightError::BadValue)
        );
        assert_eq!(d.set("start", &json!(24)), Err(NightLightError::BadValue));
        assert_eq!(d.set("enabled", &json!(1)), Err(NightLightError::BadValue));
        assert_eq!(
            d.set("brightness", &json!(1)),
            Err(NightLightError::UnknownSetting)
        );
    }
}
