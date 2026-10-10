//! Pure package-store models for Helix Software. The host supplies catalog rows from Flatpak /
//! PackageKit / fwupd; this module filters, sorts, and validates mutations. Nothing here runs a
//! package command.

use serde::Serialize;

use crate::display::sanitize_for_display;

const MAX_ID: usize = 200;
const MAX_TITLE: usize = 120;
const MAX_QUERY: usize = 120;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    Flatpak,
    Rpm,
    Firmware,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Package {
    pub id: String,
    pub title: String,
    pub summary: String,
    pub kind: Kind,
    pub installed: bool,
    pub update: bool,
    pub remote: String,
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

    pub fn library(&self, kind: Kind) -> Vec<PackageView> {
        self.packages
            .iter()
            .filter(|p| p.kind == kind && p.installed)
            .map(view_of)
            .collect()
    }

    pub fn updates(&self) -> Vec<PackageView> {
        self.packages.iter().filter(|p| p.update).map(view_of).collect()
    }

    pub fn detail(&self, id: &str) -> Option<PackageView> {
        self.packages.iter().find(|p| p.id == id).map(view_of)
    }

    /// Validates a mutation against the current catalog. The host runs the backend only after Ok.
    pub fn prepare(&self, id: &str, mutation: Mutation) -> Result<PackageView, SoftwareError> {
        if !valid_id(id) {
            return Err(SoftwareError::BadId);
        }
        let package = self.packages.iter().find(|p| p.id == id).ok_or(SoftwareError::UnknownPackage)?;
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
        let package = self.packages.iter_mut().find(|p| p.id == id).ok_or(SoftwareError::UnknownPackage)?;
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
    }
}

pub fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= MAX_ID
        && id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-' || b == b'_')
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
            },
            Package {
                id: "org.gnome.TextEditor".into(),
                title: "Text Editor".into(),
                summary: "Edit text".into(),
                kind: Kind::Flatpak,
                installed: true,
                update: true,
                remote: "flathub".into(),
            },
            Package {
                id: "firefox".into(),
                title: "Firefox".into(),
                summary: "Browser".into(),
                kind: Kind::Rpm,
                installed: true,
                update: false,
                remote: "fedora".into(),
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
        assert_eq!(s.detail("org.gnome.Calculator").unwrap().title, "Calculator");
    }

    #[test]
    fn mutations_are_refused_when_illegal_then_applied() {
        let mut s = store();
        assert_eq!(s.prepare("missing", Mutation::Install), Err(SoftwareError::UnknownPackage));
        assert_eq!(s.prepare("../x", Mutation::Install), Err(SoftwareError::BadId));
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
        s.prepare("org.gnome.Calculator", Mutation::Install).unwrap();
        s.apply("org.gnome.Calculator", Mutation::Install).unwrap();
        assert!(s.detail("org.gnome.Calculator").unwrap().installed);
        s.apply("org.gnome.TextEditor", Mutation::Update).unwrap();
        assert!(!s.detail("org.gnome.TextEditor").unwrap().update);
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
        }]);
        assert!(s.explore(Kind::Flatpak, "").is_empty());
    }
}
