use crate::{Mode, Policy, Task};

/// The first two words of a plain command (`cargo test`, `git status`), which is what a policy allows by.
fn command_prefix(command: &str) -> Option<String> {
    if command.chars().any(|c| {
        matches!(
            c,
            '|' | '&' | ';' | '<' | '>' | '`' | '$' | '(' | ')' | '\n' | '\\'
        )
    }) {
        return None;
    }
    let words: Vec<&str> = command.split_whitespace().collect();
    let first = *words.first()?;
    if matches!(
        first,
        "sudo"
            | "su"
            | "doas"
            | "pkexec"
            | "rm"
            | "dd"
            | "mv"
            | "chmod"
            | "chown"
            | "curl"
            | "wget"
            | "ssh"
            | "scp"
    ) {
        return None;
    }
    // A flag-like second word (`ls -la`) is not part of what is allowed.
    match words.get(1) {
        Some(second)
            if !second.starts_with('-') && !second.contains('/') && !second.contains('.') =>
        {
            Some(format!("{first} {second}"))
        }
        _ => Some(first.to_string()),
    }
}

/// From a finished practice run: the policy that would have let that run go through without asking. Only what the run needed is opened, and it never
/// suggests the risky: privilege commands, deleting, moving, network tools, or paths outside the folder.
pub fn suggest(task: &Task) -> Policy {
    let mut policy = task.policy.clone();
    policy.mode = Mode::Enforce;
    policy.name = "From a practice run".into();
    for step in task.steps.iter().filter(|s| !s.would_ask.is_empty()) {
        match step.tool.as_str() {
            "terminal" => {
                if let Some(prefix) = command_prefix(&step.input) {
                    if !policy.allow_commands.contains(&prefix) {
                        policy.allow_commands.push(prefix);
                    }
                }
            }
            "edit_file" => policy.auto_edits = true,
            "display_launch" => {
                let app = step.input.trim().to_string();
                if !app.is_empty() && !policy.apps.iter().any(|a| a.eq_ignore_ascii_case(&app)) {
                    policy.apps.push(app);
                }
            }
            _ => {}
        }
    }
    policy.allow_commands.truncate(40);
    policy.apps.truncate(20);
    policy
}

#[cfg(test)]
mod tests {
    use super::*;

    fn practice(steps: &[(&str, &str)]) -> Task {
        let mut policy = Policy::balanced("/p");
        policy.mode = Mode::Practice;
        let mut task = Task::new("t", "goal", policy, 0);
        for (tool, input) in steps {
            task.record(tool, input, "ok", true, "local", 1);
            task.mark_would_ask("asked");
        }
        task
    }

    #[test]
    fn a_practice_run_becomes_a_policy_that_allows_what_it_needed() {
        let task = practice(&[
            ("terminal", "cargo clippy --all"),
            ("terminal", "npm install"),
            ("edit_file", "a.rs"),
            ("display_launch", "Firefox"),
        ]);
        let policy = suggest(&task);
        assert_eq!(policy.mode, Mode::Enforce);
        assert!(policy.allow_commands.contains(&"cargo clippy".to_string()));
        assert!(policy.allow_commands.contains(&"npm install".to_string()));
        assert!(policy.auto_edits);
        assert_eq!(policy.apps, ["Firefox"]);
        assert!(
            policy.allow_commands.contains(&"cargo test".to_string()),
            "the starting policy is kept"
        );
    }

    #[test]
    fn risky_commands_are_never_suggested() {
        let task = practice(&[
            ("terminal", "sudo make install"),
            ("terminal", "rm -rf build"),
            ("terminal", "ls | sh"),
            ("terminal", "curl http://x"),
            ("terminal", "ssh host"),
        ]);
        let policy = suggest(&task);
        assert_eq!(policy.allow_commands, Policy::balanced("/p").allow_commands);
    }

    #[test]
    fn steps_the_policy_already_allowed_add_nothing() {
        let mut task = practice(&[]);
        task.record("terminal", "git status", "ok", true, "local", 1);
        assert_eq!(
            suggest(&task).allow_commands,
            Policy::balanced("/p").allow_commands
        );
    }

    #[test]
    fn a_command_is_allowed_by_its_leading_words_not_its_arguments() {
        assert_eq!(
            command_prefix("cargo test -p foo"),
            Some("cargo test".into())
        );
        assert_eq!(command_prefix("ls -la"), Some("ls".into()));
        assert_eq!(command_prefix("python3 script.py"), Some("python3".into()));
        assert_eq!(command_prefix("make"), Some("make".into()));
    }
}
