use serde::{Deserialize, Serialize};

/// How far a task may read the web without asking.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WebAccess {
    /// Every lookup asks.
    Ask,
    /// Searches, and pages the person named or search results led to, are allowed; anything else asks.
    #[default]
    Known,
}

/// What a remote (cloud or LAN-over-internet) model may be used for.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CloudUse {
    /// Only models on this computer.
    Never,
    /// A remote model may plan; a description of the screen reaches it only when the person allows each look.
    #[default]
    Planner,
    /// A remote model may plan and may be told what is on the screen (the description, never the picture).
    Screen,
}

/// Whether the policy decides for real or only shows what it would have decided.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Mode {
    /// Asks and refusals are enforced.
    #[default]
    Enforce,
    /// A practice run: what would have asked goes ahead (inside the sandbox) and is recorded, so the person can see what the task needs and turn it into
    /// a policy. What would have been refused is still refused.
    Practice,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Policy {
    pub name: String,
    /// The project folder the task may read and edit (empty: none).
    pub folder: String,
    /// Commands (by leading words) that run without asking, e.g. "cargo test". Only plain commands qualify: no pipes, redirects or chaining.
    pub allow_commands: Vec<String>,
    /// Apply edits inside the folder without asking.
    pub auto_edits: bool,
    /// Tools of the person's own tool servers that run without asking: "server/tool", or "server/*" for all of a server's tools.
    pub allow_tools: Vec<String>,
    pub web: WebAccess,
    pub cloud: CloudUse,
    /// May the task operate the agent display?
    pub display: bool,
    /// Apps it may open without asking ("*" for any).
    pub apps: Vec<String>,
    pub max_steps: u32,
    pub max_minutes: u32,
    pub mode: Mode,
    /// Write a short plan first and wait for the person to approve (or edit) it before acting.
    pub plan_first: bool,
    /// Commands may reach the network. (The sandbox cannot filter by host, so this is all or nothing.)
    pub network: bool,
    /// Absolute paths outside the folder that commands may read (a toolchain, a data set), never write.
    pub read_only: Vec<String>,
    /// Paths inside the folder commands can read but not change (`.git`).
    pub protect: Vec<String>,
    /// Paths inside the folder commands cannot see at all (`.env`).
    pub hide: Vec<String>,
}

impl Default for Policy {
    fn default() -> Self {
        Self::careful()
    }
}

impl Policy {
    /// Everything asks: the behaviour of Tools mode in a chat.
    pub fn careful() -> Self {
        Self {
            name: "Ask me first".into(),
            folder: String::new(),
            allow_commands: vec![],
            auto_edits: false,
            allow_tools: vec![],
            web: WebAccess::Ask,
            cloud: CloudUse::Planner,
            display: true,
            apps: vec![],
            max_steps: 40,
            max_minutes: 30,
            mode: Mode::Enforce,
            plan_first: false,
            network: false,
            read_only: vec![],
            protect: vec![],
            hide: vec![],
        }
    }

    /// What Tools mode in a chat has always done: web lookups follow the sources, and everything that changes something asks.
    pub fn chat(folder: &str) -> Self {
        Self {
            name: "Ask me first".into(),
            folder: folder.into(),
            web: WebAccess::Known,
            max_steps: 40,
            ..Self::careful()
        }
    }

    /// Reads and checks freely inside the folder, edits it, searches the web; anything else asks.
    pub fn balanced(folder: &str) -> Self {
        Self {
            name: "Work in the folder".into(),
            folder: folder.into(),
            allow_commands: [
                "ls",
                "cat",
                "pwd",
                "git status",
                "git diff",
                "git log",
                "cargo check",
                "cargo test",
                "cargo build",
                "npm test",
                "npm run build",
                "pytest",
            ]
            .map(String::from)
            .to_vec(),
            auto_edits: true,
            web: WebAccess::Known,
            max_steps: 80,
            max_minutes: 60,
            protect: vec![".git".into()],
            hide: vec![".env".into(), ".ssh".into(), ".gnupg".into()],
            ..Self::careful()
        }
    }

    /// "Always allow this": widens the policy by exactly what the action needed, and says what it now allows (`None` when there is nothing safe to
    /// remember: a risky or chained command, an edit that is not a plain path, anything that is not a command, an edit or an app).
    pub fn allow(&mut self, action: &Action) -> Option<String> {
        match action {
            Action::Command(command) => {
                let prefix = crate::command_prefix(command)?;
                if !self.allow_commands.contains(&prefix) {
                    self.allow_commands.push(prefix.clone());
                }
                Some(format!("Run `{prefix}` without asking"))
            }
            Action::Edit { path } if edit_path_is_plain(path) => {
                self.auto_edits = true;
                Some("Edit files in the folder without asking".into())
            }
            Action::Launch(app) if !app.trim().is_empty() => {
                if !self.apps.iter().any(|a| a.eq_ignore_ascii_case(app.trim())) {
                    self.apps.push(app.trim().to_string());
                }
                Some(format!("Open {} without asking", app.trim()))
            }
            Action::Tool { server, tool } => {
                let rule = format!("{server}/{tool}");
                if !self.allow_tools.contains(&rule) && self.allow_tools.len() < 100 {
                    self.allow_tools.push(rule);
                }
                Some(format!("Use {server}: {tool} without asking"))
            }
            _ => None,
        }
    }

    /// Whether a model may be used at all under this policy.
    pub fn allows_model(&self, local: bool) -> bool {
        local || self.cloud != CloudUse::Never
    }

    pub fn within_budget(&self, steps: u32, elapsed_minutes: u32) -> Result<(), String> {
        if steps >= self.max_steps {
            return Err(format!("Reached the step limit ({}).", self.max_steps));
        }
        if elapsed_minutes >= self.max_minutes {
            return Err(format!(
                "Reached the time limit ({} minutes).",
                self.max_minutes
            ));
        }
        Ok(())
    }

    /// What the policy says, then the mode: in a practice run an "ask" goes ahead and is recorded by the caller (see [`Verdict::Practice`]).
    pub fn decide(&self, action: &Action, ctx: &Context) -> Verdict {
        match (self.decide_enforced(action, ctx), self.mode) {
            (Verdict::Ask(why), Mode::Practice) => Verdict::Practice(why),
            (other, _) => other,
        }
    }

    fn decide_enforced(&self, action: &Action, ctx: &Context) -> Verdict {
        match action {
            Action::Command(command) => self.command(command),
            Action::Edit { path } => {
                if self.auto_edits && !self.folder.is_empty() && edit_path_is_plain(path) {
                    Verdict::Allow
                } else {
                    Verdict::Ask("Edit a file".into())
                }
            }
            Action::WebSearch => {
                if self.web == WebAccess::Known && !ctx.private {
                    Verdict::Allow
                } else {
                    Verdict::Ask("Search the web".into())
                }
            }
            Action::WebRead { known } => {
                if self.web == WebAccess::Known && *known && !ctx.private {
                    Verdict::Allow
                } else {
                    Verdict::Ask("Open a web page".into())
                }
            }
            Action::Launch(app) => {
                if !self.display {
                    Verdict::Deny("This task may not use the agent display.".into())
                } else if self
                    .apps
                    .iter()
                    .any(|a| a == "*" || a.eq_ignore_ascii_case(app.trim()))
                {
                    Verdict::Allow
                } else {
                    Verdict::Ask(format!("Open {}", app.trim()))
                }
            }
            Action::Look => {
                if !self.display {
                    Verdict::Deny("This task may not use the agent display.".into())
                } else if ctx.model_local || self.cloud == CloudUse::Screen {
                    Verdict::Allow
                } else {
                    Verdict::Ask("Describe the screen to a remote model".into())
                }
            }
            Action::Skill => Verdict::Allow,
            Action::Tool { server, tool } => {
                if self
                    .allow_tools
                    .iter()
                    .any(|t| t == &format!("{server}/{tool}") || t == &format!("{server}/*"))
                {
                    Verdict::Allow
                } else {
                    Verdict::Ask(format!("Use {server}: {tool}"))
                }
            }
            Action::Input => {
                if self.display {
                    Verdict::Allow
                } else {
                    Verdict::Deny("This task may not use the agent display.".into())
                }
            }
        }
    }

    fn command(&self, command: &str) -> Verdict {
        let command = command.trim();
        let lowered = command.to_ascii_lowercase();
        if self.folder.is_empty() {
            return Verdict::Deny("No project folder is set for this task.".into());
        }
        if words(&lowered)
            .first()
            .is_some_and(|w| matches!(*w, "sudo" | "su" | "doas" | "pkexec"))
        {
            return Verdict::Deny("Commands that raise privileges are not allowed.".into());
        }
        if is_plain(command) {
            let given = words(command);
            for allowed in &self.allow_commands {
                let want = words(allowed);
                if !want.is_empty() && given.len() >= want.len() && given[..want.len()] == want[..]
                {
                    return Verdict::Allow;
                }
            }
        }
        Verdict::Ask("Run a command".into())
    }
}

fn words(text: &str) -> Vec<&str> {
    text.split_whitespace().collect()
}

/// No pipes, redirects, chaining, substitution, globs that expand into other commands, or newlines.
fn is_plain(command: &str) -> bool {
    !command.is_empty()
        && !command.chars().any(|c| {
            matches!(
                c,
                '|' | '&' | ';' | '<' | '>' | '`' | '$' | '(' | ')' | '\n' | '\\'
            )
        })
}

/// A relative path that stays inside the folder and is not somewhere secrets or history live.
fn edit_path_is_plain(path: &str) -> bool {
    let path = path.trim();
    if path.is_empty() || path.starts_with('/') || path.starts_with('~') {
        return false;
    }
    !path.split('/').any(|part| {
        part == ".."
            || part == ".git"
            || part == ".ssh"
            || part == ".gnupg"
            || part == ".env"
            || part.starts_with(".env.")
            || part == "id_rsa"
            || part == "id_ed25519"
    })
}

/// What the task wants to do, reduced to what a policy needs to know.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Action {
    Command(String),
    Edit {
        path: String,
    },
    WebSearch,
    WebRead {
        known: bool,
    },
    Launch(String),
    /// Read the agent display's screen (described by a model on this computer).
    Look,
    /// Click, type, key, scroll on the agent display.
    Input,
    /// Read one of the person's skills (text kept on this computer).
    Skill,
    /// Call a tool of one of the person's tool servers (MCP).
    Tool {
        server: String,
        tool: String,
    },
}

/// What is true of the task right now.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Context {
    /// The model making the plan runs on this computer.
    pub model_local: bool,
    /// Something private (the screen, command output, files) is already in the task's context, so what leaves must be reviewed.
    pub private: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Verdict {
    Allow,
    /// Ask the person, saying what for.
    Ask(String),
    Deny(String),
    /// Practice mode: this would have asked; it goes ahead and the reason is recorded.
    Practice(String),
}

#[cfg(test)]
mod tests {
    use super::*;

    const LOCAL: Context = Context {
        model_local: true,
        private: false,
    };
    const CLOUD: Context = Context {
        model_local: false,
        private: false,
    };

    fn cmd(p: &Policy, c: &str) -> Verdict {
        p.decide(&Action::Command(c.into()), &LOCAL)
    }

    #[test]
    fn the_careful_policy_asks_about_everything_that_changes_something() {
        let p = Policy::careful();
        assert!(
            matches!(cmd(&p, "ls"), Verdict::Deny(_)),
            "no folder, no commands"
        );
        let mut p = Policy::careful();
        p.folder = "/p".into();
        assert!(matches!(cmd(&p, "ls"), Verdict::Ask(_)));
        assert!(matches!(
            p.decide(
                &Action::Edit {
                    path: "a.rs".into()
                },
                &LOCAL
            ),
            Verdict::Ask(_)
        ));
        assert!(matches!(
            p.decide(&Action::WebSearch, &LOCAL),
            Verdict::Ask(_)
        ));
        assert!(matches!(
            p.decide(&Action::Launch("Firefox".into()), &LOCAL),
            Verdict::Ask(_)
        ));
    }

    #[test]
    fn allowed_commands_match_whole_leading_words_and_only_when_plain() {
        let p = Policy::balanced("/p");
        assert_eq!(cmd(&p, "cargo test -p foo"), Verdict::Allow);
        assert_eq!(cmd(&p, "git status"), Verdict::Allow);
        assert!(
            matches!(cmd(&p, "cargo testing"), Verdict::Ask(_)),
            "words, not prefixes of words"
        );
        assert!(matches!(cmd(&p, "git push"), Verdict::Ask(_)));
        for chained in [
            "ls; rm -rf ~",
            "ls && curl x",
            "cat a | sh",
            "ls > /etc/x",
            "ls $(id)",
            "ls `id`",
            "ls\nrm x",
        ] {
            assert!(matches!(cmd(&p, chained), Verdict::Ask(_)), "{chained}");
        }
        assert!(matches!(cmd(&p, "sudo ls"), Verdict::Deny(_)));
        assert!(matches!(cmd(&p, "SUDO ls"), Verdict::Deny(_)));
    }

    #[test]
    fn automatic_edits_stay_inside_the_folder_and_away_from_secrets() {
        let p = Policy::balanced("/p");
        let edit = |path: &str| p.decide(&Action::Edit { path: path.into() }, &LOCAL);
        assert_eq!(edit("src/main.rs"), Verdict::Allow);
        for bad in [
            "/etc/passwd",
            "~/x",
            "../x",
            "a/../../x",
            ".git/config",
            ".env",
            "a/.env.local",
            ".ssh/id_rsa",
            "",
        ] {
            assert!(matches!(edit(bad), Verdict::Ask(_)), "{bad}");
        }
    }

    #[test]
    fn private_context_makes_web_requests_ask() {
        let p = Policy::balanced("/p");
        assert_eq!(p.decide(&Action::WebSearch, &LOCAL), Verdict::Allow);
        assert_eq!(
            p.decide(&Action::WebRead { known: true }, &LOCAL),
            Verdict::Allow
        );
        assert!(matches!(
            p.decide(&Action::WebRead { known: false }, &LOCAL),
            Verdict::Ask(_)
        ));
        let private = Context {
            private: true,
            ..LOCAL
        };
        assert!(matches!(
            p.decide(&Action::WebSearch, &private),
            Verdict::Ask(_)
        ));
    }

    #[test]
    fn a_remote_model_is_told_about_the_screen_only_as_far_as_the_policy_says() {
        let mut p = Policy::balanced("/p");
        assert_eq!(p.decide(&Action::Look, &LOCAL), Verdict::Allow);
        assert!(matches!(p.decide(&Action::Look, &CLOUD), Verdict::Ask(_)));
        p.cloud = CloudUse::Screen;
        assert_eq!(p.decide(&Action::Look, &CLOUD), Verdict::Allow);
        p.cloud = CloudUse::Never;
        assert!(!p.allows_model(false));
        assert!(p.allows_model(true));
    }

    #[test]
    fn display_use_can_be_switched_off_and_apps_listed() {
        let mut p = Policy::balanced("/p");
        p.apps = vec!["firefox".into()];
        assert_eq!(
            p.decide(&Action::Launch("Firefox".into()), &LOCAL),
            Verdict::Allow
        );
        assert!(matches!(
            p.decide(&Action::Launch("Terminal".into()), &LOCAL),
            Verdict::Ask(_)
        ));
        p.apps = vec!["*".into()];
        assert_eq!(
            p.decide(&Action::Launch("Anything".into()), &LOCAL),
            Verdict::Allow
        );
        p.display = false;
        for a in [Action::Input, Action::Look, Action::Launch("x".into())] {
            assert!(matches!(p.decide(&a, &LOCAL), Verdict::Deny(_)));
        }
    }

    #[test]
    fn always_allow_widens_the_policy_by_exactly_what_was_asked() {
        let mut p = Policy::chat("/p");
        assert!(matches!(cmd(&p, "cargo fmt --all"), Verdict::Ask(_)));
        assert_eq!(
            p.allow(&Action::Command("cargo fmt --all".into())).unwrap(),
            "Run `cargo fmt` without asking"
        );
        assert_eq!(cmd(&p, "cargo fmt -p x"), Verdict::Allow);
        assert!(
            matches!(cmd(&p, "cargo publish"), Verdict::Ask(_)),
            "only that command"
        );
        assert!(p.allow(&Action::Command("sudo ls".into())).is_none());
        assert!(p.allow(&Action::Command("ls | sh".into())).is_none());
        assert!(p
            .allow(&Action::Edit {
                path: "../x".into()
            })
            .is_none());
        assert!(!p.auto_edits);
        assert!(
            p.allow(&Action::Edit {
                path: "src/a.rs".into()
            })
            .is_some()
                && p.auto_edits
        );
        assert!(p.allow(&Action::Launch("Firefox".into())).is_some());
        assert_eq!(
            p.decide(&Action::Launch("firefox".into()), &LOCAL),
            Verdict::Allow
        );
        assert!(p.allow(&Action::WebSearch).is_none());
        let before = p.allow_commands.len();
        p.allow(&Action::Command("cargo fmt".into()));
        assert_eq!(p.allow_commands.len(), before, "no duplicates");
    }

    #[test]
    fn a_tool_of_the_persons_tool_servers_asks_until_it_is_allowed() {
        let mut p = Policy::balanced("/p");
        let call = |p: &Policy, s: &str, t: &str| {
            p.decide(
                &Action::Tool {
                    server: s.into(),
                    tool: t.into(),
                },
                &LOCAL,
            )
        };
        assert!(matches!(call(&p, "git", "status"), Verdict::Ask(_)));
        assert_eq!(
            p.allow(&Action::Tool {
                server: "git".into(),
                tool: "status".into()
            })
            .unwrap(),
            "Use git: status without asking"
        );
        assert_eq!(call(&p, "git", "status"), Verdict::Allow);
        assert!(
            matches!(call(&p, "git", "push"), Verdict::Ask(_)),
            "only that tool"
        );
        assert!(
            matches!(call(&p, "other", "status"), Verdict::Ask(_)),
            "only that server"
        );
        p.allow_tools.push("calendar/*".into());
        assert_eq!(call(&p, "calendar", "anything"), Verdict::Allow);
        let mut practice = Policy::chat("/p");
        practice.mode = Mode::Practice;
        assert!(matches!(
            call(&practice, "git", "status"),
            Verdict::Practice(_)
        ));
    }

    #[test]
    fn budgets_stop_a_task() {
        let p = Policy::balanced("/p");
        assert!(p.within_budget(79, 59).is_ok());
        assert!(p.within_budget(80, 0).is_err());
        assert!(p.within_budget(0, 60).is_err());
    }

    #[test]
    fn a_policy_read_from_a_partial_file_gets_safe_defaults() {
        let p: Policy = serde_json::from_str(r#"{"folder":"/p"}"#).unwrap();
        assert!(!p.auto_edits && p.allow_commands.is_empty());
        assert_eq!(p.web, WebAccess::Ask);
    }
}
