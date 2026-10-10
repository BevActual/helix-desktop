//! Pick and read text attachments for the next Mobius turn.
//! Binary / oversized files are refused; contents stay in-memory until send.

use helix_shell_core::chat::{MAX_ATTACH_FILES, MAX_ATTACH_FILE_CHARS};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Clone, Debug, Serialize)]
pub struct Attachment {
    pub name: String,
    pub path: String,
    pub chars: usize,
    pub content: String,
}

const TEXT_EXTENSIONS: &[&str] = &[
    "txt", "md", "markdown", "rst", "log", "csv", "tsv", "json", "toml", "yaml", "yml", "xml",
    "html", "htm", "css", "scss", "less", "js", "mjs", "cjs", "ts", "tsx", "jsx", "rs", "py",
    "go", "java", "kt", "swift", "c", "h", "cpp", "cc", "cxx", "hpp", "cs", "rb", "php", "sh",
    "bash", "zsh", "fish", "ps1", "sql", "graphql", "env", "ini", "cfg", "conf", "desktop",
    "service", "timer", "svg", "dockerfile", "makefile", "cmake", "gradle", "properties",
];

fn is_probably_text(path: &Path, bytes: &[u8]) -> bool {
    if bytes.iter().take(8000).any(|&b| b == 0) {
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

fn pick_paths() -> Result<Vec<PathBuf>, String> {
    // Prefer portal-friendly zenity; fall back to kdialog.
    if let Ok(output) = Command::new("zenity")
        .args([
            "--file-selection",
            "--multiple",
            "--separator=\n",
            "--title=Attach files to Mobius",
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
        .args(["--getopenfilename", "--multiple", "."])
        .output()
    {
        if output.status.success() {
            let text = String::from_utf8_lossy(&output.stdout);
            let paths: Vec<PathBuf> = text
                .split(|c| c == '\n' || c == ' ')
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
    let bytes =
        std::fs::read(path).map_err(|e| format!("Could not read {}: {e}", path.display()))?;
    if !is_probably_text(path, &bytes) {
        return Err(format!(
            "{} looks binary. Attach text or source files.",
            path.file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("file")
        ));
    }
    let mut text = String::from_utf8_lossy(&bytes).into_owned();
    // Normalize NUL already refused; drop other awkward controls except tab/newline.
    text = text
        .chars()
        .filter(|c| *c == '\n' || *c == '\t' || !c.is_control())
        .collect();
    if text.chars().count() > MAX_ATTACH_FILE_CHARS {
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
        content: text,
    })
}

/// Open a native multi-file picker and return readable text attachments.
pub fn pick_and_read() -> Result<Vec<Attachment>, String> {
    let paths = pick_paths()?;
    if paths.is_empty() {
        return Ok(Vec::new());
    }
    let mut out = Vec::new();
    let mut errors = Vec::new();
    for path in paths.into_iter().take(MAX_ATTACH_FILES) {
        match read_one(&path) {
            Ok(att) => out.push(att),
            Err(reason) => errors.push(reason),
        }
    }
    if out.is_empty() && !errors.is_empty() {
        return Err(errors.join(" "));
    }
    if !errors.is_empty() {
        eprintln!("MOBIUS attachment warnings: {}", errors.join("; "));
    }
    Ok(out)
}

/// Read explicit paths (e.g. future drag-drop). Same safety checks as the picker.
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
