//! Models as actions: what answers each task, choosing another, freeing memory, getting a model (`docs/design/actions.md`). The desktop
//! service carries them out with the shared routing file (`helix_config::inference_routing`) and the model service (`org.helix.Inference1`).

use serde_json::json;

use crate::{Action, Risk, Undo};

pub const LIST: &str = "models.list";
pub const USE: &str = "models.use";
pub const UNLOAD: &str = "models.unload";
pub const DOWNLOAD: &str = "models.download";

/// The tasks a model answers, as the routing file names them, with what a person calls them.
pub const TASKS: [(&str, &str); 5] = [
    ("chat", "Talk"),
    ("summarize", "Summaries"),
    ("vision", "Pictures"),
    ("code", "Code"),
    ("act", "Acting on the desktop"),
];

pub fn task_title(task: &str) -> Option<&'static str> {
    TASKS
        .iter()
        .find(|(id, _)| *id == task)
        .map(|(_, title)| *title)
}

pub fn actions() -> Vec<Action> {
    let tasks: Vec<&str> = TASKS.iter().map(|(id, _)| *id).collect();
    vec![
        Action {
            id: LIST.into(),
            title: "List models".into(),
            summary: "Which model answers each task (talk, summaries, pictures, code, acting on the desktop), where it runs, and the models \
                      installed on this computer."
                .into(),
            params: json!({ "type": "object", "properties": {}, "additionalProperties": false }),
            risk: Risk::Observe,
            undo: Undo::None,
            selection: None,
        },
        Action {
            id: USE.into(),
            title: "Choose a model for a task".into(),
            summary: "Make a model the one that answers a task, on the backend the task already uses (an installed model on this computer, \
                      or one the backend offers). Recorded; the person can undo it."
                .into(),
            params: json!({
                "type": "object",
                "properties": {
                    "task": { "enum": tasks },
                    "model": { "type": "string", "maxLength": 200, "description": "The model's name; empty for the first installed." }
                },
                "required": ["task", "model"],
                "additionalProperties": false
            }),
            risk: Risk::Act,
            undo: Undo::Restore,
            selection: None,
        },
        Action {
            id: UNLOAD.into(),
            title: "Free model memory".into(),
            summary: "Unload the model on this computer now (it loads again when something asks). Not while it is answering.".into(),
            params: json!({ "type": "object", "properties": {}, "additionalProperties": false }),
            risk: Risk::Act,
            undo: Undo::None,
            selection: None,
        },
        Action {
            id: DOWNLOAD.into(),
            title: "Download a model".into(),
            summary: "Download a GGUF model from Hugging Face into the models folder (checked against its checksum). Large: the person is asked."
                .into(),
            params: json!({
                "type": "object",
                "properties": {
                    "repo": { "type": "string", "maxLength": 200, "description": "owner/name, e.g. Qwen/Qwen3-4B-GGUF" },
                    "file": { "type": "string", "maxLength": 300, "description": "The .gguf file to get." },
                    "pictures": { "type": "boolean", "description": "Also get the picture projector when the repository has one." }
                },
                "required": ["repo", "file"],
                "additionalProperties": false
            }),
            risk: Risk::Confirm,
            undo: Undo::None,
            selection: None,
        },
    ]
}

/// A model name a person or an agent may give: printable, not too long.
pub fn model_ok(model: &str) -> bool {
    model.len() <= 200 && !model.chars().any(char::is_control)
}

/// "Talk: qwen3-4b → gemma-3-4b".
pub fn sentence(task: &str, before: &str, after: &str) -> String {
    let shown = |m: &str| {
        if m.is_empty() {
            "first installed".to_string()
        } else {
            m.trim_end_matches(".gguf").to_string()
        }
    };
    format!(
        "{}: {} → {}",
        task_title(task).unwrap_or(task),
        shown(before),
        shown(after)
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tasks_names_and_sentences() {
        assert_eq!(task_title("chat"), Some("Talk"));
        assert_eq!(task_title("nonsense"), None);
        assert_eq!(
            sentence("chat", "", "qwen3-4b.gguf"),
            "Talk: first installed → qwen3-4b"
        );
        assert!(model_ok("Qwen/Qwen3-8B"));
        assert!(!model_ok("bad\nname"));
        assert!(!model_ok(&"x".repeat(201)));
    }
}
