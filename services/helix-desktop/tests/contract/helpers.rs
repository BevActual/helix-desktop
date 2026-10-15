use helix_desktop_api::{Desktop, Domain};
use serde_json::Value;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub fn assert_setting_number(row: &Value, want: f64) {
    let got = row.get("value").expect("setting value");
    let as_f64 = got
        .as_f64()
        .or_else(|| got.as_i64().map(|whole| whole as f64));
    assert_eq!(as_f64, Some(want), "unexpected value {got}");
}
pub async fn wait_bluetooth_available(desktop: &Desktop, available: bool) {
    for _ in 0..240 {
        let snap = desktop.snapshot(Domain::Bluetooth).await.expect("bt snap");
        if snap.get("available").and_then(|v| v.as_bool()) == Some(available) {
            return;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    panic!("bluetooth availability did not become {available}");
}

pub struct CompletionLog {
    inner: Arc<Mutex<HashMap<u64, (bool, String)>>>,
}

impl CompletionLog {
    pub fn spawn_bluetooth(desktop: &Desktop) -> Self {
        let inner = Arc::new(Mutex::new(HashMap::new()));
        let listener = desktop.clone_session();
        let copy = inner.clone();
        tokio::spawn(async move {
            listener
                .watch_completed(Domain::Bluetooth, |op, ok, msg| {
                    copy.lock().unwrap().insert(op, (ok, msg));
                })
                .await
                .ok();
        });
        Self { inner }
    }

    pub async fn wait(&self, operation: u64) -> (bool, String) {
        for _ in 0..80 {
            if let Some(entry) = self.inner.lock().unwrap().get(&operation).cloned() {
                return entry;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        panic!("operation {operation} never completed");
    }
}

pub async fn wait_theme_mode(desktop: &Desktop, want: &str) {
    for _ in 0..50 {
        let row = desktop.setting("theme.mode").await.expect("theme.mode");
        if row.get("value").and_then(|v| v.as_str()) == Some(want) {
            return;
        }
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    panic!("theme.mode did not become {want}");
}
