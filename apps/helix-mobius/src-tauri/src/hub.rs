//! Finding models on Hugging Face and getting one, with the library's own safeguards.
//!
//! Only `https://huggingface.co` is asked. A search sends the person's search words and nothing else; a repository's file list comes
//! from Hugging Face's own API, and a file is offered only if that list gives its SHA-256, which the download is then checked against
//! (the page never supplies a checksum). A model that is gated, too large for the disk or for the size limit, or whose file name is not
//! a plain `.gguf` name, is not offered. The person confirms each download, which is only ever started from a button.

use std::collections::BTreeMap;

use helix_chat::model_catalog;
use serde::Serialize;
use serde_json::Value;

const API: &str = "https://huggingface.co/api/models";
/// The largest single download offered, in bytes. Larger models do not run on the computers this is for.
pub const MAX_BYTES: u64 = 40_000_000_000;
/// A reply from the API larger than this is not read.
const MAX_REPLY: usize = 8_000_000;

/// Makers whose repositories are widely used and whose names are hard to imitate by accident. A tag, not a guarantee.
const KNOWN: &[&str] = &[
    "Qwen",
    "google",
    "meta-llama",
    "microsoft",
    "mistralai",
    "unsloth",
    "bartowski",
    "ggml-org",
    "lmstudio-community",
    "nvidia",
    "deepseek-ai",
    "ibm-granite",
    "HuggingFaceTB",
    "openai",
    "ggerganov",
    "TheBloke",
    "allenai",
    "CohereLabs",
];

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct Repo {
    pub id: String,
    pub downloads: u64,
    pub likes: u64,
    pub license: String,
    /// Reads pictures as well as text (Hugging Face's own label for it).
    pub sees_pictures: bool,
    pub known_maker: bool,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct VFile {
    pub name: String,
    pub bytes: u64,
    pub sha256: String,
}

/// One way to get a model: a single file, or a set of numbered parts that load as one. `label` is its quantisation ("Q4_K_M").
#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct Variant {
    pub label: String,
    pub files: Vec<VFile>,
    pub bytes: u64,
}

#[derive(Clone, Debug, Serialize)]
pub struct RepoView {
    pub repo: Repo,
    pub variants: Vec<VariantView>,
    /// Why nothing is offered, when nothing is.
    pub note: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct VariantView {
    #[serde(flatten)]
    pub variant: Variant,
    /// `gpu`, `tight`, `cpu` or `no`, as the library says it.
    pub fit: String,
    /// The first file's name: what a download is asked for by, and what the model is called here.
    pub key: String,
}

/// `owner/name`, as Hugging Face writes it.
pub fn valid_repo(id: &str) -> bool {
    let part = |p: &str| {
        !p.is_empty()
            && p.len() <= 96
            && !p.starts_with('.')
            && p.bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'))
    };
    id.split_once('/')
        .is_some_and(|(owner, name)| part(owner) && part(name) && !name.contains('/'))
}

/// What a person pasted: a repository page or file link on huggingface.co, or a bare `owner/name`. Anything else is refused.
pub fn parse_input(text: &str) -> Option<String> {
    let text = text.trim().trim_end_matches('/');
    let path = match text.strip_prefix("https://") {
        Some(rest) => {
            let (host, path) = rest.split_once('/')?;
            if !(host == "huggingface.co" || host == "www.huggingface.co") {
                return None;
            }
            path
        }
        None if text.starts_with("http://") => return None,
        None => text,
    };
    let mut parts = path.split('/');
    let (owner, name) = (parts.next()?, parts.next()?);
    let id = format!("{owner}/{name}");
    valid_repo(&id).then_some(id)
}

pub fn known_maker(repo: &str) -> bool {
    repo.split('/')
        .next()
        .is_some_and(|owner| KNOWN.iter().any(|k| k.eq_ignore_ascii_case(owner)))
}

fn license_of(tags: &[String], card: Option<&Value>) -> String {
    if let Some(l) = tags.iter().find_map(|t| t.strip_prefix("license:")) {
        return l.to_string();
    }
    card.and_then(|c| c.get("license"))
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string()
}

fn repo_from(entry: &Value) -> Option<Repo> {
    let id = entry.get("id")?.as_str()?.to_string();
    if !valid_repo(&id) {
        return None;
    }
    let tags: Vec<String> = entry
        .get("tags")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(|t| t.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default();
    Some(Repo {
        downloads: entry.get("downloads").and_then(Value::as_u64).unwrap_or(0),
        likes: entry.get("likes").and_then(Value::as_u64).unwrap_or(0),
        license: license_of(&tags, entry.get("cardData")),
        sees_pictures: entry.get("pipeline_tag").and_then(Value::as_str)
            == Some("image-text-to-text"),
        known_maker: known_maker(&id),
        id,
    })
}

pub fn parse_search(reply: &Value) -> Vec<Repo> {
    reply
        .as_array()
        .map(|list| list.iter().filter_map(repo_from).collect())
        .unwrap_or_default()
}

/// The quantisation named in a file name: `Qwen3-4B-Q4_K_M.gguf` is `Q4_K_M`.
fn quant_label(stem: &str) -> String {
    let tokens: Vec<&str> = stem.split(['-', '.']).collect();
    // quantisations contain underscores, so rejoin the pieces after the first token that looks like one
    let start = tokens.iter().position(|t| {
        let t = t.to_ascii_uppercase();
        let digit_after = |prefix: &str| {
            t.strip_prefix(prefix)
                .is_some_and(|rest| rest.starts_with(|c: char| c.is_ascii_digit()))
        };
        digit_after("Q")
            || digit_after("IQ")
            || matches!(t.as_str(), "F16" | "BF16" | "F32" | "MXFP4")
    });
    match start {
        Some(i) => tokens[i..].join("-").to_ascii_uppercase(),
        None => "default".to_string(),
    }
}

/// Splits `name-00002-of-00005.gguf` into (`name`, 2, 5).
fn shard_of(stem: &str) -> Option<(String, u32, u32)> {
    let (head, total) = stem.rsplit_once("-of-")?;
    let (base, index) = head.rsplit_once('-')?;
    let (index, total) = (index.parse::<u32>().ok()?, total.parse::<u32>().ok()?);
    (index >= 1 && total >= 1 && index <= total).then(|| (base.to_string(), index, total))
}

/// The ways to get a model from a repository's file list: each `.gguf` that is a plain name and has a published SHA-256, with numbered
/// parts grouped (and offered only when every part is there). Picture projectors (`mmproj`) are left out: they are not a model.
pub fn parse_variants(tree: &Value) -> Vec<Variant> {
    let Some(entries) = tree.as_array() else {
        return Vec::new();
    };
    let mut singles: Vec<Variant> = Vec::new();
    let mut sets: BTreeMap<(String, u32), Vec<(u32, VFile)>> = BTreeMap::new();
    for entry in entries {
        if entry.get("type").and_then(Value::as_str) != Some("file") {
            continue;
        }
        let Some(path) = entry.get("path").and_then(Value::as_str) else {
            continue;
        };
        // one folder level is allowed (some repositories keep a quantisation per folder); the file is kept under its own name
        if path.matches('/').count() > 1 {
            continue;
        }
        let name = path.rsplit('/').next().unwrap_or(path);
        if !name.ends_with(".gguf")
            || name.to_ascii_lowercase().contains("mmproj")
            || !model_catalog::safe_file_name(name)
        {
            continue;
        }
        let bytes = entry.get("size").and_then(Value::as_u64).unwrap_or(0);
        let sha = entry
            .get("lfs")
            .and_then(|l| l.get("oid"))
            .and_then(Value::as_str)
            .unwrap_or("");
        if bytes == 0 || sha.len() != 64 || !sha.bytes().all(|b| b.is_ascii_hexdigit()) {
            continue;
        }
        let file = VFile {
            name: path.to_string(),
            bytes,
            sha256: sha.to_ascii_lowercase(),
        };
        let stem = name.trim_end_matches(".gguf");
        match shard_of(stem) {
            Some((base, index, total)) => {
                sets.entry((base, total)).or_default().push((index, file))
            }
            None => singles.push(Variant {
                label: quant_label(stem),
                bytes,
                files: vec![file],
            }),
        }
    }
    for ((base, total), mut parts) in sets {
        parts.sort_by_key(|(i, _)| *i);
        let complete = parts.len() as u32 == total
            && parts
                .iter()
                .enumerate()
                .all(|(n, (i, _))| *i == n as u32 + 1);
        if complete {
            let files: Vec<VFile> = parts.into_iter().map(|(_, f)| f).collect();
            singles.push(Variant {
                label: quant_label(&base),
                bytes: files.iter().map(|f| f.bytes).sum(),
                files,
            });
        }
    }
    singles.sort_by(|a, b| a.bytes.cmp(&b.bytes).then_with(|| a.label.cmp(&b.label)));
    singles
}

/// Tests answer from files in `$HELIX_HUB_FIXTURES` instead of the network: `search.json`, `info-<owner>-<name>.json` and
/// `tree-<owner>-<name>.json`.
fn fixture(dir: &std::path::Path, url: &str) -> Result<Value, String> {
    let rest = url.strip_prefix(API).unwrap_or("");
    let file = if rest.starts_with('?') {
        "search.json".to_string()
    } else {
        let path = rest.trim_start_matches('/').split('?').next().unwrap_or("");
        match path.strip_suffix("/tree/main") {
            Some(repo) => format!("tree-{}.json", repo.replace('/', "-")),
            None => format!("info-{}.json", path.replace('/', "-")),
        }
    };
    let text = std::fs::read_to_string(dir.join(file))
        .map_err(|_| "Hugging Face has no such repository.".to_string())?;
    serde_json::from_str(&text).map_err(|e| e.to_string())
}

async fn get(url: &str) -> Result<Value, String> {
    if !url.starts_with("https://huggingface.co/") {
        return Err("Only huggingface.co is asked.".into());
    }
    if let Some(dir) = std::env::var_os("HELIX_HUB_FIXTURES") {
        return fixture(std::path::Path::new(&dir), url);
    }
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(25))
        .redirect(reqwest::redirect::Policy::none())
        .user_agent("helix-mobius")
        .build()
        .map_err(|e| e.to_string())?;
    let response = client
        .get(url)
        .send()
        .await
        .map_err(|_| "Could not reach huggingface.co.".to_string())?;
    match response.status().as_u16() {
        200 => {}
        401 | 403 => {
            return Err(
                "That repository needs a Hugging Face login, so Mobius cannot download from it."
                    .into(),
            )
        }
        404 => return Err("Hugging Face has no such repository.".into()),
        code => return Err(format!("Hugging Face answered {code}.")),
    }
    let bytes = response
        .bytes()
        .await
        .map_err(|_| "The reply from Hugging Face was cut off.".to_string())?;
    if bytes.len() > MAX_REPLY {
        return Err("The reply from Hugging Face was too large to read.".into());
    }
    serde_json::from_slice(&bytes)
        .map_err(|_| "The reply from Hugging Face was not understood.".to_string())
}

/// Searches Hugging Face for GGUF models. The words typed are the only thing sent.
pub async fn search(query: &str) -> Result<Vec<Repo>, String> {
    let query: String = query.chars().filter(|c| !c.is_control()).take(80).collect();
    let query = query.trim();
    if query.chars().count() < 2 {
        return Err("Type at least two letters to search.".into());
    }
    let encoded: String = query
        .bytes()
        .map(|b| {
            if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.') {
                (b as char).to_string()
            } else {
                format!("%{b:02X}")
            }
        })
        .collect();
    let reply = get(&format!(
        "{API}?search={encoded}&filter=gguf&sort=downloads&direction=-1&limit=24"
    ))
    .await?;
    Ok(parse_search(&reply))
}

/// A repository's details and the ways to get it, with how each would sit on this computer.
pub async fn repo(id: &str, memory: crate::hardware::Memory) -> Result<RepoView, String> {
    if !valid_repo(id) {
        return Err("That is not a Hugging Face repository name (owner/name).".into());
    }
    let info = get(&format!("{API}/{id}")).await?;
    if info
        .get("gated")
        .map(|g| g != &Value::Bool(false))
        .unwrap_or(false)
    {
        return Err("That repository is gated: it needs a Hugging Face login, so Mobius cannot download from it.".into());
    }
    let repo = repo_from(&info).ok_or("That repository could not be read.")?;
    let tree = get(&format!("{API}/{id}/tree/main?recursive=true")).await?;
    let all = parse_variants(&tree);
    let (offered, too_big): (Vec<Variant>, Vec<Variant>) =
        all.into_iter().partition(|v| v.bytes <= MAX_BYTES);
    let variants: Vec<VariantView> = offered
        .into_iter()
        .map(|variant| VariantView {
            fit: crate::hardware::fit(crate::hardware::needed_gb(0.0, variant.bytes), memory)
                .word()
                .to_string(),
            key: variant.files[0].name.clone(),
            variant,
        })
        .collect();
    let note = if variants.is_empty() {
        if too_big.is_empty() {
            "This repository has no .gguf files with a published checksum, so none is offered."
                .to_string()
        } else {
            "Every file here is larger than Mobius will download (40 GB).".to_string()
        }
    } else {
        String::new()
    };
    Ok(RepoView {
        repo,
        variants,
        note,
    })
}

/// The files to download for the variant whose first file is `key`, with the checksums Hugging Face lists, fetched now.
pub async fn spec_for(
    id: &str,
    key: &str,
    free_bytes: Option<u64>,
) -> Result<crate::library::Spec, String> {
    if !valid_repo(id) {
        return Err("That is not a Hugging Face repository name (owner/name).".into());
    }
    let info = get(&format!("{API}/{id}")).await?;
    if info
        .get("gated")
        .map(|g| g != &Value::Bool(false))
        .unwrap_or(false)
    {
        return Err("That repository is gated: it needs a Hugging Face login.".into());
    }
    let repo = repo_from(&info).ok_or("That repository could not be read.")?;
    let tree = get(&format!("{API}/{id}/tree/main?recursive=true")).await?;
    let variant = parse_variants(&tree)
        .into_iter()
        .find(|v| v.files[0].name == key)
        .ok_or("That file is no longer listed by Hugging Face.")?;
    if variant.bytes > MAX_BYTES {
        return Err("That model is larger than Mobius will download (40 GB).".into());
    }
    if let Some(free) = free_bytes {
        let need = variant.bytes + variant.bytes / 20 + 500_000_000;
        if free < need {
            return Err(format!(
                "There is not enough disk space: it needs about {:.1} GB and {:.1} GB is free.",
                need as f64 / 1e9,
                free as f64 / 1e9
            ));
        }
    }
    let files = variant
        .files
        .iter()
        .map(|f| crate::library::SpecFile {
            name: f.name.rsplit('/').next().unwrap_or(&f.name).to_string(),
            url: format!("https://huggingface.co/{id}/resolve/main/{}", f.name),
            sha256: f.sha256.clone(),
            bytes: f.bytes,
        })
        .collect::<Vec<_>>();
    Ok(crate::library::Spec {
        id: files[0].name.clone(),
        files,
        source: Some(crate::library::Source {
            repo: id.to_string(),
            license: repo.license,
        }),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn repository_names_and_pasted_links_are_checked() {
        assert!(valid_repo("Qwen/Qwen3-4B-GGUF"));
        for bad in [
            "", "Qwen", "a/b/c", "../x", "a/..", "a b/c", ".a/b", "a/b?x=1",
        ] {
            assert!(!valid_repo(bad), "{bad}");
        }
        assert_eq!(
            parse_input("Qwen/Qwen3-4B-GGUF").as_deref(),
            Some("Qwen/Qwen3-4B-GGUF")
        );
        assert_eq!(
            parse_input("https://huggingface.co/Qwen/Qwen3-4B-GGUF/").as_deref(),
            Some("Qwen/Qwen3-4B-GGUF")
        );
        assert_eq!(
            parse_input("https://huggingface.co/Qwen/Qwen3-4B-GGUF/blob/main/Qwen3-4B-Q4_K_M.gguf")
                .as_deref(),
            Some("Qwen/Qwen3-4B-GGUF")
        );
        for bad in [
            "http://huggingface.co/a/b",
            "https://evil.example/a/b",
            "https://huggingface.co.evil.example/a/b",
            "https://huggingface.co/",
            "ftp://huggingface.co/a/b",
        ] {
            assert_eq!(parse_input(bad), None, "{bad}");
        }
    }

    #[test]
    fn quantisation_and_parts_are_read_from_names() {
        assert_eq!(quant_label("Qwen3-4B-Q4_K_M"), "Q4_K_M");
        assert_eq!(quant_label("model.IQ4_XS"), "IQ4_XS");
        assert_eq!(quant_label("Qwen3-4B-BF16"), "BF16");
        assert_eq!(quant_label("mystery"), "default");
        assert_eq!(
            shard_of("m-Q4-00002-of-00005"),
            Some(("m-Q4".to_string(), 2, 5))
        );
        assert_eq!(shard_of("m-Q4"), None);
        assert_eq!(shard_of("m-00009-of-00005"), None);
    }

    fn file(path: &str, size: u64, sha: &str) -> Value {
        json!({"type": "file", "path": path, "size": size, "lfs": {"oid": sha}})
    }

    #[test]
    fn a_file_needs_a_published_checksum_and_a_plain_name() {
        let good = "a".repeat(64);
        let tree = json!([
            file("m-Q4_K_M.gguf", 2_000_000_000, &good),
            file("m-Q8_0.gguf", 4_000_000_000, "short"),
            json!({"type": "file", "path": "m-Q2_K.gguf", "size": 900_000_000}),
            file("mmproj-m-f16.gguf", 800_000_000, &good),
            file("my model.gguf", 1_000_000_000, &good),
            file("notes.md", 1000, &good),
            file("deep/er/m-Q3.gguf", 1_000_000_000, &good),
            json!({"type": "directory", "path": "folder"}),
            file("sub/m-Q5_K_M.gguf", 3_000_000_000, &good),
        ]);
        let variants = parse_variants(&tree);
        let labels: Vec<&str> = variants.iter().map(|v| v.label.as_str()).collect();
        assert_eq!(labels, ["Q4_K_M", "Q5_K_M"], "{variants:?}");
        assert_eq!(variants[1].files[0].name, "sub/m-Q5_K_M.gguf");
    }

    #[test]
    fn numbered_parts_are_offered_only_when_every_part_is_there() {
        let sha = "b".repeat(64);
        let whole = json!([
            file("m-Q4-00001-of-00002.gguf", 3_000_000_000, &sha),
            file("m-Q4-00002-of-00002.gguf", 1_000_000_000, &sha)
        ]);
        let v = parse_variants(&whole);
        assert_eq!(
            (v.len(), v[0].files.len(), v[0].bytes),
            (1, 2, 4_000_000_000)
        );
        let missing = json!([
            file("m-Q4-00001-of-00003.gguf", 3_000_000_000, &sha),
            file("m-Q4-00003-of-00003.gguf", 1_000_000_000, &sha)
        ]);
        assert!(parse_variants(&missing).is_empty());
    }

    #[test]
    fn search_results_carry_license_pictures_and_known_makers() {
        let reply = json!([
            {"id": "Qwen/Qwen3-VL-4B-GGUF", "downloads": 10, "likes": 2, "tags": ["gguf", "license:apache-2.0"], "pipeline_tag": "image-text-to-text"},
            {"id": "randomperson/some-model", "tags": ["gguf"]},
            {"id": "bad id/x"}
        ]);
        let repos = parse_search(&reply);
        assert_eq!(repos.len(), 2);
        assert_eq!(
            (
                repos[0].license.as_str(),
                repos[0].sees_pictures,
                repos[0].known_maker
            ),
            ("apache-2.0", true, true)
        );
        assert_eq!(
            (repos[1].license.as_str(), repos[1].known_maker),
            ("", false)
        );
    }

    #[test]
    fn a_download_is_built_from_hugging_faces_listing_and_refused_when_it_should_be() {
        let dir = std::env::temp_dir().join(format!("helix-hub-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let sha = "c".repeat(64);
        let write =
            |name: &str, value: Value| std::fs::write(dir.join(name), value.to_string()).unwrap();
        write(
            "info-acme-tiny-GGUF.json",
            json!({"id": "acme/tiny-GGUF", "gated": false, "tags": ["license:mit"], "downloads": 5}),
        );
        write(
            "tree-acme-tiny-GGUF.json",
            json!([
                file("tiny-Q4_K_M.gguf", 2_000_000_000, &sha),
                file("huge-Q8_0.gguf", 50_000_000_000, &sha)
            ]),
        );
        write(
            "info-acme-locked-GGUF.json",
            json!({"id": "acme/locked-GGUF", "gated": "auto"}),
        );
        write("tree-acme-locked-GGUF.json", json!([]));
        std::env::set_var("HELIX_HUB_FIXTURES", &dir);
        let run = |f: std::pin::Pin<
            Box<dyn std::future::Future<Output = Result<crate::library::Spec, String>>>,
        >| tauri::async_runtime::block_on(f);
        let spec = run(Box::pin(spec_for(
            "acme/tiny-GGUF",
            "tiny-Q4_K_M.gguf",
            Some(100_000_000_000),
        )))
        .unwrap();
        assert_eq!(spec.files.len(), 1);
        assert_eq!(spec.files[0].sha256, sha);
        assert_eq!(
            spec.files[0].url,
            "https://huggingface.co/acme/tiny-GGUF/resolve/main/tiny-Q4_K_M.gguf"
        );
        assert_eq!(
            spec.source.as_ref().map(|s| s.license.as_str()),
            Some("mit")
        );
        assert!(
            run(Box::pin(spec_for("acme/tiny-GGUF", "huge-Q8_0.gguf", None)))
                .unwrap_err()
                .contains("40 GB")
        );
        assert!(run(Box::pin(spec_for(
            "acme/tiny-GGUF",
            "tiny-Q4_K_M.gguf",
            Some(1_000_000_000)
        )))
        .unwrap_err()
        .contains("disk space"));
        assert!(
            run(Box::pin(spec_for("acme/tiny-GGUF", "other.gguf", None)))
                .unwrap_err()
                .contains("no longer listed")
        );
        assert!(run(Box::pin(spec_for("acme/locked-GGUF", "x.gguf", None)))
            .unwrap_err()
            .contains("gated"));
        assert!(run(Box::pin(spec_for("../etc", "x.gguf", None))).is_err());
        std::env::remove_var("HELIX_HUB_FIXTURES");
    }

    #[test]
    fn downloads_follow_redirects_only_to_hugging_faces_hosts() {
        use crate::library::trusted_host;
        for ok in [
            "huggingface.co",
            "cdn-lfs.huggingface.co",
            "us.aws.cdn.hf.co",
            "hf.co",
            "HF.CO",
        ] {
            assert!(trusted_host(ok), "{ok}");
        }
        for bad in [
            "evilhf.co",
            "huggingface.co.evil.example",
            "hf.co.evil.example",
            "example.com",
            "nothuggingface.co",
        ] {
            assert!(!trusted_host(bad), "{bad}");
        }
    }

    /// Downloads a real 1.2 MB model through the whole path: Hugging Face's listing, the redirect to its content host, the checksum, and
    /// the note of where it came from. Needs the network, so it is run by hand:
    ///   cargo test -p helix-mobius real_download -- --ignored
    #[test]
    #[ignore]
    fn real_download_from_hugging_face_is_checked_and_noted() {
        let dir = std::env::temp_dir().join(format!("helix-hub-real-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::env::set_var("HELIX_MODEL_DIR", &dir);
        let found = tauri::async_runtime::block_on(search("stories260K test model")).unwrap();
        assert!(
            found
                .iter()
                .any(|r| r.id == "ggml-org/test-model-stories260K"),
            "{found:?}"
        );
        let downloads = std::sync::Arc::new(crate::library::Downloads::default());
        let spec = tauri::async_runtime::block_on(spec_for(
            "ggml-org/test-model-stories260K",
            "stories260K-f32.gguf",
            crate::library::free_bytes(),
        ))
        .unwrap();
        assert_eq!(spec.files[0].bytes, 1_185_376);
        crate::library::start_spec(&downloads, spec, || {}).unwrap();
        for _ in 0..120 {
            std::thread::sleep(std::time::Duration::from_millis(500));
            if dir.join("stories260K-f32.gguf").is_file() {
                break;
            }
        }
        let file = dir.join("stories260K-f32.gguf");
        assert_eq!(
            std::fs::metadata(&file).unwrap().len(),
            1_185_376,
            "the file was not downloaded"
        );
        assert!(!dir.join("stories260K-f32.gguf.part").exists());
        let noted = std::fs::read_to_string(dir.join("sources.json")).unwrap();
        assert!(noted.contains("ggml-org/test-model-stories260K"), "{noted}");
        let cards = crate::library::list(&downloads, &[]);
        let card = cards
            .iter()
            .find(|c| c.file == "stories260K-f32.gguf")
            .expect("listed");
        assert!(
            !card.in_catalog && card.installed && card.blurb.contains("Checked against"),
            "{}",
            card.blurb
        );
        std::env::remove_var("HELIX_MODEL_DIR");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
