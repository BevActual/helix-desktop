//! Day/night schedule (`system/day_night_schedule.cpp`) for theme `auto` mode.
//!
//! Coordinates are the configured ones only: IP/address geolocation is published by the
//! running shell's `LocationService`, which the one-shot C++ helper never had either.

#[derive(Clone, Debug, Default)]
pub struct LocationConfig {
    pub custom_schedule: bool,
    /// `HH:MM`
    pub sunset: String,
    /// `HH:MM`
    pub sunrise: String,
    pub latitude: Option<f64>,
    pub longitude: Option<f64>,
}

/// Local wall-clock inputs (`localtime_r`).
#[derive(Clone, Copy, Debug)]
pub struct LocalTime {
    pub minute_of_day: i32,
    pub second: i32,
    /// `tm_yday + 1`
    pub day_of_year: i32,
    /// `tm_gmtoff`
    pub gmt_offset_secs: i64,
}

impl LocalTime {
    /// The wall clock now.
    pub fn now() -> Self {
        // SAFETY: `time` and `localtime_r` are called with valid pointers; `tm` is plain data.
        unsafe {
            let now = libc::time(std::ptr::null_mut());
            let mut tm: libc::tm = std::mem::zeroed();
            libc::localtime_r(&now, &mut tm);
            Self {
                minute_of_day: tm.tm_hour * 60 + tm.tm_min,
                second: tm.tm_sec,
                day_of_year: tm.tm_yday + 1,
                gmt_offset_secs: tm.tm_gmtoff,
            }
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Evaluation {
    pub night: bool,
    pub until_boundary_ms: i64,
    pub since_boundary_ms: i64,
}

const HOUR_MS: i64 = 3_600_000;

/// `normalizedClock`: strict `HH:MM`.
pub fn normalized_clock(value: &str) -> Option<&str> {
    let b = value.as_bytes();
    if b.len() != 5 || b[2] != b':' {
        return None;
    }
    if ![b[0], b[1], b[3], b[4]].iter().all(u8::is_ascii_digit) {
        return None;
    }
    let hour = i32::from(b[0] - b'0') * 10 + i32::from(b[1] - b'0');
    let minute = i32::from(b[3] - b'0') * 10 + i32::from(b[4] - b'0');
    (hour <= 23 && minute <= 59).then_some(value)
}

fn time_to_minutes(hhmm: &str) -> i32 {
    let b = hhmm.as_bytes();
    i32::from(b[0] - b'0') * 600
        + i32::from(b[1] - b'0') * 60
        + i32::from(b[3] - b'0') * 10
        + i32::from(b[4] - b'0')
}

fn is_manual_mode(config: &LocationConfig) -> bool {
    config.custom_schedule
        && normalized_clock(&config.sunset).is_some()
        && normalized_clock(&config.sunrise).is_some()
}

fn since_boundary_ms(now_min: i32, now_sec: i32, last_boundary_min: i32) -> i64 {
    let mut since = now_min - last_boundary_min;
    if since < 0 {
        since += 1440;
    }
    i64::from(since) * 60_000 + i64::from(now_sec) * 1000
}

/// `(sunrise_minutes, sunset_minutes)`; `(0, 0)` polar night, `(0, 1440)` polar day.
fn compute_solar_times(latitude: f64, longitude: f64, now: LocalTime) -> (i32, i32) {
    use std::f64::consts::PI;
    let fractional_year = 2.0 * PI / 365.0 * (f64::from(now.day_of_year) - 1.0);
    let equation_of_time = 229.18
        * (0.000075 + 0.001868 * fractional_year.cos()
            - 0.032077 * fractional_year.sin()
            - 0.014615 * (2.0 * fractional_year).cos()
            - 0.040849 * (2.0 * fractional_year).sin());
    let declination = 0.006918 - 0.399912 * fractional_year.cos()
        + 0.070257 * fractional_year.sin()
        - 0.006758 * (2.0 * fractional_year).cos()
        + 0.000907 * (2.0 * fractional_year).sin()
        - 0.002697 * (3.0 * fractional_year).cos()
        + 0.00148 * (3.0 * fractional_year).sin();

    let sunrise_zenith = 90.833 * PI / 180.0;
    let lat_rad = latitude * PI / 180.0;
    let hour_angle_arg = sunrise_zenith.cos() / (lat_rad.cos() * declination.cos())
        - lat_rad.tan() * declination.tan();
    if hour_angle_arg > 1.0 {
        return (0, 0);
    }
    if hour_angle_arg < -1.0 {
        return (0, 1440);
    }
    let hour_angle_deg = hour_angle_arg.clamp(-1.0, 1.0).acos() * 180.0 / PI;
    let tz_offset_min = now.gmt_offset_secs as f64 / 60.0;
    let solar_noon_min = 720.0 - 4.0 * longitude - equation_of_time + tz_offset_min;
    let normalize = |minutes: f64| -> i32 {
        let mut r = minutes.round() as i32;
        r %= 1440;
        if r < 0 {
            r += 1440;
        }
        r
    };
    (
        normalize(solar_noon_min - hour_angle_deg * 4.0),
        normalize(solar_noon_min + hour_angle_deg * 4.0),
    )
}

/// `day_night_schedule::evaluate` with no resolved (IP/address) coordinates.
pub fn evaluate(config: &LocationConfig, now: LocalTime) -> Evaluation {
    let (now_min, now_sec) = (now.minute_of_day, now.second);

    if is_manual_mode(config) {
        let sunset = time_to_minutes(&config.sunset);
        let sunrise = time_to_minutes(&config.sunrise);
        let night = if sunset < sunrise {
            now_min >= sunset && now_min < sunrise
        } else {
            now_min >= sunset || now_min < sunrise
        };
        let target = if night { sunrise } else { sunset };
        let mut diff = target - now_min;
        if diff <= 0 {
            diff += 1440;
        }
        let ms = i64::from(diff) * 60_000 - i64::from(now_sec) * 1000;
        return Evaluation {
            night,
            until_boundary_ms: ms.max(1000),
            since_boundary_ms: since_boundary_ms(
                now_min,
                now_sec,
                if night { sunset } else { sunrise },
            ),
        };
    }

    let (Some(lat), Some(lon)) = (config.latitude, config.longitude) else {
        return Evaluation {
            night: false,
            until_boundary_ms: HOUR_MS,
            since_boundary_ms: HOUR_MS,
        };
    };

    let (sunrise, sunset) = compute_solar_times(lat, lon, now);
    if sunrise == 0 && sunset == 0 {
        return Evaluation {
            night: true,
            until_boundary_ms: HOUR_MS,
            since_boundary_ms: HOUR_MS,
        };
    }
    if sunrise == 0 && sunset == 1440 {
        return Evaluation {
            night: false,
            until_boundary_ms: HOUR_MS,
            since_boundary_ms: HOUR_MS,
        };
    }
    let night = if sunset > sunrise {
        now_min >= sunset || now_min < sunrise
    } else {
        now_min >= sunset && now_min < sunrise
    };
    let target = if night { sunrise } else { sunset };
    let mut diff = target - now_min;
    if diff <= 0 {
        diff += 1440;
    }
    let ms = i64::from(diff) * 60_000 - i64::from(now_sec) * 1000;
    Evaluation {
        night,
        until_boundary_ms: ms.max(1000),
        since_boundary_ms: since_boundary_ms(
            now_min,
            now_sec,
            if night { sunset } else { sunrise },
        ),
    }
}
