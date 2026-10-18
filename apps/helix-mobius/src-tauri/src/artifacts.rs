//! Documents on the stage: what a chat has made and been edited into, kept as versions. One file per chat; the model's blocks and the person's
//! edits both land here, so nothing either writes is lost.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

pub const MAX_VERSIONS: usize = 20;
pub const MAX_BODY_CHARS: usize = 60_000;
pub const MAX_DOCUMENTS: usize = 30;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Version {
    pub body: String,
    /// `model` or `person`.
    pub by: String,
    pub at: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Document {
    pub title: String,
    pub versions: Vec<Version>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct Store {
    #[serde(default)]
    pub documents: Vec<Document>,
}

fn file(chat_id: &str) -> PathBuf {
    let safe: String = chat_id
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
        .collect();
    crate::data_home()
        .join("helix/mobius/stage")
        .join(format!("{safe}.json"))
}

impl Store {
    pub fn load(chat_id: &str) -> Store {
        std::fs::read_to_string(file(chat_id))
            .ok()
            .and_then(|t| serde_json::from_str(&t).ok())
            .unwrap_or_default()
    }

    pub fn save(&self, chat_id: &str) -> Result<(), String> {
        let path = file(chat_id);
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        }
        std::fs::write(
            path,
            serde_json::to_string(self).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())
    }

    /// Records a body as the newest version of a document (made if new). The same text as the newest version is not a new version.
    /// Renames a document (the history goes with it). Titles are unique, ignoring case.
    pub fn rename(&mut self, from: &str, to: &str) -> Result<(), String> {
        let to: String = to
            .split_whitespace()
            .collect::<Vec<_>>()
            .join(" ")
            .chars()
            .take(80)
            .collect();
        if to.is_empty() {
            return Err("A document needs a title.".into());
        }
        if self
            .documents
            .iter()
            .any(|d| d.title.eq_ignore_ascii_case(&to) && !d.title.eq_ignore_ascii_case(from))
        {
            return Err("Another document already has that title.".into());
        }
        let doc = self
            .documents
            .iter_mut()
            .find(|d| d.title.eq_ignore_ascii_case(from))
            .ok_or("That document no longer exists.")?;
        doc.title = to;
        Ok(())
    }

    pub fn record(&mut self, title: &str, body: &str, by: &str, at: u64) -> Result<(), String> {
        let title = title.split_whitespace().collect::<Vec<_>>().join(" ");
        let title: String = title.chars().take(80).collect();
        if title.is_empty() {
            return Err("A document needs a title.".into());
        }
        if body.chars().count() > MAX_BODY_CHARS {
            return Err("That document is too long for the stage.".into());
        }
        let index = match self
            .documents
            .iter()
            .position(|d| d.title.eq_ignore_ascii_case(&title))
        {
            Some(i) => i,
            None => {
                if self.documents.len() >= MAX_DOCUMENTS {
                    return Err("This chat already has 30 documents.".into());
                }
                self.documents.push(Document {
                    title,
                    versions: Vec::new(),
                });
                self.documents.len() - 1
            }
        };
        let doc = &mut self.documents[index];
        if doc.versions.last().is_some_and(|v| v.body == body) {
            return Ok(());
        }
        doc.versions.push(Version {
            body: body.to_string(),
            by: by.to_string(),
            at,
        });
        if doc.versions.len() > MAX_VERSIONS {
            let extra = doc.versions.len() - MAX_VERSIONS;
            doc.versions.drain(..extra);
        }
        Ok(())
    }
}

/// One document of any chat, for the Artifacts place.
#[derive(Serialize)]
pub struct Row {
    pub chat_id: String,
    pub chat_title: String,
    pub title: String,
    pub words: usize,
    pub versions: usize,
    pub at: u64,
}

/// Every document the chats have made, newest first (at most 100).
#[tauri::command]
pub fn stage_all() -> Vec<Row> {
    let dir = crate::data_home().join("helix/mobius/stage");
    let mut rows = Vec::new();
    for entry in std::fs::read_dir(dir).into_iter().flatten().flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let Some(chat_id) = name.strip_suffix(".json") else {
            continue;
        };
        let Some(chat) = crate::load(chat_id) else {
            continue;
        };
        for doc in Store::load(chat_id).documents {
            let Some(last) = doc.versions.last() else {
                continue;
            };
            rows.push(Row {
                chat_id: chat_id.to_string(),
                chat_title: chat.title.clone(),
                title: doc.title.clone(),
                words: last.body.split_whitespace().count(),
                versions: doc.versions.len(),
                at: last.at,
            });
        }
    }
    rows.sort_by_key(|r| std::cmp::Reverse(r.at));
    rows.truncate(100);
    rows
}

#[tauri::command]
pub fn stage_rename(chat_id: String, from: String, to: String) -> Result<Store, String> {
    let mut store = Store::load(&chat_id);
    store.rename(&from, &to)?;
    store.save(&chat_id)?;
    Ok(store)
}

#[tauri::command]
pub fn stage_delete(chat_id: String, title: String) -> Result<Store, String> {
    let mut store = Store::load(&chat_id);
    store
        .documents
        .retain(|d| !d.title.eq_ignore_ascii_case(&title));
    store.save(&chat_id)?;
    Ok(store)
}

/// What changed between two versions, as a unified diff with a few lines of context, for the history view.
#[tauri::command]
pub fn text_diff(old: String, new: String) -> String {
    let limit = |t: &str| t.chars().take(MAX_BODY_CHARS).collect::<String>();
    similar::TextDiff::from_lines(&limit(&old), &limit(&new))
        .unified_diff()
        .context_radius(3)
        .to_string()
}

fn esc(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn spans_html(spans: &[helix_chat::markdown::Span]) -> String {
    use helix_chat::markdown::Span;
    spans
        .iter()
        .map(|s| match s {
            Span::Text { text } => esc(text).replace('\n', "<br>"),
            Span::Code { text } => format!("<code>{}</code>", esc(text)),
            Span::Bold { text } => format!("<strong>{}</strong>", esc(text)),
            Span::Italic { text } => format!("<em>{}</em>", esc(text)),
        })
        .collect()
}

/// The document as one self-contained HTML page (its own styles, light and dark, no scripts, nothing fetched), made from the same blocks
/// the Read view shows. Everything is escaped; links are text, as everywhere in Mobius.
pub fn document_html(title: &str, text: &str) -> String {
    use helix_chat::markdown::{Alignment, Block};
    let mut body = String::new();
    for block in helix_chat::markdown::parse_reply(text) {
        match block {
            Block::Paragraph { spans } => {
                body.push_str(&format!("<p>{}</p>\n", spans_html(&spans)))
            }
            Block::Heading { level, spans } => {
                let n = (level + 1).clamp(2, 6);
                body.push_str(&format!("<h{n}>{}</h{n}>\n", spans_html(&spans)));
            }
            Block::Code { text, .. } => {
                body.push_str(&format!("<pre><code>{}</code></pre>\n", esc(&text)))
            }
            Block::List { ordered, items } => {
                let tag = if ordered { "ol" } else { "ul" };
                body.push_str(&format!("<{tag}>"));
                for item in items {
                    body.push_str(&format!("<li>{}</li>", spans_html(&item)));
                }
                body.push_str(&format!("</{tag}>\n"));
            }
            Block::Quote { spans } => body.push_str(&format!(
                "<blockquote>{}</blockquote>\n",
                spans_html(&spans)
            )),
            Block::Table {
                headers,
                rows,
                alignments,
            } => {
                let align = |i: usize| match alignments.get(i) {
                    Some(Alignment::Center) => " style=\"text-align:center\"",
                    Some(Alignment::Right) => " style=\"text-align:right\"",
                    _ => "",
                };
                body.push_str("<table><thead><tr>");
                for (i, h) in headers.iter().enumerate() {
                    body.push_str(&format!("<th{}>{}</th>", align(i), spans_html(h)));
                }
                body.push_str("</tr></thead><tbody>");
                for row in rows {
                    body.push_str("<tr>");
                    for (i, c) in row.iter().enumerate() {
                        body.push_str(&format!("<td{}>{}</td>", align(i), spans_html(c)));
                    }
                    body.push_str("</tr>");
                }
                body.push_str("</tbody></table>\n");
            }
            Block::Rule => body.push_str("<hr>\n"),
            Block::Thinking { .. } => {}
        }
    }
    let title = esc(title);
    format!(
        "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>{title}</title>\n<style>\n:root{{color-scheme:light dark}}body{{font:17px/1.7 ui-serif,'New York',Georgia,serif;max-width:40em;margin:3em auto;padding:0 1.2em;color:CanvasText;background:Canvas}}h1{{font:700 2em/1.2 system-ui,sans-serif;letter-spacing:-.02em;margin:0 0 .6em}}h2,h3,h4,h5,h6{{font-family:system-ui,sans-serif;line-height:1.25;margin:1.6em 0 .4em}}p{{margin:0 0 1em}}blockquote{{margin:1em 0;padding:.1em 1em;border-left:3px solid #8884;color:GrayText}}pre{{overflow:auto;padding:.8em 1em;border-radius:8px;background:#8882;font-size:.85em}}code{{font-family:ui-monospace,monospace;font-size:.9em}}table{{border-collapse:collapse;margin:1em 0}}td,th{{border:1px solid #8886;padding:.35em .7em}}hr{{border:0;border-top:1px solid #8886;margin:2em 0}}\n</style></head><body><h1>{title}</h1>\n{body}</body></html>\n"
    )
}

#[tauri::command]
pub async fn stage_export_html(title: String, text: String) -> Result<Option<String>, String> {
    let name = format!(
        "{}.html",
        title.replace(
            |c: char| !(c.is_alphanumeric() || c == '-' || c == '_'),
            "-"
        )
    );
    let html = document_html(
        &title,
        &text.chars().take(MAX_BODY_CHARS).collect::<String>(),
    );
    tokio::task::spawn_blocking(move || crate::generation::save_bytes(html.as_bytes(), &name))
        .await
        .map_err(|e| e.to_string())?
}

/// A document as the blocks the thread draws, for the Read view.
#[tauri::command]
pub fn markdown_blocks(text: String) -> Vec<helix_chat::markdown::Block> {
    helix_chat::markdown::parse_reply(&text.chars().take(MAX_BODY_CHARS).collect::<String>())
}

#[tauri::command]
pub fn stage_get(chat_id: String) -> Store {
    Store::load(&chat_id)
}

#[tauri::command]
pub fn stage_record(
    chat_id: String,
    title: String,
    body: String,
    by: String,
) -> Result<Store, String> {
    if chat_id.is_empty() {
        return Err("Send a message first so this chat has somewhere to keep documents.".into());
    }
    let by = if by == "person" { "person" } else { "model" };
    let mut store = Store::load(&chat_id);
    store.record(&title, &body, by, crate::now())?;
    store.save(&chat_id)?;
    Ok(store)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keeps_versions_and_ignores_a_repeat() {
        let mut s = Store::default();
        s.record("Plan", "one", "model", 1).unwrap();
        s.record("plan", "one", "person", 2).unwrap();
        s.record("Plan", "two", "person", 3).unwrap();
        assert_eq!(s.documents.len(), 1);
        assert_eq!(s.documents[0].versions.len(), 2);
        assert_eq!(s.documents[0].versions[1].by, "person");
    }

    #[test]
    fn the_html_export_is_escaped_complete_and_has_no_scripts() {
        let html = document_html("A <b>title</b>", "# Head\n\nSome **bold** and `code <x>`.\n\n- one\n- two\n\n<script>alert(1)</script>\n\n| a | b |\n|---|---:|\n| 1 | 2 |");
        assert!(
            html.starts_with("<!doctype html>")
                && html.contains("<title>A &lt;b&gt;title&lt;/b&gt;</title>")
        );
        assert!(
            html.contains("<h2>Head</h2>")
                && html.contains("<strong>bold</strong>")
                && html.contains("<code>code &lt;x&gt;</code>")
        );
        assert!(
            html.contains("<ul><li>one</li><li>two</li></ul>")
                && html.contains("<th style=\"text-align:right\">b</th>")
        );
        assert!(!html.contains("<script>") && html.contains("&lt;script&gt;"));
        assert!(
            !html.contains("http://") && !html.contains("src="),
            "nothing is fetched"
        );
    }

    #[test]
    fn rename_keeps_the_history_and_refuses_a_taken_title() {
        let mut s = Store::default();
        s.record("Plan", "one", "model", 1).unwrap();
        s.record("Notes", "x", "model", 1).unwrap();
        s.rename("plan", "Project plan").unwrap();
        assert_eq!(s.documents[0].title, "Project plan");
        assert_eq!(s.documents[0].versions.len(), 1);
        assert!(s.rename("Project plan", "notes").is_err());
        assert!(s.rename("Project plan", "  ").is_err());
        assert!(s.rename("Gone", "Else").is_err());
        s.rename("Project plan", "PROJECT PLAN").unwrap();
    }

    #[test]
    fn caps_versions_documents_and_size() {
        let mut s = Store::default();
        for i in 0..25 {
            s.record("Doc", &format!("v{i}"), "model", i).unwrap();
        }
        assert_eq!(s.documents[0].versions.len(), MAX_VERSIONS);
        assert_eq!(s.documents[0].versions[0].body, "v5");
        assert!(s.record("", "x", "model", 1).is_err());
        assert!(s
            .record("Big", &"x".repeat(MAX_BODY_CHARS + 1), "model", 1)
            .is_err());
        for i in 0..MAX_DOCUMENTS {
            let _ = s.record(&format!("d{i}"), "x", "model", 1);
        }
        assert!(s.record("one more", "x", "model", 1).is_err());
    }
}
