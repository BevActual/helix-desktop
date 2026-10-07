//! WebKit process budgeting helpers for Linux. Tauri/wry keeps a WebContext for the app lifetime,
//! and destroying a WebviewWindow does not reliably exit its WebKitWebProcess.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::time::Duration;

use gtk::prelude::*;
use webkit2gtk::{WebContextExt, WebView, WebViewExt};

/// Cap web processes on every WebContext reachable from this widget tree.
/// WebKit deprecated the limit API in 2.26; call it anyway as a best-effort hint.
#[allow(deprecated)]
pub fn limit_web_processes(root: &impl IsA<gtk::Widget>) {
    walk(root.as_ref(), &mut |view| {
        if let Some(context) = view.context() {
            context.set_web_process_count_limit(1);
            eprintln!(
                "WEBKIT web-process-limit {}",
                context.web_process_count_limit()
            );
        }
    });
}

fn walk(widget: &gtk::Widget, on_view: &mut impl FnMut(&WebView)) {
    if let Ok(view) = widget.clone().downcast::<WebView>() {
        on_view(&view);
    }
    if let Some(container) = widget.downcast_ref::<gtk::Container>() {
        for child in container.children() {
            walk(&child, on_view);
        }
    }
}

fn descendants(root: u32) -> Vec<u32> {
    let mut ppid = BTreeMap::<u32, u32>::new();
    let Ok(entries) = fs::read_dir("/proc") else {
        return Vec::new();
    };
    for entry in entries.flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else {
            continue;
        };
        let Ok(stat) = fs::read_to_string(format!("/proc/{pid}/stat")) else {
            continue;
        };
        let Some((_, rest)) = stat.rsplit_once(')') else {
            continue;
        };
        let mut parts = rest.split_whitespace();
        let _state = parts.next();
        let Some(parent) = parts.next().and_then(|value| value.parse().ok()) else {
            continue;
        };
        ppid.insert(pid, parent);
    }
    let mut found = vec![root];
    let mut frontier = vec![root];
    while !frontier.is_empty() {
        let mut next = Vec::new();
        for (&pid, &parent) in &ppid {
            if frontier.contains(&parent) && !found.contains(&pid) {
                found.push(pid);
                next.push(pid);
            }
        }
        frontier = next;
    }
    found
}

fn webkit_web_pids(root: u32) -> BTreeSet<u32> {
    let mut pids = BTreeSet::new();
    for pid in descendants(root) {
        let Ok(name) = fs::read_to_string(format!("/proc/{pid}/comm")) else {
            continue;
        };
        if name.trim().starts_with("WebKitWeb") {
            pids.insert(pid);
        }
    }
    pids
}

/// Snapshot the WebKit web processes that belong to the bar-only shell.
pub fn snapshot_webkit_web_pids() -> BTreeSet<u32> {
    webkit_web_pids(std::process::id())
}

/// After the panel webview is destroyed, WebKit often leaves its WebProcess alive. Reap any web
/// process that was not part of the bar-only baseline. Only descendants of this process are
/// considered — never the real session.
pub fn schedule_reap_extra_webkits(keep: BTreeSet<u32>) {
    std::thread::spawn(move || {
        // destroy() is queued on the GTK/Tauri event loop; give it a moment to drop the view.
        std::thread::sleep(Duration::from_millis(750));
        let root = std::process::id();
        for pid in webkit_web_pids(root) {
            if keep.contains(&pid) {
                continue;
            }
            let result = unsafe { libc::kill(pid as i32, libc::SIGTERM) };
            eprintln!("WEBKIT reap {pid} result={result}");
        }
    });
}
