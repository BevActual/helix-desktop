use crate::Policy;
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum State {
    Queued,
    Running,
    /// Waiting for the person: a review, or help with a step only they can do.
    Waiting {
        why: String,
    },
    Done {
        summary: String,
    },
    Failed {
        error: String,
    },
    Stopped,
}

impl State {
    pub fn finished(&self) -> bool {
        matches!(
            self,
            Self::Done { .. } | Self::Failed { .. } | Self::Stopped
        )
    }
}

/// One thing the task did, in order. `place` says where the model that chose it ran ("local", "lan" or the cloud's name), so the record shows
/// what left the computer.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Step {
    pub n: u32,
    pub tool: String,
    pub input: String,
    pub outcome: String,
    pub ok: bool,
    pub place: String,
    pub at: u64,
    /// In a practice run: what the policy would have asked about for this step (empty otherwise).
    #[serde(default)]
    pub would_ask: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Task {
    pub id: String,
    pub goal: String,
    pub policy: Policy,
    pub state: State,
    pub steps: Vec<Step>,
    /// What the task wrote down for itself to keep going after the early steps have been dropped from its context.
    pub notes: String,
    /// The chat holding the answer and the steps, once the task is done.
    #[serde(default)]
    pub result_chat: String,
    pub created: u64,
    pub updated: u64,
}

pub const MAX_RECORD_STEPS: usize = 400;
const MAX_TEXT: usize = 2000;

fn clip(text: &str) -> String {
    text.chars().take(MAX_TEXT).collect()
}

impl Task {
    pub fn new(id: &str, goal: &str, policy: Policy, now: u64) -> Self {
        Self {
            id: id.into(),
            goal: clip(goal),
            policy,
            state: State::Queued,
            steps: vec![],
            result_chat: String::new(),
            notes: String::new(),
            created: now,
            updated: now,
        }
    }

    pub fn record(
        &mut self,
        tool: &str,
        input: &str,
        outcome: &str,
        ok: bool,
        place: &str,
        now: u64,
    ) {
        if self.steps.len() >= MAX_RECORD_STEPS {
            self.steps.remove(0);
        }
        let n = self.steps.last().map_or(1, |s| s.n + 1);
        self.steps.push(Step {
            n,
            tool: tool.into(),
            input: clip(input),
            outcome: clip(outcome),
            ok,
            place: place.into(),
            at: now,
            would_ask: String::new(),
        });
        self.updated = now;
    }

    /// Marks the latest step as one a practice run let through that the policy would have asked about.
    pub fn mark_would_ask(&mut self, reason: &str) {
        if let Some(step) = self.steps.last_mut() {
            step.would_ask = clip(reason);
        }
    }

    pub fn set_state(&mut self, state: State, now: u64) {
        // A finished task stays finished: a late signal from a stopped run must not reopen it.
        if !self.state.finished() {
            self.state = state;
            self.updated = now;
        }
    }

    pub fn note(&mut self, text: &str) {
        self.notes = text.chars().take(4000).collect();
    }

    /// How many steps ran somewhere other than this computer.
    pub fn remote_steps(&self) -> usize {
        self.steps.iter().filter(|s| s.place != "local").count()
    }

    pub fn elapsed_minutes(&self, now: u64) -> u32 {
        (now.saturating_sub(self.created) / 60) as u32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_finished_task_stays_finished() {
        let mut task = Task::new("t1", "goal", Policy::careful(), 10);
        task.set_state(State::Running, 11);
        task.set_state(State::Stopped, 12);
        task.set_state(State::Running, 13);
        assert_eq!(task.state, State::Stopped);
        assert_eq!(task.updated, 12);
    }

    #[test]
    fn steps_are_numbered_clipped_and_bounded() {
        let mut task = Task::new("t1", "goal", Policy::careful(), 0);
        for i in 0..(MAX_RECORD_STEPS + 5) {
            task.record(
                "terminal",
                "ls",
                &"x".repeat(5000),
                true,
                if i % 2 == 0 { "local" } else { "acme" },
                i as u64,
            );
        }
        assert_eq!(task.steps.len(), MAX_RECORD_STEPS);
        assert_eq!(task.steps.last().unwrap().n, (MAX_RECORD_STEPS + 5) as u32);
        assert!(task.steps[0].outcome.chars().count() <= 2000);
        assert!(task.remote_steps() > 100);
    }

    #[test]
    fn elapsed_time_counts_minutes_from_creation() {
        let task = Task::new("t1", "g", Policy::careful(), 600);
        assert_eq!(task.elapsed_minutes(600 + 61), 1);
        assert_eq!(task.elapsed_minutes(0), 0);
    }
}
