//! The dock as a pure model: pinned applications first, then whatever else has a window, each with how many windows it has and whether
//! it has the focus. Windows are matched to applications by the app id they announce; a window that matches no installed application
//! still gets a place (named by its app id) so nothing that is open is ever missing from the dock. The page only sees ids this model
//! listed, and what activating one does (focus a window, start the application) is decided here.

use serde::Serialize;

use crate::display::sanitize_for_display;
use crate::launcher::App;
use crate::niri::Niri;

/// Pinned by default, when installed, in this order.
pub const PREFERRED: &[&str] = &[
    "org.gnome.Nautilus.desktop",
    "brave-browser.desktop",
    "dev.warp.Warp.desktop",
    "cursor.desktop",
    "com.anthropic.Claude.desktop",
];

const MAX_PINNED: usize = 24;
const MAX_ITEMS: usize = 40;

#[derive(Clone, Debug, PartialEq, Eq)]
struct Item {
    id: String,
    name: String,
    icon: String,
    windows: Vec<u64>,
    focused_window: Option<u64>,
    urgent: bool,
    pinned: bool,
    /// An installed application with this id exists (so it can be started).
    installed: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Dock {
    pinned: Vec<String>,
    items: Vec<Item>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ItemView {
    pub id: String,
    pub name: String,
    /// The icon name or path as the desktop entry gave it; the host turns it into something to draw.
    pub icon: String,
    pub running: usize,
    pub focused: bool,
    pub urgent: bool,
    pub pinned: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Action {
    Focus(u64),
    Launch(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    Unknown,
    /// Not an installed application, so there is nothing to start.
    NotInstalled,
}

fn stem(id: &str) -> String {
    id.strip_suffix(".desktop").unwrap_or(id).to_lowercase()
}

/// How well a window's app id names an application: 2 for its desktop file name or window class, 1 for the program it runs, 0 for no.
fn matches(app: &App, window_app_id: &str) -> u8 {
    let window = window_app_id.to_lowercase();
    if window.is_empty() {
        return 0;
    }
    if stem(&app.id) == window || app.startup_wm_class.to_lowercase() == window {
        return 2;
    }
    let program = app
        .argv
        .first()
        .and_then(|p| p.rsplit('/').next())
        .unwrap_or("")
        .to_lowercase();
    u8::from(program == window)
}

impl Dock {
    pub fn new(pinned: Vec<String>) -> Self {
        let mut dock = Self::default();
        for id in pinned {
            if !dock.pinned.contains(&id) && dock.pinned.len() < MAX_PINNED {
                dock.pinned.push(id);
            }
        }
        dock
    }

    /// What to pin on a first run: the preferred applications that are installed.
    pub fn default_pins(apps: &[App]) -> Vec<String> {
        PREFERRED
            .iter()
            .filter(|id| apps.iter().any(|a| a.id == **id))
            .map(|id| (*id).to_string())
            .collect()
    }

    pub fn pinned(&self) -> &[String] {
        &self.pinned
    }

    /// Recomputes the items from the installed applications and the open windows.
    pub fn refresh(&mut self, apps: &[App], niri: &Niri) {
        let mut items: Vec<Item> = Vec::new();
        let new_item = |id: &str, name: &str, icon: &str, installed: bool, pinned: bool| Item {
            id: id.to_string(),
            name: sanitize_for_display(name, 40),
            icon: icon.to_string(),
            windows: Vec::new(),
            focused_window: None,
            urgent: false,
            pinned,
            installed,
        };
        for id in &self.pinned {
            match apps.iter().find(|a| a.id == *id) {
                Some(app) => items.push(new_item(&app.id, &app.name, &app.icon, true, true)),
                // Pinned but no longer installed: keep its place so it comes back if the application does.
                None => items.push(new_item(
                    id,
                    id.strip_suffix(".desktop").unwrap_or(id),
                    "",
                    false,
                    true,
                )),
            }
        }
        for window in niri.windows() {
            let best = apps
                .iter()
                .map(|app| (matches(app, &window.app_id), app))
                .filter(|(score, _)| *score > 0)
                .max_by_key(|(score, _)| *score);
            let index = match best {
                Some((_, app)) => match items.iter().position(|i| i.id == app.id) {
                    Some(index) => index,
                    None => {
                        items.push(new_item(&app.id, &app.name, &app.icon, true, false));
                        items.len() - 1
                    }
                },
                None => {
                    let key = format!("app:{}", window.app_id);
                    match items.iter().position(|i| i.id == key) {
                        Some(index) => index,
                        None => {
                            let name = if window.app_id.is_empty() {
                                "Window"
                            } else {
                                &window.app_id
                            };
                            items.push(new_item(&key, name, "", false, false));
                            items.len() - 1
                        }
                    }
                }
            };
            let item = &mut items[index];
            item.windows.push(window.id);
            if window.focused {
                item.focused_window = Some(window.id);
            }
            item.urgent |= window.urgent;
        }
        items.truncate(MAX_ITEMS);
        self.items = items;
    }

    pub fn view(&self) -> Vec<ItemView> {
        self.items
            .iter()
            .map(|i| ItemView {
                id: i.id.clone(),
                name: i.name.clone(),
                icon: i.icon.clone(),
                running: i.windows.len(),
                focused: i.focused_window.is_some(),
                urgent: i.urgent,
                pinned: i.pinned,
            })
            .collect()
    }

    fn item(&self, id: &str) -> Result<&Item, RequestError> {
        self.items
            .iter()
            .find(|i| i.id == id)
            .ok_or(RequestError::Unknown)
    }

    /// What clicking the icon does: bring a window forward (the next one, if this application already has the focus), or start it.
    pub fn activate(&self, id: &str) -> Result<Action, RequestError> {
        let item = self.item(id)?;
        if item.windows.is_empty() {
            return if item.installed {
                Ok(Action::Launch(item.id.clone()))
            } else {
                Err(RequestError::NotInstalled)
            };
        }
        let position = item
            .focused_window
            .and_then(|w| item.windows.iter().position(|x| *x == w));
        let next = match position {
            Some(i) => item.windows[(i + 1) % item.windows.len()],
            None => item.windows[0],
        };
        Ok(Action::Focus(next))
    }

    /// A new window of the application (middle click, or "New window"), even when one is already open.
    pub fn launch_new(&self, id: &str) -> Result<Action, RequestError> {
        let item = self.item(id)?;
        if item.installed {
            Ok(Action::Launch(item.id.clone()))
        } else {
            Err(RequestError::NotInstalled)
        }
    }

    /// Keeps an application in the dock. Only an installed application that the dock listed can be pinned.
    pub fn pin(&mut self, id: &str) -> Result<(), RequestError> {
        let item = self.item(id)?;
        if !item.installed {
            return Err(RequestError::NotInstalled);
        }
        let id = item.id.clone();
        if !self.pinned.contains(&id) && self.pinned.len() < MAX_PINNED {
            self.pinned.push(id.clone());
        }
        if let Some(item) = self.items.iter_mut().find(|i| i.id == id) {
            item.pinned = true;
        }
        Ok(())
    }

    /// Takes an application out of the dock's permanent part; it stays while it has windows.
    pub fn unpin(&mut self, id: &str) -> Result<(), RequestError> {
        self.item(id)?;
        self.pinned.retain(|p| p != id);
        if let Some(position) = self.items.iter().position(|i| i.id == id) {
            if self.items[position].windows.is_empty() {
                self.items.remove(position);
            } else {
                self.items[position].pinned = false;
            }
        }
        Ok(())
    }

    /// The pinned order after dragging `id` to `to` (an index among the pinned items).
    pub fn move_pinned(&mut self, id: &str, to: usize) -> Result<(), RequestError> {
        let from = self
            .pinned
            .iter()
            .position(|p| p == id)
            .ok_or(RequestError::Unknown)?;
        let entry = self.pinned.remove(from);
        self.pinned.insert(to.min(self.pinned.len()), entry);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::launcher::parse_desktop_entry;
    use serde_json::json;

    fn app(id: &str, name: &str, exec: &str, wm_class: &str) -> App {
        let class = if wm_class.is_empty() {
            String::new()
        } else {
            format!("StartupWMClass={wm_class}\n")
        };
        parse_desktop_entry(
            id,
            &format!(
                "[Desktop Entry]\nType=Application\nName={name}\nExec={exec}\nIcon={name}\n{class}"
            ),
        )
        .unwrap()
    }

    fn apps() -> Vec<App> {
        vec![
            app(
                "org.gnome.Nautilus.desktop",
                "Files",
                "nautilus --new-window %U",
                "",
            ),
            app(
                "brave-browser.desktop",
                "Brave",
                "/usr/bin/brave-browser-stable %U",
                "brave-browser",
            ),
            app(
                "dev.warp.Warp.desktop",
                "Warp",
                "warp-terminal",
                "dev.warp.Warp",
            ),
            app(
                "cursor.desktop",
                "Cursor",
                "/usr/share/cursor/cursor %F",
                "cursor",
            ),
        ]
    }

    fn niri(windows: &[(u64, &str, bool)]) -> Niri {
        let list: Vec<_> = windows
            .iter()
            .map(|(id, app, focused)| json!({"id": id, "app_id": app, "title": "t", "workspace_id": 1, "is_focused": focused, "is_urgent": false}))
            .collect();
        let mut n = Niri::default();
        n.apply(&json!({"WindowsChanged": {"windows": list}}));
        n
    }

    fn dock(pins: &[&str], windows: &[(u64, &str, bool)]) -> Dock {
        let mut d = Dock::new(pins.iter().map(|p| (*p).to_string()).collect());
        d.refresh(&apps(), &niri(windows));
        d
    }

    fn ids(d: &Dock) -> Vec<String> {
        d.view().into_iter().map(|i| i.id).collect()
    }

    #[test]
    fn pinned_come_first_in_their_order_then_running_ones() {
        let d = dock(
            &["dev.warp.Warp.desktop", "org.gnome.Nautilus.desktop"],
            &[(1, "cursor", false), (2, "org.gnome.nautilus", false)],
        );
        assert_eq!(
            ids(&d),
            [
                "dev.warp.Warp.desktop",
                "org.gnome.Nautilus.desktop",
                "cursor.desktop"
            ]
        );
        let v = d.view();
        assert_eq!(v[0].running, 0);
        assert_eq!(
            v[1].running, 1,
            "matched by the desktop file name, ignoring case"
        );
        assert!(!v[2].pinned && v[2].running == 1);
    }

    #[test]
    fn windows_match_by_window_class_or_by_the_program_they_run() {
        let d = dock(
            &[],
            &[
                (1, "brave-browser", false),
                (2, "Warp-Terminal", false),
                (3, "nautilus", false),
            ],
        );
        assert_eq!(
            ids(&d),
            [
                "brave-browser.desktop",
                "dev.warp.Warp.desktop",
                "org.gnome.Nautilus.desktop"
            ]
        );
    }

    #[test]
    fn a_window_no_application_claims_still_gets_an_icon() {
        let d = dock(&[], &[(1, "some.unknown.App", true), (2, "", false)]);
        assert_eq!(ids(&d), ["app:some.unknown.App", "app:"]);
        let v = d.view();
        assert_eq!(v[0].name, "some.unknown.App");
        assert_eq!(v[1].name, "Window");
        assert!(v[0].focused);
    }

    #[test]
    fn several_windows_of_one_application_share_an_icon_and_are_counted() {
        let d = dock(
            &[],
            &[
                (1, "cursor", false),
                (2, "cursor", true),
                (3, "cursor", false),
            ],
        );
        assert_eq!(d.view().len(), 1);
        assert_eq!(d.view()[0].running, 3);
        assert!(d.view()[0].focused);
    }

    #[test]
    fn urgency_shows_on_the_icon() {
        let mut n = niri(&[(1, "cursor", false)]);
        n.apply(&json!({"WindowUrgencyChanged": {"id": 1, "urgent": true}}));
        let mut d = Dock::new(vec![]);
        d.refresh(&apps(), &n);
        assert!(d.view()[0].urgent);
    }

    #[test]
    fn a_pinned_application_that_is_not_installed_keeps_its_place_but_cannot_start() {
        let d = dock(&["gone.desktop"], &[]);
        assert_eq!(ids(&d), ["gone.desktop"]);
        assert_eq!(d.activate("gone.desktop"), Err(RequestError::NotInstalled));
        assert_eq!(
            d.launch_new("gone.desktop"),
            Err(RequestError::NotInstalled)
        );
    }

    #[test]
    fn clicking_a_stopped_application_starts_it() {
        let d = dock(&["cursor.desktop"], &[]);
        assert_eq!(
            d.activate("cursor.desktop"),
            Ok(Action::Launch("cursor.desktop".into()))
        );
    }

    #[test]
    fn clicking_a_running_application_focuses_a_window_and_repeated_clicks_cycle() {
        let d = dock(&[], &[(1, "cursor", false), (2, "cursor", false)]);
        assert_eq!(
            d.activate("cursor.desktop"),
            Ok(Action::Focus(1)),
            "nothing focused yet: the first window"
        );
        let d = dock(
            &[],
            &[
                (1, "cursor", true),
                (2, "cursor", false),
                (3, "cursor", false),
            ],
        );
        assert_eq!(d.activate("cursor.desktop"), Ok(Action::Focus(2)));
        let d = dock(
            &[],
            &[
                (1, "cursor", false),
                (2, "cursor", false),
                (3, "cursor", true),
            ],
        );
        assert_eq!(
            d.activate("cursor.desktop"),
            Ok(Action::Focus(1)),
            "wraps around"
        );
        let d = dock(&[], &[(1, "cursor", true)]);
        assert_eq!(
            d.activate("cursor.desktop"),
            Ok(Action::Focus(1)),
            "a single focused window stays"
        );
    }

    #[test]
    fn a_new_window_can_be_asked_for_even_while_running() {
        let d = dock(&[], &[(1, "cursor", true)]);
        assert_eq!(
            d.launch_new("cursor.desktop"),
            Ok(Action::Launch("cursor.desktop".into()))
        );
    }

    #[test]
    fn the_page_cannot_name_an_item_the_dock_did_not_list() {
        let d = dock(&["cursor.desktop"], &[]);
        assert_eq!(d.activate("../etc/passwd"), Err(RequestError::Unknown));
        assert_eq!(
            d.launch_new("brave-browser.desktop"),
            Err(RequestError::Unknown),
            "installed but not in the dock"
        );
    }

    #[test]
    fn pinning_keeps_a_running_application_and_unpinning_lets_it_go_when_closed() {
        let mut d = dock(&[], &[(1, "cursor", true)]);
        d.pin("cursor.desktop").unwrap();
        assert_eq!(d.pinned(), ["cursor.desktop"]);
        assert!(d.view()[0].pinned);
        d.refresh(&apps(), &niri(&[]));
        assert_eq!(ids(&d), ["cursor.desktop"], "still there with no windows");
        d.unpin("cursor.desktop").unwrap();
        assert!(d.view().is_empty());
        assert!(d.pinned().is_empty());
    }

    #[test]
    fn unpinning_a_running_application_keeps_its_icon_while_it_runs() {
        let mut d = dock(&["cursor.desktop"], &[(1, "cursor", false)]);
        d.unpin("cursor.desktop").unwrap();
        assert_eq!(ids(&d), ["cursor.desktop"]);
        assert!(!d.view()[0].pinned);
    }

    #[test]
    fn an_unknown_or_uninstalled_item_cannot_be_pinned() {
        let mut d = dock(&[], &[(1, "mystery", false)]);
        assert_eq!(d.pin("app:mystery"), Err(RequestError::NotInstalled));
        assert_eq!(d.pin("nope"), Err(RequestError::Unknown));
        assert_eq!(d.unpin("nope"), Err(RequestError::Unknown));
    }

    #[test]
    fn pinning_twice_does_not_duplicate() {
        let mut d = dock(&[], &[(1, "cursor", false)]);
        d.pin("cursor.desktop").unwrap();
        d.pin("cursor.desktop").unwrap();
        assert_eq!(d.pinned().len(), 1);
    }

    #[test]
    fn dragging_reorders_the_pinned_ones() {
        let mut d = dock(
            &[
                "org.gnome.Nautilus.desktop",
                "brave-browser.desktop",
                "cursor.desktop",
            ],
            &[],
        );
        d.move_pinned("cursor.desktop", 0).unwrap();
        assert_eq!(
            d.pinned(),
            [
                "cursor.desktop",
                "org.gnome.Nautilus.desktop",
                "brave-browser.desktop"
            ]
        );
        d.move_pinned("cursor.desktop", 99).unwrap();
        assert_eq!(d.pinned().last().unwrap(), "cursor.desktop");
        assert!(d.move_pinned("brave-browser.desktop", 0).is_ok());
        assert_eq!(d.move_pinned("nope", 0), Err(RequestError::Unknown));
    }

    #[test]
    fn first_run_pins_what_is_installed_from_the_preferred_list() {
        let pins = Dock::default_pins(&apps());
        assert_eq!(
            pins,
            [
                "org.gnome.Nautilus.desktop",
                "brave-browser.desktop",
                "dev.warp.Warp.desktop",
                "cursor.desktop"
            ]
        );
    }

    #[test]
    fn duplicates_and_excess_pins_are_dropped_on_load() {
        let many: Vec<String> = (0..60).map(|i| format!("a{}.desktop", i % 40)).collect();
        let d = Dock::new(many);
        assert_eq!(d.pinned().len(), MAX_PINNED);
        let mut unique = d.pinned().to_vec();
        unique.sort();
        unique.dedup();
        assert_eq!(unique.len(), d.pinned().len());
    }

    #[test]
    fn names_are_sanitized_for_display() {
        let d = dock(&[], &[(1, "evil\u{202e}name", false)]);
        assert!(!d.view()[0].name.contains('\u{202e}'));
    }

    #[test]
    fn the_dock_is_bounded() {
        let windows: Vec<(u64, String, bool)> = (0..100)
            .map(|i| (i + 1, format!("app.{i}"), false))
            .collect();
        let refs: Vec<(u64, &str, bool)> = windows
            .iter()
            .map(|(i, a, f)| (*i, a.as_str(), *f))
            .collect();
        assert_eq!(dock(&[], &refs).view().len(), MAX_ITEMS);
    }

    #[test]
    fn the_better_match_wins_when_two_applications_claim_one_window() {
        let mut list = vec![app("runner.desktop", "Runner", "foo", "")];
        list.push(app("foo.desktop", "Foo", "other-program", ""));
        let mut d = Dock::new(vec![]);
        d.refresh(&list, &niri(&[(1, "foo", false)]));
        assert_eq!(
            ids(&d),
            ["foo.desktop"],
            "the desktop file's own name beats a program that happens to be called the same"
        );
    }

    #[test]
    fn urgency_on_any_window_shows_even_if_a_later_one_is_calm() {
        let mut n = niri(&[(1, "cursor", false), (2, "cursor", false)]);
        n.apply(&json!({"WindowUrgencyChanged": {"id": 1, "urgent": true}}));
        let mut d = Dock::new(vec![]);
        d.refresh(&apps(), &n);
        assert!(d.view()[0].urgent);
    }

    #[test]
    fn duplicate_pins_are_dropped_wherever_they_appear() {
        let d = Dock::new(
            [
                "a.desktop",
                "b.desktop",
                "a.desktop",
                "b.desktop",
                "c.desktop",
            ]
            .iter()
            .map(|s| (*s).to_string())
            .collect(),
        );
        assert_eq!(d.pinned(), ["a.desktop", "b.desktop", "c.desktop"]);
    }

    #[test]
    fn a_full_dock_does_not_pin_more() {
        let pins: Vec<String> = (0..MAX_PINNED).map(|i| format!("p{i}.desktop")).collect();
        let mut d = Dock::new(pins);
        d.refresh(&apps(), &niri(&[(1, "cursor", false)]));
        d.pin("cursor.desktop").unwrap();
        assert_eq!(d.pinned().len(), MAX_PINNED);
        assert!(!d.pinned().contains(&"cursor.desktop".to_string()));
    }

    #[test]
    fn a_window_with_no_app_id_does_not_match_an_application_without_a_window_class() {
        let d = dock(&[], &[(1, "", false)]);
        assert_eq!(ids(&d), ["app:"]);
    }

    #[test]
    fn a_window_class_that_differs_from_the_desktop_file_name_still_matches() {
        let list = vec![app("com.example.Foo.desktop", "Foo", "foo-bin", "foo-main")];
        let mut d = Dock::new(vec![]);
        d.refresh(&list, &niri(&[(1, "Foo-Main", false)]));
        assert_eq!(ids(&d), ["com.example.Foo.desktop"]);
    }
}
