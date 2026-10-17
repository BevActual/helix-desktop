//! Fedora (RPM) applications. What there is to install comes from the AppStream data the distribution ships on disk (desktop applications with
//! a package name); what is installed, what has an update and every change go through PackageKit on the system bus, so authentication,
//! locking and the package manager's own rules stay where they belong. Nothing here runs `dnf` or `rpm`.

use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use helix_software_model::software::{valid_id, Kind, Package};
use zbus::blocking::{Connection, MessageIterator, Proxy};
use zbus::message::Type as MessageType;
use zbus::zvariant::OwnedObjectPath;
use zbus::MatchRule;

use crate::catalog::{element, split_components};

const PK: &str = "org.freedesktop.PackageKit";
const PK_PATH: &str = "/org/freedesktop/PackageKit";
const PK_TRANSACTION: &str = "org.freedesktop.PackageKit.Transaction";

/// PackageKit's filter bits (`PkFilterEnum` as a bitfield): the installed packages, and the ones that are not.
const FILTER_INSTALLED: u64 = 1 << 2;
const FILTER_NOT_INSTALLED: u64 = 1 << 3;
/// `Finished` exit codes.
const EXIT_SUCCESS: u32 = 1;
const EXIT_CANCELLED: u32 = 3;

/// The row id of the one entry that stands for every package update that is not an application.
pub const SYSTEM_UPDATES: &str = "system-updates";

// ---- the catalog: AppStream data on disk ----

/// A desktop application of the distribution's catalog.
#[derive(Clone, Debug, PartialEq)]
pub struct RpmApp {
    pub package: Package,
    /// `org.gnome.Calculator.desktop`, to start it.
    pub desktop_id: String,
    /// The cached icon's file name, under the catalog's icon directory for its origin.
    pub icon: String,
    pub origin: String,
}

/// Where AppStream catalogs live (`XDG` swcatalog, the cache PackageKit refreshes, and the older app-info path). A test points
/// `HELIX_SOFTWARE_RPM_CATALOG` at its own directory instead.
pub fn catalog_dirs() -> Vec<PathBuf> {
    if let Some(dir) = std::env::var_os("HELIX_SOFTWARE_RPM_CATALOG") {
        return vec![PathBuf::from(dir)];
    }
    [
        "/usr/share/swcatalog",
        "/var/cache/swcatalog",
        "/usr/share/app-info",
    ]
    .iter()
    .map(PathBuf::from)
    .collect()
}

/// Parses one catalog document (`<components origin="fedora">`, one element per line). Only desktop applications that name a package are kept,
/// one row per package.
pub fn parse_catalog(reader: impl BufRead, fallback_origin: &str) -> Vec<RpmApp> {
    let mut out: Vec<RpmApp> = Vec::new();
    let mut origin = fallback_origin.to_string();
    let mut current: Option<RpmApp> = None;
    let mut keep = false;
    let mut in_name_tags = false;
    for raw in reader.lines().map_while(Result::ok) {
        for line in split_components(&raw) {
            let line = line.trim();
            if line.starts_with("<components") {
                if let Some(at) = line.find("origin=\"") {
                    if let Some(value) = line[at + 8..].split('"').next().filter(|v| !v.is_empty())
                    {
                        origin = value.to_string();
                    }
                }
            } else if line.starts_with("<component") && !line.starts_with("<components") {
                keep = line.contains("type=\"desktop-application\"")
                    || line.contains("type=\"desktop\"");
                in_name_tags = false;
                current = Some(RpmApp {
                    package: Package {
                        kind: Kind::Rpm,
                        remote: origin.clone(),
                        ..Default::default()
                    },
                    desktop_id: String::new(),
                    icon: String::new(),
                    origin: origin.clone(),
                });
            } else if line.starts_with("</component>") {
                if let Some(app) = current.take() {
                    let package = &app.package;
                    if keep
                        && !package.id.is_empty()
                        && valid_id(&package.id)
                        && !package.title.is_empty()
                        && !out.iter().any(|a| a.package.id == package.id)
                    {
                        out.push(app);
                    }
                }
                keep = false;
            } else if let Some(app) = current.as_mut() {
                // Nested blocks (`<developer><name>`, `<provides>`, `<screenshots>`) have their own `<name>` or `<id>`: only the component's own count.
                const NESTED: [&str; 4] = ["developer", "provides", "releases", "screenshots"];
                if let Some(tag) = NESTED.iter().find(|t| line.starts_with(&format!("<{t}"))) {
                    in_name_tags = !line.contains(&format!("</{tag}>"));
                    if *tag != "releases" || line.contains("</releases>") {
                        continue;
                    }
                }
                if NESTED.iter().any(|t| line.starts_with(&format!("</{t}"))) {
                    in_name_tags = false;
                    continue;
                }
                if in_name_tags && !line.starts_with("<release ") {
                    continue;
                }
                if line.starts_with("<id>") && app.desktop_id.is_empty() {
                    app.desktop_id = element(line, "id").unwrap_or_default();
                } else if line.starts_with("<pkgname>") && app.package.id.is_empty() {
                    app.package.id = element(line, "pkgname").unwrap_or_default();
                } else if line.starts_with("<name>") && app.package.title.is_empty() {
                    app.package.title = element(line, "name").unwrap_or_default();
                } else if line.starts_with("<summary>") && app.package.summary.is_empty() {
                    app.package.summary = element(line, "summary").unwrap_or_default();
                } else if line.starts_with("<icon type=\"cached\"") && app.icon.is_empty() {
                    app.icon = element(line, "icon").unwrap_or_default();
                } else if line.starts_with("<launchable type=\"desktop-id\"") {
                    if let Some(id) = element(line, "launchable") {
                        app.desktop_id = id;
                    }
                } else if line.starts_with("<category>") {
                    if let Some(category) = element(line, "category") {
                        if app.package.categories.len() < 8 {
                            app.package.categories.push(category);
                        }
                    }
                } else if line.starts_with("<release ") {
                    if let Some(at) = line.find("timestamp=\"") {
                        if let Some(value) = line[at + 11..]
                            .split('"')
                            .next()
                            .and_then(|t| t.parse::<u64>().ok())
                        {
                            app.package.released = app.package.released.max(value);
                        }
                    }
                }
            }
        }
    }
    out
}

/// Every catalog file under the catalog directories (`xml/*.xml` and `xml/*.xml.gz`), as `(path, origin hint)`.
fn catalog_files() -> Vec<(PathBuf, String)> {
    let mut files = Vec::new();
    for base in catalog_dirs() {
        for sub in ["xml", "xmls", ""] {
            let dir = if sub.is_empty() {
                base.clone()
            } else {
                base.join(sub)
            };
            let Ok(entries) = std::fs::read_dir(&dir) else {
                continue;
            };
            for entry in entries.flatten() {
                let path = entry.path();
                let name = path
                    .file_name()
                    .and_then(|n| n.to_str())
                    .unwrap_or_default()
                    .to_string();
                if name.ends_with(".xml") || name.ends_with(".xml.gz") {
                    let origin = name
                        .trim_end_matches(".gz")
                        .trim_end_matches(".xml")
                        .to_string();
                    files.push((path, origin));
                }
            }
        }
    }
    files.sort();
    files.dedup();
    files
}

/// Opens a catalog file for reading lines: plain, or through `zcat` for `.gz`.
fn open_catalog(path: &Path) -> Option<Box<dyn BufRead>> {
    if path.extension().and_then(|e| e.to_str()) == Some("gz") {
        let mut child = Command::new("zcat")
            .arg(path)
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .ok()?;
        let stdout = child.stdout.take()?;
        // The decompressor ends when its output is read to the end; it is not waited on separately.
        std::thread::spawn(move || {
            let _ = child.wait();
        });
        Some(Box::new(BufReader::new(stdout)))
    } else {
        Some(Box::new(BufReader::new(std::fs::File::open(path).ok()?)))
    }
}

/// The distribution's applications, from every catalog on disk.
pub fn read_catalog() -> Vec<RpmApp> {
    let mut all: Vec<RpmApp> = Vec::new();
    for (path, origin) in catalog_files() {
        let Some(reader) = open_catalog(&path) else {
            continue;
        };
        for app in parse_catalog(reader, &origin) {
            if !all.iter().any(|a| a.package.id == app.package.id) {
                all.push(app);
            }
        }
    }
    all
}

/// Candidate icon files, best first.
pub fn icon_paths(app: &RpmApp) -> Vec<PathBuf> {
    if app.icon.is_empty()
        || app.icon.contains('/')
        || app.icon.contains("..")
        || app.origin.contains('/')
        || app.origin.contains("..")
    {
        return Vec::new();
    }
    let mut out = Vec::new();
    for base in catalog_dirs() {
        for size in ["128x128", "64x64"] {
            out.push(
                base.join("icons")
                    .join(&app.origin)
                    .join(size)
                    .join(&app.icon),
            );
        }
    }
    out
}

/// The `<component>` block of an application, for its long description (read on demand; the file is large).
pub fn find_block(app: &RpmApp) -> Option<String> {
    for (path, _) in catalog_files() {
        let Some(reader) = open_catalog(&path) else {
            continue;
        };
        let (mut block, mut inside) = (String::new(), false);
        for raw in reader.lines().map_while(Result::ok) {
            for line in split_components(&raw) {
                let trimmed = line.trim();
                if trimmed.starts_with("<component") && !trimmed.starts_with("<components") {
                    inside = true;
                    block.clear();
                }
                if inside {
                    block.push_str(trimmed);
                    block.push('\n');
                    if trimmed.starts_with("</component>") {
                        inside = false;
                        if block.contains(&format!("<pkgname>{}</pkgname>", app.package.id)) {
                            return Some(std::mem::take(&mut block));
                        }
                    }
                }
            }
        }
    }
    None
}

// ---- PackageKit ----

/// One package as PackageKit reports it: `name;version;arch;data`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PkPackage {
    pub id: String,
    pub name: String,
    pub version: String,
    pub arch: String,
    pub data: String,
    pub summary: String,
}

pub fn parse_package_id(id: &str, summary: &str) -> Option<PkPackage> {
    let mut parts = id.splitn(4, ';');
    let (name, version, arch, data) = (
        parts.next()?,
        parts.next()?,
        parts.next()?,
        parts.next().unwrap_or(""),
    );
    if name.is_empty() {
        return None;
    }
    Some(PkPackage {
        id: id.into(),
        name: name.into(),
        version: version.into(),
        arch: arch.into(),
        data: data.into(),
        summary: summary.into(),
    })
}

/// What a person asked for, resolved by the caller to package names.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Op {
    Install(String),
    Remove(String),
    /// One package, or every update that is not an application (`None`).
    Update(Option<Vec<String>>),
}

pub struct PackageKit {
    connection: Connection,
}

fn native_arch() -> &'static str {
    std::env::consts::ARCH
}

fn arch_fits(arch: &str) -> bool {
    arch == native_arch() || arch == "noarch"
}

impl PackageKit {
    /// The system bus connection; PackageKit starts on demand.
    pub fn connect() -> Result<Self, String> {
        Ok(Self {
            connection: Connection::system().map_err(|e| format!("No system bus: {e}"))?,
        })
    }

    /// One transaction: `start` calls the method on its object; packages and progress are gathered until `Finished`.
    fn transact(
        &self,
        start: impl FnOnce(&Proxy<'_>) -> zbus::Result<()>,
        cancel: Option<&Arc<AtomicBool>>,
        mut progress: impl FnMut(u8),
    ) -> Result<Vec<PkPackage>, String> {
        let root = Proxy::new(&self.connection, PK, PK_PATH, "org.freedesktop.PackageKit")
            .map_err(|e| e.to_string())?;
        let path: OwnedObjectPath = root
            .call("CreateTransaction", &())
            .map_err(|e| format!("PackageKit is not available: {e}"))?;
        let rule = MatchRule::builder()
            .msg_type(MessageType::Signal)
            .sender(PK)
            .map_err(|e| e.to_string())?
            .path(path.as_str())
            .map_err(|e| e.to_string())?
            .build();
        // Subscribed before the call, so a fast answer is never missed.
        let signals = MessageIterator::for_match_rule(rule, &self.connection, Some(256))
            .map_err(|e| e.to_string())?;
        let proxy = Proxy::new(&self.connection, PK, path.as_str(), PK_TRANSACTION)
            .map_err(|e| e.to_string())?;
        let done = Arc::new(AtomicBool::new(false));
        if let Some(flag) = cancel {
            let (flag, done, connection, path) = (
                flag.clone(),
                done.clone(),
                self.connection.clone(),
                path.clone(),
            );
            std::thread::spawn(move || {
                while !done.load(Ordering::SeqCst) {
                    if flag.load(Ordering::SeqCst) {
                        if let Ok(p) = Proxy::new(&connection, PK, path.as_str(), PK_TRANSACTION) {
                            let _: zbus::Result<()> = p.call("Cancel", &());
                        }
                        return;
                    }
                    std::thread::sleep(Duration::from_millis(100));
                }
            });
        }
        let started = start(&proxy);
        if let Err(error) = started {
            done.store(true, Ordering::SeqCst);
            return Err(clean_error(&error.to_string()));
        }
        let (mut packages, mut error, mut exit) = (Vec::new(), String::new(), None);
        for message in signals.flatten() {
            let header = message.header();
            let member = header
                .member()
                .map(|m| m.as_str().to_string())
                .unwrap_or_default();
            let body = message.body();
            match member.as_str() {
                "Package" => {
                    if let Ok((_, id, summary)) = body.deserialize::<(u32, String, String)>() {
                        if let Some(package) = parse_package_id(&id, &summary) {
                            packages.push(package);
                        }
                    }
                }
                "ItemProgress" => {
                    if let Ok((_, _, percent)) = body.deserialize::<(String, u32, u32)>() {
                        if percent <= 100 {
                            progress(percent as u8);
                        }
                    }
                }
                "PropertiesChanged" => {
                    if let Ok((_, changed, _)) = body.deserialize::<(
                        String,
                        HashMap<String, zbus::zvariant::OwnedValue>,
                        Vec<String>,
                    )>() {
                        if let Some(percent) = changed
                            .get("Percentage")
                            .and_then(|v| u32::try_from(v.clone()).ok())
                            .filter(|p| *p <= 100)
                        {
                            progress(percent as u8);
                        }
                    }
                }
                "ErrorCode" => {
                    if let Ok((_, details)) = body.deserialize::<(u32, String)>() {
                        error = details;
                    }
                }
                "Finished" => {
                    if let Ok((code, _)) = body.deserialize::<(u32, u32)>() {
                        exit = Some(code);
                    }
                    break;
                }
                _ => {}
            }
        }
        done.store(true, Ordering::SeqCst);
        match exit {
            Some(EXIT_SUCCESS) => Ok(packages),
            Some(EXIT_CANCELLED) => Err("Cancelled.".into()),
            Some(_) if !error.is_empty() => Err(clean_error(&error)),
            Some(_) => Err("PackageKit could not do that.".into()),
            None => Err("PackageKit stopped answering.".into()),
        }
    }

    pub fn installed(&self) -> Result<Vec<PkPackage>, String> {
        self.transact(
            |p| p.call("GetPackages", &(FILTER_INSTALLED,)),
            None,
            |_| {},
        )
    }

    pub fn updates(&self) -> Result<Vec<PkPackage>, String> {
        self.transact(|p| p.call("GetUpdates", &(0u64,)), None, |_| {})
    }

    /// Asks the package manager to fetch fresh repository metadata (needs the network).
    pub fn refresh(&self, cancel: Option<&Arc<AtomicBool>>) -> Result<(), String> {
        self.transact(|p| p.call("RefreshCache", &(false,)), cancel, |_| {})
            .map(|_| ())
    }

    fn resolve(&self, filter: u64, name: &str) -> Result<Vec<PkPackage>, String> {
        let names = vec![name.to_string()];
        let found = self.transact(|p| p.call("Resolve", &(filter, names)), None, |_| {})?;
        Ok(found.into_iter().filter(|p| arch_fits(&p.arch)).collect())
    }

    /// Runs a change and reports its progress. Returns when PackageKit has finished.
    pub fn run(
        &self,
        op: &Op,
        cancel: &Arc<AtomicBool>,
        mut progress: impl FnMut(u8),
    ) -> Result<(), String> {
        // Only trusted (signed) packages, and nothing is simulated: the page already asked.
        let flags: u64 = 1 << 1;
        match op {
            Op::Install(name) => {
                let ids: Vec<String> = self
                    .resolve(FILTER_NOT_INSTALLED, name)?
                    .into_iter()
                    .take(1)
                    .map(|p| p.id)
                    .collect();
                if ids.is_empty() {
                    return Err("That package is not available in the enabled repositories.".into());
                }
                self.transact(
                    |p| p.call("InstallPackages", &(flags, ids)),
                    Some(cancel),
                    &mut progress,
                )
                .map(|_| ())
            }
            Op::Remove(name) => {
                let ids: Vec<String> = self
                    .resolve(FILTER_INSTALLED, name)?
                    .into_iter()
                    .map(|p| p.id)
                    .collect();
                if ids.is_empty() {
                    return Err("That package is not installed.".into());
                }
                // Never takes other packages with it: if something needs it, PackageKit says so and nothing is removed.
                self.transact(
                    |p| p.call("RemovePackages", &(0u64, ids, false, false)),
                    Some(cancel),
                    &mut progress,
                )
                .map(|_| ())
            }
            Op::Update(names) => {
                let wanted = self.updates()?;
                let ids: Vec<String> = wanted
                    .into_iter()
                    .filter(|p| names.as_ref().is_none_or(|n| n.contains(&p.name)))
                    .map(|p| p.id)
                    .collect();
                if ids.is_empty() {
                    return Err("There is nothing to update.".into());
                }
                self.transact(
                    |p| p.call("UpdatePackages", &(flags, ids)),
                    Some(cancel),
                    &mut progress,
                )
                .map(|_| ())
            }
        }
    }
}

/// D-Bus error text without the transport prefix, as one line.
fn clean_error(text: &str) -> String {
    let text = text.rsplit(": ").next().unwrap_or(text);
    text.lines().next().unwrap_or("").trim().to_string()
}

// ---- the catalog's state ----

/// What the catalog shows: every application with whether it is installed and has an update, plus one row for the other package updates.
pub struct RpmCatalog {
    pub packages: Vec<Package>,
    pub apps: HashMap<String, RpmApp>,
}

pub fn build(apps: Vec<RpmApp>, installed: &[PkPackage], updates: &[PkPackage]) -> RpmCatalog {
    let installed_by_name: HashMap<&str, &PkPackage> = installed
        .iter()
        .filter(|p| arch_fits(&p.arch))
        .map(|p| (p.name.as_str(), p))
        .collect();
    let update_names: HashMap<&str, &PkPackage> = updates
        .iter()
        .filter(|p| arch_fits(&p.arch))
        .map(|p| (p.name.as_str(), p))
        .collect();
    let mut packages = Vec::new();
    let mut by_id = HashMap::new();
    let mut app_names = std::collections::HashSet::new();
    for app in apps {
        let mut package = app.package.clone();
        app_names.insert(package.id.clone());
        if let Some(found) = installed_by_name.get(package.id.as_str()) {
            package.installed = true;
            package.version = found.version.clone();
        }
        if let Some(found) = update_names.get(package.id.as_str()) {
            package.update = package.installed;
            if package.update {
                package.version = format!("{} to {}", package.version, found.version);
            }
        }
        by_id.insert(package.id.clone(), app);
        packages.push(package);
    }
    let other: Vec<&&PkPackage> = update_names
        .values()
        .filter(|p| !app_names.contains(&p.name))
        .collect();
    if !other.is_empty() {
        packages.push(Package {
            id: SYSTEM_UPDATES.into(),
            title: "System software".into(),
            summary: format!(
                "{} package update{}; some take effect after a restart",
                other.len(),
                if other.len() == 1 { "" } else { "s" }
            ),
            kind: Kind::Rpm,
            // Not "installed": it is an update row, not something in the library.
            installed: false,
            update: true,
            update_only: true,
            remote: "Fedora".into(),
            ..Default::default()
        });
    }
    RpmCatalog {
        packages,
        apps: by_id,
    }
}

/// The names behind the System software row: every update that is not an application of the catalog.
pub fn system_update_names(updates: &[PkPackage], apps: &HashMap<String, RpmApp>) -> Vec<String> {
    updates
        .iter()
        .filter(|p| arch_fits(&p.arch) && !apps.contains_key(&p.name))
        .map(|p| p.name.clone())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    const CATALOG: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<components origin="fedora" version="0.8">
<component type="desktop">
<id>org.gnome.Calculator.desktop</id>
<pkgname>gnome-calculator</pkgname>
<name>Calculator</name>
<name xml:lang="de">Taschenrechner</name>
<summary>Perform arithmetic</summary>
<icon type="cached" height="64" width="64">gnome-calculator.png</icon>
<categories>
<category>Utility</category>
<category>Calculator</category>
</categories>
<releases>
<release version="49.1" timestamp="1700000000"/>
</releases>
</component>
<component type="desktop-application">
<id>gimp.desktop</id>
<pkgname>gimp</pkgname>
<name>GIMP</name>
<summary>Image editor</summary>
<developer><name>The GIMP team</name></developer>
</component>
<component type="generic">
<id>rpmfusion-free-release</id>
<pkgname>rpmfusion-free-release</pkgname>
<name>RPM Fusion Free</name>
</component>
<component type="desktop">
<id>no-package.desktop</id>
<name>No package</name>
</component>
<component type="desktop">
<id>bad.desktop</id>
<pkgname>bad name!</pkgname>
<name>Bad</name>
</component>
<component type="desktop">
<id>dup.desktop</id>
<pkgname>gimp</pkgname>
<name>GIMP again</name>
</component>
</components>
"#;

    #[test]
    fn the_catalog_keeps_desktop_applications_with_a_package_name() {
        let apps = parse_catalog(CATALOG.as_bytes(), "x");
        let ids: Vec<&str> = apps.iter().map(|a| a.package.id.as_str()).collect();
        assert_eq!(ids, ["gnome-calculator", "gimp"], "generic components, rows without a package and odd names are dropped, one row per package");
        let calc = &apps[0];
        assert_eq!(
            calc.package.title, "Calculator",
            "the first, untranslated name"
        );
        assert_eq!(calc.package.summary, "Perform arithmetic");
        assert_eq!(calc.desktop_id, "org.gnome.Calculator.desktop");
        assert_eq!(calc.icon, "gnome-calculator.png");
        assert_eq!(calc.origin, "fedora");
        assert_eq!(calc.package.categories, ["Utility", "Calculator"]);
        assert_eq!(calc.package.released, 1_700_000_000);
        assert_eq!(calc.package.kind, Kind::Rpm);
        assert_eq!(
            apps[1].package.title, "GIMP",
            "a developer's name is not the application's"
        );
    }

    #[test]
    fn package_ids_split_into_their_parts() {
        let p = parse_package_id("bash;5.3.9-3.fc44;x86_64;installed:abc", "The shell").unwrap();
        assert_eq!(
            (
                p.name.as_str(),
                p.version.as_str(),
                p.arch.as_str(),
                p.data.as_str()
            ),
            ("bash", "5.3.9-3.fc44", "x86_64", "installed:abc")
        );
        assert!(parse_package_id(";1;x86_64;", "").is_none());
        assert!(parse_package_id("garbage", "").is_none());
    }

    fn pk(name: &str, version: &str, arch: &str) -> PkPackage {
        parse_package_id(&format!("{name};{version};{arch};fedora"), "").unwrap()
    }

    #[test]
    fn installed_and_updatable_state_is_merged_and_other_updates_become_one_row() {
        let apps = parse_catalog(CATALOG.as_bytes(), "x");
        let installed = [
            pk("gimp", "3.0-1", "x86_64"),
            pk("gimp", "3.0-1", "i686"),
            pk("bash", "5.3-1", "x86_64"),
        ];
        let updates = [
            pk("gimp", "3.0-2", "x86_64"),
            pk("kernel", "7.2.11", "x86_64"),
            pk("glibc", "2.43", "x86_64"),
            pk("openssl", "3.5", "i686"),
        ];
        let catalog = build(apps, &installed, &updates);
        let gimp = catalog.packages.iter().find(|p| p.id == "gimp").unwrap();
        assert!(gimp.installed && gimp.update);
        assert_eq!(gimp.version, "3.0-1 to 3.0-2");
        let calc = catalog
            .packages
            .iter()
            .find(|p| p.id == "gnome-calculator")
            .unwrap();
        assert!(!calc.installed && !calc.update);
        let system = catalog
            .packages
            .iter()
            .find(|p| p.id == SYSTEM_UPDATES)
            .expect("one row for the rest");
        assert!(
            system.update && system.summary.starts_with("2 package updates"),
            "{}",
            system.summary
        );
        assert_eq!(
            system_update_names(&updates, &catalog.apps),
            ["kernel", "glibc"],
            "other architectures are left out"
        );
    }

    #[test]
    fn an_update_for_an_application_that_is_not_installed_is_not_offered() {
        let apps = parse_catalog(CATALOG.as_bytes(), "x");
        let catalog = build(apps, &[], &[pk("gimp", "3.0-2", "x86_64")]);
        let gimp = catalog.packages.iter().find(|p| p.id == "gimp").unwrap();
        assert!(!gimp.update);
    }

    #[test]
    fn icon_paths_never_leave_the_catalog_directory() {
        let mut app = parse_catalog(CATALOG.as_bytes(), "x").remove(0);
        assert!(icon_paths(&app)
            .iter()
            .all(|p| p.starts_with("/usr/share/swcatalog")
                || p.starts_with("/var/cache/swcatalog")
                || p.starts_with("/usr/share/app-info")));
        app.icon = "../../etc/passwd".into();
        assert!(icon_paths(&app).is_empty());
        app.icon = "ok.png".into();
        app.origin = "../x".into();
        assert!(icon_paths(&app).is_empty());
    }

    #[test]
    fn errors_lose_the_transport_prefix() {
        assert_eq!(
            clean_error("org.freedesktop.DBus.Error.Failed: Package is needed by gimp\nmore"),
            "Package is needed by gimp"
        );
    }

    /// Reads this machine's real catalog and PackageKit (nothing is changed): `cargo test -p helix-software -- --ignored --nocapture real_`.
    #[test]
    #[ignore = "needs PackageKit on the system bus and the distribution's AppStream data"]
    fn real_catalog_and_packagekit_agree() {
        let apps = read_catalog();
        let packagekit = PackageKit::connect().expect("the system bus");
        let installed = packagekit.installed().expect("installed packages");
        let updates = packagekit.updates().expect("updates");
        let catalog = build(apps, &installed, &updates);
        let apps_installed = catalog
            .packages
            .iter()
            .filter(|p| p.installed && !p.update_only)
            .count();
        println!(
            "{} applications in the catalog, {} installed of {} packages, {} with an update, {} updates in all",
            catalog.apps.len(),
            apps_installed,
            installed.len(),
            catalog.packages.iter().filter(|p| p.update && !p.update_only).count(),
            updates.len()
        );
        assert!(!installed.is_empty(), "PackageKit lists installed packages");
        assert!(installed.iter().any(|p| p.name == "bash"));
    }
}
