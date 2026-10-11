//! Reading what Flatpak and the appstream data say. Everything here parses text into plain values; running the commands is the host's job,
//! so the parsers are tested against captured output.

use std::collections::HashMap;
use std::path::PathBuf;

use helix_shell_core::software::{valid_id, Kind, Package};

/// Flatpak prints sizes like `88.2 MB`, `1.0 GB`, `512 bytes` (decimal units).
pub fn parse_size(text: &str) -> u64 {
    let text = text.trim();
    let mut parts = text.split_whitespace();
    let Some(number) = parts.next().and_then(|n| n.replace(',', ".").parse::<f64>().ok()) else {
        return 0;
    };
    let factor = match parts.next().unwrap_or("bytes").to_ascii_lowercase().as_str() {
        "kb" | "kib" => 1e3,
        "mb" | "mib" => 1e6,
        "gb" | "gib" => 1e9,
        "tb" | "tib" => 1e12,
        _ => 1.0,
    };
    (number * factor) as u64
}

/// `flatpak remote-ls --app --columns=application,name,description,version,origin,download-size`
pub fn parse_remote_ls(text: &str) -> Vec<Package> {
    text.lines()
        .filter_map(|line| {
            let mut fields = line.split('\t');
            let id = fields.next()?.trim();
            if !valid_id(id) {
                return None;
            }
            let title = fields.next().unwrap_or("").trim();
            let summary = fields.next().unwrap_or("").trim();
            let version = fields.next().unwrap_or("").trim();
            let remote = fields.next().unwrap_or("").trim();
            let size = parse_size(fields.next().unwrap_or(""));
            Some(Package {
                id: id.to_string(),
                title: if title.is_empty() { id.to_string() } else { title.to_string() },
                summary: summary.to_string(),
                kind: Kind::Flatpak,
                installed: false,
                update: false,
                remote: remote.to_string(),
                version: version.to_string(),
                size,
                installation: String::new(),
            })
        })
        .collect()
}

/// An installed application: id, title, version, origin, installation, installed size.
#[derive(Debug, PartialEq)]
pub struct Installed {
    pub id: String,
    pub title: String,
    pub version: String,
    pub remote: String,
    pub installation: String,
    pub size: u64,
}

/// `flatpak list --app --columns=application,name,version,origin,installation,size`
pub fn parse_list(text: &str) -> Vec<Installed> {
    text.lines()
        .filter_map(|line| {
            let mut fields = line.split('\t');
            let id = fields.next()?.trim();
            if !valid_id(id) {
                return None;
            }
            Some(Installed {
                id: id.to_string(),
                title: fields.next().unwrap_or("").trim().to_string(),
                version: fields.next().unwrap_or("").trim().to_string(),
                remote: fields.next().unwrap_or("").trim().to_string(),
                installation: fields.next().unwrap_or("").trim().to_string(),
                size: parse_size(fields.next().unwrap_or("")),
            })
        })
        .collect()
}

/// `flatpak remote-ls --updates --app --columns=application`
pub fn parse_updates(text: &str) -> Vec<String> {
    text.lines().map(str::trim).filter(|id| valid_id(id)).map(str::to_string).collect()
}

#[derive(Debug, PartialEq, Clone, serde::Serialize)]
pub struct Remote {
    pub name: String,
    pub title: String,
    pub url: String,
    pub installation: String,
    pub enabled: bool,
}

/// `flatpak remotes --columns=name,title,url,options`; options is a comma list that includes `user` or `system`.
pub fn parse_remotes(text: &str) -> Vec<Remote> {
    text.lines()
        .filter_map(|line| {
            let mut fields = line.split('\t');
            let name = fields.next()?.trim();
            if name.is_empty() {
                return None;
            }
            let title = fields.next().unwrap_or("").trim();
            let url = fields.next().unwrap_or("").trim();
            let options = fields.next().unwrap_or("");
            let installation = if options.split(',').any(|o| o.trim() == "user") { "user" } else { "system" };
            Some(Remote {
                name: name.to_string(),
                title: if title.is_empty() { name.to_string() } else { title.to_string() },
                url: url.to_string(),
                installation: installation.to_string(),
                enabled: !options.split(',').any(|o| o.trim() == "disabled"),
            })
        })
        .collect()
}

/// Merges the remote catalog with what is installed and what has updates. One row per application id: a user installation of a remote
/// wins over the system one (installing there needs no authentication).
pub fn merge(remote: Vec<Package>, installed: Vec<Installed>, updates: &[String], user_remotes: &[String]) -> Vec<Package> {
    let mut by_id: HashMap<String, Package> = HashMap::new();
    for package in remote {
        let prefers = |candidate: &Package, current: &Package| {
            let user = |p: &Package| user_remotes.contains(&p.remote);
            (user(candidate), candidate.remote == "flathub") > (user(current), current.remote == "flathub")
        };
        match by_id.get(&package.id) {
            Some(current) if !prefers(&package, current) => {}
            _ => {
                by_id.insert(package.id.clone(), package);
            }
        }
    }
    for item in installed {
        let entry = by_id.entry(item.id.clone()).or_insert_with(|| Package {
            id: item.id.clone(),
            title: item.title.clone(),
            kind: Kind::Flatpak,
            remote: item.remote.clone(),
            ..Default::default()
        });
        entry.installed = true;
        entry.installation = item.installation;
        if !item.version.is_empty() {
            entry.version = item.version;
        }
        if item.size > 0 {
            entry.size = item.size;
        }
        if entry.title.is_empty() {
            entry.title = item.title;
        }
    }
    for id in updates {
        if let Some(entry) = by_id.get_mut(id) {
            if entry.installed {
                entry.update = true;
            }
        }
    }
    by_id.into_values().collect()
}

/// What the appstream data says about one application.
#[derive(Debug, Default, PartialEq, serde::Serialize)]
pub struct Info {
    pub developer: String,
    pub license: String,
    pub homepage: String,
    pub description: Vec<String>,
    pub categories: Vec<String>,
    pub release: String,
}

fn decode(text: &str) -> String {
    text.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&apos;", "'").replace("&amp;", "&")
}

/// The text between `<tag ...>` and `</tag>` on one line.
fn element(line: &str, tag: &str) -> Option<String> {
    let open = line.find(&format!("<{tag}"))?;
    let start = line[open..].find('>')? + open + 1;
    let end = line[start..].find(&format!("</{tag}>"))? + start;
    Some(decode(&line[start..end]))
}

/// Strips inline markup (`<em>`, `<code>`) from a paragraph.
fn plain(text: &str) -> String {
    let mut out = String::new();
    let mut inside = false;
    for ch in text.chars() {
        match ch {
            '<' => inside = true,
            '>' => inside = false,
            c if !inside => out.push(c),
            _ => {}
        }
    }
    decode(out.trim())
}

/// Parses one `<component>…</component>` block (appstream XML as Flathub publishes it, one element per line).
pub fn parse_component(block: &str) -> Info {
    let mut info = Info::default();
    let mut in_description = false;
    let mut in_releases = false;
    for raw in block.lines() {
        let line = raw.trim();
        if line.starts_with("<description") {
            in_description = true;
        }
        if line.starts_with("</description") {
            in_description = false;
        }
        if line.starts_with("<releases") {
            in_releases = true;
        }
        if line.starts_with("</releases") {
            in_releases = false;
        }
        if in_description {
            if let Some(text) = element(line, "p").or_else(|| element(line, "li").map(|t| format!("• {t}"))) {
                let text = plain(&text);
                if !text.is_empty() {
                    info.description.push(text);
                }
            }
        } else if in_releases {
            if info.release.is_empty() {
                if let Some(at) = line.find("version=\"") {
                    let rest = &line[at + 9..];
                    if let Some(end) = rest.find('"') {
                        info.release = rest[..end].to_string();
                    }
                }
            }
        } else if line.starts_with("<developer_name") && info.developer.is_empty() {
            info.developer = plain(&element(line, "developer_name").unwrap_or_default());
        } else if line.starts_with("<name>") && info.developer.is_empty() && block.contains("<developer") {
            // `<developer><name>…</name></developer>` (newer appstream)
            let _ = line;
        } else if line.starts_with("<project_license") {
            info.license = plain(&element(line, "project_license").unwrap_or_default());
        } else if line.starts_with("<url type=\"homepage\"") {
            info.homepage = element(line, "url").unwrap_or_default();
        } else if line.starts_with("<category>") {
            if let Some(category) = element(line, "category") {
                info.categories.push(category);
            }
        }
    }
    info
}

/// Where an installation keeps its appstream data and icons.
pub fn installation_base(installation: &str) -> PathBuf {
    if installation == "user" {
        std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/share")))
            .unwrap_or_default()
            .join("flatpak")
    } else {
        PathBuf::from("/var/lib/flatpak")
    }
}

pub fn arch() -> &'static str {
    match std::env::consts::ARCH {
        "aarch64" => "aarch64",
        "arm" => "arm",
        other => other,
    }
}

/// Candidate icon files for an application, best first.
pub fn icon_paths(remote: &str, id: &str, installation: &str) -> Vec<PathBuf> {
    if remote.is_empty() || remote.contains('/') || remote.contains("..") || !valid_id(id) {
        return Vec::new();
    }
    let mut out = Vec::new();
    let order: [&str; 2] = if installation == "system" { ["system", "user"] } else { ["user", "system"] };
    for which in order {
        let base = installation_base(which).join("appstream").join(remote).join(arch()).join("active/icons");
        for size in ["128x128", "64x64"] {
            out.push(base.join(size).join(format!("{id}.png")));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes_are_decimal() {
        assert_eq!(parse_size("88.2 MB"), 88_200_000);
        assert_eq!(parse_size("1.0 GB"), 1_000_000_000);
        assert_eq!(parse_size("512 bytes"), 512);
        assert_eq!(parse_size("4,8 MB"), 4_800_000);
        assert_eq!(parse_size(""), 0);
    }

    #[test]
    fn the_remote_catalog_is_parsed_and_odd_ids_dropped() {
        let text = "ai.jan.Jan\tJan\tPrivate offline AI assistant\t0.8.4\tflathub\t88.2 MB\nbad id\tX\t\t\t\t\norg.x.NoSummary\tNo Summary\t\t1.0\tflathub\t1.0 MB\n";
        let rows = parse_remote_ls(text);
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].version, "0.8.4");
        assert_eq!(rows[0].size, 88_200_000);
        assert_eq!(rows[1].summary, "");
    }

    #[test]
    fn merge_prefers_the_user_remote_and_marks_installed_and_updates() {
        let remote = vec![
            Package { id: "org.a.A".into(), title: "A".into(), remote: "flathub-sys".into(), ..Default::default() },
            Package { id: "org.a.A".into(), title: "A".into(), remote: "flathub".into(), size: 10, ..Default::default() },
            Package { id: "org.b.B".into(), title: "B".into(), remote: "fedora".into(), ..Default::default() },
        ];
        let installed = vec![Installed {
            id: "org.b.B".into(),
            title: "B".into(),
            version: "2.0".into(),
            remote: "fedora".into(),
            installation: "system".into(),
            size: 5,
        }];
        let merged = merge(remote, installed, &["org.b.B".to_string(), "org.a.A".to_string()], &["flathub".to_string()]);
        let a = merged.iter().find(|p| p.id == "org.a.A").unwrap();
        assert_eq!(a.remote, "flathub");
        assert!(!a.installed && !a.update, "an update only counts for an installed app");
        let b = merged.iter().find(|p| p.id == "org.b.B").unwrap();
        assert!(b.installed && b.update);
        assert_eq!(b.installation, "system");
        assert_eq!(b.version, "2.0");
    }

    #[test]
    fn remotes_know_their_installation() {
        let text = "fedora\tFedora Flatpaks\toci+https://registry.fedoraproject.org\tsystem,oci\nflathub\tFlathub\thttps://dl.flathub.org/repo/\tuser\n";
        let remotes = parse_remotes(text);
        assert_eq!(remotes[0].installation, "system");
        assert_eq!(remotes[1].installation, "user");
    }

    #[test]
    fn a_component_gives_description_developer_and_license() {
        let block = "<component type=\"desktop-application\">\n  <id>org.gnome.Calculator</id>\n  <developer_name>The GNOME Project</developer_name>\n  <project_license>GPL-3.0-or-later</project_license>\n  <url type=\"homepage\">https://apps.gnome.org/Calculator/</url>\n  <description>\n    <p>Calculator is an application that solves mathematical equations &amp; more.</p>\n    <p>It has <em>advanced</em> mode.</p>\n    <ul>\n      <li>Scientific mode</li>\n    </ul>\n  </description>\n  <categories>\n    <category>Utility</category>\n    <category>Science</category>\n  </categories>\n  <releases>\n    <release version=\"51.0\" date=\"2026-03-01\"/>\n  </releases>\n</component>";
        let info = parse_component(block);
        assert_eq!(info.developer, "The GNOME Project");
        assert_eq!(info.license, "GPL-3.0-or-later");
        assert_eq!(info.homepage, "https://apps.gnome.org/Calculator/");
        assert_eq!(info.description[0], "Calculator is an application that solves mathematical equations & more.");
        assert_eq!(info.description[1], "It has advanced mode.");
        assert_eq!(info.description[2], "• Scientific mode");
        assert_eq!(info.categories, vec!["Utility", "Science"]);
        assert_eq!(info.release, "51.0");
    }

    #[test]
    fn icon_paths_refuse_odd_remotes() {
        assert!(icon_paths("../etc", "org.a.A", "user").is_empty());
        assert!(!icon_paths("flathub", "org.a.A", "user").is_empty());
    }
}
