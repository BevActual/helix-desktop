//! Parity with `tests/template_hook_runner_test.cpp` and async hook timing from `template_hook_async_test.cpp`.

use std::fs;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use helix_theme::hook_runner::HookRunner;

fn sentinel(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!("helix_hook_runner_{}_{}", name, std::process::id()))
}

fn read_sentinel(path: &PathBuf) -> String {
    fs::read_to_string(path).unwrap_or_default().trim().to_string()
}

#[test]
fn runs_every_enqueued_hook() {
    let path = sentinel("run");
    let _ = fs::remove_file(&path);
    {
        let runner = HookRunner::new(2);
        for _ in 0..4 {
            runner.enqueue(format!("printf x >> {}", path.display()), 1, String::new());
        }
        runner.wait_idle();
        assert_eq!(runner.pending_count(), 0);
    }
    assert_eq!(read_sentinel(&path), "xxxx");
    let _ = fs::remove_file(&path);
}

#[test]
fn drops_hooks_from_superseded_generations() {
    let path = sentinel("generation");
    let _ = fs::remove_file(&path);
    {
        let runner = HookRunner::new(2);
        runner.invalidate_before(2);
        runner.enqueue(format!("printf stale > {}", path.display()), 1, String::new());
        runner.enqueue(format!("printf current > {}", path.display()), 2, String::new());
        runner.wait_idle();
        assert_eq!(runner.pending_count(), 0);
    }
    assert_eq!(read_sentinel(&path), "current");
    let _ = fs::remove_file(&path);
}

#[test]
fn reports_failed_hooks_with_template_id() {
    let failed = Arc::new(Mutex::new((String::new(), String::new())));
    {
        let runner = HookRunner::new(1);
        runner.set_on_failure({
            let failed = Arc::clone(&failed);
            move |id, error| {
                *failed.lock().unwrap() = (id, error);
            }
        });
        runner.enqueue("false".into(), 1, "cava".into());
        runner.wait_idle();
    }
    let (id, error) = failed.lock().unwrap().clone();
    assert_eq!(id, "cava");
    assert!(!error.is_empty());
}

#[test]
fn invalidate_drops_queued_hooks() {
    let path = sentinel("invalidate");
    let _ = fs::remove_file(&path);
    {
        let runner = HookRunner::new(1);
        runner.enqueue("sleep 0.2".into(), 1, String::new());
        runner.enqueue(format!("printf stale > {}", path.display()), 1, String::new());
        runner.invalidate_before(2);
        runner.wait_idle();
        assert_eq!(runner.pending_count(), 0);
    }
    assert!(!path.exists());
}

#[test]
fn shutdown_drops_backlog_and_awaits_running() {
    let running = sentinel("shutdown_running");
    let queued = sentinel("shutdown_queued");
    let _ = fs::remove_file(&running);
    let _ = fs::remove_file(&queued);
    {
        let runner = HookRunner::new(1);
        runner.enqueue(
            format!("sleep 0.2; printf ran > {}", running.display()),
            1,
            String::new(),
        );
        runner.enqueue(format!("printf ran > {}", queued.display()), 1, String::new());
        runner.request_shutdown();
        runner.wait_idle();
    }
    assert!(running.exists());
    assert!(!queued.exists());
    let _ = fs::remove_file(&running);
}

#[test]
fn async_post_hook_returns_before_slow_hook_finishes() {
    let marker = sentinel("async_marker");
    let _ = fs::remove_file(&marker);
    let runner = HookRunner::new(4);
    let start = Instant::now();
    runner.enqueue(format!("sleep 1; touch {}", marker.display()), 1, String::new());
    assert!(start.elapsed().as_secs_f64() < 0.5);
    assert!(!marker.exists());
    runner.wait_idle();
    assert!(marker.exists());
}

#[test]
fn inline_post_hook_waits_for_started_hooks() {
    let order = sentinel("order");
    let _ = fs::remove_file(&order);
    let runner = HookRunner::new(4);
    let start = Instant::now();
    runner.enqueue(
        format!("sleep 0.4; printf background, >> {}", order.display()),
        1,
        "background".into(),
    );
    // `execute_entries` calls `wait_idle` before a synchronous post_hook.
    runner.wait_idle();
    std::process::Command::new("/bin/sh")
        .arg("-lc")
        .arg(format!("printf inline, >> {}", order.display()))
        .status()
        .unwrap();
    assert_eq!(fs::read_to_string(&order).unwrap(), "background,inline,");
    assert!(start.elapsed().as_secs_f64() >= 0.4);
}
