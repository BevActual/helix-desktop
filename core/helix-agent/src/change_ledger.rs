//! What an assistant command changed, and whether it can be put back.
//!
//! The shell fills this in after a write. The chat stores only the display strings. The machine value
//! stays here, in the shell process, so a saved chat cannot name a command to run. See `docs/design/act-undo.md`.

use super::agent_msg::Command;

/// One setting. A newer change of the same key retires the older undo.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SettingKey {
    Volume,
    Brightness,
    Theme,
    NightLight,
    DoNotDisturb,
    Bar,
    Workspace,
}

/// Which live shell commands have a setting to put back. `None` means the command is not one of those
/// (opening a panel, launching an app, reading state).
pub fn setting_of(command: &Command) -> Option<SettingKey> {
    Some(match command {
        Command::VolumeStep(_) | Command::VolumeToggleMute | Command::VolumeSet(_) => {
            SettingKey::Volume
        }
        Command::BrightnessStep(_) | Command::BrightnessSet(_) => SettingKey::Brightness,
        Command::ThemeSet(_) | Command::ThemeToggle => SettingKey::Theme,
        Command::NightLight(_) => SettingKey::NightLight,
        Command::DndSet(_) | Command::DndToggle => SettingKey::DoNotDisturb,
        Command::BarShow | Command::BarHide | Command::BarToggle => SettingKey::Bar,
        Command::WorkspaceSwitch(_) | Command::AgentDeskFocus => SettingKey::Workspace,
        _ => return None,
    })
}

/// The sentence the shell returns, and that the chat shows. Empty when either side is missing.
pub fn sentence(label: &str, before: &str, after: &str) -> String {
    if before.is_empty() || after.is_empty() {
        return String::new();
    }
    if before == after {
        format!("{label} is {after}.")
    } else {
        format!("{label} {before} to {after}.")
    }
}

pub fn volume_text(percent: u8, muted: bool) -> String {
    if muted {
        format!("{percent}%, muted")
    } else {
        format!("{percent}%")
    }
}

/// Percent and mute together. Raising the volume clears mute, so undo writes both back.
pub fn volume_machine(percent: u8, muted: bool) -> String {
    format!("{percent} {}", u8::from(muted))
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChangeRecord {
    pub task: u64,
    pub key: SettingKey,
    pub before_machine: String,
    pub after_machine: String,
    pub before: String,
    pub after: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ChangeLedger {
    entries: Vec<ChangeRecord>,
}

impl ChangeLedger {
    pub const fn new() -> Self {
        Self {
            entries: Vec::new(),
        }
    }

    /// Remembers a change that actually moved a value. Task 0 is not an assistant task. A repeat of the same
    /// setting drops the older entry, and so does a later setting from the same task: the feed has one pair.
    pub fn record(
        &mut self,
        task: u64,
        key: SettingKey,
        before_machine: &str,
        after_machine: &str,
        before: &str,
        after: &str,
    ) -> bool {
        if task == 0
            || before_machine.is_empty()
            || after_machine.is_empty()
            || before_machine == after_machine
        {
            return false;
        }
        self.entries
            .retain(|entry| entry.key != key && entry.task != task);
        self.entries.push(ChangeRecord {
            task,
            key,
            before_machine: before_machine.to_string(),
            after_machine: after_machine.to_string(),
            before: before.to_string(),
            after: after.to_string(),
        });
        true
    }

    pub fn get(&self, task: u64) -> Option<&ChangeRecord> {
        self.entries.iter().rev().find(|entry| entry.task == task)
    }

    /// Undo may proceed only while the live value is still the value this task wrote. Otherwise the entry is dropped.
    pub fn take_if_current(&mut self, task: u64, live_machine: &str) -> Option<ChangeRecord> {
        let position = self.entries.iter().position(|entry| entry.task == task)?;
        if self.entries[position].after_machine != live_machine {
            self.entries.remove(position);
            return None;
        }
        Some(self.entries.remove(position))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent_msg::{Direction, Theme};

    #[test]
    fn the_live_restore_commands_have_a_setting_and_a_panel_does_not() {
        assert_eq!(
            setting_of(&Command::VolumeSet(40.0)),
            Some(SettingKey::Volume)
        );
        assert_eq!(
            setting_of(&Command::VolumeStep(-5)),
            Some(SettingKey::Volume)
        );
        assert_eq!(
            setting_of(&Command::VolumeToggleMute),
            Some(SettingKey::Volume)
        );
        assert_eq!(
            setting_of(&Command::BrightnessSet(20.0)),
            Some(SettingKey::Brightness)
        );
        assert_eq!(
            setting_of(&Command::BrightnessStep(10)),
            Some(SettingKey::Brightness)
        );
        assert_eq!(
            setting_of(&Command::ThemeSet(Theme::Auto)),
            Some(SettingKey::Theme)
        );
        assert_eq!(setting_of(&Command::ThemeToggle), Some(SettingKey::Theme));
        assert_eq!(
            setting_of(&Command::NightLight(Some(true))),
            Some(SettingKey::NightLight)
        );
        assert_eq!(
            setting_of(&Command::DndSet(true)),
            Some(SettingKey::DoNotDisturb)
        );
        assert_eq!(
            setting_of(&Command::DndToggle),
            Some(SettingKey::DoNotDisturb)
        );
        assert_eq!(setting_of(&Command::BarShow), Some(SettingKey::Bar));
        assert_eq!(setting_of(&Command::BarHide), Some(SettingKey::Bar));
        assert_eq!(setting_of(&Command::BarToggle), Some(SettingKey::Bar));
        assert_eq!(
            setting_of(&Command::WorkspaceSwitch(Direction::Next)),
            Some(SettingKey::Workspace)
        );
        assert_eq!(
            setting_of(&Command::AgentDeskFocus),
            Some(SettingKey::Workspace)
        );
        assert_eq!(setting_of(&Command::PanelOpen("launcher".into())), None);
        assert_eq!(setting_of(&Command::SettingsOpen(None)), None);
        assert_eq!(setting_of(&Command::WindowClose(Some(1))), None);
        assert_eq!(setting_of(&Command::AppLaunch("Firefox".into())), None);
        assert_eq!(setting_of(&Command::ThemeGet), None);
        assert_eq!(setting_of(&Command::Status), None);
    }

    #[test]
    fn volume_remembers_percent_and_mute_and_a_later_change_retires_it() {
        let mut ledger = ChangeLedger::new();
        ledger.record(
            3,
            SettingKey::Volume,
            &volume_machine(40, true),
            &volume_machine(45, false),
            &volume_text(40, true),
            &volume_text(45, false),
        );
        let saved = ledger.get(3).unwrap();
        assert_eq!(saved.before_machine, "40 1");
        assert_eq!(saved.after_machine, "45 0");
        assert_eq!(
            sentence("Volume", &saved.before, &saved.after),
            "Volume 40%, muted to 45%."
        );
        ledger.record(4, SettingKey::Volume, "45 0", "50 0", "45%", "50%");
        assert!(ledger.get(3).is_none(), "the older volume undo is retired");
        assert_eq!(ledger.get(4).unwrap().after, "50%");
        assert!(ledger.record(4, SettingKey::Brightness, "30%", "40%", "30%", "40%"));
        assert_eq!(
            ledger.get(4).unwrap().key,
            SettingKey::Brightness,
            "one task keeps the latest pair"
        );
    }

    #[test]
    fn undo_is_refused_once_the_value_has_moved_again() {
        let mut ledger = ChangeLedger::new();
        ledger.record(2, SettingKey::Brightness, "30", "40", "30%", "40%");
        assert!(ledger.take_if_current(2, "55").is_none());
        assert!(ledger.get(2).is_none());
        ledger.record(2, SettingKey::Brightness, "30", "40", "30%", "40%");
        let taken = ledger.take_if_current(2, "40").unwrap();
        assert_eq!(taken.before_machine, "30");
        assert!(ledger.get(2).is_none());
    }

    #[test]
    fn an_unchanged_value_and_a_missing_task_are_not_recorded() {
        let mut ledger = ChangeLedger::new();
        ledger.record(1, SettingKey::Theme, "dark", "dark", "dark", "dark");
        ledger.record(0, SettingKey::Theme, "dark", "light", "dark", "light");
        ledger.record(1, SettingKey::Bar, "", "hidden", "", "hidden");
        assert!(ledger.get(1).is_none());
        assert!(ledger.get(0).is_none());
        assert_eq!(sentence("Volume", "100%", "100%"), "Volume is 100%.");
        assert!(sentence("Volume", "", "40%").is_empty());
    }
}
