//! The battery tile: percentage and charging state from the kernel's power-supply files (`capacity` and `status`). Parsing is pure; reading
//! the files is the shell's job. A missing, unreadable or implausible value is "no battery", never a made-up number.

use serde::Serialize;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Charge {
    Charging,
    Discharging,
    Full,
    NotCharging,
    Unknown,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Battery {
    percent: u8,
    charge: Charge,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct BatteryView {
    pub present: bool,
    pub percent: u8,
    /// "79%", or empty when there is no battery.
    pub label: String,
    /// "Charging", "Discharging", "Full", "Plugged in" or empty.
    pub state: &'static str,
    pub charging: bool,
    /// Low and not on mains: the page can colour it.
    pub low: bool,
}

const LOW_PERCENT: u8 = 15;

impl Battery {
    /// `capacity` and `status` are the contents of the two files. `None` for anything that is not a percentage from 0 to 100.
    pub fn from_files(capacity: Option<&str>, status: Option<&str>) -> Option<Self> {
        let percent: u8 = capacity?.trim().parse().ok().filter(|p| *p <= 100)?;
        let charge = match status.map(str::trim) {
            Some("Charging") => Charge::Charging,
            Some("Discharging") => Charge::Discharging,
            Some("Full") => Charge::Full,
            Some("Not charging") => Charge::NotCharging,
            _ => Charge::Unknown,
        };
        Some(Self { percent, charge })
    }
}

pub fn view(battery: Option<Battery>) -> BatteryView {
    let Some(battery) = battery else {
        return BatteryView {
            present: false,
            percent: 0,
            label: String::new(),
            state: "",
            charging: false,
            low: false,
        };
    };
    let (state, charging) = match battery.charge {
        Charge::Charging => ("Charging", true),
        Charge::Discharging => ("Discharging", false),
        Charge::Full => ("Full", false),
        Charge::NotCharging => ("Plugged in", false),
        Charge::Unknown => ("", false),
    };
    BatteryView {
        present: true,
        percent: battery.percent,
        label: format!("{}%", battery.percent),
        state,
        charging,
        low: battery.percent <= LOW_PERCENT && battery.charge == Charge::Discharging,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn shown(capacity: &str, status: &str) -> BatteryView {
        view(Battery::from_files(Some(capacity), Some(status)))
    }

    #[test]
    fn percentage_and_state_are_read_from_the_files() {
        let v = shown("79\n", "Not charging\n");
        assert_eq!(
            (v.present, v.percent, v.label.as_str(), v.state, v.charging),
            (true, 79, "79%", "Plugged in", false)
        );
        let v = shown("42", "Charging");
        assert_eq!((v.state, v.charging), ("Charging", true));
        assert_eq!(shown("100", "Full").state, "Full");
        assert_eq!(shown("30", "Discharging").state, "Discharging");
        assert_eq!(
            shown("30", "something new").state,
            "",
            "an unknown status is left unsaid"
        );
    }

    #[test]
    fn low_means_low_and_running_on_battery() {
        assert!(shown("15", "Discharging").low);
        assert!(shown("3", "Discharging").low);
        assert!(!shown("16", "Discharging").low);
        assert!(!shown("5", "Charging").low, "low but charging is fine");
        assert!(!shown("5", "Not charging").low);
        assert!(!shown("5", "Unknown").low);
    }

    #[test]
    fn implausible_values_are_no_battery() {
        for capacity in ["", "abc", "101", "-5", "1e2", "99.5", "  "] {
            assert!(
                Battery::from_files(Some(capacity), Some("Charging")).is_none(),
                "{capacity:?}"
            );
        }
        assert!(Battery::from_files(None, Some("Charging")).is_none());
        let none = view(None);
        assert!(!none.present && none.label.is_empty() && none.state.is_empty() && !none.low);
        assert!(
            view(Battery::from_files(Some("50"), None)).present,
            "a missing status does not hide the percentage"
        );
    }
}
