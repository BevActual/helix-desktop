//! Scheduled prompts: a question or a task for the model that runs by itself at a time ("every morning, summarise what I should look at").
//!
//! The timing belongs to systemd: each task is a user timer and a oneshot service that runs `helix-mobius --run-scheduled <id>`, so it fires
//! whether or not Mobius is open (`Persistent=true` catches up a run missed while the computer was off). The run is headless: it asks the model
//! on this computer, saves the answer as a new chat named after the task and tells the person with a notification. It is a chat-only task:
//! no tools, no desktop actions, never a cloud model, so nothing unattended can change anything or send text away.

use std::collections::HashMap;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

pub const MAX_TASKS: usize = 12;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum When {
    Daily {
        time: String,
    },
    /// Days are 0 (Monday) to 6 (Sunday).
    Weekly {
        time: String,
        days: Vec<u8>,
    },
    Once {
        date: String,
        time: String,
    },
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Task {
    pub id: String,
    pub name: String,
    pub prompt: String,
    pub when: When,
    pub enabled: bool,
    #[serde(default)]
    pub last_run: u64,
    #[serde(default)]
    pub last_chat: String,
    #[serde(default)]
    pub last_error: String,
}

fn file() -> PathBuf {
    crate::data_home().join("helix/mobius/scheduled.json")
}

pub fn load() -> Vec<Task> {
    std::fs::read_to_string(file())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn save(tasks: &[Task]) -> Result<(), String> {
    if let Some(dir) = file().parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    std::fs::write(
        file(),
        serde_json::to_string_pretty(tasks).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())
}

fn valid_time(time: &str) -> bool {
    let Some((h, m)) = time.split_once(':') else {
        return false;
    };
    h.len() == 2
        && m.len() == 2
        && h.parse::<u8>().is_ok_and(|h| h < 24)
        && m.parse::<u8>().is_ok_and(|m| m < 60)
}

fn valid_date(date: &str) -> bool {
    let parts: Vec<&str> = date.split('-').collect();
    parts.len() == 3
        && parts[0].len() == 4
        && parts[0].parse::<u16>().is_ok_and(|y| y >= 2024)
        && parts[1].len() == 2
        && parts[1].parse::<u8>().is_ok_and(|m| (1..=12).contains(&m))
        && parts[2].len() == 2
        && parts[2].parse::<u8>().is_ok_and(|d| (1..=31).contains(&d))
}

const DAYS: [&str; 7] = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];

/// The systemd calendar expression for a schedule; checked, so nothing from the page reaches a unit file unchecked.
pub fn on_calendar(when: &When) -> Result<String, String> {
    match when {
        When::Daily { time } if valid_time(time) => Ok(format!("*-*-* {time}:00")),
        When::Weekly { time, days }
            if valid_time(time) && !days.is_empty() && days.iter().all(|d| *d < 7) =>
        {
            let mut days = days.clone();
            days.sort_unstable();
            days.dedup();
            Ok(format!(
                "{} *-*-* {time}:00",
                days.iter()
                    .map(|d| DAYS[*d as usize])
                    .collect::<Vec<_>>()
                    .join(",")
            ))
        }
        When::Once { date, time } if valid_date(date) && valid_time(time) => {
            Ok(format!("{date} {time}:00"))
        }
        _ => Err("That time is not valid.".into()),
    }
}

pub fn describe(when: &When) -> String {
    match when {
        When::Daily { time } => format!("Every day at {time}"),
        When::Weekly { time, days } => format!(
            "{} at {time}",
            days.iter()
                .filter(|d| **d < 7)
                .map(|d| DAYS[*d as usize])
                .collect::<Vec<_>>()
                .join(", ")
        ),
        When::Once { date, time } => format!("Once, on {date} at {time}"),
    }
}

fn unit_dir() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
        .unwrap_or_else(|| PathBuf::from("."))
        .join("systemd/user")
}

fn unit_name(id: &str) -> String {
    format!("helix-mobius-task-{id}")
}

pub fn unit_texts(task: &Task, exe: &str) -> Result<(String, String), String> {
    let calendar = on_calendar(&task.when)?;
    let name: String = task
        .name
        .chars()
        .filter(|c| !c.is_control())
        .take(60)
        .collect();
    let service = format!(
        "[Unit]\nDescription=Mobius scheduled prompt: {name}\n\n[Service]\nType=oneshot\nExecStart=\"{exe}\" --run-scheduled {}\n",
        task.id
    );
    let timer = format!(
        "[Unit]\nDescription=Mobius scheduled prompt: {name}\n\n[Timer]\nOnCalendar={calendar}\nPersistent=true\n\n[Install]\nWantedBy=timers.target\n"
    );
    Ok((service, timer))
}

fn systemctl(args: &[&str]) {
    if std::env::var_os("HELIX_SCHEDULED_NO_SYSTEMD").is_some() {
        return;
    }
    let _ = std::process::Command::new("systemctl")
        .arg("--user")
        .args(args)
        .status();
}

fn install(task: &Task) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let (service, timer) = unit_texts(task, &exe.to_string_lossy())?;
    let dir = unit_dir();
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let base = unit_name(&task.id);
    std::fs::write(dir.join(format!("{base}.service")), service).map_err(|e| e.to_string())?;
    std::fs::write(dir.join(format!("{base}.timer")), timer).map_err(|e| e.to_string())?;
    systemctl(&["daemon-reload"]);
    systemctl(&["enable", "--now", &format!("{base}.timer")]);
    Ok(())
}

fn uninstall(id: &str) {
    let base = unit_name(id);
    systemctl(&["disable", "--now", &format!("{base}.timer")]);
    let dir = unit_dir();
    let _ = std::fs::remove_file(dir.join(format!("{base}.timer")));
    let _ = std::fs::remove_file(dir.join(format!("{base}.service")));
    systemctl(&["daemon-reload"]);
}

#[derive(Serialize)]
pub struct Row {
    #[serde(flatten)]
    pub task: Task,
    pub summary: String,
}

fn rows() -> Vec<Row> {
    load()
        .into_iter()
        .map(|task| Row {
            summary: describe(&task.when),
            task,
        })
        .collect()
}

#[tauri::command]
pub fn scheduled_list() -> Vec<Row> {
    rows()
}

#[tauri::command]
pub fn scheduled_save(name: String, prompt: String, when: When) -> Result<Vec<Row>, String> {
    let name = name.split_whitespace().collect::<Vec<_>>().join(" ");
    let prompt = prompt.trim().to_string();
    if name.is_empty() || name.chars().count() > 60 {
        return Err("Give the task a short name.".into());
    }
    if prompt.is_empty()
        || prompt.chars().count() > 2000
        || prompt
            .chars()
            .any(|c| c.is_control() && c != '\n' && c != '\t')
    {
        return Err("Write what to ask (up to 2,000 characters).".into());
    }
    on_calendar(&when)?;
    let mut tasks = load();
    if tasks.len() >= MAX_TASKS {
        return Err(format!(
            "{MAX_TASKS} scheduled tasks already exist. Delete one first."
        ));
    }
    let task = Task {
        id: crate::new_id(),
        name,
        prompt,
        when,
        enabled: true,
        last_run: 0,
        last_chat: String::new(),
        last_error: String::new(),
    };
    install(&task)?;
    tasks.push(task);
    save(&tasks)?;
    Ok(rows())
}

#[tauri::command]
pub fn scheduled_set_enabled(id: String, enabled: bool) -> Result<Vec<Row>, String> {
    let mut tasks = load();
    let task = tasks
        .iter_mut()
        .find(|t| t.id == id)
        .ok_or("That task no longer exists.")?;
    task.enabled = enabled;
    if enabled {
        install(task)?;
    } else {
        systemctl(&["disable", "--now", &format!("{}.timer", unit_name(&id))]);
    }
    save(&tasks)?;
    Ok(rows())
}

#[tauri::command]
pub fn scheduled_delete(id: String) -> Result<Vec<Row>, String> {
    let mut tasks = load();
    if !tasks.iter().any(|t| t.id == id) {
        return Err("That task no longer exists.".into());
    }
    uninstall(&id);
    tasks.retain(|t| t.id != id);
    save(&tasks)?;
    Ok(rows())
}

/// Runs a task now, in the same way the timer would, without waiting for the program to finish.
#[tauri::command]
pub fn scheduled_run_now(id: String) -> Result<(), String> {
    if !load().iter().any(|t| t.id == id) {
        return Err("That task no longer exists.".into());
    }
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    std::process::Command::new(exe)
        .arg("--run-scheduled")
        .arg(&id)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

fn record_result(id: &str, chat: &str, error: &str) {
    let mut tasks = load();
    if let Some(task) = tasks.iter_mut().find(|t| t.id == id) {
        task.last_run = crate::now();
        task.last_chat = chat.to_string();
        task.last_error = error.to_string();
        if matches!(task.when, When::Once { .. }) {
            task.enabled = false;
        }
    }
    let _ = save(&tasks);
}

/// A short desktop notice (scheduled tasks and Mobius when a tool needs approval).
pub(crate) async fn desktop_notify(summary: &str, body: &str) {
    let Ok(connection) = zbus::Connection::session().await else {
        return;
    };
    let Ok(proxy) = zbus::Proxy::new(
        &connection,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
    )
    .await
    else {
        return;
    };
    let hints: HashMap<&str, zbus::zvariant::Value<'_>> = HashMap::new();
    let _: Result<u32, _> = proxy
        .call(
            "Notify",
            &(
                "Mobius",
                0u32,
                "org.helix.Mobius",
                summary,
                body,
                Vec::<String>::new(),
                hints,
                -1i32,
            ),
        )
        .await;
}

/// The headless run for `--run-scheduled <id>`. Returns the process's exit code.
pub async fn run(id: &str) -> i32 {
    let Some(task) = load().into_iter().find(|t| t.id == id) else {
        return 2;
    };
    match ask(&task).await {
        Ok(chat) => {
            record_result(id, &chat, "");
            desktop_notify(
                &format!("{} is ready", task.name),
                "Open Mobius to read it.",
            )
            .await;
            0
        }
        Err(error) => {
            record_result(id, "", &error);
            desktop_notify(&format!("{} could not run", task.name), &error).await;
            1
        }
    }
}

async fn ask(task: &Task) -> Result<String, String> {
    use futures_util::StreamExt;
    let routing = helix_config::inference_routing::load();
    let persona = helix_chat::personas::load().active_persona().clone();
    let (route_task, model, is_local, _) = crate::resolve_route(&routing, &persona);
    if !is_local {
        return Err("Scheduled prompts only use a model on this computer. Set the chat route to a local model.".into());
    }
    let mut system = helix_chat::chat::SYSTEM_PROMPT.to_string();
    if let Some(text) = helix_chat::memory::Memory::load(&crate::memory_file()).prompt_text() {
        system.push_str(&text);
    }
    let messages = vec![("system", system), ("user", task.prompt.clone())];
    let args =
        crate::chat_args(&messages, &[], &route_task, &model, None).map_err(|e| e.to_string())?;
    let connection = zbus::Connection::session()
        .await
        .map_err(|e| e.to_string())?;
    let proxy = zbus::Proxy::new(
        &connection,
        crate::BUS_NAME,
        crate::OBJECT_PATH,
        crate::INTERFACE,
    )
    .await
    .map_err(|_| "The inference service is not available.".to_string())?;
    let mut done = proxy
        .receive_signal("Completed")
        .await
        .map_err(|e| e.to_string())?;
    let operation: u64 = proxy
        .call("Action", &("chat", args))
        .await
        .map_err(|e| e.to_string())?;
    let text = tokio::time::timeout(std::time::Duration::from_secs(1800), async {
        while let Some(message) = done.next().await {
            if let Ok((op, ok, result)) = message.body().deserialize::<(u64, bool, String)>() {
                if op == operation {
                    if !ok {
                        return Err("The model could not answer.".to_string());
                    }
                    let value: serde_json::Value =
                        serde_json::from_str(&result).unwrap_or_default();
                    return Ok(value["text"].as_str().unwrap_or("").to_string());
                }
            }
        }
        Err("The inference service stopped.".to_string())
    })
    .await
    .map_err(|_| "The model took too long.".to_string())??;
    let answer = helix_chat::markdown::strip_thinking(&text)
        .trim()
        .to_string();
    if answer.is_empty() {
        return Err("The model gave no answer.".into());
    }
    let id = crate::new_id();
    let mut chat =
        helix_chat::chat::Conversation::new(&id, crate::now()).ok_or("Could not make a chat.")?;
    chat.title = task.name.chars().take(60).collect();
    for (role, body) in [
        (helix_chat::chat::Role::User, task.prompt.clone()),
        (helix_chat::chat::Role::Assistant, answer),
    ] {
        chat.messages.push(helix_chat::chat::Message {
            role,
            text: body,
            images: Vec::new(),
            sources: Vec::new(),
            activities: Vec::new(),
            stats: None,
        });
    }
    if !crate::save(&chat) {
        return Err("Could not save the answer.".into());
    }
    Ok(id)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn calendars_are_checked_and_written_for_systemd() {
        assert_eq!(
            on_calendar(&When::Daily {
                time: "08:30".into()
            })
            .unwrap(),
            "*-*-* 08:30:00"
        );
        assert_eq!(
            on_calendar(&When::Weekly {
                time: "07:00".into(),
                days: vec![2, 0, 0]
            })
            .unwrap(),
            "Mon,Wed *-*-* 07:00:00"
        );
        assert_eq!(
            on_calendar(&When::Once {
                date: "2026-12-24".into(),
                time: "09:15".into()
            })
            .unwrap(),
            "2026-12-24 09:15:00"
        );
        for bad in [
            When::Daily {
                time: "8:30".into(),
            },
            When::Daily {
                time: "24:00".into(),
            },
            When::Daily {
                time: "08:30:00\nExecStart=/bin/sh".into(),
            },
            When::Weekly {
                time: "08:00".into(),
                days: vec![],
            },
            When::Weekly {
                time: "08:00".into(),
                days: vec![7],
            },
            When::Once {
                date: "2026-13-01".into(),
                time: "08:00".into(),
            },
        ] {
            assert!(on_calendar(&bad).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn the_units_run_this_program_with_the_task_id_only() {
        let task = Task {
            id: "abc123".into(),
            name: "Morning\nbrief".into(),
            prompt: "secret prompt".into(),
            when: When::Daily {
                time: "08:00".into(),
            },
            enabled: true,
            last_run: 0,
            last_chat: String::new(),
            last_error: String::new(),
        };
        let (service, timer) = unit_texts(&task, "/usr/bin/helix-mobius").unwrap();
        assert!(service.contains("ExecStart=\"/usr/bin/helix-mobius\" --run-scheduled abc123"));
        assert!(!service.contains("secret prompt"));
        assert!(!service.contains("Morning\nbrief"));
        assert!(timer.contains("OnCalendar=*-*-* 08:00:00") && timer.contains("Persistent=true"));
    }

    #[test]
    fn describes_in_words() {
        assert_eq!(
            describe(&When::Weekly {
                time: "07:00".into(),
                days: vec![0, 2]
            }),
            "Mon, Wed at 07:00"
        );
    }
}
