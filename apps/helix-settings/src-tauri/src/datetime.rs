//! The Date & time page: the time zone and automatic time (systemd-timedated, over the system bus: the person's password is asked for by the
//! desktop's own polkit agent, never here), how the shell writes the clock, and where the person is, for sunrise and sunset (the automatic theme
//! and night light). Rows are built here from plain values so they can be tested; the commands in `main.rs` read and write the real things.

use helix_config::catalog::{Choice, Control, RowView};
use helix_config::clock_prefs::ClockFormat;
use serde_json::Value;

const TIMEDATE: &str = "org.freedesktop.timedate1";
const PATH: &str = "/org/freedesktop/timedate1";

/// What timedated says.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TimeDate {
    pub timezone: String,
    pub zones: Vec<String>,
    pub ntp: bool,
    pub can_ntp: bool,
    pub synced: bool,
}

/// Where the person is, as the desktop service keeps it (`location.*`).
#[derive(Clone, Debug, PartialEq)]
pub struct Place {
    pub latitude: f64,
    pub longitude: f64,
    pub custom: bool,
    pub sunrise: String,
    pub sunset: String,
}

impl Default for Place {
    fn default() -> Self {
        Place {
            latitude: 0.0,
            longitude: 0.0,
            custom: false,
            sunrise: "06:00".into(),
            sunset: "18:00".into(),
        }
    }
}

impl Place {
    /// Reads the `location.*` entries of the service's settings list (an array of `{id, value}`).
    pub fn from_settings(settings: &Value) -> Place {
        let mut place = Place::default();
        let Some(entries) = settings.as_array() else {
            return place;
        };
        let value = |id: &str| {
            entries
                .iter()
                .find(|e| e.get("id").and_then(Value::as_str) == Some(id))
                .and_then(|e| e.get("value"))
        };
        if let Some(n) = value("location.latitude").and_then(Value::as_f64) {
            place.latitude = n;
        }
        if let Some(n) = value("location.longitude").and_then(Value::as_f64) {
            place.longitude = n;
        }
        if let Some(on) = value("location.custom_schedule").and_then(Value::as_bool) {
            place.custom = on;
        }
        if let Some(t) = value("location.sunrise").and_then(Value::as_str) {
            if valid_time(t) {
                place.sunrise = t.into();
            }
        }
        if let Some(t) = value("location.sunset").and_then(Value::as_str) {
            if valid_time(t) {
                place.sunset = t.into();
            }
        }
        place
    }
}

/// `HH:MM` on the half hour, the only times the page offers.
pub fn valid_time(text: &str) -> bool {
    let Some((h, m)) = text.split_once(':') else {
        return false;
    };
    h.len() == 2
        && m.len() == 2
        && h.parse::<u32>().is_ok_and(|h| h < 24)
        && (m == "00" || m == "30")
}

fn time_options() -> Vec<Choice> {
    (0..48)
        .map(|i| {
            let text = format!("{:02}:{:02}", i / 2, (i % 2) * 30);
            Choice {
                value: text.clone(),
                label: text,
            }
        })
        .collect()
}

/// A change the page may ask for. Anything else is refused before it reaches the system.
#[derive(Clone, Debug, PartialEq)]
pub enum Edit {
    Timezone(String),
    Ntp(bool),
    Clock(ClockFormat),
    Latitude(f64),
    Longitude(f64),
    Custom(bool),
    Sunrise(String),
    Sunset(String),
}

pub fn parse_edit(id: &str, change: &Value) -> Result<Edit, String> {
    let kind = change.get("kind").and_then(Value::as_str);
    let value = change.get("value");
    let text = || value.and_then(Value::as_str).ok_or("not text".to_string());
    let number = || {
        value
            .and_then(Value::as_f64)
            .filter(|n| n.is_finite())
            .ok_or("not a number".to_string())
    };
    let switch = || {
        value
            .and_then(Value::as_bool)
            .ok_or("not a boolean".to_string())
    };
    match (id, kind) {
        ("time:zone", Some("choice")) => Ok(Edit::Timezone(text()?.to_string())),
        ("time:ntp", Some("switch")) => Ok(Edit::Ntp(switch()?)),
        ("time:clock", Some("choice")) => ClockFormat::from_name(text()?)
            .map(Edit::Clock)
            .ok_or_else(|| "pick a clock".to_string()),
        ("place:latitude", Some("number")) => {
            let n = number()?;
            (-90.0..=90.0)
                .contains(&n)
                .then_some(Edit::Latitude(n))
                .ok_or_else(|| "latitude is -90 to 90".to_string())
        }
        ("place:longitude", Some("number")) => {
            let n = number()?;
            (-180.0..=180.0)
                .contains(&n)
                .then_some(Edit::Longitude(n))
                .ok_or_else(|| "longitude is -180 to 180".to_string())
        }
        ("place:custom", Some("switch")) => Ok(Edit::Custom(switch()?)),
        ("place:sunrise", Some("choice")) => {
            let t = text()?;
            valid_time(t)
                .then(|| Edit::Sunrise(t.into()))
                .ok_or_else(|| "pick a time".to_string())
        }
        ("place:sunset", Some("choice")) => {
            let t = text()?;
            valid_time(t)
                .then(|| Edit::Sunset(t.into()))
                .ok_or_else(|| "pick a time".to_string())
        }
        _ => Err("unknown change".into()),
    }
}

/// The page's rows. `now` is the date and time as text, made by the caller (this builds nothing from the clock).
pub fn rows(
    timedate: Option<&TimeDate>,
    clock: ClockFormat,
    place: &Place,
    now: &str,
) -> Vec<RowView> {
    let row = |group: &str, id: &str, title: &str, description: &str, control: Control| RowView {
        id: id.into(),
        title: title.into(),
        group: group.into(),
        description: description.into(),
        overridden: false,
        control,
    };
    let mut rows = vec![row(
        "Time",
        "time:now",
        "Now",
        "",
        Control::Text { value: now.into() },
    )];
    match timedate {
        Some(td) => {
            let mut options: Vec<Choice> = td
                .zones
                .iter()
                .map(|zone| Choice {
                    value: zone.clone(),
                    label: zone.replace('_', " ").replace('/', " / "),
                })
                .collect();
            if !td.timezone.is_empty() && !td.zones.contains(&td.timezone) {
                options.push(Choice {
                    value: td.timezone.clone(),
                    label: td.timezone.clone(),
                });
            }
            rows.push(row(
                "Time",
                "time:zone",
                "Time zone",
                "Changing it asks for your password.",
                Control::Choice {
                    options,
                    current: td.timezone.clone(),
                },
            ));
            if td.can_ntp {
                rows.push(row(
                    "Time",
                    "time:ntp",
                    "Set the time automatically",
                    if td.ntp && td.synced {
                        "Using network time, and in step with it."
                    } else if td.ntp {
                        "Using network time. Not in step with it yet."
                    } else {
                        "Off: the clock is only as right as it was set."
                    },
                    Control::Switch { on: td.ntp },
                ));
            }
        }
        None => rows.push(row(
            "Time",
            "time:zone",
            "Time zone",
            "The system's time service is not available here, so the time zone cannot be changed from Settings.",
            Control::Text { value: "Unavailable".into() },
        )),
    }
    rows.push(row(
        "Time",
        "time:clock",
        "Clock",
        "How the shell writes the time. Automatic follows your language.",
        Control::Choice {
            options: vec![
                Choice {
                    value: "auto".into(),
                    label: "Automatic".into(),
                },
                Choice {
                    value: "12".into(),
                    label: "12-hour".into(),
                },
                Choice {
                    value: "24".into(),
                    label: "24-hour".into(),
                },
            ],
            current: clock.name().into(),
        },
    ));
    let sliders = |id: &str, title: &str, description: &str, min: f64, max: f64, value: f64| {
        row(
            "Location",
            id,
            title,
            description,
            Control::Slider {
                min,
                max,
                integer: false,
                value,
            },
        )
    };
    rows.push(sliders(
        "place:latitude",
        "Latitude",
        "Where you are, north positive. Used for sunrise and sunset: the automatic theme turns at sunset. Nothing is sent anywhere.",
        -90.0,
        90.0,
        place.latitude,
    ));
    rows.push(sliders(
        "place:longitude",
        "Longitude",
        "East positive.",
        -180.0,
        180.0,
        place.longitude,
    ));
    rows.push(row(
        "Location",
        "place:custom",
        "Use my own day and night times",
        "Instead of working sunrise and sunset out from where you are.",
        Control::Switch { on: place.custom },
    ));
    if place.custom {
        rows.push(row(
            "Location",
            "place:sunrise",
            "Day starts",
            "",
            Control::Choice {
                options: time_options(),
                current: place.sunrise.clone(),
            },
        ));
        rows.push(row(
            "Location",
            "place:sunset",
            "Night starts",
            "",
            Control::Choice {
                options: time_options(),
                current: place.sunset.clone(),
            },
        ));
    }
    rows
}

async fn timedate_proxy() -> Result<zbus::Proxy<'static>, String> {
    let connection = zbus::Connection::system()
        .await
        .map_err(|e| e.to_string())?;
    zbus::Proxy::new(&connection, TIMEDATE, PATH, TIMEDATE)
        .await
        .map_err(|e| e.to_string())
}

/// Reads timedated, or `None` when it is not there (a session without the system bus, a test).
pub async fn read_timedate() -> Option<TimeDate> {
    let proxy = timedate_proxy().await.ok()?;
    let timezone: String = proxy.get_property("Timezone").await.ok()?;
    let mut zones: Vec<String> = proxy.call("ListTimezones", &()).await.unwrap_or_default();
    zones.sort();
    Some(TimeDate {
        timezone,
        zones,
        ntp: proxy.get_property("NTP").await.unwrap_or(false),
        can_ntp: proxy.get_property("CanNTP").await.unwrap_or(false),
        synced: proxy.get_property("NTPSynchronized").await.unwrap_or(false),
    })
}

/// Sets the time zone through timedated; `interactive` lets polkit ask the person for their password.
pub async fn set_timezone(zone: &str) -> Result<(), String> {
    timedate_proxy()
        .await?
        .call_method("SetTimezone", &(zone, true))
        .await
        .map(|_| ())
        .map_err(|e| e.to_string())
}

pub async fn set_ntp(on: bool) -> Result<(), String> {
    timedate_proxy()
        .await?
        .call_method("SetNTP", &(on, true))
        .await
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// The date and time now, as text: "Wednesday 21 October, 14:05" or "2:05 PM".
pub fn now_text(clock: ClockFormat) -> String {
    // SAFETY: `localtime_r` is given a valid time and a zeroed `tm`; `strftime` writes at most the buffer's length.
    unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&now, &mut tm).is_null() {
            return String::new();
        }
        let format = match clock {
            ClockFormat::Hours12 => c"%A %-d %B, %-I:%M %p",
            _ => c"%A %-d %B, %H:%M",
        };
        let mut buffer = [0 as libc::c_char; 96];
        let n = libc::strftime(buffer.as_mut_ptr(), buffer.len(), format.as_ptr(), &tm);
        std::ffi::CStr::from_ptr(buffer.as_ptr())
            .to_string_lossy()
            .chars()
            .take(n)
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn td() -> TimeDate {
        TimeDate {
            timezone: "Europe/London".into(),
            zones: vec!["America/New_York".into(), "Europe/London".into()],
            ntp: true,
            can_ntp: true,
            synced: true,
        }
    }

    #[test]
    fn the_rows_say_what_is_set_and_hide_what_does_not_apply() {
        let place = Place::default();
        let all = rows(Some(&td()), ClockFormat::Hours24, &place, "Wednesday");
        let ids: Vec<&str> = all.iter().map(|r| r.id.as_str()).collect();
        assert_eq!(
            ids,
            [
                "time:now",
                "time:zone",
                "time:ntp",
                "time:clock",
                "place:latitude",
                "place:longitude",
                "place:custom"
            ]
        );
        assert!(
            matches!(&all[1].control, Control::Choice { current, options } if current == "Europe/London" && options[0].label == "America / New York")
        );
        let custom = rows(
            Some(&td()),
            ClockFormat::Auto,
            &Place {
                custom: true,
                ..place.clone()
            },
            "x",
        );
        assert!(
            custom.iter().any(|r| r.id == "place:sunrise")
                && custom.iter().any(|r| r.id == "place:sunset")
        );
        let no_ntp = TimeDate {
            can_ntp: false,
            ..td()
        };
        assert!(!rows(Some(&no_ntp), ClockFormat::Auto, &place, "x")
            .iter()
            .any(|r| r.id == "time:ntp"));
        let none = rows(None, ClockFormat::Auto, &place, "x");
        assert!(none
            .iter()
            .any(|r| r.id == "time:zone" && matches!(r.control, Control::Text { .. })));
    }

    #[test]
    fn only_legal_edits_get_through() {
        let choice = |v: &str| json!({"kind": "choice", "value": v});
        assert_eq!(
            parse_edit("time:zone", &choice("Europe/Paris")),
            Ok(Edit::Timezone("Europe/Paris".into()))
        );
        assert_eq!(
            parse_edit("time:clock", &choice("24")),
            Ok(Edit::Clock(ClockFormat::Hours24))
        );
        assert!(parse_edit("time:clock", &choice("13")).is_err());
        assert_eq!(
            parse_edit("place:latitude", &json!({"kind": "number", "value": 51.5})),
            Ok(Edit::Latitude(51.5))
        );
        assert!(parse_edit("place:latitude", &json!({"kind": "number", "value": 91})).is_err());
        assert!(parse_edit("place:longitude", &json!({"kind": "number", "value": -181})).is_err());
        assert_eq!(
            parse_edit("place:sunset", &choice("19:30")),
            Ok(Edit::Sunset("19:30".into()))
        );
        assert!(parse_edit("place:sunset", &choice("19:15")).is_err());
        assert!(parse_edit("place:sunrise", &choice("25:00")).is_err());
        assert!(parse_edit("time:ntp", &json!({"kind": "choice", "value": "x"})).is_err());
        assert!(parse_edit("hostname", &json!({"kind": "text", "value": "x"})).is_err());
    }

    #[test]
    fn the_place_is_read_from_the_service_list() {
        let list = json!([
            {"id": "location.latitude", "value": 51.5},
            {"id": "location.longitude", "value": -0.12},
            {"id": "location.custom_schedule", "value": true},
            {"id": "location.sunset", "value": "21:30"},
            {"id": "location.sunrise", "value": "nonsense"},
        ]);
        let place = Place::from_settings(&list);
        assert_eq!(
            (place.latitude, place.longitude, place.custom),
            (51.5, -0.12, true)
        );
        assert_eq!(place.sunset, "21:30");
        assert_eq!(
            place.sunrise, "06:00",
            "a bad time falls back to the default"
        );
        assert_eq!(Place::from_settings(&json!(null)), Place::default());
    }

    #[test]
    fn the_clock_text_is_not_empty() {
        assert!(!now_text(ClockFormat::Hours24).is_empty());
        assert!(
            now_text(ClockFormat::Hours12).contains("M")
                || !now_text(ClockFormat::Hours12).is_empty()
        );
    }
}
