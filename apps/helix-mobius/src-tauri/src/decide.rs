//! What kind of message is this? A small sentence-embedding model (helix-embed, on the CPU, in this process) compares the text with a few
//! example sentences per route and says how sure it is. Used for the composer's "this looks like a desktop action" hint; there is no
//! engine to start and no chat model is swapped out. Measured in `docs/design/decision-model.md`.
//!
//! The model is optional: a folder with `config.json`, `tokenizer.json` and `model.safetensors` (bge-small-en-v1.5) in
//! `$HELIX_EMBED_DIR`, else the models folder's `embed/bge-small-en-v1.5` (Settings → Library downloads it there), else
//! `~/.local/share/helix/embed/bge-small-en-v1.5`. Without it every answer is "not sure".

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::OnceLock;

use helix_embed::{Decider, Embedder, Pooling, Route};
use serde::Serialize;

const ROUTES: &str = include_str!("../routes.json");
/// Below this gap between the best route and the next, the answer is "not sure". The two misses on the 34 test messages had 0.008 and 0.019.
const MIN_MARGIN: f32 = 0.03;

struct Loaded {
    embedder: Embedder,
    decider: Decider,
}

/// Where the embedding model is: `$HELIX_EMBED_DIR`, else the models folder (what Settings → Library downloads into), else the folder
/// earlier versions used. The first of them that holds a model wins.
fn model_dir() -> Option<PathBuf> {
    let from_env = std::env::var_os("HELIX_EMBED_DIR").map(PathBuf::from);
    [
        from_env,
        Some(crate::library::models_dir().join("embed/bge-small-en-v1.5")),
        Some(crate::data_home().join("helix/embed/bge-small-en-v1.5")),
    ]
    .into_iter()
    .flatten()
    .find(|dir| dir.join("model.safetensors").exists())
}

fn load() -> Option<Loaded> {
    let dir = model_dir()?;
    let embedder = Embedder::load(&dir, Pooling::Cls).ok()?;
    let table: BTreeMap<String, Vec<String>> = serde_json::from_str(ROUTES).ok()?;
    let mut routes = Vec::new();
    let mut vectors = Vec::new();
    for (name, examples) in table {
        let refs: Vec<&str> = examples.iter().map(String::as_str).collect();
        vectors.push(embedder.embed(&refs).ok()?);
        routes.push(Route { name, examples });
    }
    let decider = Decider::new(&routes, &vectors);
    Some(Loaded { embedder, decider })
}

static LOADED: OnceLock<Loaded> = OnceLock::new();

/// The loaded model. Looked for again each time until there is one, so a model downloaded while Mobius runs is used from then on.
fn loaded() -> Option<&'static Loaded> {
    if let Some(found) = LOADED.get() {
        return Some(found);
    }
    let fresh = load()?;
    Some(LOADED.get_or_init(|| fresh))
}

#[derive(Serialize)]
pub struct Decision {
    route: String,
    margin: f32,
}

/// The loaded embedding model, if one is installed. The knowledge store reads passages and questions with the same one.
pub fn embedder() -> Option<&'static Embedder> {
    loaded().map(|l| &l.embedder)
}

/// The route the text most resembles, when that is clear; `None` when it is not, the text is too short to tell, or no model is installed.
pub fn decide(text: &str) -> Option<Decision> {
    let text: String = text.trim().chars().take(300).collect();
    if text.chars().count() < 6 {
        return None;
    }
    let loaded = loaded()?;
    let vector = loaded.embedder.embed(&[text.as_str()]).ok()?.pop()?;
    let choice = loaded.decider.decide(&vector)?;
    (choice.margin >= MIN_MARGIN).then_some(Decision {
        route: choice.route,
        margin: choice.margin,
    })
}

#[tauri::command]
pub async fn decide_message(text: String) -> Option<Decision> {
    tauri::async_runtime::spawn_blocking(move || decide(&text))
        .await
        .ok()
        .flatten()
}
