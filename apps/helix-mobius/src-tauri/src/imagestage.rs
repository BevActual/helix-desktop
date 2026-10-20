//! The Image stage's documents: a picture and the steps taken on it. Every step is a new stored picture (the files are never changed), so the
//! original is always one undo away from where the person is, and undo and redo are just a position in the list.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::imageops::{self, Op};

fn you() -> String {
    "you".into()
}

pub const MAX_STEPS: usize = 30;
pub const MAX_DOCS: usize = 20;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Step {
    pub label: String,
    /// Who made the edit: "you", or "mobius" for one the model suggested and the person applied.
    #[serde(default = "you")]
    pub by: String,
    pub op: Op,
    pub picture: String,
    pub width: u32,
    pub height: u32,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Doc {
    pub id: String,
    pub title: String,
    pub base: String,
    pub base_width: u32,
    pub base_height: u32,
    pub steps: Vec<Step>,
    /// How many of the steps are in effect (0 = the original).
    pub cursor: usize,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
struct Store {
    #[serde(default)]
    docs: Vec<Doc>,
}

/// What the page needs to draw a document.
#[derive(Serialize)]
pub struct View {
    #[serde(flatten)]
    pub doc: Doc,
    pub current: String,
    pub width: u32,
    pub height: u32,
    pub can_undo: bool,
    pub can_redo: bool,
}

fn view(doc: Doc) -> View {
    let (current, width, height) = match doc.cursor.checked_sub(1).and_then(|i| doc.steps.get(i)) {
        Some(step) => (step.picture.clone(), step.width, step.height),
        None => (doc.base.clone(), doc.base_width, doc.base_height),
    };
    let (can_undo, can_redo) = (doc.cursor > 0, doc.cursor < doc.steps.len());
    View {
        doc,
        current,
        width,
        height,
        can_undo,
        can_redo,
    }
}

fn file(chat_id: &str) -> PathBuf {
    let key: String = chat_id
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
        .collect();
    let key = if key.is_empty() {
        "scratch".to_string()
    } else {
        key
    };
    crate::data_home()
        .join("helix/mobius/imagestage")
        .join(format!("{key}.json"))
}

fn load(chat_id: &str) -> Store {
    std::fs::read_to_string(file(chat_id))
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn save(chat_id: &str, store: &Store) -> Result<(), String> {
    let path = file(chat_id);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    std::fs::write(
        path,
        serde_json::to_string(store).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())
}

fn find<'a>(store: &'a mut Store, id: &str) -> Result<&'a mut Doc, String> {
    store
        .docs
        .iter_mut()
        .find(|d| d.id == id)
        .ok_or_else(|| "That picture is no longer open.".to_string())
}

#[tauri::command]
pub fn imgstage_list(chat_id: String) -> Vec<View> {
    load(&chat_id).docs.into_iter().map(view).collect()
}

/// Opens a stored picture for editing (the same picture again returns the document already made from it).
#[tauri::command]
pub fn imgstage_open(chat_id: String, picture: String, title: String) -> Result<View, String> {
    let mut store = load(&chat_id);
    if let Some(doc) = store.docs.iter().find(|d| d.base == picture) {
        return Ok(view(doc.clone()));
    }
    if store.docs.len() >= MAX_DOCS {
        return Err(format!(
            "{MAX_DOCS} pictures are already open in this chat. Close one first."
        ));
    }
    let bytes = crate::images::load(&crate::images_dir(), &picture)?;
    let image = imageops::decode(&bytes)?;
    let (base_width, base_height) = image::GenericImageView::dimensions(&image);
    let title: String = title
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(60)
        .collect();
    let doc = Doc {
        id: crate::new_id(),
        title: if title.is_empty() {
            "Picture".into()
        } else {
            title
        },
        base: picture,
        base_width,
        base_height,
        steps: Vec::new(),
        cursor: 0,
    };
    store.docs.push(doc.clone());
    save(&chat_id, &store)?;
    Ok(view(doc))
}

/// Applies operations in order to the picture as it stands; anything undone and not redone is dropped first.
#[tauri::command]
pub fn imgstage_apply(
    chat_id: String,
    doc_id: String,
    ops: Vec<Op>,
    by: Option<String>,
) -> Result<View, String> {
    let by = if by.as_deref() == Some("mobius") {
        "mobius"
    } else {
        "you"
    };
    if ops.is_empty() || ops.len() > 10 {
        return Err("Apply between one and ten edits at a time.".into());
    }
    let mut store = load(&chat_id);
    let doc = find(&mut store, &doc_id)?;
    let dropped: Vec<String> = doc.steps.drain(doc.cursor..).map(|s| s.picture).collect();
    crate::images::remove(&crate::images_dir(), &dropped);
    let mut current = view(doc.clone()).current;
    for op in ops {
        if doc.steps.len() >= MAX_STEPS {
            return Err(format!("{MAX_STEPS} edits are the most one picture keeps. Open the result as a new picture to go on."));
        }
        let image = imageops::decode(&crate::images::load(&crate::images_dir(), &current)?)?;
        let result = imageops::apply(&image, &op)?;
        let (width, height) = image::GenericImageView::dimensions(&result);
        let bytes = imageops::encode(&result, image::ImageFormat::Png)?;
        let picture = crate::images::store(&crate::images_dir(), &bytes).map_err(|_| {
            "The result is larger than Mobius keeps (8 MB). Resize it first.".to_string()
        })?;
        doc.steps.push(Step {
            label: op.label(),
            by: by.into(),
            op,
            picture: picture.clone(),
            width,
            height,
        });
        doc.cursor = doc.steps.len();
        current = picture;
    }
    let out = view(doc.clone());
    save(&chat_id, &store)?;
    Ok(out)
}

/// A copy of one area of the picture as it stands, stored as a picture of its own (to ask about that part). Nothing in the document changes.
#[tauri::command]
pub fn imgstage_region(
    chat_id: String,
    doc_id: String,
    x: u32,
    y: u32,
    w: u32,
    h: u32,
) -> Result<String, String> {
    let mut store = load(&chat_id);
    let doc = find(&mut store, &doc_id)?.clone();
    let current = view(doc).current;
    let image = imageops::decode(&crate::images::load(&crate::images_dir(), &current)?)?;
    let part = imageops::apply(&image, &Op::Crop { x, y, w, h })?;
    let bytes = imageops::encode(&part, image::ImageFormat::Png)?;
    crate::images::store(&crate::images_dir(), &bytes)
        .map_err(|_| "That area is larger than Mobius keeps (8 MB).".to_string())
}

/// What the operations would make of the picture as it stands, as PNG bytes, without storing or changing anything: the stage shows it on the canvas
/// for the person to accept or reject.
#[tauri::command]
pub fn imgstage_preview(
    chat_id: String,
    doc_id: String,
    ops: Vec<Op>,
) -> Result<tauri::ipc::Response, String> {
    if ops.is_empty() || ops.len() > 10 {
        return Err("Preview between one and ten edits at a time.".into());
    }
    let mut store = load(&chat_id);
    let doc = find(&mut store, &doc_id)?.clone();
    let current = view(doc).current;
    let mut image = imageops::decode(&crate::images::load(&crate::images_dir(), &current)?)?;
    for op in &ops {
        image = imageops::apply(&image, op)?;
    }
    imageops::encode(&image, image::ImageFormat::Png).map(tauri::ipc::Response::new)
}

#[tauri::command]
pub fn imgstage_move(chat_id: String, doc_id: String, delta: i32) -> Result<View, String> {
    let mut store = load(&chat_id);
    let doc = find(&mut store, &doc_id)?;
    let target = (doc.cursor as i64 + i64::from(delta)).clamp(0, doc.steps.len() as i64) as usize;
    doc.cursor = target;
    let out = view(doc.clone());
    save(&chat_id, &store)?;
    Ok(out)
}

#[tauri::command]
pub fn imgstage_close(chat_id: String, doc_id: String) -> Result<Vec<View>, String> {
    let mut store = load(&chat_id);
    if let Some(index) = store.docs.iter().position(|d| d.id == doc_id) {
        let doc = store.docs.remove(index);
        // The original may be a picture the chat still shows; only the edits' own files go.
        let owned: Vec<String> = doc.steps.into_iter().map(|s| s.picture).collect();
        crate::images::remove(&crate::images_dir(), &owned);
    }
    save(&chat_id, &store)?;
    Ok(store.docs.into_iter().map(view).collect())
}

/// Saves the picture as it stands in the chosen format (png, jpeg or webp) with the system's save dialog; `None` when the person cancels.
#[tauri::command]
pub async fn imgstage_export(
    chat_id: String,
    doc_id: String,
    format: String,
) -> Result<Option<String>, String> {
    let (format, ext) = match format.as_str() {
        "png" => (image::ImageFormat::Png, "png"),
        "jpeg" => (image::ImageFormat::Jpeg, "jpg"),
        "webp" => (image::ImageFormat::WebP, "webp"),
        _ => return Err("Export as PNG, JPEG or WebP.".into()),
    };
    let (current, title) = {
        let mut store = load(&chat_id);
        let doc = find(&mut store, &doc_id)?;
        (view(doc.clone()).current, doc.title.clone())
    };
    tokio::task::spawn_blocking(move || {
        let image = imageops::decode(&crate::images::load(&crate::images_dir(), &current)?)?;
        let bytes = imageops::encode(&image, format)?;
        let name = format!(
            "{}.{ext}",
            title.replace(
                |c: char| !(c.is_alphanumeric() || c == '-' || c == '_'),
                "-"
            )
        );
        crate::generation::save_bytes(&bytes, &name)
    })
    .await
    .map_err(|e| e.to_string())?
}
