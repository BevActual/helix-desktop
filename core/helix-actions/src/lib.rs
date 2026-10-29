//! Helix's catalogue of actions (`docs/design/north-star.md`, `docs/design/actions.md`).
//!
//! An action is something Helix can do, described once and used by everyone: the person's buttons and an agent's tool calls are the
//! same actions, checked by the same risk policy, recorded in the same history and undone the same way. A provider (the desktop
//! service for Settings, later the shell, Models, Software, Mobius) publishes its actions; the Agent Surface lists them, decides
//! whether a caller may run one, runs it through the provider and records the result here.

pub mod history;
pub mod settings;

use serde::{Deserialize, Serialize};
use serde_json::Value;

/// How much an action may do, from the caller's point of view. The same names as the Agent Surface tiers
/// (`protocols/ipc/msg-risk.toml`), so grants mean the same thing for every action.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Risk {
    /// Reads ordinary state.
    Observe,
    /// Reads something private (where the person is, what is on the screen); the result is untrusted data for the caller.
    ObservePrivate,
    /// Changes something, and can be undone or is harmless.
    Act,
    /// Changes something that matters; the person confirms each time, in plain words.
    Confirm,
    /// Never for an agent (it could run a program, unlock the computer, or give itself more power).
    Blocked,
}

impl Risk {
    pub fn name(self) -> &'static str {
        match self {
            Self::Observe => "observe",
            Self::ObservePrivate => "observe-private",
            Self::Act => "act",
            Self::Confirm => "confirm",
            Self::Blocked => "blocked",
        }
    }
}

/// Whether an action's effect can be taken back.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Undo {
    /// The value before is kept and put back.
    Restore,
    /// Nothing to undo (reading, or an effect that cannot be taken back).
    None,
}

/// One action in the catalogue.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Action {
    /// `provider.verb`, for example `settings.set`. Shell commands keep their own names (no dot), so the two never collide.
    pub id: String,
    /// What a person would call it: "Change a setting".
    pub title: String,
    /// One sentence a model reads to decide whether this is the action it wants.
    pub summary: String,
    /// The parameters, as JSON Schema.
    pub params: Value,
    /// The usual risk. A provider may make one call riskier than this (one setting, one argument), never less.
    pub risk: Risk,
    pub undo: Undo,
}

/// Who did something.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Actor {
    /// `person` for the person at the computer (through any Helix app), `agent` for anything acting for them.
    pub kind: ActorKind,
    /// "You", "Mobius", "Claude Code", the client's label.
    pub label: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ActorKind {
    Person,
    Agent,
}

impl Actor {
    pub fn person() -> Self {
        Self {
            kind: ActorKind::Person,
            label: "You".into(),
        }
    }

    pub fn agent(label: &str) -> Self {
        Self {
            kind: ActorKind::Agent,
            label: if label.trim().is_empty() {
                "An agent".into()
            } else {
                label.trim().chars().take(60).collect()
            },
        }
    }
}

/// Every action Helix offers today, from every provider in this crate.
pub fn catalogue() -> Vec<Action> {
    settings::actions()
}

/// The action with this id, if there is one.
pub fn find(id: &str) -> Option<Action> {
    catalogue().into_iter().find(|action| action.id == id)
}

/// Whether `id` names a catalogue action rather than a shell command.
pub fn is_action(id: &str) -> bool {
    id.contains('.')
}
