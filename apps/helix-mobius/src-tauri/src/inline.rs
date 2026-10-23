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
            "continue" => "Continue the text naturally from where it stops, in the same voice and tense, with one or two short paragraphs. Reply with the continuation only, not the text so far.",
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

/// One request to the model on this computer (the route the chat uses, refused when it is not local) and its cleaned answer.
async fn ask_model(
    app: &AppHandle,
    messages: Vec<(&'static str, String)>,
    max_tokens: u32,
) -> Result<String, String> {
    let routing = helix_config::inference_routing::load();
    let persona = helix_chat::personas::load().active_persona().clone();
    let (task, model, is_local, _) = crate::resolve_route(&routing, &persona);
    if !is_local {
        return Err("Editing a selection uses the model on this computer. Set the chat route to a local model.".into());
    }
    let mut args =
        crate::chat_args(&messages, &[], &task, &model, None).map_err(|e| e.to_string())?;
    args.insert("max_tokens", Value::U32(max_tokens));
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
    ask_model(&app, messages(&instruction, &text), 2048).await
}

const CODE_SYSTEM: &str = "You edit source code for the person. You are shown a file with some lines marked as selected, and an instruction. Reply with the replacement for the selected lines only: the code itself, with the same indentation style, no explanation, no line numbers, no Markdown fences. Keep names and behaviour that the instruction does not ask to change. If the instruction cannot be applied to those lines, reply with the selected lines unchanged.";

/// The prompt for editing lines `start..=end` (1-based) of a file: the lines around them for context, the selected ones marked.
pub fn code_messages(
    path: &str,
    lines: &[&str],
    start: usize,
    end: usize,
    instruction: &str,
) -> Vec<(&'static str, String)> {
    let from = start.saturating_sub(41);
    let to = (end + 40).min(lines.len());
    let mut shown = String::new();
    for (i, line) in lines.iter().enumerate().take(to).skip(from) {
        let number = i + 1;
        let mark = if number >= start && number <= end {
            ">>"
        } else {
            "  "
        };
        shown.push_str(&format!("{mark} {line}\n"));
    }
    vec![("system", CODE_SYSTEM.to_string()), ("user", format!("File: {path}\nLines marked >> are selected (lines {start} to {end}).\n\n{shown}\nInstruction: {instruction}\n\nReply with the replacement for the selected lines only."))]
}

#[derive(serde::Serialize)]
pub struct CodeProposal {
    pub old: String,
    pub text: String,
}

fn project_file(app: &AppHandle, path: &str) -> Result<(std::path::PathBuf, String), String> {
    let workspace = {
        let shared = crate::shared(app);
        let state = shared.lock().expect("state");
        state.chat.conversation.workspace.clone()
    };
    let root = crate::terminal::workspace(&workspace)?;
    let file = crate::edits::resolve(&root, path)?;
    let meta = std::fs::metadata(&file).map_err(|_| "That file does not exist.".to_string())?;
    if !meta.is_file() || meta.len() > crate::edits::MAX_FILE_BYTES {
        return Err("That is not a text file small enough to edit.".into());
    }
    let text = std::fs::read_to_string(&file)
        .map_err(|_| "That file is not readable text.".to_string())?;
    Ok((file, text))
}

/// Edit with Mobius on selected lines: the proposal for those lines (nothing is written).
#[tauri::command]
pub async fn code_transform(
    app: AppHandle,
    path: String,
    start: usize,
    end: usize,
    instruction: String,
) -> Result<CodeProposal, String> {
    if instruction.trim().is_empty() || instruction.chars().count() > MAX_INSTRUCTION_CHARS {
        return Err("Say, briefly, what to do with the selected lines.".into());
    }
    let (_, text) = project_file(&app, &path)?;
    let lines: Vec<&str> = text.lines().collect();
    if start == 0 || end < start || end > lines.len() || end - start >= 200 {
        return Err("Select between 1 and 200 lines.".into());
    }
    let old = lines[start - 1..end].join("\n");
    let proposal = ask_model(
        &app,
        code_messages(&path, &lines, start, end, instruction.trim()),
        2048,
    )
    .await?;
    Ok(CodeProposal {
        old,
        text: proposal,
    })
}

/// Writes an accepted proposal over the selected lines, keeping a restore point (`tools_undo_edit` with the returned id puts it back). Refused
/// when the lines are no longer what the proposal was made from.
#[tauri::command]
pub fn code_apply(
    app: AppHandle,
    path: String,
    start: usize,
    end: usize,
    old: String,
    text: String,
) -> Result<String, String> {
    let (file, content) = project_file(&app, &path)?;
    let lines: Vec<&str> = content.lines().collect();
    if start == 0 || end < start || end > lines.len() || lines[start - 1..end].join("\n") != old {
        return Err(
            "Those lines changed since the suggestion was made, so it was not applied.".into(),
        );
    }
    if text.chars().count() > crate::edits::MAX_TEXT_CHARS * 2 {
        return Err("The replacement is too long.".into());
    }
    let mut next: Vec<String> = lines[..start - 1]
        .iter()
        .map(|l| (*l).to_string())
        .collect();
    next.extend(text.lines().map(str::to_string));
    next.extend(lines[end..].iter().map(|l| (*l).to_string()));
    let mut after = next.join("\n");
    if content.ends_with('\n') {
        after.push('\n');
    }
    let checkpoint = crate::edits::Checkpoint {
        path: file,
        before: Some(content),
        after,
        after_present: true,
    };
    let chat_id = crate::shared(&app)
        .lock()
        .expect("state")
        .chat
        .conversation
        .id
        .clone();
    let id = format!("codeedit-{}", crate::new_id());
    crate::edits::apply(&checkpoint, &crate::edit_store(&chat_id, &id))?;
    Ok(id)
}

/// The whole-file replacement a person typed in the viewer, or why it must not be written. `before` is the file as they opened it.
pub fn save_plan(current: &str, before: &str, after: &str) -> Result<(), String> {
    if after.len() > crate::edits::MAX_FILE_BYTES as usize {
        return Err("That is too long to save.".into());
    }
    if current != before {
        return Err("The file changed since you started editing, so it was not saved.".into());
    }
    if after == before {
        return Err("Nothing changed.".into());
    }
    Ok(())
}

/// Writes a file the person edited in the viewer, after they reviewed the diff. Same restore point as `edit_file` (`tools_undo_edit`).
#[tauri::command]
pub fn code_save(
    app: AppHandle,
    path: String,
    before: String,
    after: String,
) -> Result<String, String> {
    let (file, content) = project_file(&app, &path)?;
    save_plan(&content, &before, &after)?;
    let checkpoint = crate::edits::Checkpoint {
        path: file,
        before: Some(content),
        after,
        after_present: true,
    };
    let chat_id = crate::shared(&app)
        .lock()
        .expect("state")
        .chat
        .conversation
        .id
        .clone();
    let id = format!("codeedit-{}", crate::new_id());
    crate::edits::apply(&checkpoint, &crate::edit_store(&chat_id, &id))?;
    Ok(id)
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
    fn the_code_prompt_marks_the_selected_lines_and_bounds_the_context() {
        let lines: Vec<String> = (1..=200).map(|i| format!("line {i}")).collect();
        let refs: Vec<&str> = lines.iter().map(String::as_str).collect();
        let m = code_messages("src/a.rs", &refs, 100, 102, "rename it");
        let user = &m[1].1;
        assert!(
            user.contains(">> line 100")
                && user.contains(">> line 102")
                && user.contains("   line 103")
                || user.contains("  line 103")
        );
        assert!(
            !user.contains("line 50\n") && !user.contains("line 150\n"),
            "only about 40 lines of context each way"
        );
        assert!(user.contains("lines 100 to 102") && user.contains("rename it"));
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

    #[test]
    fn a_viewer_edit_is_refused_when_the_file_moved_or_nothing_changed() {
        assert!(save_plan("a\n", "a\n", "b\n").is_ok());
        assert!(save_plan("a\n", "a\n", "a\n")
            .unwrap_err()
            .contains("Nothing changed"));
        assert!(save_plan("changed\n", "a\n", "b\n")
            .unwrap_err()
            .contains("changed since"));
        assert!(save_plan(
            "a",
            "a",
            &"x".repeat(crate::edits::MAX_FILE_BYTES as usize + 1)
        )
        .is_err());
    }
}
