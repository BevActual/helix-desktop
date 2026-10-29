//! The models folder as the engine's clients see it: which `.gguf` files are models, how a model split into shards is listed once, and which
//! vision projector (`mmproj-<model>…gguf`) belongs to a model. Only a file this listing offers can be loaded, so a client cannot point the engine at
//! an arbitrary path or a stray shard. Port of the same logic in `native/inference/llama_backend.cpp`.

use std::path::{Path, PathBuf};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Found {
    /// File name without directory (the first shard for a split model).
    pub id: String,
    pub bytes: u64,
    /// A projector for it is installed.
    pub vision: bool,
}

/// `name-00001-of-00003.gguf` → `("name", 1, 3)`. Only files in that exact shape are shards.
fn parse_shard(file: &str) -> Option<(&str, u32, u32)> {
    let stem = file.strip_suffix(".gguf")?;
    // `-NNNNN-of-NNNNN` is 15 bytes.
    let at = stem.len().checked_sub(15)?;
    let (base, tail) = stem.split_at_checked(at)?;
    let digits = |s: &str| s.len() == 5 && s.bytes().all(|b| b.is_ascii_digit());
    let rest = tail.strip_prefix('-')?;
    let (index, total) = rest.split_once("-of-")?;
    if base.is_empty() || !digits(index) || !digits(total) {
        return None;
    }
    Some((base, index.parse().ok()?, total.parse().ok()?))
}

/// `Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf` → `Qwen2.5-VL-7B-Instruct`; a sharded `name-00001-of-00002.gguf` → `name`.
pub(crate) fn base_of(file: &str) -> String {
    let stem = file.strip_suffix(".gguf").unwrap_or(file);
    let as_file = format!("{stem}.gguf");
    let stem = match parse_shard(&as_file) {
        Some((base, _, _)) => base,
        None => stem,
    };
    // A trailing quantisation tag: `-Q4_K_M`, `-iq3_xs`, `-F16`, `-f32`, `-BF16`.
    if let Some((head, tag)) = stem.rsplit_once('-') {
        let mut chars = tag.chars();
        let quant = match chars.next() {
            Some('i' | 'I') => {
                matches!(chars.next(), Some('q' | 'Q'))
                    && chars.next().is_some_and(|c| c.is_ascii_digit())
            }
            Some('q' | 'Q') => chars.next().is_some_and(|c| c.is_ascii_digit()),
            _ => false,
        } && tag.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
        let float = matches!(tag.to_ascii_lowercase().as_str(), "f16" | "f32" | "bf16");
        if (quant || float) && !head.is_empty() {
            return head.to_string();
        }
    }
    stem.to_string()
}

fn gguf_files(dir: &Path) -> Vec<(String, u64)> {
    let mut files: Vec<(String, u64)> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            let meta = entry.metadata().ok()?;
            let name = entry.file_name().to_string_lossy().into_owned();
            (meta.is_file() && name.ends_with(".gguf")).then_some((name, meta.len()))
        })
        .collect();
    files.sort();
    files
}

/// The projector for `model`, if one is installed (the first by name).
pub fn projector_for(dir: &Path, model: &str) -> Option<PathBuf> {
    let base = base_of(model);
    if base.is_empty() {
        return None;
    }
    let prefix = format!("mmproj-{base}");
    gguf_files(dir)
        .into_iter()
        .find(|(name, _)| name.starts_with(&prefix))
        .map(|(name, _)| dir.join(name))
}

pub fn list(dir: &Path) -> Vec<Found> {
    let files: Vec<(String, u64)> = gguf_files(dir)
        .into_iter()
        // A projector is part of a model, not a model.
        .filter(|(name, _)| !name.starts_with("mmproj"))
        .collect();
    let mut out = Vec::new();
    for (name, bytes) in &files {
        let vision = projector_for(dir, name).is_some();
        match parse_shard(name) {
            None => out.push(Found {
                id: name.clone(),
                bytes: *bytes,
                vision,
            }),
            // A model split into shards is listed once, by its first shard (the engine finds the rest).
            Some((base, 1, total)) => {
                let parts: Vec<&(String, u64)> = files
                    .iter()
                    .filter(|(other, _)| {
                        parse_shard(other).is_some_and(|(b, _, t)| b == base && t == total)
                    })
                    .collect();
                // An incomplete set is not offered.
                if parts.len() == total as usize {
                    out.push(Found {
                        id: name.clone(),
                        bytes: parts.iter().map(|p| p.1).sum(),
                        vision,
                    });
                }
            }
            Some(_) => {}
        }
    }
    out.sort_by(|a, b| a.id.cmp(&b.id));
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("helix-models-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn touch(dir: &Path, name: &str, bytes: usize) {
        std::fs::write(dir.join(name), vec![0u8; bytes]).unwrap();
    }

    #[test]
    fn shards_and_bases_are_read_from_names() {
        assert_eq!(
            parse_shard("qwen-00001-of-00002.gguf"),
            Some(("qwen", 1, 2))
        );
        assert_eq!(parse_shard("qwen-1-of-2.gguf"), None);
        assert_eq!(parse_shard("qwen.gguf"), None);
        assert_eq!(
            base_of("Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf"),
            "Qwen2.5-VL-7B-Instruct"
        );
        assert_eq!(
            base_of("qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf"),
            "qwen2.5-7b-instruct"
        );
        assert_eq!(base_of("llama-3-8b-F16.gguf"), "llama-3-8b");
        assert_eq!(base_of("tiny-iq3_xs.gguf"), "tiny");
        assert_eq!(base_of("plain.gguf"), "plain");
        assert_eq!(base_of("my-model-quest.gguf"), "my-model-quest");
    }

    #[test]
    fn the_folder_is_listed_like_the_engine_lists_it() {
        let dir = scratch("list");
        touch(&dir, "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf", 100);
        touch(&dir, "mmproj-Qwen2.5-VL-7B-Instruct-Q8_0.gguf", 10);
        touch(&dir, "small-q4_k_m.gguf", 30);
        touch(&dir, "split-q4-00001-of-00002.gguf", 40);
        touch(&dir, "split-q4-00002-of-00002.gguf", 50);
        touch(&dir, "broken-00001-of-00003.gguf", 5);
        touch(&dir, "broken-00002-of-00003.gguf", 5);
        touch(&dir, "notes.txt", 5);
        let found = list(&dir);
        let ids: Vec<&str> = found.iter().map(|f| f.id.as_str()).collect();
        assert_eq!(
            ids,
            [
                "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf",
                "small-q4_k_m.gguf",
                "split-q4-00001-of-00002.gguf"
            ]
        );
        assert!(found[0].vision && !found[1].vision);
        assert_eq!(found[2].bytes, 90, "a split model's size is all its parts");
        assert_eq!(
            projector_for(&dir, "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf")
                .unwrap()
                .file_name()
                .unwrap(),
            "mmproj-Qwen2.5-VL-7B-Instruct-Q8_0.gguf"
        );
        assert!(projector_for(&dir, "small-q4_k_m.gguf").is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
