//! Skills: short procedures Mobius wrote down after a task worked ("how to run this project's tests", "how to file the weekly report"), kept on this
//! computer as plain JSON the person can read, edit and delete. A task sees only their names and one-line descriptions and reads one when it fits
//! (progressive disclosure keeps the prompt small). They are never given to a remote model.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

pub const MAX_SKILLS: usize = 60;
const MAX_BODY: usize = 4000;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Skill {
    pub name: String,
    pub description: String,
    pub body: String,
    #[serde(default)]
    pub created: u64,
}

pub fn dir() -> PathBuf {
    crate::data_home().join("helix/mobius/skills")
}

pub fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 40
        && name
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
        && !name.starts_with('-')
}

/// Lower-case words joined with dashes, for a name the person typed freely.
pub fn slug(text: &str) -> String {
    let mut out = String::new();
    for c in text.chars() {
        if c.is_ascii_alphanumeric() {
            out.push(c.to_ascii_lowercase());
        } else if !out.ends_with('-') && !out.is_empty() {
            out.push('-');
        }
    }
    out.trim_end_matches('-')
        .chars()
        .take(40)
        .collect::<String>()
        .trim_end_matches('-')
        .to_string()
}

pub fn list() -> Vec<Skill> {
    let mut skills: Vec<Skill> = std::fs::read_dir(dir())
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.path().extension().is_some_and(|x| x == "json"))
        .filter_map(|e| serde_json::from_slice::<Skill>(&std::fs::read(e.path()).ok()?).ok())
        .filter(|s| valid_name(&s.name))
        .collect();
    skills.sort_by(|a, b| a.name.cmp(&b.name));
    skills
}

pub fn read(name: &str) -> Option<Skill> {
    if !valid_name(name) {
        return None;
    }
    serde_json::from_slice(&std::fs::read(dir().join(format!("{name}.json"))).ok()?).ok()
}

pub fn save(name: &str, description: &str, body: &str, now: u64) -> Result<Skill, String> {
    let name = slug(name);
    if !valid_name(&name) {
        return Err("Give the skill a short name (letters, numbers and dashes).".into());
    }
    let description: String = description
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .chars()
        .take(200)
        .collect();
    let body: String = body.trim().chars().take(MAX_BODY).collect();
    if description.is_empty() || body.is_empty() {
        return Err("A skill needs a one-line description and the steps.".into());
    }
    if read(&name).is_none() && list().len() >= MAX_SKILLS {
        return Err("There are already as many skills as Mobius keeps. Delete one first.".into());
    }
    let skill = Skill {
        name: name.clone(),
        description,
        body,
        created: read(&name).map_or(now, |s| s.created),
    };
    std::fs::create_dir_all(dir()).map_err(|e| e.to_string())?;
    let path = dir().join(format!("{name}.json"));
    let temp = path.with_extension("json.tmp");
    std::fs::write(
        &temp,
        serde_json::to_vec_pretty(&skill).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    std::fs::rename(&temp, &path).map_err(|e| e.to_string())?;
    Ok(skill)
}

pub fn delete(name: &str) -> Result<(), String> {
    if !valid_name(name) {
        return Err("That is not a skill.".into());
    }
    match std::fs::remove_file(dir().join(format!("{name}.json"))) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e.to_string()),
    }
}

/// The lines a planner sees: each skill's name and what it is for.
pub fn index_text(skills: &[Skill]) -> String {
    if skills.is_empty() {
        return String::new();
    }
    let mut text = String::from("\n\nSkills you can read with skill_read (procedures the person kept; read one when it fits the task):\n");
    for skill in skills.iter().take(30) {
        text.push_str(&format!("- {}: {}\n", skill.name, skill.description));
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_are_slugs_and_cannot_escape_the_folder() {
        assert_eq!(slug("Run the Tests!"), "run-the-tests");
        assert_eq!(slug("  --  "), "");
        assert_eq!(slug("../../etc/passwd"), "etc-passwd");
        assert!(
            valid_name("run-tests")
                && !valid_name("../x")
                && !valid_name("Run")
                && !valid_name("-a")
                && !valid_name("")
        );
        assert!(slug(&"a".repeat(100)).len() <= 40);
    }

    #[test]
    fn a_skill_is_saved_edited_listed_and_deleted() {
        let _turn = crate::DATA_ENV.lock().unwrap_or_else(|e| e.into_inner());
        let data = std::env::temp_dir().join(format!("mobius-skills-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&data);
        std::env::set_var("XDG_DATA_HOME", &data);
        assert!(save("", "d", "b", 1).is_err());
        assert!(save("x", "", "b", 1).is_err());
        let first = save(
            "Run Tests",
            "How to run  the\ntests",
            "1. cargo test\n2. read failures",
            10,
        )
        .unwrap();
        assert_eq!(first.name, "run-tests");
        assert_eq!(first.description, "How to run the tests");
        let edited = save("run-tests", "How to run the tests", "1. cargo test -q", 99).unwrap();
        assert_eq!(edited.created, 10, "editing keeps when it was made");
        assert_eq!(read("run-tests").unwrap().body, "1. cargo test -q");
        assert!(read("../run-tests").is_none());
        save("deploy", "Ship it", "1. push", 11).unwrap();
        assert_eq!(
            list().iter().map(|s| s.name.as_str()).collect::<Vec<_>>(),
            ["deploy", "run-tests"]
        );
        let index = index_text(&list());
        assert!(index.contains("- deploy: Ship it") && index.contains("skill_read"));
        assert_eq!(index_text(&[]), "");
        delete("run-tests").unwrap();
        delete("run-tests").unwrap();
        assert!(delete("../x").is_err());
        assert_eq!(list().len(), 1);
    }
}

#[tauri::command]
pub fn agent_skills() -> Vec<Skill> {
    list()
}

#[tauri::command]
pub fn agent_skill_save(
    name: String,
    description: String,
    body: String,
) -> Result<Vec<Skill>, String> {
    save(&name, &description, &body, crate::now())?;
    Ok(list())
}

#[tauri::command]
pub fn agent_skill_delete(name: String) -> Result<Vec<Skill>, String> {
    delete(&name)?;
    Ok(list())
}

/// A skill written down from what a task did, when the model's own draft cannot be used: the goal, then each step that worked.
pub fn from_steps(goal: &str, steps: &[(String, String)]) -> Skill {
    let name = slug(
        goal.split_whitespace()
            .take(5)
            .collect::<Vec<_>>()
            .join(" ")
            .as_str(),
    );
    let mut body = String::new();
    for (n, (tool, input)) in steps.iter().take(20).enumerate() {
        let input: String = input.chars().take(200).collect();
        body.push_str(&format!(
            "{}. {tool}{}\n",
            n + 1,
            if input.is_empty() {
                String::new()
            } else {
                format!(": {input}")
            }
        ));
    }
    Skill {
        name: if name.is_empty() { "task".into() } else { name },
        description: goal
            .split_whitespace()
            .collect::<Vec<_>>()
            .join(" ")
            .chars()
            .take(120)
            .collect(),
        body: body.trim().to_string(),
        created: 0,
    }
}

#[cfg(test)]
mod draft_tests {
    use super::*;

    #[test]
    fn a_skill_can_be_written_from_the_steps_that_worked() {
        let skill = from_steps(
            "Run the project's tests and report failures",
            &[
                ("terminal".into(), "cargo test".into()),
                ("web_search".into(), "".into()),
            ],
        );
        assert_eq!(skill.name, "run-the-project-s-tests-and");
        assert!(skill.description.starts_with("Run the project's tests"));
        assert_eq!(skill.body, "1. terminal: cargo test\n2. web_search");
        assert_eq!(from_steps("???", &[]).name, "task");
    }
}
