//! Pure package-store models for Helix Software. The host supplies catalog rows from Flatpak /
//! PackageKit / fwupd; this module filters, sorts, and validates mutations. Nothing here runs a
//! package command.

use serde::Serialize;

use crate::display::sanitize_for_display;

const MAX_ID: usize = 200;
const MAX_TITLE: usize = 120;
const MAX_QUERY: usize = 120;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    #[default]
    Flatpak,
    Rpm,
    Firmware,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize)]
pub struct Package {
    pub id: String,
    pub title: String,
    pub summary: String,
    pub kind: Kind,
    pub installed: bool,
    pub update: bool,
    pub remote: String,
    /// Display version (empty when unknown).
    pub version: String,
    /// Download or installed size in bytes (0 when unknown).
    pub size: u64,
    /// `user` or `system` for Flatpak; empty otherwise.
    pub installation: String,
    /// Appstream categories as published (`Utility`, `Game`, …); empty until the appstream index has been read.
    pub categories: Vec<String>,
    /// Unix time of the newest release (0 when unknown).
    pub released: u64,
    /// The publisher is verified by its remote.
    pub verified: bool,
}

/// What the appstream index adds to a package.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Meta {
    pub categories: Vec<String>,
    pub released: u64,
    pub verified: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct PackageView {
    pub id: String,
    pub title: String,
    pub summary: String,
    pub kind: Kind,
    pub installed: bool,
    pub update: bool,
    pub remote: String,
    pub version: String,
    pub size: u64,
    pub installation: String,
    pub verified: bool,
}

/// A row of apps on the Discover page.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Shelf {
    pub title: String,
    /// The category this shelf stands for (empty for "Recently updated"); "See All" opens it.
    pub category: String,
    pub rows: Vec<PackageView>,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Discover {
    pub hero: Option<PackageView>,
    pub shelves: Vec<Shelf>,
}

/// The categories the store shows: (appstream category, name on screen).
pub const CATEGORIES: [(&str, &str); 9] = [
    ("Office", "Productivity"),
    ("Graphics", "Graphics & Design"),
    ("AudioVideo", "Audio & Video"),
    ("Development", "Developer Tools"),
    ("Game", "Games"),
    ("Network", "Internet"),
    ("Education", "Education"),
    ("Science", "Science"),
    ("Utility", "Utilities"),
];

/// A package belongs to a store category when any of its appstream categories maps to it (Audio and Video count as AudioVideo).
fn in_category(package: &Package, category: &str) -> bool {
    package
        .categories
        .iter()
        .any(|c| c == category || (category == "AudioVideo" && (c == "Audio" || c == "Video")))
}

/// One page of a result list (the catalog is thousands of rows; the page asks for a window).
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Page {
    pub total: usize,
    pub rows: Vec<PackageView>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mutation {
    Install,
    Remove,
    Update,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SoftwareError {
    UnknownPackage,
    BadId,
    AlreadyInstalled,
    NotInstalled,
    NoUpdate,
    WrongKind,
}

#[derive(Clone, Debug, Default)]
pub struct Store {
    packages: Vec<Package>,
}

impl Store {
    pub fn set(&mut self, packages: Vec<Package>) {
        self.packages = packages
            .into_iter()
            .filter(|p| valid_id(&p.id))
            .map(|p| Package {
                id: p.id,
                title: sanitize_for_display(&p.title, MAX_TITLE),
                summary: sanitize_for_display(&p.summary, 300),
                kind: p.kind,
                installed: p.installed,
                update: p.update,
                remote: sanitize_for_display(&p.remote, 80),
                version: sanitize_for_display(&p.version, 40),
                size: p.size,
                installation: sanitize_for_display(&p.installation, 16),
                categories: p
                    .categories
                    .into_iter()
                    .take(8)
                    .map(|c| sanitize_for_display(&c, 32))
                    .collect(),
                released: p.released,
                verified: p.verified,
            })
            .collect();
    }

    pub fn explore(&self, kind: Kind, query: &str) -> Vec<PackageView> {
        let q = sanitize_for_display(query, MAX_QUERY).to_ascii_lowercase();
        self.packages
            .iter()
            .filter(|p| p.kind == kind)
            .filter(|p| {
                q.is_empty()
                    || p.title.to_ascii_lowercase().contains(&q)
                    || p.id.to_ascii_lowercase().contains(&q)
                    || p.summary.to_ascii_lowercase().contains(&q)
            })
            .map(view_of)
            .collect()
    }

    /// A ranked window of the catalog: title matches first (exact, prefix, word prefix, contains), then id, then summary; ties by title.
    pub fn explore_page(&self, kind: Kind, query: &str, offset: usize, limit: usize) -> Page {
        self.explore_in(kind, query, "", offset, limit)
    }

    /// As `explore_page`, limited to one store category (an appstream name from [`CATEGORIES`]; empty = all).
    pub fn explore_in(
        &self,
        kind: Kind,
        query: &str,
        category: &str,
        offset: usize,
        limit: usize,
    ) -> Page {
        let q = sanitize_for_display(query, MAX_QUERY).to_ascii_lowercase();
        let mut ranked: Vec<(u8, String, &Package)> = self
            .packages
            .iter()
            .filter(|p| p.kind == kind)
            .filter(|p| category.is_empty() || in_category(p, category))
            .filter_map(|p| {
                let title = p.title.to_ascii_lowercase();
                let rank = if q.is_empty() || title == q {
                    0
                } else if title.starts_with(&q) {
                    1
                } else if title
                    .split(|c: char| !c.is_alphanumeric())
                    .any(|word| word.starts_with(&q))
                {
                    2
                } else if title.contains(&q) {
                    3
                } else if p.id.to_ascii_lowercase().contains(&q) {
                    4
                } else if p.summary.to_ascii_lowercase().contains(&q) {
                    5
                } else {
                    return None;
                };
                Some((rank, title, p))
            })
            .collect();
        ranked.sort_by(|a, b| (a.0, &a.1).cmp(&(b.0, &b.1)));
        Page {
            total: ranked.len(),
            rows: ranked
                .into_iter()
                .skip(offset)
                .take(limit)
                .map(|(_, _, p)| view_of(p))
                .collect(),
        }
    }

    pub fn library(&self, kind: Kind) -> Vec<PackageView> {
        let mut rows: Vec<PackageView> = self
            .packages
            .iter()
            .filter(|p| p.kind == kind && p.installed)
            .map(view_of)
            .collect();
        rows.sort_by_key(|row| row.title.to_ascii_lowercase());
        rows
    }

    pub fn updates(&self) -> Vec<PackageView> {
        let mut rows: Vec<PackageView> = self
            .packages
            .iter()
            .filter(|p| p.update)
            .map(view_of)
            .collect();
        rows.sort_by_key(|row| row.title.to_ascii_lowercase());
        rows
    }

    /// Attaches what the appstream index found (categories, newest release, verified publisher).
    pub fn apply_meta(&mut self, meta: &std::collections::HashMap<String, Meta>) {
        for package in &mut self.packages {
            if let Some(found) = meta.get(&package.id) {
                package.categories = found
                    .categories
                    .iter()
                    .take(8)
                    .map(|c| sanitize_for_display(c, 32))
                    .collect();
                package.released = found.released;
                package.verified = found.verified;
            }
        }
    }

    /// The Discover page: one hero and a shelf per category, built from the index. `day` rotates the hero daily; `has_icon` keeps apps
    /// that would show a lettered tile off the shelves.
    pub fn discover(
        &self,
        kind: Kind,
        day: u64,
        per_shelf: usize,
        has_icon: &dyn Fn(&Package) -> bool,
    ) -> Discover {
        let good = |p: &&Package| {
            p.kind == kind && !p.title.is_empty() && !p.summary.is_empty() && has_icon(p)
        };
        // Quality order: verified first, then the most recently released.
        let order =
            |a: &&Package, b: &&Package| (b.verified, b.released).cmp(&(a.verified, a.released));
        let mut all: Vec<&Package> = self.packages.iter().filter(good).collect();
        all.sort_by(order);
        let recent_cut = all
            .iter()
            .map(|p| p.released)
            .max()
            .unwrap_or(0)
            .saturating_sub(180 * 86_400);
        let heroes: Vec<&&Package> = all
            .iter()
            .filter(|p| p.verified && p.released >= recent_cut && p.released > 0)
            .take(30)
            .collect();
        let hero = if heroes.is_empty() {
            None
        } else {
            Some(view_of(heroes[(day as usize) % heroes.len()]))
        };
        let hero_id = hero.as_ref().map(|h| h.id.clone());
        let mut shelves = Vec::new();
        let mut recent: Vec<&Package> = all
            .iter()
            .copied()
            .filter(|p| p.released > 0 && Some(&p.id) != hero_id.as_ref())
            .collect();
        recent.sort_by_key(|p| std::cmp::Reverse(p.released));
        if !recent.is_empty() {
            shelves.push(Shelf {
                title: "Recently updated".into(),
                category: String::new(),
                rows: recent.into_iter().take(per_shelf).map(view_of).collect(),
            });
        }
        for (category, title) in CATEGORIES {
            let rows: Vec<PackageView> = all
                .iter()
                .copied()
                .filter(|p| in_category(p, category) && Some(&p.id) != hero_id.as_ref())
                .take(per_shelf)
                .map(view_of)
                .collect();
            if rows.len() >= 3 {
                shelves.push(Shelf {
                    title: title.into(),
                    category: category.into(),
                    rows,
                });
            }
        }
        Discover { hero, shelves }
    }

    pub fn is_empty(&self) -> bool {
        self.packages.is_empty()
    }

    pub fn detail(&self, id: &str) -> Option<PackageView> {
        self.packages.iter().find(|p| p.id == id).map(view_of)
    }

    /// Validates a mutation against the current catalog. The host runs the backend only after Ok.
    pub fn prepare(&self, id: &str, mutation: Mutation) -> Result<PackageView, SoftwareError> {
        if !valid_id(id) {
            return Err(SoftwareError::BadId);
        }
        let package = self
            .packages
            .iter()
            .find(|p| p.id == id)
            .ok_or(SoftwareError::UnknownPackage)?;
        match mutation {
            Mutation::Install if package.installed => return Err(SoftwareError::AlreadyInstalled),
            Mutation::Remove if !package.installed => return Err(SoftwareError::NotInstalled),
            Mutation::Update if !package.update => return Err(SoftwareError::NoUpdate),
            _ => {}
        }
        Ok(view_of(package))
    }

    /// Applies a successful mutation locally (after the host reports success).
    pub fn apply(&mut self, id: &str, mutation: Mutation) -> Result<(), SoftwareError> {
        let package = self
            .packages
            .iter_mut()
            .find(|p| p.id == id)
            .ok_or(SoftwareError::UnknownPackage)?;
        match mutation {
            Mutation::Install => {
                package.installed = true;
            }
            Mutation::Remove => {
                package.installed = false;
                package.update = false;
            }
            Mutation::Update => {
                package.update = false;
                package.installed = true;
            }
        }
        Ok(())
    }
}

fn view_of(package: &Package) -> PackageView {
    PackageView {
        id: package.id.clone(),
        title: package.title.clone(),
        summary: package.summary.clone(),
        kind: package.kind,
        installed: package.installed,
        update: package.update,
        remote: package.remote.clone(),
        version: package.version.clone(),
        size: package.size,
        installation: package.installation.clone(),
        verified: package.verified,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum JobState {
    Running,
    Done,
    Failed,
    Cancelled,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct JobView {
    pub id: u64,
    /// Package id, or `*` for "all updates".
    pub package_id: String,
    pub title: String,
    pub action: &'static str,
    pub state: JobState,
    pub percent: Option<u8>,
    pub message: String,
}

/// The queue of install, remove and update jobs. The host runs the backend; this records what is running and what just finished, so the
/// page can show progress, offer Cancel and say how it ended.
#[derive(Clone, Debug, Default)]
pub struct Jobs {
    next: u64,
    jobs: Vec<JobView>,
}

const KEEP_FINISHED: usize = 4;

impl Jobs {
    pub fn start(&mut self, package_id: &str, title: &str, mutation: Mutation) -> u64 {
        self.next += 1;
        let id = self.next;
        self.jobs.push(JobView {
            id,
            package_id: package_id.to_string(),
            title: sanitize_for_display(title, MAX_TITLE),
            action: match mutation {
                Mutation::Install => "install",
                Mutation::Remove => "remove",
                Mutation::Update => "update",
            },
            state: JobState::Running,
            percent: None,
            message: String::new(),
        });
        id
    }

    pub fn progress(&mut self, id: u64, percent: u8) {
        if let Some(job) = self
            .jobs
            .iter_mut()
            .find(|j| j.id == id && j.state == JobState::Running)
        {
            job.percent = Some(percent.min(100));
        }
    }

    pub fn finish(&mut self, id: u64, state: JobState, message: &str) {
        if let Some(job) = self.jobs.iter_mut().find(|j| j.id == id) {
            if job.state == JobState::Running {
                job.state = state;
                job.message = sanitize_for_display(message, 200);
                if state == JobState::Done {
                    job.percent = Some(100);
                }
            }
        }
        let finished = self
            .jobs
            .iter()
            .filter(|j| j.state != JobState::Running)
            .count();
        if finished > KEEP_FINISHED {
            let mut drop = finished - KEEP_FINISHED;
            self.jobs.retain(|j| {
                if j.state != JobState::Running && drop > 0 {
                    drop -= 1;
                    false
                } else {
                    true
                }
            });
        }
    }

    /// Forget a finished job (the person dismissed it).
    pub fn dismiss(&mut self, id: u64) {
        self.jobs
            .retain(|j| !(j.id == id && j.state != JobState::Running));
    }

    pub fn running(&self) -> bool {
        self.jobs.iter().any(|j| j.state == JobState::Running)
    }

    pub fn is_running_for(&self, package_id: &str) -> bool {
        self.jobs.iter().any(|j| {
            j.state == JobState::Running && (j.package_id == package_id || j.package_id == "*")
        })
    }

    pub fn view(&self) -> Vec<JobView> {
        self.jobs.clone()
    }
}

pub fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= MAX_ID
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-' || b == b'_')
}

#[cfg(test)]
mod tests {
    use super::*;

    fn store() -> Store {
        let mut s = Store::default();
        s.set(vec![
            Package {
                id: "org.gnome.Calculator".into(),
                title: "Calculator".into(),
                summary: "Do maths".into(),
                kind: Kind::Flatpak,
                installed: false,
                update: false,
                remote: "flathub".into(),
                ..Default::default()
            },
            Package {
                id: "org.gnome.TextEditor".into(),
                title: "Text Editor".into(),
                summary: "Edit text".into(),
                kind: Kind::Flatpak,
                installed: true,
                update: true,
                remote: "flathub".into(),
                ..Default::default()
            },
            Package {
                id: "firefox".into(),
                title: "Firefox".into(),
                summary: "Browser".into(),
                kind: Kind::Rpm,
                installed: true,
                update: false,
                remote: "fedora".into(),
                ..Default::default()
            },
        ]);
        s
    }

    #[test]
    fn explore_filters_by_kind_and_query() {
        let s = store();
        assert_eq!(s.explore(Kind::Flatpak, "").len(), 2);
        assert_eq!(s.explore(Kind::Flatpak, "calc").len(), 1);
        assert_eq!(s.explore(Kind::Rpm, "").len(), 1);
        assert!(s.explore(Kind::Firmware, "").is_empty());
    }

    #[test]
    fn library_and_updates() {
        let s = store();
        assert_eq!(s.library(Kind::Flatpak).len(), 1);
        assert_eq!(s.updates().len(), 1);
        assert_eq!(
            s.detail("org.gnome.Calculator").unwrap().title,
            "Calculator"
        );
    }

    #[test]
    fn mutations_are_refused_when_illegal_then_applied() {
        let mut s = store();
        assert_eq!(
            s.prepare("missing", Mutation::Install),
            Err(SoftwareError::UnknownPackage)
        );
        assert_eq!(
            s.prepare("../x", Mutation::Install),
            Err(SoftwareError::BadId)
        );
        assert_eq!(
            s.prepare("org.gnome.TextEditor", Mutation::Install),
            Err(SoftwareError::AlreadyInstalled)
        );
        assert_eq!(
            s.prepare("org.gnome.Calculator", Mutation::Remove),
            Err(SoftwareError::NotInstalled)
        );
        assert_eq!(
            s.prepare("org.gnome.Calculator", Mutation::Update),
            Err(SoftwareError::NoUpdate)
        );
        s.prepare("org.gnome.Calculator", Mutation::Install)
            .unwrap();
        s.apply("org.gnome.Calculator", Mutation::Install).unwrap();
        assert!(s.detail("org.gnome.Calculator").unwrap().installed);
        s.apply("org.gnome.TextEditor", Mutation::Update).unwrap();
        assert!(!s.detail("org.gnome.TextEditor").unwrap().update);
    }

    #[test]
    fn explore_pages_are_ranked_and_windowed() {
        let mut s = Store::default();
        let p = |id: &str, title: &str, summary: &str| Package {
            id: id.into(),
            title: title.into(),
            summary: summary.into(),
            ..Default::default()
        };
        s.set(vec![
            p("a.One", "Zebra Calc", ""),
            p("b.Two", "Calc", ""),
            p("c.Three", "Calculator", ""),
            p("d.Four", "Spreadsheet", "does calc things"),
            p("e.Five", "Notes", ""),
        ]);
        let page = s.explore_page(Kind::Flatpak, "calc", 0, 10);
        let titles: Vec<_> = page.rows.iter().map(|r| r.title.as_str()).collect();
        assert_eq!(
            titles,
            vec!["Calc", "Calculator", "Zebra Calc", "Spreadsheet"]
        );
        assert_eq!(page.total, 4);
        let window = s.explore_page(Kind::Flatpak, "calc", 1, 2);
        assert_eq!(window.rows.len(), 2);
        assert_eq!(window.total, 4);
        // No query: every package, by title.
        assert_eq!(
            s.explore_page(Kind::Flatpak, "", 0, 100)
                .rows
                .first()
                .unwrap()
                .title,
            "Calc"
        );
    }

    #[test]
    fn discover_builds_a_hero_and_shelves_from_the_index() {
        let mut s = Store::default();
        let mut meta = std::collections::HashMap::new();
        for i in 0..8u64 {
            let id = format!("org.t.App{i}");
            s.set(
                s.packages
                    .iter()
                    .cloned()
                    .chain([Package {
                        id: id.clone(),
                        title: format!("App {i}"),
                        summary: "something".into(),
                        ..Default::default()
                    }])
                    .collect(),
            );
            meta.insert(
                id,
                Meta {
                    categories: vec![if i % 2 == 0 {
                        "Game".into()
                    } else {
                        "Utility".into()
                    }],
                    released: 1_700_000_000 + i * 1000,
                    verified: i != 0,
                },
            );
        }
        s.apply_meta(&meta);
        let found = s.discover(Kind::Flatpak, 3, 12, &|_| true);
        let hero = found.hero.expect("a hero");
        assert!(hero.verified);
        let titles: Vec<_> = found
            .shelves
            .iter()
            .map(|shelf| shelf.title.as_str())
            .collect();
        assert_eq!(titles.first(), Some(&"Recently updated"));
        assert!(titles.contains(&"Games") && titles.contains(&"Utilities"));
        // The hero is not repeated on a shelf.
        assert!(found
            .shelves
            .iter()
            .all(|shelf| shelf.rows.iter().all(|r| r.id != hero.id)));
        // An app without an icon is kept off the shelves.
        let none = s.discover(Kind::Flatpak, 0, 12, &|_| false);
        assert!(none.hero.is_none() && none.shelves.is_empty());
        // Category pages use the same grouping.
        let games = s.explore_in(Kind::Flatpak, "", "Game", 0, 50);
        assert_eq!(games.total, 4);
    }

    #[test]
    fn jobs_track_progress_and_keep_only_recent_results() {
        let mut jobs = Jobs::default();
        let a = jobs.start("org.x.A", "A", Mutation::Install);
        assert!(jobs.running());
        assert!(jobs.is_running_for("org.x.A") && !jobs.is_running_for("org.x.B"));
        jobs.progress(a, 40);
        assert_eq!(jobs.view()[0].percent, Some(40));
        jobs.finish(a, JobState::Done, "");
        assert!(!jobs.running());
        assert_eq!(jobs.view()[0].percent, Some(100));
        // A finished job cannot be revived by late progress.
        jobs.progress(a, 10);
        assert_eq!(jobs.view()[0].percent, Some(100));
        for i in 0..8 {
            let id = jobs.start(&format!("org.x.P{i}"), "P", Mutation::Update);
            jobs.finish(id, JobState::Failed, "no network");
        }
        assert_eq!(jobs.view().len(), 4);
        let last = jobs.view().last().unwrap().id;
        jobs.dismiss(last);
        assert_eq!(jobs.view().len(), 3);
    }

    #[test]
    fn odd_ids_are_dropped_from_the_catalog() {
        let mut s = Store::default();
        s.set(vec![Package {
            id: "bad id".into(),
            title: "X".into(),
            summary: "".into(),
            kind: Kind::Flatpak,
            installed: false,
            update: false,
            remote: "".into(),
            ..Default::default()
        }]);
        assert!(s.explore(Kind::Flatpak, "").is_empty());
    }
}
