//! Searching wallhaven.cc for wallpapers. This is the only place that builds the request and the only place that reads the answer:
//! the search is always safe-for-work (the purity is not a parameter), every address taken from an answer must be on wallhaven's own
//! picture hosts, and a picture's file name on disk comes from its validated id, never from the answer's text.

use serde::Serialize;
use serde_json::Value;

use crate::display::sanitize_for_display;

pub const API: &str = "https://wallhaven.cc/api/v1/search";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Sorting {
    Toplist,
    Latest,
    Random,
    Views,
    Favorites,
}

impl Sorting {
    pub fn parse(name: &str) -> Option<Sorting> {
        Some(match name {
            "toplist" => Sorting::Toplist,
            "latest" => Sorting::Latest,
            "random" => Sorting::Random,
            "views" => Sorting::Views,
            "favorites" => Sorting::Favorites,
            _ => return None,
        })
    }

    fn wire(self) -> &'static str {
        match self {
            Sorting::Toplist => "toplist",
            Sorting::Latest => "date_added",
            Sorting::Random => "random",
            Sorting::Views => "views",
            Sorting::Favorites => "favorites",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SearchError {
    BadQuery,
    BadSorting,
    BadPage,
}

/// A search as the person typed it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Search {
    pub query: String,
    pub sorting: Sorting,
    pub page: u32,
}

impl Search {
    /// `query` is free text of letters, digits, spaces and a few marks (wallhaven's own `+tag -tag` and `"phrase"` forms are fine).
    pub fn new(query: &str, sorting: &str, page: u32) -> Result<Search, SearchError> {
        let query = query.split_whitespace().collect::<Vec<_>>().join(" ");
        let allowed = |c: char| c.is_alphanumeric() || " +-_\"'.".contains(c);
        if query.chars().count() > 80 || !query.chars().all(allowed) {
            return Err(SearchError::BadQuery);
        }
        let sorting = Sorting::parse(sorting).ok_or(SearchError::BadSorting)?;
        if !(1..=100).contains(&page) {
            return Err(SearchError::BadPage);
        }
        Ok(Search { query, sorting, page })
    }

    /// The query pairs to send, already in final form (the caller percent-encodes them). General and anime categories, never people;
    /// purity is fixed at safe; large enough for a desktop.
    pub fn parameters(&self) -> Vec<(&'static str, String)> {
        let mut all = vec![
            ("categories", "110".to_string()),
            ("purity", "100".to_string()),
            ("sorting", self.sorting.wire().to_string()),
            ("atleast", "1920x1080".to_string()),
            ("page", self.page.to_string()),
        ];
        if self.sorting == Sorting::Toplist {
            all.push(("topRange", "1y".to_string()));
        }
        if !self.query.is_empty() {
            all.push(("q", self.query.clone()));
        }
        all
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Hit {
    pub id: String,
    pub resolution: String,
    pub size_mb: String,
    /// `#rrggbb` colours the picture is mostly made of, up to five.
    pub colors: Vec<String>,
    #[serde(skip)]
    pub thumb: String,
    #[serde(skip)]
    pub full: String,
    pub extension: &'static str,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Results {
    pub hits: Vec<Hit>,
    pub page: u32,
    pub last_page: u32,
}

/// An id is what wallhaven uses in file names: 5 to 8 lowercase letters and digits.
pub fn valid_id(id: &str) -> bool {
    (5..=8).contains(&id.len()) && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
}

/// Only the two hosts that serve wallhaven's pictures, over https, with no credentials or port.
pub fn trusted_picture_url(url: &str) -> bool {
    let Some(rest) = url.strip_prefix("https://") else { return false };
    let (host, path) = rest.split_once('/').unwrap_or((rest, ""));
    (host == "w.wallhaven.cc" || host == "th.wallhaven.cc")
        && !path.is_empty()
        && path.bytes().all(|b| b.is_ascii_alphanumeric() || b"/._-".contains(&b))
        && !path.contains("..")
}

fn extension_of(mime: &str) -> Option<&'static str> {
    match mime {
        "image/jpeg" => Some("jpg"),
        "image/png" => Some("png"),
        _ => None,
    }
}

fn color(text: &str) -> Option<String> {
    let hex = text.strip_prefix('#')?;
    (hex.len() == 6 && hex.bytes().all(|b| b.is_ascii_hexdigit())).then(|| text.to_ascii_lowercase())
}

/// Reads a search answer; entries that are not safe-for-work, not a picture type we can use, or carry an address off wallhaven are dropped.
pub fn parse_results(body: &Value) -> Results {
    let mut hits = Vec::new();
    for item in body.get("data").and_then(Value::as_array).into_iter().flatten().take(64) {
        let text = |key: &str| item.get(key).and_then(Value::as_str).unwrap_or("");
        let id = text("id");
        let Some(extension) = extension_of(text("file_type")) else { continue };
        let thumb = item.pointer("/thumbs/large").and_then(Value::as_str).unwrap_or("");
        let full = text("path");
        if !valid_id(id) || text("purity") != "sfw" || !trusted_picture_url(thumb) || !trusted_picture_url(full) {
            continue;
        }
        let bytes = item.get("file_size").and_then(Value::as_u64).unwrap_or(0);
        hits.push(Hit {
            id: id.to_string(),
            resolution: sanitize_for_display(text("resolution"), 12),
            size_mb: format!("{:.1} MB", bytes as f64 / 1_048_576.0),
            colors: item.get("colors").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str).filter_map(color).take(5).collect(),
            thumb: thumb.to_string(),
            full: full.to_string(),
            extension,
        });
    }
    let meta = |key: &str| body.pointer(&format!("/meta/{key}")).and_then(Value::as_u64).unwrap_or(1).clamp(1, 1000) as u32;
    Results { hits, page: meta("current_page"), last_page: meta("last_page") }
}

impl Hit {
    pub fn thumb_url(&self) -> String {
        self.thumb.clone()
    }

    pub fn full_url(&self) -> String {
        self.full.clone()
    }
}

/// The file a downloaded picture is saved as.
pub fn file_name(hit: &Hit) -> String {
    format!("wallhaven-{}.{}", hit.id, hit.extension)
}

/// Largest picture taken (wallhaven's 4K and 8K files are a few megabytes to a few tens).
pub const MAX_DOWNLOAD: u64 = 60 * 1024 * 1024;

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn item(id: &str) -> Value {
        json!({"id": id, "purity": "sfw", "file_type": "image/jpeg", "resolution": "3840x2160", "file_size": 3177780,
               "colors": ["#424153", "#BADCOL", "#ABBCDA"],
               "path": format!("https://w.wallhaven.cc/full/{}/wallhaven-{id}.jpg", &id[..2]),
               "thumbs": {"large": format!("https://th.wallhaven.cc/lg/{}/{id}.jpg", &id[..2])}})
    }

    #[test]
    fn a_search_is_always_safe_and_never_takes_the_purity_from_the_person() {
        let search = Search::new("  mountain   lake ", "toplist", 2).unwrap();
        let p = search.parameters();
        assert!(p.contains(&("purity", "100".into())));
        assert!(p.contains(&("q", "mountain lake".into())));
        assert!(p.contains(&("topRange", "1y".into())));
        assert!(p.contains(&("categories", "110".into())), "no people category");
        assert!(!Search::new("", "latest", 1).unwrap().parameters().iter().any(|(k, _)| *k == "q" || *k == "topRange"));
        let injected = Search::new("cat&purity=111", "toplist", 1);
        assert_eq!(injected, Err(SearchError::BadQuery), "a query cannot add parameters");
        assert_eq!(Search::new("a\nb", "toplist", 1).unwrap().query, "a b", "line breaks are just spaces");
        assert_eq!(Search::new("a\u{0}b", "toplist", 1), Err(SearchError::BadQuery));
        assert_eq!(Search::new(&"x".repeat(81), "toplist", 1), Err(SearchError::BadQuery));
        assert_eq!(Search::new("x", "nsfw", 1), Err(SearchError::BadSorting));
        assert_eq!(Search::new("x", "latest", 0), Err(SearchError::BadPage));
        assert_eq!(Search::new("x", "latest", 101), Err(SearchError::BadPage));
        assert!(Search::new("+forest -city \"night sky\"", "random", 1).is_ok());
    }

    #[test]
    fn answers_are_read_strictly() {
        let mut offsite = item("abc123");
        offsite["path"] = json!("https://evil.example/full/ab/wallhaven-abc123.jpg");
        let mut lewd = item("def456");
        lewd["purity"] = json!("sketchy");
        let mut gif = item("ghi789");
        gif["file_type"] = json!("image/gif");
        let mut traversal = item("jkl012");
        traversal["thumbs"]["large"] = json!("https://th.wallhaven.cc/lg/../../x.jpg");
        let mut oddid = item("mno345");
        oddid["id"] = json!("../etc");
        let body = json!({"data": [item("3q797y"), offsite, lewd, gif, traversal, oddid], "meta": {"current_page": 2, "last_page": 9}});
        let r = parse_results(&body);
        assert_eq!(r.hits.len(), 1);
        assert_eq!((r.page, r.last_page), (2, 9));
        let hit = &r.hits[0];
        assert_eq!((hit.id.as_str(), hit.resolution.as_str(), hit.size_mb.as_str()), ("3q797y", "3840x2160", "3.0 MB"));
        assert_eq!(hit.colors, ["#424153", "#abbcda"], "a bad colour is dropped");
        assert_eq!(file_name(hit), "wallhaven-3q797y.jpg");
        assert_eq!(parse_results(&json!({"nonsense": 1})).hits.len(), 0);
    }

    #[test]
    fn only_wallhavens_picture_hosts_are_trusted() {
        assert!(trusted_picture_url("https://w.wallhaven.cc/full/3q/wallhaven-3q797y.jpg"));
        assert!(trusted_picture_url("https://th.wallhaven.cc/lg/3q/3q797y.jpg"));
        for bad in [
            "http://w.wallhaven.cc/full/a.jpg", "https://wallhaven.cc/x.jpg", "https://w.wallhaven.cc.evil.com/a.jpg",
            "https://user@w.wallhaven.cc/a.jpg", "https://w.wallhaven.cc:444/a.jpg", "https://w.wallhaven.cc/", "https://w.wallhaven.cc/a b.jpg",
            "https://w.wallhaven.cc/a?x=1", "https://w.wallhaven.cc/../a.jpg", "file:///etc/passwd", "",
        ] {
            assert!(!trusted_picture_url(bad), "{bad}");
        }
    }

    #[test]
    fn ids_are_file_name_safe() {
        assert!(valid_id("3q797y") && valid_id("abcde") && valid_id("abcdefgh"));
        for bad in ["", "abcd", "abcdefghi", "ABCDEF", "ab/cde", "ab.cde", "ab cde", "ábcdef"] {
            assert!(!valid_id(bad), "{bad}");
        }
    }
}
