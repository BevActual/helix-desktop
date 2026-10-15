//! One conversation with `helix-assistant`. No process and no widgets: a runner starts the
//! assistant, and [`Session::deliver`] is how its output comes back. The rules match
//! Former C++ `assistant_session` (now Rust):
//! a change runs only after confirm, and only the exact call that was shown;
//! cancel or a newer request makes a late answer harmless;
//! everything shown is display-safe.

use std::collections::VecDeque;

use serde_json::Value;

use crate::display::sanitize_for_display;

pub const MAX_REQUEST_CHARS: usize = 500;
const MAX_SHOWN: usize = 600;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Idle,
    Thinking,
    Confirm,
    Acting,
    Result,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct View {
    pub phase: Phase,
    pub text: String,
    pub description: String,
    pub reason: String,
    pub failed: bool,
    pub unverified: bool,
}

impl Default for View {
    fn default() -> Self {
        Self {
            phase: Phase::Idle,
            text: String::new(),
            description: String::new(),
            reason: String::new(),
            failed: false,
            unverified: false,
        }
    }
}

#[derive(Clone, Copy)]
enum Job {
    Plan,
    Execute,
}

struct Queued {
    generation: u64,
    job: Job,
}

/// A command the shell should start. Tests ignore it and call [`Session::deliver`] instead.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Spawn {
    pub generation: u64,
    pub args: Vec<String>,
}

type Runner = Box<dyn FnMut(&[String]) -> bool + Send>;

pub struct Session {
    runner: Runner,
    view: View,
    generation: u64,
    request: String,
    tool: String,
    args: Vec<String>,
    queue: VecDeque<Queued>,
    spawns: Vec<Spawn>,
}

impl Session {
    pub fn new(runner: impl FnMut(&[String]) -> bool + Send + 'static) -> Self {
        Self {
            runner: Box::new(runner),
            view: View::default(),
            generation: 0,
            request: String::new(),
            tool: String::new(),
            args: Vec::new(),
            queue: VecDeque::new(),
            spawns: Vec::new(),
        }
    }

    pub fn view(&self) -> &View {
        &self.view
    }

    /// Commands the runner accepted and nobody has taken yet. The shell spawns these.
    pub fn take_spawns(&mut self) -> Vec<Spawn> {
        std::mem::take(&mut self.spawns)
    }

    pub fn submit(&mut self, request: &str) {
        if self.view.phase != Phase::Idle && self.view.phase != Phase::Result {
            return;
        }
        let trimmed = request.trim_matches(|c: char| c.is_ascii_whitespace());
        if trimmed.is_empty() {
            return;
        }
        if trimmed.len() > MAX_REQUEST_CHARS {
            self.fail("That is too long. Keep it under 500 characters.");
            return;
        }
        self.request = trimmed.to_string();
        self.tool.clear();
        self.args.clear();
        self.generation += 1;
        let generation = self.generation;
        self.view = View {
            phase: Phase::Thinking,
            ..View::default()
        };
        let arguments = vec!["--json".to_string(), self.request.clone()];
        self.start(generation, Job::Plan, arguments);
    }

    pub fn confirm(&mut self) {
        if self.view.phase != Phase::Confirm || self.tool.is_empty() {
            return;
        }
        self.generation += 1;
        let generation = self.generation;
        self.view.phase = Phase::Acting;
        let mut arguments = vec![
            "--json".to_string(),
            "--request".to_string(),
            self.request.clone(),
            "--execute".to_string(),
            self.tool.clone(),
        ];
        arguments.extend(self.args.iter().cloned());
        // A second confirm cannot run it again.
        self.tool.clear();
        self.args.clear();
        self.start(generation, Job::Execute, arguments);
    }

    /// Back to idle from anywhere. An answer still on its way is dropped.
    pub fn cancel(&mut self) {
        self.generation += 1;
        self.tool.clear();
        self.args.clear();
        self.view = View::default();
    }

    /// The oldest answer still waiting. Late answers for an older generation are ignored.
    pub fn deliver(&mut self, code: i32, output: String) {
        let Some(queued) = self.queue.pop_front() else {
            return;
        };
        self.finish(queued, code, &output);
    }

    /// An answer for a known generation, in any order. Unknown generations are ignored.
    pub fn deliver_generation(&mut self, generation: u64, code: i32, output: String) {
        let Some(pos) = self
            .queue
            .iter()
            .position(|queued| queued.generation == generation)
        else {
            return;
        };
        let queued = self.queue.remove(pos).expect("position was just found");
        self.finish(queued, code, &output);
    }

    /// The process never started. Drops that generation and, if it is still current, says so.
    pub fn launch_failed(&mut self, generation: u64) {
        self.queue.retain(|queued| queued.generation != generation);
        if generation == self.generation {
            self.fail("The assistant is not installed (helix-assistant was not found).");
        }
    }

    fn start(&mut self, generation: u64, job: Job, arguments: Vec<String>) {
        let started = (self.runner)(&arguments);
        if started {
            self.queue.push_back(Queued { generation, job });
            self.spawns.push(Spawn {
                generation,
                args: arguments,
            });
        } else if generation == self.generation {
            self.fail("The assistant is not installed (helix-assistant was not found).");
        }
    }

    fn finish(&mut self, queued: Queued, code: i32, output: &str) {
        match queued.job {
            Job::Plan => self.handle_plan(code, output, queued.generation),
            Job::Execute => self.handle_result(output, queued.generation),
        }
    }

    fn handle_plan(&mut self, _code: i32, output: &str, generation: u64) {
        if generation != self.generation || self.view.phase != Phase::Thinking {
            return;
        }
        let Ok(reply) = serde_json::from_str::<Value>(output) else {
            self.fail("The assistant did not give an answer.");
            return;
        };
        let Some(kind) = reply.get("kind").and_then(Value::as_str) else {
            self.fail("The assistant did not give an answer.");
            return;
        };
        if kind == "confirm" {
            let Some(tool) = reply
                .get("tool")
                .and_then(Value::as_str)
                .filter(|tool| !tool.is_empty())
            else {
                self.fail("The assistant gave an answer that could not be read.");
                return;
            };
            let Some(raw_args) = reply.get("args").and_then(Value::as_array) else {
                self.fail("The assistant gave an answer that could not be read.");
                return;
            };
            let mut args = Vec::with_capacity(raw_args.len());
            for arg in raw_args {
                let Some(text) = arg.as_str() else {
                    self.fail("The assistant gave an answer that could not be read.");
                    return;
                };
                args.push(text.to_string());
            }
            let description = shown(&reply, "description");
            if description.is_empty() {
                self.fail("The assistant gave an answer that could not be read.");
                return;
            }
            self.tool = tool.to_string();
            self.args = args;
            self.view = View {
                phase: Phase::Confirm,
                description,
                reason: shown(&reply, "reason"),
                ..View::default()
            };
            return;
        }
        if kind == "done" {
            self.handle_result(output, generation);
            return;
        }
        let text = shown(&reply, "text");
        self.fail(if text.is_empty() {
            "Something went wrong."
        } else {
            &text
        });
    }

    fn handle_result(&mut self, output: &str, generation: u64) {
        if generation != self.generation
            || (self.view.phase != Phase::Acting && self.view.phase != Phase::Thinking)
        {
            return;
        }
        let Ok(reply) = serde_json::from_str::<Value>(output) else {
            self.fail("The assistant did not give an answer.");
            return;
        };
        let Some(kind) = reply.get("kind").and_then(Value::as_str) else {
            self.fail("The assistant did not give an answer.");
            return;
        };
        let text = shown(&reply, "text");
        if kind == "done" {
            self.view = View {
                phase: Phase::Result,
                text: if text.is_empty() {
                    "Done.".to_string()
                } else {
                    text
                },
                unverified: reply
                    .get("untrusted")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                ..View::default()
            };
        } else {
            self.fail(if text.is_empty() {
                "Something went wrong."
            } else {
                &text
            });
        }
    }

    fn fail(&mut self, text: &str) {
        self.view = View {
            phase: Phase::Result,
            text: text.to_string(),
            failed: true,
            ..View::default()
        };
    }
}

fn shown(object: &Value, key: &str) -> String {
    object
        .get(key)
        .and_then(Value::as_str)
        .map(|text| sanitize_for_display(text, MAX_SHOWN))
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use super::{Phase, Session};

    const CONFIRM: &str = r#"{"kind":"confirm","description":"Make the screen dimmer (`brightness-down`)","reason":"you asked","tool":"brightness-down","args":[]}"#;

    struct Harness {
        started: Arc<Mutex<Vec<Vec<String>>>>,
        can_start: Arc<Mutex<bool>>,
        session: Session,
    }

    impl Harness {
        fn new() -> Self {
            let started = Arc::new(Mutex::new(Vec::new()));
            let can_start = Arc::new(Mutex::new(true));
            let started_for_runner = started.clone();
            let can_start_for_runner = can_start.clone();
            let session = Session::new(move |arguments| {
                if !*can_start_for_runner.lock().expect("can_start") {
                    return false;
                }
                started_for_runner
                    .lock()
                    .expect("started")
                    .push(arguments.to_vec());
                true
            });
            Self {
                started,
                can_start,
                session,
            }
        }

        fn finish(&mut self, output: &str) {
            self.session.deliver(0, output.to_string());
        }
    }

    #[test]
    fn confirm_runs_exactly_the_call_that_was_shown() {
        let mut harness = Harness::new();
        harness.session.submit("  make the screen dimmer \n");
        assert_eq!(harness.session.view().phase, Phase::Thinking);
        assert_eq!(
            harness.started.lock().expect("started").as_slice(),
            [vec![
                "--json".to_string(),
                "make the screen dimmer".to_string()
            ]]
        );
        harness.finish(CONFIRM);
        assert_eq!(harness.session.view().phase, Phase::Confirm);
        assert_eq!(harness.started.lock().expect("started").len(), 1);
        assert_eq!(
            harness.session.view().description,
            "Make the screen dimmer (`brightness-down`)"
        );
        assert_eq!(harness.session.view().reason, "you asked");

        harness.session.confirm();
        assert_eq!(harness.session.view().phase, Phase::Acting);
        assert_eq!(
            harness.started.lock().expect("started")[1],
            vec![
                "--json".to_string(),
                "--request".to_string(),
                "make the screen dimmer".to_string(),
                "--execute".to_string(),
                "brightness-down".to_string(),
            ]
        );
        harness.session.confirm();
        assert_eq!(harness.started.lock().expect("started").len(), 2);
        harness.finish(r#"{"kind":"done","text":"Done."}"#);
        assert_eq!(harness.session.view().phase, Phase::Result);
        assert!(!harness.session.view().failed);
        assert_eq!(harness.session.view().text, "Done.");
    }

    #[test]
    fn arguments_pass_through_after_the_tool() {
        let mut harness = Harness::new();
        harness.session.submit("dark mode");
        harness.finish(r#"{"kind":"confirm","description":"Switch to dark","reason":"","tool":"theme-mode-set","args":["dark"]}"#);
        harness.session.confirm();
        let started = harness.started.lock().expect("started");
        assert_eq!(started[1].last().map(String::as_str), Some("dark"));
        assert_eq!(started[1][4], "theme-mode-set");
    }

    #[test]
    fn reading_is_answered_without_asking() {
        let mut harness = Harness::new();
        harness.session.submit("is the bar visible?");
        harness.finish(r#"{"kind":"done","text":"{\"barVisible\":true}","untrusted":true}"#);
        assert_eq!(harness.session.view().phase, Phase::Result);
        assert!(harness.session.view().unverified);
        assert_eq!(harness.started.lock().expect("started").len(), 1);
    }

    #[test]
    fn declines_and_errors_never_run() {
        let mut harness = Harness::new();
        harness.session.submit("restart");
        harness.finish(r#"{"kind":"explain","text":"I can't do that: no tool for it."}"#);
        assert!(harness.session.view().failed);
        assert_eq!(
            harness.session.view().text,
            "I can't do that: no tool for it."
        );
        assert_eq!(harness.started.lock().expect("started").len(), 1);
        harness.session.confirm();
        assert_eq!(harness.started.lock().expect("started").len(), 1);
        harness.session.submit("again");
        harness.finish(r#"{"kind":"error","text":"The model could not answer"}"#);
        assert!(harness.session.view().failed);
    }

    #[test]
    fn cancel_drops_a_late_answer() {
        let mut harness = Harness::new();
        harness.session.submit("dim");
        harness.session.cancel();
        assert_eq!(harness.session.view().phase, Phase::Idle);
        harness.finish(CONFIRM);
        assert_eq!(harness.session.view().phase, Phase::Idle);
        harness.session.submit("dim");
        harness.finish(CONFIRM);
        harness.session.cancel();
        harness.session.confirm();
        assert_eq!(harness.started.lock().expect("started").len(), 2);
        assert_eq!(harness.session.view().phase, Phase::Idle);
    }

    #[test]
    fn a_stale_answer_cannot_overwrite_a_newer_request() {
        let mut harness = Harness::new();
        harness.session.submit("first");
        harness.session.cancel();
        harness.session.submit("second");
        harness.finish(CONFIRM);
        assert_eq!(harness.session.view().phase, Phase::Thinking);
        harness.finish(r#"{"kind":"done","text":"second answer"}"#);
        assert_eq!(harness.session.view().text, "second answer");
    }

    #[test]
    fn cancel_while_acting_drops_the_late_result() {
        let mut harness = Harness::new();
        harness.session.submit("dim");
        harness.finish(CONFIRM);
        harness.session.confirm();
        harness.session.cancel();
        harness.finish(r#"{"kind":"done","text":"Done."}"#);
        assert_eq!(harness.session.view().phase, Phase::Idle);
    }

    #[test]
    fn busy_empty_and_oversized_requests_do_not_start() {
        let mut harness = Harness::new();
        harness.session.submit("one");
        harness.session.submit("two");
        assert_eq!(harness.started.lock().expect("started").len(), 1);
        harness.finish(CONFIRM);
        harness.session.submit("three");
        assert_eq!(harness.started.lock().expect("started").len(), 1);
        assert_eq!(harness.session.view().phase, Phase::Confirm);
        harness.session.cancel();
        harness.session.submit("   ");
        assert_eq!(harness.started.lock().expect("started").len(), 1);
        assert_eq!(harness.session.view().phase, Phase::Idle);
        harness.session.submit(&"x".repeat(501));
        assert_eq!(harness.started.lock().expect("started").len(), 1);
        assert!(harness.session.view().failed);
    }

    #[test]
    fn malformed_output_is_an_error_and_never_a_call() {
        for output in [
            "",
            "not json",
            "[]",
            r#"{"kind":5}"#,
            r#"{"kind":"confirm"}"#,
            r#"{"kind":"confirm","description":"x","tool":"","args":[]}"#,
            r#"{"kind":"confirm","description":"x","tool":"a","args":[1]}"#,
            r#"{"kind":"confirm","description":"","tool":"a","args":[]}"#,
        ] {
            let mut harness = Harness::new();
            harness.session.submit("x");
            harness.finish(output);
            assert_eq!(harness.session.view().phase, Phase::Result, "{output}");
            assert!(harness.session.view().failed, "{output}");
            harness.session.confirm();
            assert_eq!(
                harness.started.lock().expect("started").len(),
                1,
                "{output}"
            );
        }
    }

    #[test]
    fn model_text_is_display_safe() {
        let mut harness = Harness::new();
        harness.session.submit("x");
        harness.finish(r#"{"kind":"confirm","description":"Line one\nLine two\u0007\u202e","reason":"","tool":"a","args":[]}"#);
        let description = &harness.session.view().description;
        assert!(!description.contains('\n'));
        assert!(!description.contains('\u{7}'));
        assert!(!description.contains('\u{202e}'));
        assert_eq!(description, "Line one Line two");
    }

    #[test]
    fn a_missing_assistant_is_reported_and_the_session_recovers() {
        let mut harness = Harness::new();
        *harness.can_start.lock().expect("can_start") = false;
        harness.session.submit("x");
        assert_eq!(harness.session.view().phase, Phase::Result);
        assert!(harness.session.view().failed);
        *harness.can_start.lock().expect("can_start") = true;
        harness.session.submit("x");
        assert_eq!(harness.session.view().phase, Phase::Thinking);
    }
}
