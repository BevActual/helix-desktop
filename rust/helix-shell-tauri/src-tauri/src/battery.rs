//! Reads the battery from the kernel's power-supply directory. Parsing and display are in helix_shell_core::power; this only finds the
//! files. `HELIX_POWER_SUPPLY_DIR` points it at another directory (the tests do).

use std::path::{Path, PathBuf};

use helix_shell_core::power::Battery;

fn directory() -> PathBuf {
    std::env::var_os("HELIX_POWER_SUPPLY_DIR")
        .filter(|value| !value.is_empty())
        .map_or_else(|| PathBuf::from("/sys/class/power_supply"), PathBuf::from)
}

fn read(path: &Path) -> Option<String> {
    use std::io::Read;
    // These files hold a few bytes, but the kernel reports every sysfs attribute as 4096 bytes long, so the reported size says nothing.
    // Read at most a few hundred bytes, whatever the file claims.
    let mut text = String::new();
    std::fs::File::open(path)
        .ok()?
        .take(512)
        .read_to_string(&mut text)
        .ok()?;
    Some(text)
}

/// The first supply in name order whose `type` is `Battery`.
pub fn read_battery() -> Option<Battery> {
    let mut supplies: Vec<PathBuf> = std::fs::read_dir(directory())
        .ok()?
        .flatten()
        .map(|entry| entry.path())
        .collect();
    supplies.sort();
    supplies
        .into_iter()
        .filter(|supply| read(&supply.join("type")).is_some_and(|kind| kind.trim() == "Battery"))
        .find_map(|supply| {
            Battery::from_files(
                read(&supply.join("capacity")).as_deref(),
                read(&supply.join("status")).as_deref(),
            )
        })
}

#[cfg(test)]
mod tests {
    use super::*;
    use helix_shell_core::power::view;

    fn supply(root: &Path, name: &str, kind: &str, capacity: &str, status: &str) {
        let dir = root.join(name);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("type"), format!("{kind}\n")).unwrap();
        std::fs::write(dir.join("capacity"), format!("{capacity}\n")).unwrap();
        std::fs::write(dir.join("status"), format!("{status}\n")).unwrap();
    }

    #[test]
    fn the_battery_is_the_first_supply_of_type_battery() {
        let root = std::env::temp_dir().join(format!("helix-battery-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        supply(&root, "ADP0", "Mains", "1", "Unknown");
        supply(&root, "BAT0", "Battery", "79", "Not charging");
        supply(&root, "BAT1", "Battery", "10", "Discharging");
        std::env::set_var("HELIX_POWER_SUPPLY_DIR", &root);
        let shown = view(read_battery());
        assert_eq!(
            (shown.present, shown.percent, shown.state),
            (true, 79, "Plugged in")
        );
        // A directory with no battery is "no battery", and so is one that does not exist.
        std::fs::remove_dir_all(root.join("BAT0")).unwrap();
        std::fs::remove_dir_all(root.join("BAT1")).unwrap();
        assert!(!view(read_battery()).present, "only a mains adapter");
        std::fs::remove_dir_all(&root).unwrap();
        assert!(!view(read_battery()).present, "no directory");
        // A file far larger than any power-supply attribute is cut off, not trusted: its start is not a percentage.
        supply(&root, "BAT0", "Battery", &"9".repeat(20_000), "Discharging");
        std::env::set_var("HELIX_POWER_SUPPLY_DIR", &root);
        assert!(!view(read_battery()).present, "an oversized capacity file");
        std::fs::remove_dir_all(&root).unwrap();
        std::env::remove_var("HELIX_POWER_SUPPLY_DIR");
    }
}
