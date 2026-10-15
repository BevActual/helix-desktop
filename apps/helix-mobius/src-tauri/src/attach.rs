//! Pick and read text attachments for the next Mobius turn.
//! Binary / oversized files are refused; contents stay in-memory until send.

use helix_chat::chat::{MAX_ATTACH_CHARS, MAX_ATTACH_FILES, MAX_ATTACH_FILE_CHARS};
use serde::Serialize;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Clone, Debug, Serialize)]
pub struct Attachment {
    pub name: String,
    pub path: String,
    pub chars: usize,
    pub original_chars: usize,
    pub content: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct Selection {
    pub files: Vec<Attachment>,
    pub warnings: Vec<String>,
    pub max_files: usize,
    pub max_chars: usize,
}

const TEXT_EXTENSIONS: &[&str] = &[
    "txt", "md", "markdown", "rst", "log", "csv", "tsv", "json", "toml", "yaml", "yml", "xml",
    "html", "htm", "css", "scss", "less", "js", "mjs", "cjs", "ts", "tsx", "jsx", "rs", "py",
    "go", "java", "kt", "swift", "c", "h", "cpp", "cc", "cxx", "hpp", "cs", "rb", "php", "sh",
    "bash", "zsh", "fish", "ps1", "sql", "graphql", "env", "ini", "cfg", "conf", "desktop",
    "service", "timer", "svg", "dockerfile", "makefile", "cmake", "gradle", "properties",
];

fn is_probably_text(path: &Path, bytes: &[u8]) -> bool {
    if bytes.contains(&0) {
        return false;
    }
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        let ext = ext.to_ascii_lowercase();
        if TEXT_EXTENSIONS.iter().any(|e| *e == ext) {
            return true;
        }
    }
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    matches!(
        name.as_str(),
        "makefile" | "dockerfile" | "gemfile" | "rakefile" | "procfile" | "cmakelists.txt"
    ) || std::str::from_utf8(bytes).is_ok()
}

/// Files that hold secrets are never sent to a model, whatever the person picked: a private key or a credentials file is one slip away
/// from leaving the computer when the backend is a remote one.
fn is_secret_file(path: &Path, text: &str) -> bool {
    let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("").to_ascii_lowercase();
    let by_name = matches!(
        name.as_str(),
        "id_rsa" | "id_dsa" | "id_ecdsa" | "id_ed25519" | ".netrc" | "_netrc" | ".pgpass" | "credentials" | ".git-credentials" | "secrets.toml" | "secrets.json" | "secrets.yaml" | "secrets.yml" | "kdbx"
    ) || name.ends_with(".pem")
        || name.ends_with(".key")
        || name.ends_with(".p12")
        || name.ends_with(".pfx")
        || name.ends_with(".kdbx")
        || name.ends_with(".gpg");
    by_name || text.contains("-----BEGIN") && text.contains("PRIVATE KEY-----")
}

fn pick_paths(pictures: bool) -> Result<Vec<PathBuf>, String> {
    // Prefer portal-friendly zenity; fall back to kdialog.
    if let Ok(output) = Command::new("zenity")
        .args([
            "--file-selection",
            "--multiple",
            "--separator=\n",
            if pictures { "--title=Add pictures to Mobius" } else { "--title=Attach files to Mobius" },
            if pictures { "--file-filter=Pictures | *.png *.jpg *.jpeg *.gif *.webp *.bmp" } else { "--file-filter=All files | *" },
        ])
        .output()
    {
        if output.status.success() {
            let text = String::from_utf8_lossy(&output.stdout);
            let paths: Vec<PathBuf> = text
                .lines()
                .map(str::trim)
                .filter(|l| !l.is_empty())
                .map(PathBuf::from)
                .collect();
            if !paths.is_empty() {
                return Ok(paths);
            }
        } else if output.status.code() == Some(1) {
            return Ok(Vec::new()); // cancelled
        }
    }
    if let Ok(output) = Command::new("kdialog")
        .args(["--getopenfilename", "--multiple", "--separate-output", "."])
        .output()
    {
        if output.status.success() {
            let text = String::from_utf8_lossy(&output.stdout);
            // One path per line (`--separate-output`): a path may well have spaces in it.
            let paths: Vec<PathBuf> = text
                .lines()
                .map(str::trim)
                .filter(|l| !l.is_empty())
                .map(PathBuf::from)
                .collect();
            return Ok(paths);
        } else if output.status.code() == Some(1) {
            return Ok(Vec::new());
        }
    }
    Err("No file picker available (install zenity or kdialog).".into())
}

fn read_one(path: &Path) -> Result<Attachment, String> {
    let meta = std::fs::metadata(path).map_err(|e| format!("Could not read {}: {e}", path.display()))?;
    if !meta.is_file() {
        return Err(format!("{} is not a file.", path.display()));
    }
    if meta.len() > 512 * 1024 {
        return Err(format!(
            "{} is larger than 512 KiB. Attach a smaller excerpt.",
            path.file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("file")
        ));
    }
    // The file may grow after metadata was read; bound the actual read as well.
    let mut bytes = Vec::new();
    std::fs::File::open(path)
        .and_then(|file| file.take(512 * 1024 + 1).read_to_end(&mut bytes))
        .map_err(|e| format!("Could not read {}: {e}", path.display()))?;
    if bytes.len() > 512 * 1024 {
        return Err(format!("{} is larger than 512 KiB. Attach a smaller excerpt.", path.display()));
    }
    if !is_probably_text(path, &bytes) {
        return Err(format!(
            "{} looks binary. Attach text or source files.",
            path.file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("file")
        ));
    }
    if is_secret_file(path, &String::from_utf8_lossy(&bytes)) {
        return Err(format!(
            "{} looks like it holds a key or credentials, so it is not attached.",
            path.file_name().and_then(|n| n.to_str()).unwrap_or("file")
        ));
    }
    let mut text = String::from_utf8_lossy(&bytes).into_owned();
    // Normalize NUL already refused; drop other awkward controls except tab/newline.
    text = text
        .chars()
        .filter(|c| *c == '\n' || *c == '\t' || !c.is_control())
        .collect();
    let original_chars = text.chars().count();
    if original_chars > MAX_ATTACH_FILE_CHARS {
        text = text.chars().take(MAX_ATTACH_FILE_CHARS).collect();
    }
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("file")
        .chars()
        .take(120)
        .collect::<String>();
    let chars = text.chars().count();
    Ok(Attachment {
        name,
        path: path.display().to_string(),
        chars,
        original_chars,
        content: text,
    })
}

/// Open a native multi-file picker and return readable text attachments.
pub fn pick_and_read() -> Result<Selection, String> {
    Ok(read_selection(&pick_paths(false)?))
}

fn read_selection(paths: &[PathBuf]) -> Selection {
    let mut files = Vec::new();
    let mut warnings = Vec::new();
    for path in paths.iter().take(MAX_ATTACH_FILES) {
        match read_one(path) {
            Ok(file) => files.push(file),
            Err(reason) => warnings.push(reason),
        }
    }
    if paths.len() > MAX_ATTACH_FILES {
        warnings.push(format!("Only the first {MAX_ATTACH_FILES} selected files were checked. Add the remaining files separately."));
    }
    Selection { files, warnings, max_files: MAX_ATTACH_FILES, max_chars: MAX_ATTACH_CHARS }
}

/// Read explicit paths (e.g. future drag-drop). Same safety checks as the picker.
#[allow(dead_code)]
pub fn read_paths(paths: &[String]) -> Result<Vec<Attachment>, String> {
    let mut out = Vec::new();
    for path in paths.iter().take(MAX_ATTACH_FILES) {
        let path = PathBuf::from(path.trim());
        if path.as_os_str().is_empty() {
            continue;
        }
        out.push(read_one(&path)?);
    }
    Ok(out)
}

/// Choose pictures with the system picker; returns their bytes (the caller checks and stores them).
pub fn pick_pictures() -> Result<Vec<Vec<u8>>, String> {
    let mut out = Vec::new();
    for path in pick_paths(true)?.into_iter().take(4) {
        let meta = std::fs::metadata(&path).map_err(|_| "Could not read that file.".to_string())?;
        if !meta.is_file() || meta.len() > crate::images::MAX_BYTES as u64 {
            return Err("Pictures can be up to 8 MB.".into());
        }
        out.push(std::fs::read(&path).map_err(|_| "Could not read that file.".to_string())?);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys_and_credentials_are_not_attached() {
        for name in ["id_rsa", "id_ed25519", ".netrc", "server.pem", "tls.key", "store.p12", "vault.kdbx", ".git-credentials"] {
            assert!(is_secret_file(Path::new(&format!("/home/me/{name}")), ""), "{name}");
        }
        assert!(is_secret_file(Path::new("/tmp/notes.txt"), "x\n-----BEGIN OPENSSH PRIVATE KEY-----\nabc"));
        assert!(!is_secret_file(Path::new("/tmp/notes.txt"), "a public key is -----BEGIN PUBLIC KEY----- fine"));
        assert!(!is_secret_file(Path::new("/src/main.rs"), "fn main() {}"));
    }

    #[test]
    fn a_secret_file_is_refused_when_read() {
        let dir = std::env::temp_dir().join(format!("mobius-attach-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let key = dir.join("id_ed25519");
        std::fs::write(&key, "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n-----END OPENSSH PRIVATE KEY-----\n").unwrap();
        let note = dir.join("note.md");
        std::fs::write(&note, "hello").unwrap();
        assert!(read_one(&key).unwrap_err().contains("key or credentials"));
        assert_eq!(read_one(&note).unwrap().content, "hello");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn selection_preserves_excerpt_counts_and_partial_errors() {
        let dir = std::env::temp_dir().join(format!("mobius-selection-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let note = dir.join("long.txt");
        let binary = dir.join("binary.txt");
        std::fs::write(&note, "é".repeat(7000)).unwrap();
        let mut bytes = vec![b'a'; 9000];
        bytes.push(0);
        std::fs::write(&binary, bytes).unwrap();
        let selected = read_selection(&[note, binary]);
        assert_eq!(selected.files.len(), 1);
        assert_eq!(selected.files[0].original_chars, 7000);
        assert_eq!(selected.files[0].chars, 6000);
        assert_eq!(selected.files[0].content.chars().count(), 6000);
        assert_eq!(selected.warnings.len(), 1);
        assert!(selected.warnings[0].contains("binary"));
        assert_eq!(selected.max_chars, MAX_ATTACH_CHARS);
        let empty = read_selection(&[]);
        assert!(empty.files.is_empty() && empty.warnings.is_empty());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn binary_and_text_are_told_apart() {
        assert!(!is_probably_text(Path::new("a.bin"), &[0, 1, 2]));
        assert!(is_probably_text(Path::new("a.txt"), b"hello"));
    }
}
