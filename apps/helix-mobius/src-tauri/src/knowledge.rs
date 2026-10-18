//! The knowledge store inside Mobius: folders the person chose are read into a local database (`helix-knowledge`) so their files can be
//! searched by words and by meaning. Everything stays on this computer: the database is a file in the data folder, passages are only
//! ever shown to the person or given to a model running here, and removing a folder removes everything read from it.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

use helix_embed::Embedder;
use helix_knowledge::{Embed, NewPage, SearchOptions, Store, Tier};
use serde::{Deserialize, Serialize};

const MODEL: &str = "bge-small-en-v1.5";
/// bge models read a question better with this in front of it.
const QUERY_PREFIX: &str = "Represent this sentence for searching relevant passages: ";
const MAX_FILES: usize = 3000;
const MAX_FILE_BYTES: u64 = 300_000;
const TEXT_EXTENSIONS: &[&str] = &[
    "md", "markdown", "txt", "rst", "org", "adoc", "rs", "py", "js", "mjs", "ts", "tsx", "jsx",
    "go", "c", "h", "cpp", "hpp", "cc", "java", "kt", "swift", "rb", "php", "sh", "bash", "toml",
    "yaml", "yml", "css", "html", "sql", "lua", "mdx",
];
const SKIP_DIRS: &[&str] = &[
    "node_modules",
    "target",
    "dist",
    "build",
    "vendor",
    "venv",
    "__pycache__",
    "site-packages",
    "out",
    "coverage",
];

/// The embedding model as the store wants it.
struct Bge(&'static Embedder);

impl Embed for Bge {
    fn model_id(&self) -> &str {
        MODEL
    }
    fn embed(&self, texts: &[&str]) -> helix_knowledge::Result<Vec<Vec<f32>>> {
        self.0.embed(texts)
    }
    fn embed_query(&self, query: &str) -> helix_knowledge::Result<Vec<f32>> {
        let text = format!("{QUERY_PREFIX}{query}");
        self.0
            .embed(&[text.as_str()])?
            .into_iter()
            .next()
            .ok_or_else(|| "no vector".to_string())
    }
}

fn meaning() -> Option<Bge> {
    crate::decide::embedder().map(Bge)
}

fn db_path() -> PathBuf {
    crate::data_home().join("helix/knowledge.db")
}

fn store() -> Result<&'static Store, String> {
    static STORE: OnceLock<Result<Store, String>> = OnceLock::new();
    STORE
        .get_or_init(|| Store::open(&db_path()))
        .as_ref()
        .map_err(|e| format!("The knowledge store could not be opened: {e}"))
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Folder {
    pub path: String,
    pub added: u64,
    /// Unix seconds of the last finished index; 0 if it never finished.
    pub indexed: u64,
}

fn folders_path() -> PathBuf {
    crate::data_home().join("helix/mobius/knowledge-folders.json")
}

pub fn folders() -> Vec<Folder> {
    std::fs::read_to_string(folders_path())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn save_folders(list: &[Folder]) -> Result<(), String> {
    let path = folders_path();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let text = serde_json::to_string_pretty(list).map_err(|e| e.to_string())?;
    std::fs::write(path, text).map_err(|e| e.to_string())
}

fn pack_of(root: &str) -> String {
    format!("folder:{root}")
}

/// A folder may be indexed when it is a real directory that is not so wide it would swallow the computer.
pub fn check_folder(path: &Path) -> Result<PathBuf, String> {
    let real =
        std::fs::canonicalize(path).map_err(|_| "That folder cannot be read.".to_string())?;
    if !real.is_dir() {
        return Err("That is not a folder.".into());
    }
    let home = std::env::var_os("HOME").map(PathBuf::from);
    // System folders are refused with everything under them; /tmp and /var themselves are too wide, but a project folder inside them is fine.
    let too_wide = real.parent().is_none()
        || home.as_ref().is_some_and(|h| &real == h)
        || ["/proc", "/sys", "/dev", "/run", "/usr", "/etc", "/boot"]
            .iter()
            .any(|p| real.starts_with(p))
        || ["/tmp", "/var"].iter().any(|p| real == Path::new(p));
    if too_wide {
        return Err(
            "Choose a project folder, not your whole home folder or a system folder.".into(),
        );
    }
    Ok(real)
}

/// The text files under `root` worth reading: known text extensions, under 300 KB, outside hidden and build folders, and no symlinks
/// (so a link back up the tree cannot loop). At most 3000; the second value is how many more there were.
pub fn collect_files(root: &Path) -> (Vec<PathBuf>, usize) {
    let mut found = Vec::new();
    let mut extra = 0;
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&dir) else {
            continue;
        };
        let mut entries: Vec<_> = entries.flatten().collect();
        entries.sort_by_key(|e| e.file_name());
        for entry in entries.into_iter().rev() {
            let name = entry.file_name().to_string_lossy().to_string();
            let Ok(kind) = entry.file_type() else {
                continue;
            };
            if kind.is_symlink() || name.starts_with('.') {
                continue;
            }
            let path = entry.path();
            if kind.is_dir() {
                if !SKIP_DIRS.contains(&name.as_str()) {
                    stack.push(path);
                }
            } else if kind.is_file() {
                let ext = path
                    .extension()
                    .and_then(|e| e.to_str())
                    .unwrap_or("")
                    .to_ascii_lowercase();
                let small = entry
                    .metadata()
                    .map(|m| m.len() <= MAX_FILE_BYTES && m.len() > 0)
                    .unwrap_or(false);
                if TEXT_EXTENSIONS.contains(&ext.as_str()) && small {
                    if found.len() < MAX_FILES {
                        found.push(path);
                    } else {
                        extra += 1;
                    }
                }
            }
        }
    }
    found.sort();
    (found, extra)
}

#[derive(Clone, Debug, Default, Serialize, PartialEq, Eq)]
pub struct Summary {
    pub files: usize,
    pub removed: usize,
    pub not_read: usize,
    pub over_limit: usize,
}

/// Reads every file under `root` into the store as a page of the folder's pack, and drops pages whose files are gone. A page whose text did
/// not change costs nothing. `step(done, total)` is called as it goes; `cancel` stops it between files.
pub fn index_folder(
    root: &Path,
    store: &Store,
    embedder: Option<&dyn Embed>,
    step: &dyn Fn(usize, usize),
    cancel: &AtomicBool,
) -> Result<Summary, String> {
    let root_text = root.to_string_lossy().to_string();
    let pack = pack_of(&root_text);
    let (files, over_limit) = collect_files(root);
    let total = files.len();
    let mut seen: HashSet<String> = HashSet::new();
    let mut summary = Summary {
        over_limit,
        ..Summary::default()
    };
    for (index, path) in files.iter().enumerate() {
        step(index, total);
        if cancel.load(Ordering::Relaxed) {
            return Err("Stopped.".into());
        }
        let key = path.to_string_lossy().to_string();
        let Ok(body) = std::fs::read_to_string(path) else {
            summary.not_read += 1;
            continue;
        };
        let title = path
            .strip_prefix(root)
            .unwrap_or(path)
            .to_string_lossy()
            .to_string();
        let mut page = NewPage::note(&key, &title, &body);
        page.kind = "file".into();
        page.tier = Tier::Yours;
        page.pack = Some(pack.clone());
        store.put_page(&page, embedder)?;
        seen.insert(key);
        summary.files += 1;
    }
    for key in store.pack_keys(&pack)? {
        if !seen.contains(&key) {
            store.delete_page(&key)?;
            summary.removed += 1;
        }
    }
    step(total, total);
    Ok(summary)
}

// ---- what the page sees -----------------------------------------------------------------------------------------------------------

#[derive(Clone, Debug, Default, Serialize)]
pub struct Running {
    pub folder: String,
    pub done: usize,
    pub total: usize,
}

static RUNNING: Mutex<Option<Running>> = Mutex::new(None);
static CANCEL: AtomicBool = AtomicBool::new(false);
static LAST_ERROR: Mutex<String> = Mutex::new(String::new());

#[derive(Clone, Serialize)]
pub struct FolderView {
    pub path: String,
    pub indexed: u64,
    pub pages: usize,
    pub passages: usize,
    pub with_meaning: usize,
}

#[derive(Clone, Serialize)]
pub struct Status {
    /// The model that reads meaning is installed. Without it files are still searchable by words.
    pub meaning: bool,
    pub folders: Vec<FolderView>,
    pub running: Option<Running>,
    pub error: String,
    pub total_pages: usize,
}

pub fn status() -> Status {
    let list = folders();
    let (views, total) = match store() {
        Ok(store) => {
            let views = list
                .iter()
                .map(|f| {
                    let stats = store.pack_stats(&pack_of(&f.path)).unwrap_or_default();
                    FolderView {
                        path: f.path.clone(),
                        indexed: f.indexed,
                        pages: stats.pages,
                        passages: stats.chunks,
                        with_meaning: stats.embedded,
                    }
                })
                .collect();
            (views, store.stats().map(|s| s.pages).unwrap_or(0))
        }
        Err(_) => (Vec::new(), 0),
    };
    Status {
        meaning: crate::decide::embedder().is_some(),
        folders: views,
        running: RUNNING.lock().ok().and_then(|r| r.clone()),
        error: LAST_ERROR.lock().map(|e| e.clone()).unwrap_or_default(),
        total_pages: total,
    }
}

/// Starts reading a folder in the background. Only one runs at a time.
pub fn start(root: PathBuf, notify: impl Fn() + Send + Sync + 'static) -> Result<(), String> {
    let store = store()?;
    {
        let mut running = RUNNING.lock().map_err(|_| "busy".to_string())?;
        if running.is_some() {
            return Err("A folder is being read already. Wait for it, or stop it.".into());
        }
        *running = Some(Running {
            folder: root.to_string_lossy().to_string(),
            done: 0,
            total: 0,
        });
    }
    CANCEL.store(false, Ordering::Relaxed);
    if let Ok(mut error) = LAST_ERROR.lock() {
        error.clear();
    }
    std::thread::spawn(move || {
        let root_text = root.to_string_lossy().to_string();
        let embedder = meaning();
        let last = Mutex::new(std::time::Instant::now());
        let result = index_folder(
            &root,
            store,
            embedder.as_ref().map(|e| e as &dyn Embed),
            &|done, total| {
                if let Ok(mut running) = RUNNING.lock() {
                    if let Some(r) = running.as_mut() {
                        r.done = done;
                        r.total = total;
                    }
                }
                if let Ok(mut at) = last.lock() {
                    if at.elapsed() >= std::time::Duration::from_millis(400) {
                        *at = std::time::Instant::now();
                        notify();
                    }
                }
            },
            &CANCEL,
        );
        match result {
            Ok(_) => {
                let mut list = folders();
                if let Some(folder) = list.iter_mut().find(|f| f.path == root_text) {
                    folder.indexed = crate::now();
                }
                let _ = save_folders(&list);
            }
            Err(message) if message == "Stopped." => {}
            Err(message) => {
                if let Ok(mut error) = LAST_ERROR.lock() {
                    *error = message;
                }
            }
        }
        if let Ok(mut running) = RUNNING.lock() {
            *running = None;
        }
        notify();
    });
    Ok(())
}

pub fn cancel() {
    CANCEL.store(true, Ordering::Relaxed);
}

pub fn add_folder(path: &Path) -> Result<PathBuf, String> {
    let real = check_folder(path)?;
    let text = real.to_string_lossy().to_string();
    let mut list = folders();
    if list.iter().any(|f| f.path == text) {
        return Err("That folder is already there.".into());
    }
    if list.len() >= 20 {
        return Err("That is plenty of folders. Remove one first.".into());
    }
    list.push(Folder {
        path: text,
        added: crate::now(),
        indexed: 0,
    });
    save_folders(&list)?;
    Ok(real)
}

/// Forgets a folder and everything read from it.
pub fn remove_folder(path: &str) -> Result<(), String> {
    let mut list = folders();
    list.retain(|f| f.path != path);
    save_folders(&list)?;
    store()?.delete_pack(&pack_of(path))?;
    Ok(())
}

#[derive(Serialize)]
pub struct Found {
    pub title: String,
    pub heading: String,
    pub snippet: String,
    pub key: String,
    /// Found by meaning as well as by words.
    pub by_meaning: bool,
}

/// Looks for passages in the indexed folders. Meaning is used when its model is installed; otherwise words only.
pub fn search(query: &str, limit: usize) -> Result<Vec<Found>, String> {
    let query = query.trim();
    if query.chars().count() < 2 {
        return Ok(Vec::new());
    }
    let store = store()?;
    let embedder = meaning();
    let vector = match &embedder {
        Some(e) => Some(e.embed_query(query)?),
        None => None,
    };
    let options = SearchOptions {
        limit,
        tiers: vec![Tier::Yours],
        ..SearchOptions::default()
    };
    let hits = store.search(query, vector.as_deref().map(|v| (MODEL, v)), &options)?;
    Ok(hits
        .into_iter()
        .map(|h| Found {
            title: h.title,
            heading: h.heading,
            snippet: h.text.chars().take(360).collect(),
            key: h.key,
            by_meaning: h.vector_rank.is_some(),
        })
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Toy;
    impl Embed for Toy {
        fn model_id(&self) -> &str {
            "toy"
        }
        fn embed(&self, texts: &[&str]) -> helix_knowledge::Result<Vec<Vec<f32>>> {
            Ok(texts
                .iter()
                .map(|t| {
                    vec![
                        t.matches("cat").count() as f32,
                        t.matches("dog").count() as f32 + 0.01,
                    ]
                })
                .collect())
        }
    }

    fn scratch(tag: &str) -> PathBuf {
        // each call gets its own folder: the tests run side by side
        static COUNT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let n = COUNT.fetch_add(1, Ordering::Relaxed);
        let dir =
            std::env::temp_dir().join(format!("helix-knowledge-{tag}-{}-{n}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn tree() -> PathBuf {
        let root = scratch("tree");
        for (path, text) in [
            ("notes.md", "# Pets\n\nThe cat sleeps all day."),
            ("src/main.rs", "fn main() { println!(\"a dog barks\"); }"),
            ("src/deep/lib.py", "# parse the config\ndef parse(): pass"),
            ("node_modules/pkg/index.js", "module.exports = 1"),
            (".git/config", "[core]"),
            (".hidden/secret.md", "nope"),
            ("big.txt", &"x".repeat(400_000)),
            ("image.png", "not text"),
            ("empty.md", ""),
        ] {
            let file = root.join(path);
            std::fs::create_dir_all(file.parent().unwrap()).unwrap();
            std::fs::write(file, text).unwrap();
        }
        root
    }

    #[test]
    fn collects_text_files_and_skips_hidden_build_big_and_empty() {
        let root = tree();
        let (files, extra) = collect_files(&root);
        let names: Vec<String> = files
            .iter()
            .map(|f| f.strip_prefix(&root).unwrap().to_string_lossy().to_string())
            .collect();
        assert_eq!(names, ["notes.md", "src/deep/lib.py", "src/main.rs"]);
        assert_eq!(extra, 0);
    }

    #[test]
    fn a_symlink_back_up_the_tree_is_not_followed() {
        let root = tree();
        std::os::unix::fs::symlink(&root, root.join("loop")).unwrap();
        let (files, _) = collect_files(&root);
        assert_eq!(files.len(), 3);
    }

    #[test]
    fn reading_a_folder_again_updates_and_forgets_deleted_files() {
        let root = tree();
        let store = Store::in_memory().unwrap();
        let never = AtomicBool::new(false);
        let first = index_folder(&root, &store, Some(&Toy), &|_, _| {}, &never).unwrap();
        assert_eq!((first.files, first.removed), (3, 0));
        let pack = pack_of(&root.to_string_lossy());
        assert_eq!(store.pack_stats(&pack).unwrap().pages, 3);
        let hits = store
            .search("cat", None, &SearchOptions::default())
            .unwrap();
        assert_eq!(hits[0].title, "notes.md");
        std::fs::remove_file(root.join("notes.md")).unwrap();
        std::fs::write(
            root.join("src/main.rs"),
            "fn main() { println!(\"a hippo\"); }",
        )
        .unwrap();
        let second = index_folder(&root, &store, Some(&Toy), &|_, _| {}, &never).unwrap();
        assert_eq!((second.files, second.removed), (2, 1));
        assert!(store
            .search("cat", None, &SearchOptions::default())
            .unwrap()
            .is_empty());
        assert!(!store
            .search("hippo", None, &SearchOptions::default())
            .unwrap()
            .is_empty());
    }

    #[test]
    fn stopping_leaves_what_was_read_and_removing_a_folder_removes_it_all() {
        let root = tree();
        let store = Store::in_memory().unwrap();
        let stop = AtomicBool::new(false);
        let result = index_folder(
            &root,
            &store,
            None,
            &|done, _| {
                if done == 1 {
                    stop.store(true, Ordering::Relaxed);
                }
            },
            &stop,
        );
        assert_eq!(result, Err("Stopped.".to_string()));
        let pack = pack_of(&root.to_string_lossy());
        assert_eq!(store.pack_stats(&pack).unwrap().pages, 1);
        assert_eq!(store.delete_pack(&pack).unwrap(), 1);
        assert_eq!(store.stats().unwrap().chunks, 0);
    }

    #[test]
    fn folders_too_wide_are_refused() {
        for path in ["/", "/usr", "/etc", "/tmp"] {
            assert!(check_folder(Path::new(path)).is_err(), "{path}");
        }
        if let Some(home) = std::env::var_os("HOME") {
            assert!(check_folder(Path::new(&home)).is_err());
        }
        assert!(check_folder(Path::new("/nonexistent/folder/xyz")).is_err());
        let inside = scratch("inside");
        assert!(
            check_folder(&inside).is_ok(),
            "a project folder under /tmp is allowed"
        );
    }
}
