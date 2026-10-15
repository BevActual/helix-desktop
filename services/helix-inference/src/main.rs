//! helix-inference: the sandboxed model service at `org.helix.Inference` (D-Bus activated). Port of C++
//! `src/inference/service_main.cpp`. Engines: a scripted one for tests (`HELIX_INFERENCE_BACKEND=fake`); the llama.cpp
//! engine is added in a later change.

mod backend;
mod fake;
mod pictures;
mod queue;
mod routing;
mod service;
mod workers;

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use tokio::signal::unix::{signal, SignalKind};
use zbus::fdo::DBusProxy;
use zbus::names::BusName;

use backend::{Backend, ImageGenerator, Transcriber};
use service::{Config, Inner, Service, BUS_NAME, INTERFACE, OBJECT_PATH};

fn env(name: &str, fallback: &str) -> String {
    std::env::var(name).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| fallback.to_string())
}

fn default_model_dir() -> PathBuf {
    if let Ok(data) = std::env::var("XDG_DATA_HOME") {
        if !data.is_empty() {
            return PathBuf::from(data).join("helix").join("models");
        }
    }
    if let Ok(home) = std::env::var("HOME") {
        if !home.is_empty() {
            return PathBuf::from(home).join(".local/share/helix/models");
        }
    }
    PathBuf::from("/nonexistent")
}

fn sibling(name: &str) -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(|dir| dir.join(name)))
        .unwrap_or_else(|| PathBuf::from(name))
}

#[tokio::main]
async fn main() {
    // An engine worker may fail before reading its input.
    // SAFETY: ignoring SIGPIPE has no preconditions.
    unsafe { libc::signal(libc::SIGPIPE, libc::SIG_IGN) };

    let model_dir = env("HELIX_MODEL_DIR", &default_model_dir().to_string_lossy());
    let idle_seconds: u64 = env("HELIX_INFERENCE_IDLE_SECONDS", "300").parse().unwrap_or(300);
    let kind = env("HELIX_INFERENCE_BACKEND", "llama");

    let backend: Arc<dyn Backend> = if kind == "fake" {
        let delay: u64 = env("HELIX_FAKE_DELAY_MS", "0").parse().unwrap_or(0);
        Arc::new(fake::FakeBackend::new(
            &env("HELIX_FAKE_REPLY", "hello from the fake backend"),
            Duration::from_millis(delay),
        ))
    } else {
        eprintln!("helix-inference: built without an engine; set HELIX_INFERENCE_BACKEND=fake to test");
        std::process::exit(1);
    };

    // Speech to text: the worker program sits beside this one (or where HELIX_TRANSCRIBE_BIN says).
    let transcriber: Arc<dyn Transcriber> = if kind == "fake" && env("HELIX_TRANSCRIBER", "") != "process" {
        Arc::new(fake::FakeTranscriber)
    } else {
        let worker = std::env::var_os("HELIX_TRANSCRIBE_BIN")
            .map(PathBuf::from)
            .unwrap_or_else(|| sibling("helix-transcribe"));
        Arc::new(workers::ProcessTranscriber::new(worker, PathBuf::from(&model_dir).join("speech")))
    };
    let image_generator: Arc<dyn ImageGenerator> =
        if kind == "fake" && env("HELIX_IMAGE_GENERATOR", "") != "process" {
            Arc::new(fake::FakeImageGenerator)
        } else {
            let worker = std::env::var_os("HELIX_IMAGE_BIN")
                .map(PathBuf::from)
                .unwrap_or_else(|| sibling("helix-image"));
            Arc::new(workers::ProcessImageGenerator::new(worker, PathBuf::from(&model_dir).join("image")))
        };

    let config = Config {
        model_dir,
        idle_unload: Duration::from_secs(idle_seconds),
        limits: queue::Limits::default(),
    };

    let builder = match zbus::connection::Builder::session() {
        Ok(builder) => builder,
        Err(error) => {
            eprintln!("helix-inference: no session bus: {error}");
            std::process::exit(1);
        }
    };
    // The connection exists before the service so its dispatcher can emit; the name is requested after the object is up.
    let connection = match builder.build().await {
        Ok(connection) => connection,
        Err(error) => {
            eprintln!("helix-inference: could not connect: {error}");
            std::process::exit(1);
        }
    };
    let service = Arc::new(Inner::new(connection.clone(), config, backend, transcriber, image_generator));
    // Registered through a thin wrapper so the main loop keeps a handle for owner-gone and idle ticks.
    if let Err(error) = connection
        .object_server()
        .at(OBJECT_PATH, Service(Arc::clone(&service)))
        .await
    {
        eprintln!("helix-inference: could not register: {error}");
        std::process::exit(1);
    }
    if let Err(error) = connection.request_name(BUS_NAME).await {
        eprintln!("helix-inference: could not own {BUS_NAME}: {error}");
        std::process::exit(1);
    }

    // Callers that vanish have what they queued cancelled.
    if let Ok(proxy) = DBusProxy::new(&connection).await {
        if let Ok(mut changes) = proxy.receive_name_owner_changed().await {
            let watched = Arc::clone(&service);
            tokio::spawn(async move {
                use futures_util::StreamExt;
                while let Some(change) = changes.next().await {
                    if let Ok(args) = change.args() {
                        if args.new_owner().is_none() {
                            watched.owner_gone(&args.name().to_string());
                        }
                    }
                }
            });
        }
    }

    // Frees the model after a long idle spell.
    {
        let ticking = Arc::clone(&service);
        let connection = connection.clone();
        tokio::spawn(async move {
            let mut every = tokio::time::interval(Duration::from_secs(15));
            loop {
                every.tick().await;
                if ticking.tick() {
                    let _ = connection
                        .emit_signal(None::<BusName<'_>>, OBJECT_PATH, INTERFACE, "Changed", &())
                        .await;
                }
            }
        });
    }

    let mut term = signal(SignalKind::terminate()).expect("SIGTERM handler");
    let mut interrupt = signal(SignalKind::interrupt()).expect("SIGINT handler");
    tokio::select! {
        _ = term.recv() => {}
        _ = interrupt.recv() => {}
    }
    drop(connection); // stops serving before the worker is joined
    drop(service);
}

