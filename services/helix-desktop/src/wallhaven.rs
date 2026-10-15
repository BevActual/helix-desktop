//! Wallhaven search/download jobs for `Appearance1` (C++ `desktop/wallhaven.h` and
//! `net/wallhaven_client.cpp`).
//!
//! Every client owns its jobs; network work is bounded, cancellable and never exposes the API
//! key or accepts client-supplied download URLs or output paths.

use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

const API_SEARCH_URL: &str = "https://wallhaven.cc/api/v1/search";
const ASPECT_RATIOS: [&str; 15] = [
    "", "wide", "portrait", "16x9", "16x10", "21x9", "32x9", "48x9", "9x16", "10x16", "9x18",
    "1x1", "3x2", "4x3", "5x4",
];
const MAX_ITEMS: usize = 24;
const SEARCH_LIMIT: usize = 2 * 1024 * 1024;
const THUMB_LIMIT: usize = 4 * 1024 * 1024;
const DOWNLOAD_LIMIT: usize = 64 * 1024 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(45);
const JOB_IDLE_LIMIT: Duration = Duration::from_secs(5 * 60);
const THUMB_CHAINS: usize = 4;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum WhError {
    /// Maps to `org.helix.Desktop.Error.InvalidArgument`.
    Invalid(String),
    /// Maps to `org.helix.Desktop.Error.Failed`.
    Failed(String),
}

fn invalid(message: &str) -> WhError {
    WhError::Invalid(message.to_string())
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Item {
    pub id: String,
    pub path: String,
    pub file_type: String,
    pub resolution: String,
    pub purity: String,
    pub thumb_large: String,
    pub thumb_original: String,
    pub thumb_small: String,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Query {
    pub tags: String,
    pub category_general: bool,
    pub category_anime: bool,
    pub category_people: bool,
    pub purity_sfw: bool,
    pub purity_sketchy: bool,
    pub purity_nsfw: bool,
    /// 0 relevance, 1 random, 2 date added, 3 views, 4 favorites, 5 toplist, 6 hot
    pub sort: u32,
    pub ascending: bool,
    pub exact_resolution: bool,
    pub resolution_width: String,
    pub resolution_height: String,
    pub aspect_ratio_index: usize,
    pub page: u32,
}

// ---- validation and URL building -------------------------------------------------------

pub fn is_wallhaven_id(value: &str) -> bool {
    value.len() == 6
        && value
            .bytes()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit())
}

/// `https://{th|w}.wallhaven.cc/...` with no credentials, query, fragment or odd port.
pub fn is_image_url(url: &str, thumbnail: bool) -> bool {
    let Some(rest) = url.strip_prefix("https://") else {
        return false;
    };
    let host = if thumbnail {
        "th.wallhaven.cc"
    } else {
        "w.wallhaven.cc"
    };
    let authority_end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let authority = &rest[..authority_end];
    let tail = &rest[authority_end..];
    if tail.contains('?') || tail.contains('#') || authority.contains('@') {
        return false;
    }
    match authority.split_once(':') {
        Some((h, port)) => h == host && port == "443",
        None => authority == host,
    }
}

fn whole_number(value: &Value) -> Option<f64> {
    // D-Bus integers arrive as JSON floats; only whole numbers count as integers.
    value.as_f64().filter(|n| n.fract() == 0.0)
}

/// C++ `wallhavenQuery`.
pub fn query_from_args(args: &Value, authenticated: bool) -> Result<Query, WhError> {
    let object = args
        .as_object()
        .ok_or_else(|| invalid("Invalid Wallhaven search options"))?;
    for field in ["page", "sort", "ratio", "width", "height"] {
        if let Some(value) = object.get(field) {
            match whole_number(value) {
                Some(n) if (0.0..=32768.0).contains(&n) => {}
                _ => return Err(invalid("Invalid Wallhaven numeric filter")),
            }
        }
    }
    let number =
        |key: &str, default: f64| object.get(key).and_then(whole_number).unwrap_or(default);
    let flag =
        |key: &str, default: bool| object.get(key).and_then(Value::as_bool).unwrap_or(default);
    let tags = object
        .get("query")
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string();
    let page = number("page", 1.0);
    let sort = number("sort", 2.0);
    let ratio = number("ratio", 0.0);
    if tags.len() > 512
        || page < 1.0
        || page > 10000.0
        || !(0.0..=6.0).contains(&sort)
        || ratio < 0.0
        || ratio as usize >= ASPECT_RATIOS.len()
    {
        return Err(invalid("Invalid Wallhaven search options"));
    }
    let purity_nsfw = flag("nsfw", false);
    if purity_nsfw && !authenticated {
        return Err(invalid(
            "A saved Wallhaven API key is required for this filter",
        ));
    }
    let (width, height) = (number("width", 0.0), number("height", 0.0));
    if (width == 0.0) != (height == 0.0) {
        return Err(invalid("Specify both resolution dimensions"));
    }
    let (resolution_width, resolution_height) = if width != 0.0 {
        (format!("{}", width as i64), format!("{}", height as i64))
    } else {
        Default::default()
    };
    Ok(Query {
        tags,
        category_general: flag("general", true),
        category_anime: flag("anime", true),
        category_people: flag("people", true),
        purity_sfw: flag("sfw", true),
        purity_sketchy: flag("sketchy", false),
        purity_nsfw,
        sort: sort as u32,
        ascending: flag("ascending", false),
        exact_resolution: flag("exact", false),
        resolution_width,
        resolution_height,
        aspect_ratio_index: ratio as usize,
        page: page as u32,
    })
}

fn url_encode(text: &str) -> String {
    let mut out = String::new();
    for b in text.bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b'~') {
            out.push(char::from(b));
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

fn url_decode(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hex = std::str::from_utf8(&bytes[i + 1..i + 3]).ok();
            if let Some(v) = hex.and_then(|h| u8::from_str_radix(h, 16).ok()) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn wallpaper_id_from_text(text: &str) -> Option<String> {
    for marker in ["wallhaven.cc/w/", "wallhaven-"] {
        if let Some(pos) = text.find(marker) {
            let rest = &text[pos + marker.len()..];
            let id: String = rest
                .chars()
                .take_while(|c| c.is_ascii_alphanumeric())
                .collect();
            if id.len() >= 4 {
                return Some(id);
            }
        }
    }
    None
}

fn query_param(text: &str, key: &str) -> Option<String> {
    let pos = text
        .find(&format!("?{key}="))
        .or_else(|| text.find(&format!("&{key}=")))?;
    let start = pos + key.len() + 2;
    let end = text[start..]
        .find(['&', '#'])
        .map_or(text.len(), |i| start + i);
    Some(url_decode(&text[start..end].replace('+', " ")))
}

/// C++ `normalizeSearchQuery`: links collapse to `id:<id>` or their `q`, tokens re-joined.
pub fn normalize_search_query(raw: &str) -> String {
    let mut text = raw.trim().to_string();
    if text.is_empty() {
        return String::new();
    }
    let looks_like_link = text.contains("://") || text.contains("wallhaven.cc");
    if looks_like_link {
        if let Some(id) = wallpaper_id_from_text(&text) {
            return format!("id:{id}");
        }
        if let Some(query) = query_param(&text, "q") {
            text = query.trim().to_string();
        }
    } else if text.starts_with("wallhaven-") || text.contains("/wallhaven-") {
        if let Some(id) = wallpaper_id_from_text(&text) {
            return format!("id:{id}");
        }
    }
    text.replace(',', " ")
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
}

fn relevance_without_query(query: &Query) -> bool {
    query.sort == 0 && normalize_search_query(&query.tags).is_empty()
}

fn tri_flag(a: bool, b: bool, c: bool) -> String {
    let value: String = [a, b, c]
        .iter()
        .map(|f| if *f { '1' } else { '0' })
        .collect();
    if value == "000" {
        "100".into()
    } else {
        value
    }
}

fn parse_positive_int(text: &str) -> Option<u32> {
    let t = text.trim();
    if t.is_empty() || !t.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    t.parse::<u32>()
        .ok()
        .filter(|v| *v >= 1 && *v <= i32::MAX as u32)
}

fn parse_resolution(text: &str) -> Option<(u32, u32)> {
    let (w, h) = text.split_once('x')?;
    Some((parse_positive_int(w)?, parse_positive_int(h)?))
}

pub fn api_page(query: &Query) -> u32 {
    let page = query.page.max(1);
    if relevance_without_query(query) {
        page + 1
    } else {
        page
    }
}

pub fn normalize_total_pages(query: &Query, last_page: i64) -> i64 {
    let pages = last_page.max(1);
    if relevance_without_query(query) {
        (pages - 1).max(1)
    } else {
        pages
    }
}

/// C++ `buildSearchUrl`.
pub fn build_search_url(query: &Query, nsfw_allowed: bool) -> String {
    const SORTS: [&str; 7] = [
        "relevance",
        "random",
        "date_added",
        "views",
        "favorites",
        "toplist",
        "hot",
    ];
    let mut url = format!(
        "{API_SEARCH_URL}?categories={}&purity={}&page={}&sorting={}&order={}",
        tri_flag(
            query.category_general,
            query.category_anime,
            query.category_people
        ),
        tri_flag(
            query.purity_sfw,
            query.purity_sketchy,
            query.purity_nsfw && nsfw_allowed
        ),
        api_page(query),
        SORTS
            .get(query.sort as usize)
            .copied()
            .unwrap_or("date_added"),
        if query.ascending { "asc" } else { "desc" },
    );
    let tags = normalize_search_query(&query.tags);
    if !tags.is_empty() {
        url.push_str("&q=");
        url.push_str(&url_encode(&tags));
    }
    if let (Some(w), Some(h)) = (
        parse_positive_int(&query.resolution_width),
        parse_positive_int(&query.resolution_height),
    ) {
        let key = if query.exact_resolution {
            "resolutions"
        } else {
            "atleast"
        };
        url.push_str(&format!("&{key}={w}x{h}"));
    }
    match ASPECT_RATIOS.get(query.aspect_ratio_index).copied() {
        None | Some("") => {}
        Some("wide") => url.push_str("&ratios=landscape"),
        Some("portrait") => url.push_str("&ratios=portrait"),
        Some(other) => {
            url.push_str("&ratios=");
            url.push_str(other);
        }
    }
    url
}

pub struct SearchResponse {
    pub items: Vec<Item>,
    pub last_page: i64,
}

/// C++ `parseSearchBody`; `None` for malformed bodies and API errors.
pub fn parse_search_body(body: &[u8]) -> Option<SearchResponse> {
    let decoded: Value = serde_json::from_slice(body).ok()?;
    let object = decoded.as_object()?;
    let data = object.get("data")?.as_array()?;
    let last_page = object
        .get("meta")
        .and_then(|m| m.get("last_page"))
        .and_then(Value::as_f64)
        .map_or(1, |n| (n as i64).max(1));
    let text = |entry: &Value, key: &str| {
        entry
            .get(key)
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string()
    };
    let items = data
        .iter()
        .filter(|entry| entry.is_object())
        .map(|entry| {
            let thumbs = entry.get("thumbs").filter(|t| t.is_object());
            let thumb = |key: &str| thumbs.map(|t| text(t, key)).unwrap_or_default();
            Item {
                id: text(entry, "id"),
                path: text(entry, "path"),
                file_type: text(entry, "file_type"),
                resolution: text(entry, "resolution"),
                purity: text(entry, "purity"),
                thumb_large: thumb("large"),
                thumb_original: thumb("original"),
                thumb_small: thumb("small"),
            }
        })
        .filter(|item| !item.id.is_empty() && !item.path.is_empty())
        .collect();
    Some(SearchResponse { items, last_page })
}

/// C++ `filterByAspectRatio`.
pub fn filter_by_aspect_ratio(items: Vec<Item>, index: usize) -> Vec<Item> {
    if index == 0 || index >= ASPECT_RATIOS.len() {
        return items;
    }
    let selected = ASPECT_RATIOS[index];
    items
        .into_iter()
        .filter(|item| {
            let Some((w, h)) = parse_resolution(&item.resolution) else {
                return false;
            };
            let ratio = f64::from(w) / f64::from(h);
            match selected {
                "wide" => ratio > 1.0,
                "portrait" => ratio < 1.0,
                s if s.contains('x') => match parse_resolution(s) {
                    Some((tw, th)) => (ratio - f64::from(tw) / f64::from(th)).abs() <= 0.05,
                    None => true,
                },
                _ => true,
            }
        })
        .collect()
}

fn thumb_urls(item: &Item) -> Vec<&str> {
    let mut urls: Vec<&str> = Vec::new();
    for url in [&item.thumb_large, &item.thumb_original, &item.thumb_small] {
        if !url.is_empty() && !urls.contains(&url.as_str()) {
            urls.push(url);
        }
    }
    urls
}

pub fn thumb_cache_path(state_dir: &Path, id: &str) -> PathBuf {
    state_dir
        .join("wallhaven")
        .join("thumbs")
        .join(format!("{id}.jpg"))
}

pub fn download_path(dir: &Path, item: &Item) -> PathBuf {
    let extension = if item.file_type == "image/png" {
        ".png"
    } else {
        ".jpg"
    };
    dir.join(format!("wallhaven-{}{extension}", item.id))
}

fn expand_user_path(path: &str) -> PathBuf {
    match (path.strip_prefix("~/"), std::env::var("HOME")) {
        (Some(rest), Ok(home)) if !home.is_empty() => PathBuf::from(home).join(rest),
        _ if path == "~" => std::env::var("HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|_| PathBuf::from(path)),
        _ => PathBuf::from(path),
    }
}

/// C++ `FileUtils::defaultPicturesDirectory` (XDG_PICTURES_DIR, user-dirs.dirs, `~/Pictures`).
fn default_pictures_dir() -> PathBuf {
    let home = match std::env::var("HOME") {
        Ok(h) if !h.is_empty() && Path::new(&h).is_absolute() => PathBuf::from(h),
        _ => return PathBuf::from("/tmp"),
    };
    let expand = |raw: &str| -> Option<PathBuf> {
        if raw.is_empty() {
            return None;
        }
        // Undefined variables make the value unusable.
        let mut out = String::new();
        let mut rest = raw;
        while let Some(pos) = rest.find('$') {
            out.push_str(&rest[..pos]);
            let after = &rest[pos + 1..];
            let (name, remainder) = if let Some(braced) = after.strip_prefix('{') {
                let close = braced.find('}')?;
                (&braced[..close], &braced[close + 1..])
            } else {
                let end = after
                    .find(|c: char| !(c.is_ascii_alphanumeric() || c == '_'))
                    .unwrap_or(after.len());
                if end == 0 {
                    out.push('$');
                    rest = after;
                    continue;
                }
                (&after[..end], &after[end..])
            };
            let value = std::env::var(name).ok().filter(|v| !v.is_empty())?;
            out.push_str(&value);
            rest = remainder;
        }
        out.push_str(rest);
        let path = expand_user_path(&out);
        path.is_absolute().then_some(path)
    };
    if let Ok(raw) = std::env::var("XDG_PICTURES_DIR") {
        if let Some(path) = expand(&raw) {
            return path;
        }
    }
    let config_home = std::env::var("XDG_CONFIG_HOME")
        .ok()
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .unwrap_or_else(|| home.join(".config"));
    if let Ok(text) = std::fs::read_to_string(config_home.join("user-dirs.dirs")) {
        for line in text.lines() {
            let line = line.trim_start();
            let Some(rest) = line.strip_prefix("XDG_PICTURES_DIR=") else {
                continue;
            };
            let Some(quoted) = rest.trim_start().strip_prefix('"') else {
                continue;
            };
            let mut value = String::new();
            let mut chars = quoted.chars();
            let mut terminated = false;
            while let Some(c) = chars.next() {
                match c {
                    '"' => {
                        terminated = true;
                        break;
                    }
                    '\\' => value.extend(chars.next()),
                    other => value.push(other),
                }
            }
            if !terminated {
                continue;
            }
            if let Some(rel) = value.strip_prefix("$HOME/") {
                return home.join(rel);
            }
            if value.starts_with('/') {
                return PathBuf::from(value);
            }
        }
    }
    home.join("Pictures")
}

/// C++ `resolveDownloadDir`: configured folder, else the wallpaper folder, else Pictures.
pub fn resolve_download_dir(configured: &str, wallpaper_directory: &str) -> PathBuf {
    if !configured.trim().is_empty() {
        return expand_user_path(configured);
    }
    if !wallpaper_directory.trim().is_empty() {
        return expand_user_path(wallpaper_directory);
    }
    default_pictures_dir()
}

// ---- transport -----------------------------------------------------------------------

/// Fetches `url` (headers as `(name, value)`) with a hard size limit; `None` on any failure.
pub trait Fetch: Send + Sync {
    fn get(
        &self,
        url: &str,
        headers: &[(String, String)],
        limit: usize,
        cancelled: &AtomicBool,
    ) -> Option<Vec<u8>>;
}

pub struct UreqFetch;

impl Fetch for UreqFetch {
    fn get(
        &self,
        url: &str,
        headers: &[(String, String)],
        limit: usize,
        cancelled: &AtomicBool,
    ) -> Option<Vec<u8>> {
        let agent = ureq::AgentBuilder::new().timeout(REQUEST_TIMEOUT).build();
        let mut request = agent.get(url);
        for (name, value) in headers {
            request = request.set(name, value);
        }
        let response = request.call().ok()?;
        let mut reader = response.into_reader();
        let mut bytes = Vec::new();
        let mut chunk = [0u8; 16 * 1024];
        loop {
            if cancelled.load(Ordering::Relaxed) {
                return None;
            }
            let read = reader.read(&mut chunk).ok()?;
            if read == 0 {
                return Some(bytes);
            }
            if bytes.len() + read > limit {
                return None;
            }
            bytes.extend_from_slice(&chunk[..read]);
        }
    }
}

// ---- jobs -------------------------------------------------------------------------------

struct Job {
    owner: String,
    cancelled: AtomicBool,
    state: Mutex<Value>,
    items: Mutex<Vec<Item>>,
    touched: Mutex<Instant>,
}

impl Job {
    fn set(&self, key: &str, value: Value) {
        self.state.lock().unwrap()[key] = value;
    }
}

#[derive(Default)]
struct Inner {
    jobs: BTreeMap<u64, Arc<Job>>,
    next: u64,
    /// Wallpapers fetched since the last `take_downloaded` (applied by the service loop).
    downloaded: Vec<PathBuf>,
}

/// What a request needs from the service/config at call time.
#[derive(Clone, Debug)]
pub struct Context {
    pub desktop: bool,
    pub offline: bool,
    pub api_key: String,
    pub download_dir: PathBuf,
    pub state_dir: PathBuf,
}

pub struct Wallhaven {
    inner: Arc<Mutex<Inner>>,
    fetch: Arc<dyn Fetch>,
    counter: Arc<AtomicU64>,
}

impl Wallhaven {
    pub fn new() -> Self {
        Self::with_fetch(Arc::new(UreqFetch))
    }

    pub fn with_fetch(fetch: Arc<dyn Fetch>) -> Self {
        Self {
            inner: Arc::new(Mutex::new(Inner {
                next: 1,
                ..Default::default()
            })),
            fetch,
            counter: Arc::new(AtomicU64::new(1)),
        }
    }

    fn cancel_job(job: &Job) {
        job.cancelled.store(true, Ordering::Relaxed);
    }

    /// Drops jobs that are stale or no longer allowed (no shell, offline, idle > 5 min).
    pub fn expire(&self, desktop: bool, offline: bool) {
        let mut inner = self.inner.lock().unwrap();
        let stale: Vec<u64> = inner
            .jobs
            .iter()
            .filter(|(_, job)| {
                !desktop || offline || job.touched.lock().unwrap().elapsed() > JOB_IDLE_LIMIT
            })
            .map(|(id, _)| *id)
            .collect();
        for id in stale {
            if let Some(job) = inner.jobs.remove(&id) {
                Self::cancel_job(&job);
            }
        }
    }

    /// A client went away: cancel everything it owned.
    pub fn disconnect(&self, owner: &str) {
        let mut inner = self.inner.lock().unwrap();
        let ids: Vec<u64> = inner
            .jobs
            .iter()
            .filter(|(_, j)| j.owner == owner)
            .map(|(id, _)| *id)
            .collect();
        for id in ids {
            if let Some(job) = inner.jobs.remove(&id) {
                Self::cancel_job(&job);
            }
        }
    }

    pub fn cancel(&self, id: u64, owner: &str) -> Result<(), WhError> {
        let job = self.owned(id, owner)?;
        Self::cancel_job(&job);
        self.inner.lock().unwrap().jobs.remove(&id);
        Ok(())
    }

    pub fn result(&self, id: u64, owner: &str) -> Result<Value, WhError> {
        let job = self.owned(id, owner)?;
        let state = job.state.lock().unwrap().clone();
        Ok(state)
    }

    /// Paths of pictures downloaded since the last call.
    pub fn take_downloaded(&self) -> Vec<PathBuf> {
        std::mem::take(&mut self.inner.lock().unwrap().downloaded)
    }

    fn owned(&self, id: u64, owner: &str) -> Result<Arc<Job>, WhError> {
        let inner = self.inner.lock().unwrap();
        match inner.jobs.get(&id) {
            Some(job) if job.owner == owner => {
                *job.touched.lock().unwrap() = Instant::now();
                Ok(Arc::clone(job))
            }
            _ => Err(invalid("Wallhaven request is no longer available")),
        }
    }

    fn create(&self, owner: &str, ctx: &Context) -> Result<(u64, Arc<Job>), WhError> {
        self.expire(ctx.desktop, ctx.offline);
        let mut inner = self.inner.lock().unwrap();
        let own = inner.jobs.values().filter(|j| j.owner == owner).count();
        if own >= 4 || inner.jobs.len() >= 16 {
            return Err(WhError::Failed(
                "Finish or cancel an existing Wallhaven request first".into(),
            ));
        }
        let job = Arc::new(Job {
            owner: owner.to_string(),
            cancelled: AtomicBool::new(false),
            state: Mutex::new(json!({"busy": true, "error": "", "items": []})),
            items: Mutex::new(Vec::new()),
            touched: Mutex::new(Instant::now()),
        });
        let id = inner.next;
        inner.next += 1;
        inner.jobs.insert(id, Arc::clone(&job));
        Ok((id, job))
    }

    pub fn search(&self, ctx: &Context, args: &Value, owner: &str) -> Result<u64, WhError> {
        if !ctx.desktop {
            return Err(invalid("Wallpaper requires a Helix desktop"));
        }
        if ctx.offline {
            return Err(WhError::Failed(
                "Wallhaven is unavailable while shell network requests are restricted".into(),
            ));
        }
        let has_key = !ctx.api_key.trim().is_empty();
        let query = query_from_args(args, has_key)?;
        let key = ctx.api_key.trim().to_string();
        if key.len() > 256 || key.contains(['\r', '\n']) {
            return Err(invalid("Invalid saved Wallhaven API key"));
        }
        let (id, job) = self.create(owner, ctx)?;
        let url = build_search_url(&query, has_key);
        let mut headers = vec![("Accept".to_string(), "application/json".to_string())];
        if !key.is_empty() {
            headers.push(("X-API-Key".to_string(), key));
        }
        let fetch = Arc::clone(&self.fetch);
        let state_dir = ctx.state_dir.clone();
        let counter = Arc::clone(&self.counter);
        std::thread::spawn(move || {
            let body = fetch.get(&url, &headers, SEARCH_LIMIT, &job.cancelled);
            if job.cancelled.load(Ordering::Relaxed) {
                return;
            }
            let parsed = body.as_deref().and_then(parse_search_body);
            job.set("busy", json!(false));
            let Some(parsed) = parsed else {
                job.set(
                    "error",
                    json!("Wallhaven could not be reached. Check your connection and saved API key, then retry."),
                );
                return;
            };
            let mut accepted = Vec::new();
            let mut rows = Vec::new();
            for item in filter_by_aspect_ratio(parsed.items, query.aspect_ratio_index) {
                if accepted.len() == MAX_ITEMS {
                    break;
                }
                if !is_wallhaven_id(&item.id)
                    || !is_image_url(&item.path, false)
                    || (item.file_type != "image/png" && item.file_type != "image/jpeg")
                {
                    continue;
                }
                rows.push(json!({"id": item.id, "resolution": item.resolution, "thumbnail": ""}));
                accepted.push(item);
            }
            *job.items.lock().unwrap() = accepted;
            {
                let mut state = job.state.lock().unwrap();
                state["items"] = Value::Array(rows);
                state["pages"] =
                    json!(normalize_total_pages(&query, parsed.last_page).clamp(1, 10000));
            }
            for start in 0..THUMB_CHAINS {
                let (job, fetch, state_dir, counter) = (
                    Arc::clone(&job),
                    Arc::clone(&fetch),
                    state_dir.clone(),
                    Arc::clone(&counter),
                );
                std::thread::spawn(move || {
                    let mut index = start;
                    loop {
                        let item = {
                            let items = job.items.lock().unwrap();
                            match items.get(index) {
                                Some(item) => item.clone(),
                                None => return,
                            }
                        };
                        if job.cancelled.load(Ordering::Relaxed) {
                            return;
                        }
                        if let Some(url) = thumb_urls(&item)
                            .into_iter()
                            .find(|u| is_image_url(u, true))
                        {
                            let path = thumb_cache_path(&state_dir, &item.id);
                            let bytes = fetch.get(url, &[], THUMB_LIMIT, &job.cancelled);
                            if let Some(bytes) = bytes {
                                if save_image(&path, &bytes, id, &counter)
                                    && !job.cancelled.load(Ordering::Relaxed)
                                {
                                    job.state.lock().unwrap()["items"][index]["thumbnail"] =
                                        json!(path.to_string_lossy());
                                }
                            }
                        }
                        index += THUMB_CHAINS;
                    }
                });
            }
        });
        Ok(id)
    }

    pub fn download(
        &self,
        ctx: &Context,
        search_id: u64,
        image_id: &str,
        owner: &str,
    ) -> Result<u64, WhError> {
        if !ctx.desktop || ctx.offline {
            return Err(invalid("Wallpaper download is unavailable"));
        }
        let search = self.owned(search_id, owner)?;
        let item = search
            .items
            .lock()
            .unwrap()
            .iter()
            .find(|candidate| candidate.id == image_id)
            .cloned()
            .ok_or_else(|| invalid("Choose an image from the current search"))?;
        let (id, job) = self.create(owner, ctx)?;
        let path = download_path(&ctx.download_dir, &item);
        let fetch = Arc::clone(&self.fetch);
        let inner = Arc::clone(&self.inner);
        let counter = Arc::clone(&self.counter);
        let (desktop, offline) = (ctx.desktop, ctx.offline);
        std::thread::spawn(move || {
            let bytes = fetch.get(&item.path, &[], DOWNLOAD_LIMIT, &job.cancelled);
            if job.cancelled.load(Ordering::Relaxed) {
                return;
            }
            job.set("busy", json!(false));
            let saved = !(!desktop || offline)
                && bytes
                    .as_deref()
                    .is_some_and(|bytes| save_image(&path, bytes, id, &counter));
            if !saved {
                job.set(
                    "error",
                    json!("Wallpaper could not be downloaded. Try again or choose another image."),
                );
                return;
            }
            job.set("path", json!(path.to_string_lossy()));
            inner.lock().unwrap().downloaded.push(path);
        });
        Ok(id)
    }
}

/// Only raster JPEG/PNG payloads are installed; written to a temp file then renamed.
fn save_image(path: &Path, bytes: &[u8], job_id: u64, counter: &AtomicU64) -> bool {
    if !(bytes.starts_with(&[0xff, 0xd8, 0xff]) || bytes.starts_with(b"\x89PNG\r\n\x1a\n")) {
        return false;
    }
    let Some(parent) = path.parent() else {
        return false;
    };
    if std::fs::create_dir_all(parent).is_err() {
        return false;
    }
    let unique = counter.fetch_add(1, Ordering::Relaxed);
    let temporary = PathBuf::from(format!(
        "{}.helix-{job_id}-{}-{unique}",
        path.display(),
        std::process::id()
    ));
    if std::fs::write(&temporary, bytes).is_err() {
        let _ = std::fs::remove_file(&temporary);
        return false;
    }
    if std::fs::rename(&temporary, path).is_err() {
        let _ = std::fs::remove_file(&temporary);
        return false;
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    struct FakeFetch(Mutex<HashMap<String, Vec<u8>>>);

    impl Fetch for FakeFetch {
        fn get(
            &self,
            url: &str,
            _h: &[(String, String)],
            limit: usize,
            _c: &AtomicBool,
        ) -> Option<Vec<u8>> {
            self.0
                .lock()
                .unwrap()
                .get(url)
                .filter(|b| b.len() <= limit)
                .cloned()
        }
    }

    fn ctx(dir: &Path) -> Context {
        Context {
            desktop: true,
            offline: false,
            api_key: String::new(),
            download_dir: dir.join("dl"),
            state_dir: dir.join("state"),
        }
    }

    fn temp(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("helix-wh-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn wait_idle(wh: &Wallhaven, id: u64, owner: &str) -> Value {
        for _ in 0..200 {
            let state = wh.result(id, owner).unwrap();
            if state["busy"] == json!(false) {
                // thumbnails settle shortly after
                std::thread::sleep(Duration::from_millis(100));
                return wh.result(id, owner).unwrap();
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        panic!("job never finished");
    }

    #[test]
    fn validates_search_arguments_like_cpp() {
        for bad in [
            json!({"page": 0}),
            json!({"sort": 7}),
            json!({"width": 1920}),
            json!({"page": 1.5}),
            json!({"nsfw": true}),
            json!({"ratio": 99}),
            json!("query"),
        ] {
            assert!(query_from_args(&bad, false).is_err(), "{bad}");
        }
        assert!(query_from_args(&json!({}), false).is_ok());
        assert!(query_from_args(&json!({"nsfw": true}), true).is_ok());
    }

    #[test]
    fn builds_urls_and_normalizes_links() {
        let query = query_from_args(
            &json!({"query": "forest, night", "sort": 5, "ratio": 3, "width": 1920, "height": 1080}),
            false,
        )
        .unwrap();
        assert_eq!(
            build_search_url(&query, false),
            "https://wallhaven.cc/api/v1/search?categories=111&purity=100&page=1&sorting=toplist&order=desc\
             &q=forest%20night&atleast=1920x1080&ratios=16x9"
        );
        assert_eq!(
            normalize_search_query("https://wallhaven.cc/w/abc123"),
            "id:abc123"
        );
        assert_eq!(
            normalize_search_query("https://wallhaven.cc/search?q=cats+dogs&page=2"),
            "cats dogs"
        );
        let relevance = query_from_args(&json!({"sort": 0, "page": 2}), false).unwrap();
        assert!(build_search_url(&relevance, false).contains("&page=3&"));
        assert_eq!(normalize_total_pages(&relevance, 5), 4);
    }

    #[test]
    fn trusted_image_hosts_only() {
        assert!(is_image_url(
            "https://w.wallhaven.cc/full/ab/wallhaven-abc123.jpg",
            false
        ));
        assert!(is_image_url(
            "https://th.wallhaven.cc/small/ab/abc123.jpg",
            true
        ));
        assert!(!is_image_url(
            "https://th.wallhaven.cc/small/ab/abc123.jpg",
            false
        ));
        for bad in [
            "http://w.wallhaven.cc/a.jpg",
            "https://w.wallhaven.cc.evil.example/a.jpg",
            "https://user@w.wallhaven.cc/a.jpg",
            "https://w.wallhaven.cc/a.jpg?x=1",
            "https://w.wallhaven.cc:8443/a.jpg",
        ] {
            assert!(!is_image_url(bad, false), "{bad}");
        }
    }

    #[test]
    fn search_thumbnails_and_download_round_trip() {
        let dir = temp("roundtrip");
        let body = json!({"data": [
            {"id": "abc123", "path": "https://w.wallhaven.cc/full/ab/wallhaven-abc123.jpg", "file_type": "image/jpeg",
             "resolution": "1920x1080", "thumbs": {"large": "https://th.wallhaven.cc/lg/ab/abc123.jpg"}},
            {"id": "bad", "path": "https://evil.example/x.jpg", "file_type": "image/jpeg", "resolution": "1x1"},
            {"id": "def456", "path": "https://w.wallhaven.cc/full/de/wallhaven-def456.png", "file_type": "image/png",
             "resolution": "1080x1920"}
        ], "meta": {"last_page": 3}});
        let mut responses = HashMap::new();
        let mut url_query = query_from_args(&json!({}), false).unwrap();
        url_query.page = 1;
        responses.insert(
            build_search_url(&url_query, false),
            serde_json::to_vec(&body).unwrap(),
        );
        responses.insert(
            "https://th.wallhaven.cc/lg/ab/abc123.jpg".to_string(),
            b"\xff\xd8\xffthumb".to_vec(),
        );
        responses.insert(
            "https://w.wallhaven.cc/full/ab/wallhaven-abc123.jpg".to_string(),
            b"\xff\xd8\xffpicture".to_vec(),
        );
        let wh = Wallhaven::with_fetch(Arc::new(FakeFetch(Mutex::new(responses))));
        let context = ctx(&dir);

        let id = wh.search(&context, &json!({}), ":1.5").unwrap();
        let state = wait_idle(&wh, id, ":1.5");
        assert_eq!(state["error"], "");
        assert_eq!(state["pages"], 3);
        let items = state["items"].as_array().unwrap();
        assert_eq!(items.len(), 2, "untrusted host dropped");
        assert!(items[0]["thumbnail"]
            .as_str()
            .unwrap()
            .ends_with("abc123.jpg"));
        assert!(
            wh.result(id, ":1.9").is_err(),
            "other clients cannot read the job"
        );

        assert!(wh.download(&context, id, "zzzzzz", ":1.5").is_err());
        let download = wh.download(&context, id, "abc123", ":1.5").unwrap();
        let done = wait_idle(&wh, download, ":1.5");
        let path = PathBuf::from(done["path"].as_str().unwrap());
        assert_eq!(std::fs::read(&path).unwrap(), b"\xff\xd8\xffpicture");
        assert_eq!(wh.take_downloaded(), vec![path]);
    }

    #[test]
    fn rejects_non_images_and_enforces_limits() {
        let dir = temp("limits");
        let mut responses = HashMap::new();
        let body = json!({"data": [{"id": "abc123", "path": "https://w.wallhaven.cc/a.jpg",
            "file_type": "image/jpeg", "resolution": "1x1"}], "meta": {"last_page": 1}});
        responses.insert(
            build_search_url(&query_from_args(&json!({}), false).unwrap(), false),
            serde_json::to_vec(&body).unwrap(),
        );
        responses.insert(
            "https://w.wallhaven.cc/a.jpg".to_string(),
            b"<html>nope</html>".to_vec(),
        );
        let wh = Wallhaven::with_fetch(Arc::new(FakeFetch(Mutex::new(responses))));
        let context = ctx(&dir);
        let id = wh.search(&context, &json!({}), ":1.1").unwrap();
        wait_idle(&wh, id, ":1.1");
        let download = wh.download(&context, id, "abc123", ":1.1").unwrap();
        let state = wait_idle(&wh, download, ":1.1");
        assert!(state["error"]
            .as_str()
            .unwrap()
            .contains("could not be downloaded"));

        // Four jobs per client.
        let mut ids = vec![id, download];
        while ids.len() < 4 {
            ids.push(wh.search(&context, &json!({}), ":1.1").unwrap());
        }
        assert!(wh.search(&context, &json!({}), ":1.1").is_err());
        wh.disconnect(":1.1");
        assert!(wh.result(id, ":1.1").is_err());
        assert!(wh.search(&context, &json!({}), ":1.1").is_ok());
    }

    /// Parity with `net/wallhaven_client.cpp` (`services/helix-desktop/tools/gen_wallhaven_golden.cpp`).
    #[test]
    fn matches_cpp_client() {
        let golden = include_str!("../tests/golden/wallhaven.tsv");
        let items: Vec<Item> = [
            "1920x1080",
            "1080x1920",
            "3440x1440",
            "1000x1000",
            "2560x1600",
            "1600x1200",
            "bogus",
            "0x5",
        ]
        .iter()
        .map(|r| Item {
            id: (*r).into(),
            path: "p".into(),
            resolution: (*r).into(),
            ..Default::default()
        })
        .collect();
        let mut rows = 0;
        for line in golden.lines() {
            let f: Vec<&str> = line.split('\t').collect();
            match f[0] {
                "norm" => assert_eq!(normalize_search_query(f[1]), f[2], "norm {:?}", f[1]),
                "url" => {
                    let flag = |i: usize| f[i] == "1";
                    let query = Query {
                        tags: f[1].to_string(),
                        category_general: flag(7),
                        category_anime: flag(8),
                        category_people: flag(9),
                        purity_sfw: flag(10),
                        purity_sketchy: flag(11),
                        purity_nsfw: flag(14),
                        sort: f[2].parse().unwrap(),
                        ascending: f[3] == "asc",
                        exact_resolution: flag(16),
                        resolution_width: f[12].to_string(),
                        resolution_height: f[13].to_string(),
                        aspect_ratio_index: f[4].parse().unwrap(),
                        page: f[6].parse().unwrap(),
                    };
                    assert_eq!(build_search_url(&query, flag(15)), f[17], "{line}");
                }
                "pages" => {
                    let query = Query {
                        tags: f[1].to_string(),
                        sort: f[2].parse().unwrap(),
                        ..query_from_args(&json!({}), false).unwrap()
                    };
                    let last: i64 = f[3].parse().unwrap();
                    assert_eq!(
                        normalize_total_pages(&query, last),
                        f[4].parse::<i64>().unwrap(),
                        "{line}"
                    );
                }
                "ratio" => {
                    let kept: Vec<String> =
                        filter_by_aspect_ratio(items.clone(), f[1].parse().unwrap())
                            .into_iter()
                            .map(|i| i.id)
                            .collect();
                    assert_eq!(
                        kept,
                        f[2..].iter().map(|s| s.to_string()).collect::<Vec<_>>(),
                        "{line}"
                    );
                }
                other => panic!("unknown row {other}"),
            }
            rows += 1;
        }
        assert_eq!(rows, 2125);
    }

    #[test]
    fn download_dir_resolution_order() {
        assert_eq!(resolve_download_dir("/a", "/b"), PathBuf::from("/a"));
        assert_eq!(resolve_download_dir("  ", "/b"), PathBuf::from("/b"));
    }
}
