//! Helix display profiles: how the connected displays are set up (scale, orientation, resolution, position, on or off), remembered per *set* of
//! connected displays and applied by the compositor itself when that set appears again: a laptop on its own, the same laptop docked with two
//! monitors. A display is known by "make model serial", or by its connector when it reports no identity. The file is
//! `$XDG_STATE_HOME/helix/display-profiles.json`; the person's `config.kdl` stays the baseline and is never rewritten by this.

use std::path::PathBuf;

use niri_config::FloatOrInt;
use niri_ipc::{HelixDisplayProfile, Output as IpcOutput, Transform};
use serde::{Deserialize, Serialize};
use tracing::{info, warn};

use crate::niri::State;

const MAX_PROFILES: usize = 32;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
struct SavedDisplay {
    /// The name the setup is applied under: "make model serial", or the connector.
    name: String,
    off: bool,
    scale: Option<f64>,
    transform: Option<Transform>,
    /// Width, height and refresh rate in millihertz of the mode in use.
    mode: Option<(u16, u16, u32)>,
    position: Option<(i32, i32)>,
    vrr: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
struct Profile {
    /// The set of displays this is for, sorted.
    displays: Vec<String>,
    outputs: Vec<SavedDisplay>,
}

#[derive(Debug, Default, Serialize, Deserialize)]
struct Store {
    #[serde(default)]
    profiles: Vec<Profile>,
}

fn file() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/state")))
        .unwrap_or_default();
    base.join("helix/display-profiles.json")
}

fn load() -> Store {
    std::fs::read_to_string(file())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn save(store: &Store) -> Result<(), String> {
    let path = file();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temporary = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(store).map_err(|e| e.to_string())?;
    std::fs::write(&temporary, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// The name a display is known by: "make model serial" when all three are reported and none is "Unknown", else its connector.
fn identity(output: &IpcOutput) -> String {
    let known = |text: &str| !text.trim().is_empty() && !text.eq_ignore_ascii_case("unknown");
    match &output.serial {
        Some(serial) if known(&output.make) && known(&output.model) && known(serial) => {
            format!("{} {} {}", output.make, output.model, serial)
        }
        _ => output.name.clone(),
    }
}

/// Every connected display with the name it is saved under. A name two displays share is replaced by their connectors, so each is its own.
fn named(outputs: &[IpcOutput]) -> Vec<(String, &IpcOutput)> {
    let names: Vec<String> = outputs.iter().map(identity).collect();
    outputs
        .iter()
        .zip(&names)
        .map(|(output, name)| {
            let twins = names.iter().filter(|other| other.eq_ignore_ascii_case(name)).count();
            (if twins > 1 { output.name.clone() } else { name.clone() }, output)
        })
        .collect()
}

fn set_of(named: &[(String, &IpcOutput)]) -> Vec<String> {
    let mut displays: Vec<String> = named.iter().map(|(name, _)| name.clone()).collect();
    displays.sort();
    displays
}

fn snapshot(name: String, output: &IpcOutput) -> SavedDisplay {
    let mode = output
        .current_mode
        .and_then(|index| output.modes.get(index))
        .map(|mode| (mode.width, mode.height, mode.refresh_rate));
    SavedDisplay {
        name,
        off: output.logical.is_none(),
        scale: output.logical.as_ref().map(|l| l.scale),
        transform: output.logical.as_ref().map(|l| l.transform),
        mode,
        position: output.logical.as_ref().map(|l| (l.x, l.y)),
        vrr: output.vrr_enabled,
    }
}

impl State {
    fn helix_connected(&self) -> Vec<IpcOutput> {
        let mut outputs: Vec<IpcOutput> = self
            .backend
            .ipc_outputs()
            .lock()
            .unwrap()
            .values()
            .cloned()
            .collect();
        outputs.sort_by(|a, b| a.name.cmp(&b.name));
        outputs
    }

    pub fn helix_display_profile_info(&self) -> HelixDisplayProfile {
        let outputs = self.helix_connected();
        let displays = set_of(&named(&outputs));
        let saved = load().profiles.iter().any(|p| p.displays == displays);
        HelixDisplayProfile { displays, saved }
    }

    /// Remembers the setup of the displays connected now, for exactly this set.
    pub fn helix_save_display_profile(&mut self) -> Result<(), String> {
        let outputs = self.helix_connected();
        if outputs.is_empty() {
            return Err("no display is connected".into());
        }
        let names = named(&outputs);
        let displays = set_of(&names);
        let profile = Profile {
            outputs: names
                .iter()
                .map(|(name, output)| snapshot(name.clone(), output))
                .collect(),
            displays: displays.clone(),
        };
        let mut store = load();
        store.profiles.retain(|p| p.displays != displays);
        store.profiles.push(profile);
        if store.profiles.len() > MAX_PROFILES {
            let extra = store.profiles.len() - MAX_PROFILES;
            store.profiles.drain(..extra);
        }
        // What was just saved is what is on screen, so it does not need applying when the set is next looked at.
        self.niri.helix_profile_key = Some(displays);
        save(&store)
    }

    pub fn helix_forget_display_profile(&mut self) -> Result<(), String> {
        let displays = set_of(&named(&self.helix_connected()));
        let mut store = load();
        store.profiles.retain(|p| p.displays != displays);
        save(&store)
    }

    /// Applies the remembered setup for the displays connected now. `Ok(false)` when none is remembered.
    pub fn helix_apply_display_profile(&mut self) -> Result<bool, String> {
        let outputs = self.helix_connected();
        let displays = set_of(&named(&outputs));
        let Some(profile) = load().profiles.into_iter().find(|p| p.displays == displays) else {
            return Ok(false);
        };
        for saved in &profile.outputs {
            self.modify_output_config(&saved.name, |config| {
                config.off = saved.off;
                if saved.off {
                    return;
                }
                if let Some(scale) = saved.scale {
                    config.scale = Some(FloatOrInt(scale));
                }
                if let Some(transform) = saved.transform {
                    config.transform = transform;
                }
                if let Some((width, height, millihertz)) = saved.mode {
                    config.mode = Some(niri_config::output::Mode {
                        custom: false,
                        mode: niri_ipc::ConfiguredMode {
                            width,
                            height,
                            refresh: Some(f64::from(millihertz) / 1000.0),
                        },
                    });
                    config.modeline = None;
                }
                config.position = saved
                    .position
                    .map(|(x, y)| niri_config::Position { x, y });
                config.variable_refresh_rate = saved
                    .vrr
                    .then_some(niri_config::Vrr { on_demand: false });
            });
        }
        self.reload_output_config();
        info!("HELIX display profile applied for {}", displays.join(" + "));
        Ok(true)
    }

    /// Called when the set of connected displays may have changed: if it is a set the person has set up before, set it up that way again.
    pub fn helix_displays_changed(&mut self) {
        let outputs = self.helix_connected();
        if outputs.is_empty() {
            return;
        }
        let displays = set_of(&named(&outputs));
        if self.niri.helix_profile_key.as_ref() == Some(&displays) {
            return;
        }
        self.niri.helix_profile_key = Some(displays);
        if let Err(error) = self.helix_apply_display_profile() {
            warn!("could not apply the display profile: {error}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn output(name: &str, make: &str, model: &str, serial: Option<&str>) -> IpcOutput {
        IpcOutput {
            name: name.into(),
            make: make.into(),
            model: model.into(),
            serial: serial.map(str::to_string),
            physical_size: None,
            modes: Vec::new(),
            current_mode: None,
            is_custom_mode: false,
            vrr_supported: false,
            vrr_enabled: false,
            logical: None,
            max_bpc: None,
        }
    }

    #[test]
    fn a_display_is_known_by_identity_unless_it_has_none_or_shares_it() {
        let a = output("DP-3", "Dell", "U27", Some("1"));
        let b = output("eDP-1", "BOE", "0x1", None);
        let all = [a.clone(), b.clone()];
        let names = named(&all);
        assert_eq!(names[0].0, "Dell U27 1");
        assert_eq!(names[1].0, "eDP-1");
        assert_eq!(set_of(&names), ["Dell U27 1", "eDP-1"]);
        let twin = output("DP-4", "Dell", "U27", Some("1"));
        let both = [a, twin];
        let names: Vec<String> = named(&both).into_iter().map(|(n, _)| n).collect();
        assert_eq!(names, ["DP-3", "DP-4"]);
        assert_eq!(identity(&output("X", "Unknown", "M", Some("1"))), "X");
    }

    #[test]
    fn the_set_is_the_same_whichever_order_the_displays_are_found_in() {
        let a = output("DP-3", "Dell", "U27", Some("1"));
        let b = output("DP-4", "LG", "TV", Some("2"));
        assert_eq!(set_of(&named(&[a.clone(), b.clone()])), set_of(&named(&[b, a])));
    }
}
