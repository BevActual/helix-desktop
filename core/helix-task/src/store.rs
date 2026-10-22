use crate::Task;
use std::path::{Path, PathBuf};

/// Tasks on disk: one JSON file each, so a task survives the window, the app and a restart, and a bad file loses only itself.
pub struct Store {
    dir: PathBuf,
}

fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 64
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
}

impl Store {
    pub fn new(dir: &Path) -> Self {
        Self {
            dir: dir.to_path_buf(),
        }
    }

    fn path(&self, id: &str) -> Result<PathBuf, String> {
        if valid_id(id) {
            Ok(self.dir.join(format!("{id}.json")))
        } else {
            Err("That is not a task id.".into())
        }
    }

    pub fn save(&self, task: &Task) -> Result<(), String> {
        let path = self.path(&task.id)?;
        std::fs::create_dir_all(&self.dir).map_err(|e| e.to_string())?;
        let temp = path.with_extension("json.tmp");
        std::fs::write(
            &temp,
            serde_json::to_vec_pretty(task).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
        std::fs::rename(&temp, &path).map_err(|e| e.to_string())
    }

    pub fn load(&self, id: &str) -> Option<Task> {
        serde_json::from_slice(&std::fs::read(self.path(id).ok()?).ok()?).ok()
    }

    /// Newest first. Files that do not read are skipped.
    pub fn list(&self) -> Vec<Task> {
        let mut tasks: Vec<Task> = std::fs::read_dir(&self.dir)
            .into_iter()
            .flatten()
            .flatten()
            .filter(|e| e.path().extension().is_some_and(|x| x == "json"))
            .filter_map(|e| serde_json::from_slice(&std::fs::read(e.path()).ok()?).ok())
            .collect();
        tasks.sort_by(|a: &Task, b: &Task| b.updated.cmp(&a.updated).then(a.id.cmp(&b.id)));
        tasks
    }

    pub fn delete(&self, id: &str) -> Result<(), String> {
        match std::fs::remove_file(self.path(id)?) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(e.to_string()),
        }
    }

    /// A task that was running when the app stopped did not finish: mark it stopped so it is not shown as live.
    pub fn settle_after_restart(&self, now: u64) {
        for mut task in self.list() {
            if !task.state.finished() {
                task.set_state(crate::State::Stopped, now);
                let _ = self.save(&task);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Policy, State};

    fn dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("helix-task-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        dir
    }

    #[test]
    fn a_task_round_trips_and_lists_newest_first() {
        let store = Store::new(&dir("roundtrip"));
        let mut a = Task::new("a", "first", Policy::balanced("/p"), 1);
        a.record("terminal", "ls", "ok", true, "local", 5);
        let b = Task::new("b", "second", Policy::careful(), 9);
        store.save(&a).unwrap();
        store.save(&b).unwrap();
        assert_eq!(store.load("a").unwrap(), a);
        let ids: Vec<_> = store.list().into_iter().map(|t| t.id).collect();
        assert_eq!(ids, ["b", "a"]);
        store.delete("a").unwrap();
        assert!(store.load("a").is_none());
        store.delete("a").unwrap();
    }

    #[test]
    fn ids_cannot_leave_the_folder_and_bad_files_are_skipped() {
        let d = dir("ids");
        let store = Store::new(&d);
        assert!(store
            .save(&Task::new("../evil", "g", Policy::careful(), 0))
            .is_err());
        assert!(store.load("../etc/passwd").is_none());
        std::fs::create_dir_all(&d).unwrap();
        std::fs::write(d.join("junk.json"), "{nope").unwrap();
        store
            .save(&Task::new("ok", "g", Policy::careful(), 0))
            .unwrap();
        assert_eq!(store.list().len(), 1);
    }

    #[test]
    fn a_task_left_running_is_stopped_on_restart() {
        let store = Store::new(&dir("restart"));
        let mut t = Task::new("t", "g", Policy::careful(), 0);
        t.set_state(State::Running, 1);
        store.save(&t).unwrap();
        let mut done = Task::new("d", "g", Policy::careful(), 0);
        done.set_state(
            State::Done {
                summary: "x".into(),
            },
            2,
        );
        store.save(&done).unwrap();
        store.settle_after_restart(50);
        assert_eq!(store.load("t").unwrap().state, State::Stopped);
        assert!(matches!(store.load("d").unwrap().state, State::Done { .. }));
    }
}
