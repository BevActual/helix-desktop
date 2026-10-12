//! One chat with the local model, as a pure model: the conversation, what is sent to the model for a turn (the system prompt, then as much
//! recent history as fits), and the turn state machine (a reply streams in, may be stopped, and a late answer to an older turn is
//! harmless). No D-Bus, no files, no widgets: the app feeds it what the model service says and draws what it returns.

use serde::Serialize;
use serde_json::{json, Value};

use crate::display::sanitize_for_display;
use crate::markdown::{self, Block};

pub const MAX_INPUT_CHARS: usize = 4000;
pub const MAX_MESSAGES: usize = 400;
pub const MAX_MESSAGE_CHARS: usize = 20_000;
/// How much history goes to the model with a turn (characters; a token is about four).
pub const CONTEXT_CHARS: usize = 12_000;
/// Attached file bodies included with a turn (total, after truncation).
pub const MAX_ATTACH_CHARS: usize = 8_000;
pub const MAX_ATTACH_FILES: usize = 6;
pub const MAX_ATTACH_FILE_CHARS: usize = 6_000;

pub const SYSTEM_PROMPT: &str = "You are Mobius, the AI built into the person's Helix Linux desktop. Be direct and brief. \
Use plain text; use a fenced code block for code and commands. If asked to change something on the desktop, tell them to use \
Ask Mobius / Act (bar). Do not claim to have done it yourself.";

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Role {
    User,
    Assistant,
}

impl Role {
    fn wire(self) -> &'static str {
        match self {
            Role::User => "user",
            Role::Assistant => "assistant",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Message {
    pub role: Role,
    pub text: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Conversation {
    pub id: String,
    pub title: String,
    pub messages: Vec<Message>,
    /// Seconds since the epoch, for ordering.
    pub updated: u64,
    /// Pinned chats stay at the top of the sidebar (ChatGPT-style).
    pub pinned: bool,
    /// Optional project / folder name for grouping.
    pub project: String,
    /// The style overlay for this chat (an id from the lab; empty = none).
    pub style: String,
}

/// An id is 16 lowercase hex digits (it names a file).
pub fn valid_id(id: &str) -> bool {
    id.len() == 16 && id.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn tidy(text: &str, max: usize) -> String {
    // Keep line breaks and tabs; drop every other control character and anything that could reorder or hide text.
    let cleaned: String = text
        .chars()
        .filter(|c| *c == '\n' || *c == '\t' || (!c.is_control() && !matches!(*c, '\u{200b}'..='\u{200f}' | '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}')))
        .take(max)
        .collect();
    cleaned
}

fn fence_lang_for(name: &str) -> &'static str {
    let ext = name.rsplit('.').next().unwrap_or("").to_ascii_lowercase();
    match ext.as_str() {
        "rs" => "rust",
        "py" => "python",
        "js" | "mjs" | "cjs" => "javascript",
        "ts" | "tsx" => "typescript",
        "jsx" => "jsx",
        "json" => "json",
        "toml" => "toml",
        "yaml" | "yml" => "yaml",
        "md" | "markdown" => "markdown",
        "sh" | "bash" | "zsh" => "bash",
        "css" => "css",
        "html" | "htm" => "html",
        "c" | "h" => "c",
        "cpp" | "cc" | "cxx" | "hpp" => "cpp",
        "go" => "go",
        "java" => "java",
        "kt" => "kotlin",
        "swift" => "swift",
        "sql" => "sql",
        "xml" => "xml",
        "csv" => "csv",
        "txt" | "log" | "text" => "text",
        _ => "",
    }
}

fn title_of(text: &str) -> String {
    let line = text.lines().find(|l| !l.trim().is_empty()).unwrap_or("New chat");
    let title = sanitize_for_display(line.trim(), 48);
    if title.is_empty() {
        "New chat".into()
    } else {
        title
    }
}

impl Conversation {
    pub fn new(id: &str, now: u64) -> Option<Conversation> {
        valid_id(id).then(|| Conversation {
            id: id.to_string(),
            title: "New chat".into(),
            messages: Vec::new(),
            updated: now,
            pinned: false,
            project: String::new(),
            style: String::new(),
        })
    }

    /// Seed turns from the bar quick-Talk handoff (user/assistant pairs). Caps length like a normal send.
    pub fn seed_from_bar(&mut self, turns: &[(Role, String)], now: u64) {
        self.messages.clear();
        for (role, text) in turns {
            let text = tidy(text, MAX_MESSAGE_CHARS);
            if text.trim().is_empty() {
                continue;
            }
            if self.messages.len() >= MAX_MESSAGES {
                break;
            }
            self.messages.push(Message {
                role: *role,
                text,
            });
        }
        self.updated = now;
        if let Some(first) = self.messages.iter().find(|m| m.role == Role::User) {
            self.title = title_of(&first.text);
        }
    }

    /// The messages to send for the next turn: the system prompt, then the newest history that fits, oldest first. The last user message
    /// always goes (cut if it alone is too long). Empty `system` uses [`SYSTEM_PROMPT`].
    pub fn context(&self) -> Vec<(&'static str, String)> {
        self.context_with_system(SYSTEM_PROMPT)
    }

    pub fn context_with_system(&self, system: &str) -> Vec<(&'static str, String)> {
        let mut budget = CONTEXT_CHARS;
        let mut kept: Vec<&Message> = Vec::new();
        for message in self.messages.iter().rev() {
            let cost = message.text.chars().count() + 8;
            if cost > budget && !kept.is_empty() {
                break;
            }
            budget = budget.saturating_sub(cost);
            kept.push(message);
        }
        kept.reverse();
        // A conversation sent to the model starts with a user turn.
        while kept.first().is_some_and(|m| m.role == Role::Assistant) {
            kept.remove(0);
        }
        let prompt = if system.trim().is_empty() {
            SYSTEM_PROMPT.to_string()
        } else {
            system.to_string()
        };
        let mut out = vec![("system", prompt)];
        out.extend(kept.into_iter().map(|m| (m.role.wire(), m.text.chars().take(CONTEXT_CHARS).collect::<String>())));
        out
    }

    /// Build a user message from typed text plus `(name, body)` attachments.
    /// Bodies are truncated to fit [`MAX_ATTACH_CHARS`]; returns `(composed, truncated)`.
    pub fn compose_with_attachments(
        text: &str,
        attachments: &[(String, String)],
    ) -> (String, bool) {
        let mut body = text.trim_end().to_string();
        if attachments.is_empty() {
            return (body, false);
        }
        let mut budget = MAX_ATTACH_CHARS;
        let mut truncated = false;
        if !body.is_empty() {
            body.push_str("\n\n");
        }
        for (index, (name, content)) in attachments.iter().enumerate() {
            if index >= MAX_ATTACH_FILES {
                truncated = true;
                break;
            }
            if budget == 0 {
                truncated = true;
                break;
            }
            let safe_name = sanitize_for_display(name.trim(), 80);
            let name = if safe_name.is_empty() {
                "file".to_string()
            } else {
                safe_name
            };
            let fence = fence_lang_for(&name);
            let mut chunk = content.clone();
            if chunk.chars().count() > MAX_ATTACH_FILE_CHARS {
                chunk = chunk.chars().take(MAX_ATTACH_FILE_CHARS).collect();
                truncated = true;
            }
            if chunk.chars().count() > budget {
                chunk = chunk.chars().take(budget).collect();
                truncated = true;
            }
            budget = budget.saturating_sub(chunk.chars().count());
            body.push_str("Attached: ");
            body.push_str(&name);
            body.push('\n');
            body.push_str("```");
            body.push_str(fence);
            body.push('\n');
            body.push_str(&chunk);
            if !chunk.ends_with('\n') {
                body.push('\n');
            }
            body.push_str("```\n\n");
        }
        if truncated {
            body.push_str("(Some attachment content was truncated to fit the context budget.)\n");
        }
        (body.trim_end().to_string(), truncated)
    }

    /// First user turn preview for sidebar search (trimmed, capped).
    pub fn preview(&self) -> String {
        self.messages
            .iter()
            .find(|m| m.role == Role::User)
            .map(|m| {
                let line = m.text.lines().find(|l| !l.trim().is_empty()).unwrap_or("");
                sanitize_for_display(line.trim(), 96)
            })
            .unwrap_or_default()
    }

    /// Copy this conversation into a new id, optionally cutting after `through` (inclusive).
    pub fn fork(&self, new_id: &str, through: Option<usize>, now: u64) -> Option<Conversation> {
        let mut next = Conversation::new(new_id, now)?;
        if self.messages.is_empty() {
            next.title = "Forked chat".into();
            next.project = self.project.clone();
            return Some(next);
        }
        let last = self.messages.len() - 1;
        let end = through.map(|i| i.min(last)).unwrap_or(last);
        next.messages = self.messages[..=end].to_vec();
        // Drop a trailing empty assistant bubble (interrupted stream).
        if next
            .messages
            .last()
            .is_some_and(|m| m.role == Role::Assistant && m.text.trim().is_empty())
        {
            next.messages.pop();
        }
        if next.messages.is_empty() {
            next.title = "Forked chat".into();
        } else {
            let base = if self.title.trim().is_empty() || self.title == "New chat" {
                title_of(&next.messages[0].text)
            } else {
                self.title.clone()
            };
            let forked = format!("{base} (fork)");
            next.title = sanitize_for_display(&forked, 48);
        }
        next.project = self.project.clone();
        next.style = self.style.clone();
        next.pinned = false;
        next.updated = now;
        Some(next)
    }

    pub fn to_json(&self) -> String {
        json!({
            "version": 1, "id": self.id, "title": self.title, "updated": self.updated,
            "pinned": self.pinned,
            "project": self.project,
            "style": self.style,
            "messages": self.messages.iter().map(|m| json!({"role": m.role.wire(), "text": m.text})).collect::<Vec<_>>(),
        })
        .to_string()
    }

    /// Reads a saved conversation. Anything wrong with the file is a refusal (not a guess); sizes are bounded.
    pub fn from_json(text: &str) -> Option<Conversation> {
        if text.len() > 8 * 1024 * 1024 {
            return None;
        }
        let value: Value = serde_json::from_str(text).ok()?;
        let id = value.get("id")?.as_str()?;
        if !valid_id(id) {
            return None;
        }
        let mut messages = Vec::new();
        for entry in value.get("messages")?.as_array()?.iter().take(MAX_MESSAGES) {
            let role = match entry.get("role")?.as_str()? {
                "user" => Role::User,
                "assistant" => Role::Assistant,
                _ => return None,
            };
            messages.push(Message { role, text: tidy(entry.get("text")?.as_str()?, MAX_MESSAGE_CHARS) });
        }
        let first_user = messages.iter().find(|m| m.role == Role::User).map(|m| title_of(&m.text));
        Some(Conversation {
            id: id.to_string(),
            title: value.get("title").and_then(Value::as_str).map(|t| sanitize_for_display(t, 48)).filter(|t| !t.is_empty()).or(first_user).unwrap_or_else(|| "New chat".into()),
            messages,
            updated: value.get("updated").and_then(Value::as_u64).unwrap_or(0),
            pinned: value.get("pinned").and_then(Value::as_bool).unwrap_or(false),
            project: value
                .get("project")
                .and_then(Value::as_str)
                .map(|t| sanitize_for_display(t, 40))
                .unwrap_or_default(),
            style: value
                .get("style")
                .and_then(Value::as_str)
                .map(|t| sanitize_for_display(t, 40))
                .unwrap_or_default(),
        })
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ChatError {
    Empty,
    TooLong,
    Busy,
    Full,
    /// No completed assistant reply to regenerate, or no user turn to edit.
    NothingToReplay,
}

/// A turn the app should start: these messages to the model.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Turn {
    pub generation: u64,
    pub messages: Vec<(&'static str, String)>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    Finished,
    Failed(String),
    Stopped,
}

#[derive(Debug, Default)]
struct Streaming {
    generation: u64,
    operation: Option<u64>,
    /// Deltas and endings that arrived before the service's operation id did.
    early_deltas: Vec<(u64, String)>,
    early_done: Vec<(u64, bool, String)>,
    stopping: bool,
}

#[derive(Debug)]
pub struct Chat {
    pub conversation: Conversation,
    streaming: Option<Streaming>,
    generation: u64,
    pub error: String,
    /// When true, the hub must not write this conversation to disk (ChatGPT temporary chat).
    pub temporary: bool,
}

#[derive(Clone, Debug, Serialize)]
pub struct MessageView {
    pub role: Role,
    pub text: String,
    pub blocks: Vec<Block>,
}

#[derive(Clone, Debug, Serialize)]
pub struct ChatView {
    pub id: String,
    pub title: String,
    pub messages: Vec<MessageView>,
    pub streaming: bool,
    pub error: String,
    pub temporary: bool,
    pub pinned: bool,
    pub project: String,
    pub style: String,
}

fn view_of(message: &Message) -> MessageView {
    MessageView {
        role: message.role,
        text: message.text.clone(),
        blocks: if message.role == Role::Assistant { markdown::parse(&message.text) } else { Vec::new() },
    }
}

impl Chat {
    pub fn new(conversation: Conversation) -> Chat {
        Chat {
            conversation,
            streaming: None,
            generation: 0,
            error: String::new(),
            temporary: false,
        }
    }

    pub fn temporary(conversation: Conversation) -> Chat {
        let mut chat = Chat::new(conversation);
        chat.temporary = true;
        chat.conversation.title = "Temporary chat".into();
        chat
    }

    pub fn is_streaming(&self) -> bool {
        self.streaming.is_some()
    }

    /// The person sent `text`. Refused while a reply is on its way.
    pub fn send(&mut self, text: &str, now: u64) -> Result<Turn, ChatError> {
        self.send_with_system(text, now, SYSTEM_PROMPT)
    }

    /// Like [`send`] but uses a persona system prompt (empty → [`SYSTEM_PROMPT`]).
    pub fn send_with_system(&mut self, text: &str, now: u64, system: &str) -> Result<Turn, ChatError> {
        self.send_limited(text, MAX_INPUT_CHARS, now, system)
    }

    /// Send a pre-composed user turn (e.g. text + attached files). Cap is [`MAX_MESSAGE_CHARS`].
    pub fn send_composed(&mut self, text: &str, now: u64, system: &str) -> Result<Turn, ChatError> {
        self.send_limited(text, MAX_MESSAGE_CHARS, now, system)
    }

    fn send_limited(
        &mut self,
        text: &str,
        max_chars: usize,
        now: u64,
        system: &str,
    ) -> Result<Turn, ChatError> {
        if self.streaming.is_some() {
            return Err(ChatError::Busy);
        }
        let text = tidy(text, max_chars + 1);
        if text.trim().is_empty() {
            return Err(ChatError::Empty);
        }
        if text.chars().count() > max_chars {
            return Err(ChatError::TooLong);
        }
        if self.conversation.messages.len() + 2 > MAX_MESSAGES {
            return Err(ChatError::Full);
        }
        if self.conversation.messages.is_empty() && !self.temporary {
            self.conversation.title = title_of(&text);
        }
        self.conversation.messages.push(Message {
            role: Role::User,
            text,
        });
        self.begin_assistant_turn(now, system)
    }

    /// Rename the conversation (ChatGPT-style). Empty titles are refused.
    pub fn set_title(&mut self, title: &str, now: u64) -> Result<(), ChatError> {
        let title = sanitize_for_display(title.trim(), 48);
        if title.is_empty() {
            return Err(ChatError::Empty);
        }
        self.conversation.title = title;
        self.conversation.updated = now;
        Ok(())
    }

    pub fn set_pinned(&mut self, pinned: bool, now: u64) {
        self.conversation.pinned = pinned;
        self.conversation.updated = now;
    }

    pub fn set_style(&mut self, style: &str, now: u64) {
        self.conversation.style = sanitize_for_display(style.trim(), 40);
        self.conversation.updated = now;
    }

    pub fn set_project(&mut self, project: &str, now: u64) {
        self.conversation.project = sanitize_for_display(project.trim(), 40);
        self.conversation.updated = now;
    }

    /// Drop the last assistant reply and ask again with the same user turn (ChatGPT Regenerate).
    pub fn regenerate_with_system(&mut self, now: u64, system: &str) -> Result<Turn, ChatError> {
        if self.streaming.is_some() {
            return Err(ChatError::Busy);
        }
        match self.conversation.messages.last() {
            Some(m) if m.role == Role::Assistant => {
                self.conversation.messages.pop();
            }
            _ => return Err(ChatError::NothingToReplay),
        }
        if !self.conversation.messages.last().is_some_and(|m| m.role == Role::User) {
            return Err(ChatError::NothingToReplay);
        }
        self.begin_assistant_turn(now, system)
    }

    /// Replace the user message at `index`, drop everything after it, and resend (ChatGPT Edit).
    pub fn edit_and_resend(&mut self, index: usize, text: &str, now: u64, system: &str) -> Result<Turn, ChatError> {
        if self.streaming.is_some() {
            return Err(ChatError::Busy);
        }
        if index >= self.conversation.messages.len()
            || self.conversation.messages[index].role != Role::User
        {
            return Err(ChatError::NothingToReplay);
        }
        // The edit may be refused (empty, too long); the conversation must then be exactly as it was.
        let kept_messages = self.conversation.messages.clone();
        let kept_title = self.conversation.title.clone();
        self.conversation.messages.truncate(index);
        let result = self.send_with_system(text, now, system);
        if result.is_err() {
            self.conversation.messages = kept_messages;
            self.conversation.title = kept_title;
        }
        result
    }

    fn begin_assistant_turn(&mut self, now: u64, system: &str) -> Result<Turn, ChatError> {
        if self.conversation.messages.len() + 1 > MAX_MESSAGES {
            return Err(ChatError::Full);
        }
        self.conversation.messages.push(Message { role: Role::Assistant, text: String::new() });
        self.conversation.updated = now;
        self.generation += 1;
        self.error.clear();
        self.streaming = Some(Streaming { generation: self.generation, ..Streaming::default() });
        let mut sent = self.conversation.clone();
        sent.messages.pop();
        Ok(Turn {
            generation: self.generation,
            messages: sent.context_with_system(system),
        })
    }

    /// The service accepted the turn as `operation`. Anything it sent before that is applied now. Returns the outcome if it already ended.
    pub fn started(&mut self, generation: u64, operation: u64) -> Option<Outcome> {
        let streaming = self.streaming.as_mut().filter(|s| s.generation == generation)?;
        streaming.operation = Some(operation);
        let deltas: Vec<String> = streaming.early_deltas.iter().filter(|(op, _)| *op == operation).map(|(_, t)| t.clone()).collect();
        let done = streaming.early_done.iter().find(|(op, _, _)| *op == operation).cloned();
        streaming.early_deltas.clear();
        streaming.early_done.clear();
        for text in deltas {
            self.append(&text);
        }
        done.and_then(|(op, ok, result)| self.complete(op, ok, &result))
    }

    fn append(&mut self, text: &str) {
        if let Some(last) = self.conversation.messages.last_mut() {
            if last.role == Role::Assistant && last.text.chars().count() < MAX_MESSAGE_CHARS {
                let room = MAX_MESSAGE_CHARS - last.text.chars().count();
                last.text.push_str(&tidy(text, room));
            }
        }
    }

    /// Streamed text. Returns whether the visible reply changed.
    pub fn delta(&mut self, operation: u64, text: &str) -> bool {
        let Some(streaming) = self.streaming.as_mut() else { return false };
        match streaming.operation {
            Some(own) if own == operation => {
                if streaming.stopping {
                    return false;
                }
                self.append(text);
                true
            }
            Some(_) => false,
            None => {
                if streaming.early_deltas.len() < 256 {
                    streaming.early_deltas.push((operation, text.to_string()));
                }
                false
            }
        }
    }

    /// The model service says the turn ended. `result` is its JSON.
    pub fn complete(&mut self, operation: u64, ok: bool, result: &str) -> Option<Outcome> {
        let streaming = self.streaming.as_mut()?;
        match streaming.operation {
            Some(own) if own == operation => {}
            Some(_) => return None,
            None => {
                if streaming.early_done.len() < 8 {
                    streaming.early_done.push((operation, ok, result.to_string()));
                }
                return None;
            }
        }
        let stopped = streaming.stopping;
        self.streaming = None;
        let value: Value = serde_json::from_str(result).unwrap_or(Value::Null);
        let cancelled = value.get("cancelled").and_then(Value::as_bool).unwrap_or(false);
        let reply_empty = self.conversation.messages.last().is_some_and(|m| m.role == Role::Assistant && m.text.trim().is_empty());
        if stopped || cancelled {
            if reply_empty {
                self.conversation.messages.pop();
                // Take the unanswered question back out too, so it can be edited and sent again.
                if self.conversation.messages.last().is_some_and(|m| m.role == Role::User) {
                    self.conversation.messages.pop();
                }
            }
            return Some(Outcome::Stopped);
        }
        if !ok {
            let reason = value.get("error").and_then(Value::as_str).map(|e| sanitize_for_display(e, 200)).filter(|e| !e.is_empty()).unwrap_or_else(|| "The model could not answer.".into());
            if reply_empty {
                self.conversation.messages.pop();
                self.conversation.messages.pop();
            }
            self.error = reason.clone();
            return Some(Outcome::Failed(reason));
        }
        if reply_empty {
            self.conversation.messages.pop();
            self.conversation.messages.pop();
            self.error = "The model returned nothing.".into();
            return Some(Outcome::Failed(self.error.clone()));
        }
        Some(Outcome::Finished)
    }

    /// The turn could not be started at all (the service is not there).
    pub fn failed_to_start(&mut self, generation: u64, reason: &str) {
        if self.streaming.as_ref().is_some_and(|s| s.generation == generation) {
            self.streaming = None;
            self.conversation.messages.pop();
            self.conversation.messages.pop();
            self.error = sanitize_for_display(reason, 200);
        }
    }

    /// Whether Stop was pressed for the turn on its way (so a turn that gets its id late can still be cancelled).
    pub fn stop_requested(&self) -> bool {
        self.streaming.as_ref().is_some_and(|s| s.stopping)
    }

    /// The person pressed Stop. Returns the operation to cancel, if the service has given one.
    pub fn stop(&mut self) -> Option<u64> {
        let streaming = self.streaming.as_mut()?;
        streaming.stopping = true;
        streaming.operation
    }

    pub fn view(&self) -> ChatView {
        ChatView {
            id: self.conversation.id.clone(),
            title: self.conversation.title.clone(),
            messages: self.conversation.messages.iter().map(view_of).collect(),
            streaming: self.streaming.is_some(),
            error: self.error.clone(),
            temporary: self.temporary,
            pinned: self.conversation.pinned,
            project: self.conversation.project.clone(),
            style: self.conversation.style.clone(),
        }
    }

    /// The reply being written, as blocks (what a streaming update sends).
    pub fn last_blocks(&self) -> Vec<Block> {
        self.conversation.messages.last().filter(|m| m.role == Role::Assistant).map(|m| markdown::parse(&m.text)).unwrap_or_default()
    }

    /// Markdown export of the conversation (ChatGPT share/export shape).
    pub fn to_markdown(&self) -> String {
        let mut out = String::new();
        let title = if self.temporary {
            "Temporary chat"
        } else if self.conversation.title.trim().is_empty() {
            "Chat"
        } else {
            self.conversation.title.as_str()
        };
        out.push_str("# ");
        out.push_str(title);
        out.push_str("\n\n");
        for message in &self.conversation.messages {
            let role = match message.role {
                Role::User => "You",
                Role::Assistant => "Mobius",
            };
            out.push_str("## ");
            out.push_str(role);
            out.push_str("\n\n");
            out.push_str(message.text.trim());
            out.push_str("\n\n");
        }
        out
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Summary {
    pub id: String,
    pub title: String,
    pub updated: u64,
    pub pinned: bool,
    pub project: String,
    /// Short preview of the first user message (sidebar + light search).
    #[serde(default)]
    pub preview: String,
}

/// Full-text search hit across conversation bodies.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct SearchHit {
    pub id: String,
    pub title: String,
    pub snippet: String,
    pub updated: u64,
    pub pinned: bool,
    pub project: String,
}

#[cfg(test)]
mod tests {
    use super::*;

    const ID: &str = "0123456789abcdef";

    fn chat() -> Chat {
        Chat::new(Conversation::new(ID, 1).unwrap())
    }

    #[test]
    fn a_refused_edit_leaves_the_conversation_alone() {
        let mut c = chat();
        let t = c.send("first question", 1).unwrap();
        c.started(t.generation, 1);
        c.delta(1, "first answer");
        c.complete(1, true, "{}");
        let t = c.send("second question", 2).unwrap();
        c.started(t.generation, 2);
        c.delta(2, "second answer");
        c.complete(2, true, "{}");
        let before = c.conversation.messages.clone();
        let title = c.conversation.title.clone();
        assert_eq!(c.edit_and_resend(0, "   ", 3, SYSTEM_PROMPT).unwrap_err(), ChatError::Empty);
        assert_eq!(c.edit_and_resend(0, &"x".repeat(MAX_INPUT_CHARS + 1), 3, SYSTEM_PROMPT).unwrap_err(), ChatError::TooLong);
        assert_eq!(c.conversation.messages, before);
        assert_eq!(c.conversation.title, title);
        assert!(!c.is_streaming());
    }

    #[test]
    fn ids_name_files_safely() {
        assert!(valid_id(ID));
        for bad in ["", "0123456789abcde", "0123456789abcdeg", "0123456789ABCDEF", "../../etc/passwd0", "0123456789abcdef0"] {
            assert!(!valid_id(bad), "{bad}");
        }
        assert!(Conversation::new("nope", 0).is_none());
    }

    #[test]
    fn a_turn_streams_in_and_finishes() {
        let mut c = chat();
        let turn = c.send("  What is a cron job?  ", 5).unwrap();
        assert_eq!(turn.messages[0].0, "system");
        assert_eq!(turn.messages.last().unwrap(), &("user", "  What is a cron job?  ".to_string()));
        assert_eq!(c.conversation.title, "What is a cron job?");
        assert!(c.is_streaming());
        assert_eq!(c.started(turn.generation, 7), None);
        assert!(c.delta(7, "A cron "));
        assert!(c.delta(7, "job runs on a schedule."));
        assert_eq!(c.complete(7, true, "{\"ok\":true}"), Some(Outcome::Finished));
        assert!(!c.is_streaming());
        assert_eq!(c.conversation.messages[1].text, "A cron job runs on a schedule.");
        assert!(c.send("again", 6).is_ok());
    }

    #[test]
    fn one_turn_at_a_time_and_bad_input_is_refused() {
        let mut c = chat();
        assert_eq!(c.send("   ", 0).unwrap_err(), ChatError::Empty);
        assert_eq!(c.send(&"x".repeat(MAX_INPUT_CHARS + 1), 0).unwrap_err(), ChatError::TooLong);
        assert!(c.send("hi", 0).is_ok());
        assert_eq!(c.send("again", 0).unwrap_err(), ChatError::Busy);
    }

    #[test]
    fn rename_regenerate_and_edit() {
        let mut c = chat();
        c.set_title("  My notes  ", 2).unwrap();
        assert_eq!(c.conversation.title, "My notes");
        assert_eq!(c.set_title("   ", 3).unwrap_err(), ChatError::Empty);
        assert_eq!(c.regenerate_with_system(4, SYSTEM_PROMPT).unwrap_err(), ChatError::NothingToReplay);

        let turn = c.send("alpha", 5).unwrap();
        c.started(turn.generation, 1);
        c.delta(1, "A");
        c.complete(1, true, "{\"ok\":true}");
        let again = c.regenerate_with_system(6, SYSTEM_PROMPT).unwrap();
        assert!(c.is_streaming());
        assert_eq!(c.conversation.messages.len(), 2);
        assert_eq!(c.conversation.messages[0].text, "alpha");
        assert_eq!(c.conversation.messages[1].text, "");
        c.started(again.generation, 2);
        c.delta(2, "B");
        c.complete(2, true, "{\"ok\":true}");

        let edited = c.edit_and_resend(0, "beta", 7, SYSTEM_PROMPT).unwrap();
        assert_eq!(c.conversation.messages[0].text, "beta");
        assert_eq!(c.conversation.messages.len(), 2);
        c.started(edited.generation, 3);
        c.delta(3, "ok");
        assert_eq!(c.complete(3, true, "{\"ok\":true}"), Some(Outcome::Finished));
        assert_eq!(c.conversation.messages[1].text, "ok");
    }

    #[test]
    fn temporary_chat_keeps_title_and_exports_markdown() {
        let mut c = Chat::temporary(Conversation::new(ID, 1).unwrap());
        assert!(c.temporary);
        assert_eq!(c.conversation.title, "Temporary chat");
        let turn = c.send("secret question", 2).unwrap();
        assert_eq!(c.conversation.title, "Temporary chat");
        c.started(turn.generation, 1);
        c.delta(1, "quiet answer");
        c.complete(1, true, "{\"ok\":true}");
        let md = c.to_markdown();
        assert!(md.contains("# Temporary chat"));
        assert!(md.contains("## You"));
        assert!(md.contains("secret question"));
        assert!(md.contains("## Mobius"));
        assert!(md.contains("quiet answer"));
    }

    #[test]
    fn what_arrives_before_the_operation_id_is_not_lost() {
        let mut c = chat();
        let turn = c.send("hi", 0).unwrap();
        assert!(!c.delta(9, "Hel"));
        assert!(!c.delta(9, "lo"));
        assert_eq!(c.complete(9, true, "{}"), None, "the end came before the id");
        assert_eq!(c.started(turn.generation, 9), Some(Outcome::Finished));
        assert_eq!(c.conversation.messages[1].text, "Hello");
    }

    #[test]
    fn stopping_keeps_what_was_written_and_ignores_the_rest() {
        let mut c = chat();
        let turn = c.send("tell me a story", 0).unwrap();
        c.started(turn.generation, 3);
        c.delta(3, "Once upon");
        assert_eq!(c.stop(), Some(3));
        assert!(!c.delta(3, " a time"), "text after Stop is not added");
        assert_eq!(c.complete(3, true, "{\"cancelled\":true}"), Some(Outcome::Stopped));
        assert_eq!(c.conversation.messages[1].text, "Once upon");
        // Stopping before anything was written takes the question back.
        let mut d = chat();
        let t = d.send("q", 0).unwrap();
        d.started(t.generation, 4);
        d.stop();
        assert_eq!(d.complete(4, false, "{\"cancelled\":true}"), Some(Outcome::Stopped));
        assert!(d.conversation.messages.is_empty());
    }

    #[test]
    fn a_failure_says_why_and_leaves_no_empty_reply() {
        let mut c = chat();
        let t = c.send("hi", 0).unwrap();
        c.started(t.generation, 1);
        let out = c.complete(1, false, "{\"error\":\"model not loaded\"}");
        assert_eq!(out, Some(Outcome::Failed("model not loaded".into())));
        assert!(c.conversation.messages.is_empty());
        assert_eq!(c.error, "model not loaded");
        let mut d = chat();
        let t = d.send("hi", 0).unwrap();
        d.failed_to_start(t.generation, "The assistant is not running.");
        assert!(d.conversation.messages.is_empty() && !d.is_streaming());
    }

    #[test]
    fn a_late_answer_to_an_older_turn_is_harmless() {
        let mut c = chat();
        let t = c.send("one", 0).unwrap();
        c.started(t.generation, 1);
        c.complete(1, true, "{}");
        c.delta(1, "late");
        assert_eq!(c.complete(1, true, "{}"), None);
        let t2 = c.send("two", 0).unwrap();
        c.started(t2.generation, 2);
        assert!(!c.delta(1, "from the old turn"));
        assert_eq!(c.conversation.messages.last().unwrap().text, "");
    }

    #[test]
    fn history_sent_to_the_model_fits_and_starts_with_the_person() {
        let mut c = chat();
        for i in 0..40 {
            c.conversation.messages.push(Message { role: Role::User, text: format!("question {i} {}", "x".repeat(600)) });
            c.conversation.messages.push(Message { role: Role::Assistant, text: format!("answer {i} {}", "y".repeat(600)) });
        }
        let ctx = c.conversation.context();
        let total: usize = ctx.iter().skip(1).map(|(_, t)| t.chars().count()).sum();
        assert!(total <= CONTEXT_CHARS + 200, "{total}");
        assert_eq!(ctx[0].0, "system");
        assert_eq!(ctx[1].0, "user");
        assert!(ctx.last().unwrap().1.starts_with("answer 39"));
    }

    #[test]
    fn saved_files_are_read_strictly() {
        let mut c = chat();
        let t = c.send("hello\u{202e}there", 12).unwrap();
        c.started(t.generation, 1);
        c.delta(1, "hi!");
        c.complete(1, true, "{}");
        let back = Conversation::from_json(&c.conversation.to_json()).unwrap();
        assert_eq!(back.messages.len(), 2);
        assert_eq!(back.messages[0].text, "hellothere", "direction overrides are dropped");
        assert_eq!(back.id, ID);
        for bad in ["", "{", "[]", "{\"id\":\"../x\",\"messages\":[]}", &format!("{{\"id\":\"{ID}\",\"messages\":[{{\"role\":\"system\",\"text\":\"x\"}}]}}")] {
            assert!(Conversation::from_json(bad).is_none(), "{bad}");
        }
    }

    #[test]
    fn the_view_renders_the_model_as_blocks_and_the_person_as_text() {
        let mut c = chat();
        let t = c.send("show **me**", 0).unwrap();
        c.started(t.generation, 1);
        c.delta(1, "# Hi\n\n```sh\nls\n```");
        let v = c.view();
        assert!(v.messages[0].blocks.is_empty() && v.messages[0].text == "show **me**");
        assert_eq!(v.messages[1].blocks.len(), 2);
        assert!(v.streaming);
    }

    #[test]
    fn compose_attachments_fences_and_truncates() {
        let (out, truncated) = Conversation::compose_with_attachments(
            "Explain this",
            &[("demo.rs".into(), "fn main() {}\n".into())],
        );
        assert!(out.contains("Explain this"));
        assert!(out.contains("Attached: demo.rs"));
        assert!(out.contains("```rust"));
        assert!(out.contains("fn main()"));
        assert!(!truncated);
        let big = "x".repeat(MAX_ATTACH_CHARS + 500);
        let (_, cut) = Conversation::compose_with_attachments("hi", &[("big.txt".into(), big)]);
        assert!(cut);
    }

    #[test]
    fn fork_copies_history_and_renames() {
        let mut c = chat();
        let t = c.send("plan the week", 1).unwrap();
        c.started(t.generation, 1);
        c.delta(1, "focus mornings");
        c.complete(1, true, "{\"ok\":true}");
        c.conversation.project = "Life".into();
        assert_eq!(c.conversation.preview(), "plan the week");
        let forked = c.conversation.fork("fedcba9876543210", Some(0), 9).unwrap();
        assert_eq!(forked.id, "fedcba9876543210");
        assert_eq!(forked.messages.len(), 1);
        assert_eq!(forked.messages[0].text, "plan the week");
        assert!(forked.title.contains("fork"));
        assert_eq!(forked.project, "Life");
        assert!(!forked.pinned);
        let full = c.conversation.fork("aaaaaaaaaaaaaaaa", None, 10).unwrap();
        assert_eq!(full.messages.len(), 2);
    }
}
