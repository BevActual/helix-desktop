//! Ask on the selection: one instruction applied to a piece of text the person selected in a document, answered by the model on this computer.
//! The answer is a proposal the page shows as a change (old and new) to accept or reject; nothing is written here.

use std::sync::Arc;

use tauri::{AppHandle, Manager};
use zbus::zvariant::Value;

const MAX_TEXT_CHARS: usize = 6000;
const MAX_INSTRUCTION_CHARS: usize = 300;

const SYSTEM: &str = "You edit text for the person. Apply their instruction to the text they give you. Reply with the edited text only: no preface, no explanation, no quotation marks around it, no Markdown fences unless the text already has them. Keep the person's meaning, names and facts, and keep the language they wrote in. If the instruction cannot be applied, reply with the original text unchanged.";

/// The named instructions the selection bar offers, written out for the model.
pub fn instruction_for(kind: &str, custom: &str) -> Option<String> {
    Some(
        match kind {
            "improve" => {
                "Improve the writing: clearer, smoother, same meaning and about the same length."
            }
            "shorter" => "Make it shorter, keeping the important points.",
            "longer" => "Make it longer by adding useful detail, without inventing facts.",
            "simplify" => "Rewrite it in simpler language that a non-expert can follow.",
            "fix" => {
                "Fix spelling, grammar and punctuation only. Do not change the wording otherwise."
            }
            "custom" => custom.trim(),
            _ => return None,
        }
        .to_string(),
    )
    .filter(|s| !s.is_empty())
}

pub fn messages(instruction: &str, text: &str) -> Vec<(&'static str, String)> {
    vec![
        ("system", SYSTEM.to_string()),
        (
            "user",
            format!("Instruction: {instruction}\n\nText:\n{text}"),
        ),
    ]
}

/// What the model gave back, with the wrappers small models add taken off (surrounding quotes, a single outer code fence, a thinking prelude).
pub fn clean(raw: &str) -> String {
    let answer = helix_chat::markdown::strip_thinking(raw);
    let mut text = answer.trim().to_string();
    if let Some(inner) = text.strip_prefix("```").and_then(|t| t.strip_suffix("```")) {
        // An outer fence with an optional language word on its first line.
        let body = inner.split_once('\n').map_or(inner, |(first, rest)| {
            if first.trim().chars().all(|c| c.is_ascii_alphanumeric()) {
                rest
            } else {
                inner
            }
        });
        text = body.trim().to_string();
    }
    if text.len() > 2
        && text.starts_with('"')
        && text.ends_with('"')
        && !text[1..text.len() - 1].contains('"')
    {
        text = text[1..text.len() - 1].to_string();
    }
    text
}

#[tauri::command]
pub async fn stage_transform(
    app: AppHandle,
    kind: String,
    custom: String,
    text: String,
) -> Result<String, String> {
    if text.trim().is_empty() || text.chars().count() > MAX_TEXT_CHARS {
        return Err(format!("Select between 1 and {MAX_TEXT_CHARS} characters."));
    }
    if custom.chars().count() > MAX_INSTRUCTION_CHARS {
        return Err("Keep the instruction short.".into());
    }
    let instruction =
        instruction_for(&kind, &custom).ok_or("Say what to do with the selection.")?;
    let routing = helix_config::inference_routing::load();
    let persona = helix_chat::personas::load().active_persona().clone();
    let (task, model, is_local, _) = crate::resolve_route(&routing, &persona);
    if !is_local {
        return Err("Editing a selection uses the model on this computer. Set the chat route to a local model.".into());
    }
    let mut args = crate::chat_args(&messages(&instruction, &text), &[], &task, &model)
        .map_err(|e| e.to_string())?;
    args.insert("max_tokens", Value::U32(2048));
    args.insert("temperature", Value::F64(0.3));
    let connection = crate::connection().await.map_err(|e| e.to_string())?;
    let proxy = crate::proxy(&connection)
        .await
        .map_err(|_| "The inference service is not available.".to_string())?;
    let operation: u64 = proxy
        .call("Action", &("chat", args))
        .await
        .map_err(|e| e.to_string())?;
    let hub = app.state::<Arc<crate::dictate::Hub>>().inner().clone();
    hub.expect(operation);
    let (ok, result) = hub.wait(operation).await?;
    if !ok {
        return Err("The model could not answer.".into());
    }
    let value: serde_json::Value = serde_json::from_str(&result).unwrap_or_default();
    let edited = clean(value["text"].as_str().unwrap_or(""));
    if edited.is_empty() {
        return Err("The model gave no text back.".into());
    }
    Ok(edited)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn named_instructions_and_custom_ones() {
        assert!(instruction_for("shorter", "").unwrap().contains("shorter"));
        assert_eq!(
            instruction_for("custom", "  make it rhyme ").unwrap(),
            "make it rhyme"
        );
        assert!(instruction_for("custom", "  ").is_none());
        assert!(instruction_for("delete-everything", "").is_none());
    }

    #[test]
    fn wrappers_small_models_add_are_taken_off() {
        assert_eq!(clean("  \"Hello there\"  "), "Hello there");
        assert_eq!(clean("```\nHello\nthere\n```"), "Hello\nthere");
        assert_eq!(clean("```text\nHello\n```"), "Hello");
        assert_eq!(clean("<think>hmm</think>Done."), "Done.");
        assert_eq!(
            clean("She said \"hi\" and left."),
            "She said \"hi\" and left."
        );
        assert_eq!(clean("\"A\" and \"B\""), "\"A\" and \"B\"");
    }
}
