//! The coding agents the person started with `helix-agents` (Claude Code, OpenCode, Codex): which are running now, so the bar's Agents pane can list
//! them next to what Agent Surface clients are doing, and stop one. The launcher writes one line when an agent starts and one when it ends
//! (`$XDG_STATE_HOME/helix/agents/launches.jsonl`); a start with no end whose launcher process is still alive is a running session.

use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Serialize;
use serde_json::Value;

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Session {
    pub session: String,
    pub agent: String,
    pub title: String,
    pub profile: String,
    /// The folder, with the home folder shortened to `~`.
    pub project: String,
    pub boxed: bool,
    pub minutes: u64,
    pub pid: u32,
}

fn log_path() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/state")))
        .unwrap_or_default();
    base.join("helix/agents/launches.jsonl")
}

fn plain(text: &str, max: usize) -> String {
    text.chars().filter(|c| !c.is_control()).take(max).collect()
}

/// The running sessions in `log`, newest first. `alive` says whether a launcher process is still there.
pub fn running(log: &str, home: &str, now: u64, alive: &dyn Fn(u32) -> bool) -> Vec<Session> {
    let mut starts: Vec<Value> = Vec::new();
    let mut ended: Vec<String> = Vec::new();
    for line in log.lines().rev().take(400) {
        let Ok(value) = serde_json::from_str::<Value>(line) else {
            continue;
        };
        match value["event"].as_str() {
            Some("end") => ended.extend(value["session"].as_str().map(str::to_string)),
            Some("start") => starts.push(value),
            _ => {}
        }
    }
    starts
        .iter()
        .filter_map(|s| {
            let session = s["session"].as_str()?.to_string();
            let pid = u32::try_from(s["pid"].as_u64()?).ok()?;
            if ended.contains(&session) || !alive(pid) {
                return None;
            }
            let project = s["project"].as_str().unwrap_or("");
            let project = match project.strip_prefix(home) {
                Some(rest) if !home.is_empty() => format!("~{rest}"),
                _ => project.to_string(),
            };
            Some(Session {
                session: plain(&session, 64),
                agent: plain(s["agent"].as_str().unwrap_or(""), 20),
                title: plain(
                    s["title"]
                        .as_str()
                        .unwrap_or(s["agent"].as_str().unwrap_or("")),
                    30,
                ),
                profile: plain(s["profile"].as_str().unwrap_or(""), 10),
                project: plain(&project, 120),
                boxed: s["boxed"].as_bool().unwrap_or(false),
                minutes: now.saturating_sub(s["time"].as_u64().unwrap_or(now)) / 60,
                pid,
            })
        })
        .collect()
}

/// Whether `pid` is a `helix-agents` process (not a recycled number that now belongs to something else).
fn launcher_alive(pid: u32) -> bool {
    std::fs::read_link(Path::new("/proc").join(pid.to_string()).join("exe"))
        .map(|exe| {
            exe.to_string_lossy()
                .trim_end_matches(" (deleted)")
                .ends_with("/helix-agents")
        })
        .unwrap_or(false)
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

fn sessions() -> Vec<Session> {
    let text = std::fs::read_to_string(log_path()).unwrap_or_default();
    let home = std::env::var("HOME").unwrap_or_default();
    running(&text, &home, now(), &launcher_alive)
}

#[tauri::command]
pub fn agent_sessions() -> Vec<Session> {
    sessions()
}

/// Ends a session the launcher started: only one that is listed as running, and only if its process is still the launcher.
#[tauri::command]
pub fn agent_session_stop(session: String) -> Result<(), String> {
    let found = sessions()
        .into_iter()
        .find(|s| s.session == session)
        .ok_or("That agent is not running any more.")?;
    // The launcher's child (bwrap and the agent) goes with it: they are told to die with their parent.
    // SAFETY: a signal to a process just checked to be the launcher.
    let sent = unsafe { libc::kill(found.pid as i32, libc::SIGTERM) };
    if sent == 0 {
        eprintln!("AGENTS stopped {} ({})", found.title, found.session);
        Ok(())
    } else {
        Err("It could not be stopped.".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(event: &str, session: &str, pid: u64, time: u64) -> String {
        format!(
            r#"{{"event":"{event}","session":"{session}","pid":{pid},"time":{time},"agent":"claude","title":"Claude Code","profile":"auto","project":"/home/a/work/app","boxed":true}}"#
        )
    }

    #[test]
    fn a_start_without_an_end_whose_launcher_is_alive_is_running() {
        let log = [
            line("start", "s1", 100, 1000),
            line("end", "s1", 100, 1100),
            line("start", "s2", 200, 1200),
            line("start", "s3", 300, 1300),
            "not json".to_string(),
        ]
        .join("\n");
        let alive = |pid: u32| pid == 200;
        let running = running(&log, "/home/a", 1500, &alive);
        assert_eq!(running.len(), 1, "s1 ended, s3's launcher is gone");
        assert_eq!(
            (
                running[0].session.as_str(),
                running[0].project.as_str(),
                running[0].minutes
            ),
            ("s2", "~/work/app", 5)
        );
        assert!(running[0].boxed);
    }

    #[test]
    fn what_the_log_says_is_shown_as_plain_bounded_text() {
        let nasty = r#"{"event":"start","session":"s\u0007","pid":1,"time":1,"agent":"x","title":"A\u001b[31mB","profile":"auto","project":"/h/p"}"#;
        let rows = running(nasty, "", 61, &|_| true);
        assert!(!rows[0].title.contains('\u{1b}') && !rows[0].session.contains('\u{7}'));
    }
}
