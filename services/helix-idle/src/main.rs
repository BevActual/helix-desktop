//! helix-idle: when nobody has touched the machine for a while, lock the session and then turn the screen off; turn it back on at the first
//! touch. It also locks when the system asks (`loginctl lock-session`) and before the machine sleeps. Idle time comes from the compositor
//! (`ext-idle-notify-v1`), so a video or a presentation that inhibits idling is respected without this process knowing anything about it.
//! The commands it runs come from `~/.config/helix/idle.toml` as argument lists, never shell strings.

use std::process::{Command, Stdio};
use std::time::Duration;

use helix_idle::Config;
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{wl_registry::WlRegistry, wl_seat::WlSeat};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols::ext::idle_notify::v1::client::{
    ext_idle_notification_v1::{self, ExtIdleNotificationV1},
    ext_idle_notifier_v1::ExtIdleNotifierV1,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    Lock,
    ScreenOff,
}

struct App {
    config: Config,
}

fn config_path() -> std::path::PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config")))
        .unwrap_or_default();
    base.join("helix/idle.toml")
}

/// Starts a command without waiting for it (reaped on its own thread).
fn spawn(argv: &[String]) {
    let Some((program, args)) = argv.split_first() else {
        return;
    };
    match Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(mut child) => {
            std::thread::spawn(move || {
                let _ = child.wait();
            });
        }
        Err(error) => eprintln!("IDLE could not run {program}: {error}"),
    }
}

/// Runs a command and waits for it, for the moment before sleep when the lock has to be in place first.
fn run_and_wait(argv: &[String]) {
    if let Some((program, args)) = argv.split_first() {
        let _ = Command::new(program)
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
}

impl Dispatch<ExtIdleNotificationV1, Kind> for App {
    fn event(
        app: &mut Self,
        _: &ExtIdleNotificationV1,
        event: ext_idle_notification_v1::Event,
        kind: &Kind,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match (event, kind) {
            (ext_idle_notification_v1::Event::Idled, Kind::Lock) => {
                eprintln!("IDLE lock");
                spawn(&app.config.lock_command);
            }
            (ext_idle_notification_v1::Event::Idled, Kind::ScreenOff) => {
                eprintln!("IDLE screen off");
                spawn(&app.config.screen_off_command);
            }
            (ext_idle_notification_v1::Event::Resumed, Kind::ScreenOff) => {
                eprintln!("IDLE screen on");
                spawn(&app.config.screen_on_command);
            }
            _ => {}
        }
    }
}

macro_rules! ignore_events {
    ($($proxy:ty),* $(,)?) => {$(
        impl Dispatch<$proxy, ()> for App {
            fn event(_: &mut Self, _: &$proxy, _: <$proxy as wayland_client::Proxy>::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
        }
    )*};
}
ignore_events!(WlSeat, ExtIdleNotifierV1);

impl Dispatch<WlRegistry, GlobalListContents> for App {
    fn event(
        _: &mut Self,
        _: &WlRegistry,
        _: <WlRegistry as wayland_client::Proxy>::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

/// Locks when the system says to (`loginctl lock-session`, a lid, another program) and before the machine sleeps. Needs the system bus; a
/// session without one (a test) simply never gets these.
fn watch_logind(config: Config) {
    std::thread::spawn(move || {
        let Ok(connection) = zbus::blocking::Connection::system() else {
            eprintln!("IDLE no system bus: only idle locking");
            return;
        };
        let lock_config = config.clone();
        let session = zbus::blocking::Proxy::new(
            &connection,
            "org.freedesktop.login1",
            "/org/freedesktop/login1/session/auto",
            "org.freedesktop.login1.Session",
        );
        if let Ok(session) = session {
            std::thread::spawn(move || {
                if let Ok(signals) = session.receive_signal("Lock") {
                    for _ in signals {
                        eprintln!("IDLE lock requested");
                        spawn(&lock_config.lock_command);
                    }
                }
            });
        }
        let Ok(manager) = zbus::blocking::Proxy::new(
            &connection,
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
        ) else {
            return;
        };
        // A delay inhibitor makes the system wait for us (up to its limit) at the start of a sleep, so the lock is up before the screen is.
        let take = |m: &zbus::blocking::Proxy<'_>| {
            m.call::<_, _, zbus::zvariant::OwnedFd>(
                "Inhibit",
                &("sleep", "helix-idle", "lock before sleep", "delay"),
            )
            .ok()
        };
        let mut inhibitor = take(&manager);
        let Ok(signals) = manager.receive_signal("PrepareForSleep") else {
            return;
        };
        for message in signals {
            let Ok(going_to_sleep) = message.body().deserialize::<bool>() else {
                continue;
            };
            if going_to_sleep {
                eprintln!("IDLE lock before sleep");
                run_and_wait(&config.lock_command);
                inhibitor = None; // lets the sleep go ahead
            } else if inhibitor.is_none() {
                inhibitor = take(&manager);
            }
        }
    });
}

fn main() {
    let config = std::fs::read_to_string(config_path())
        .map(|t| Config::parse(&t))
        .unwrap_or_default();
    if !config.enabled {
        eprintln!("IDLE switched off in idle.toml");
        loop {
            std::thread::sleep(Duration::from_secs(3600));
        }
    }
    let connection = Connection::connect_to_env().expect("no Wayland compositor");
    let (globals, mut queue) = registry_queue_init::<App>(&connection).expect("registry");
    let qh = queue.handle();
    let seat: WlSeat = globals.bind(&qh, 1..=9, ()).expect("a seat");
    let notifier: ExtIdleNotifierV1 = globals
        .bind(&qh, 1..=2, ())
        .expect("ext-idle-notify-v1: the compositor cannot report idleness");
    let millis = |d: Duration| u32::try_from(d.as_millis()).unwrap_or(u32::MAX);
    let _lock = notifier.get_idle_notification(millis(config.lock_after), &seat, &qh, Kind::Lock);
    let _off = config
        .screen_off_after
        .map(|after| notifier.get_idle_notification(millis(after), &seat, &qh, Kind::ScreenOff));
    watch_logind(config.clone());
    eprintln!(
        "IDLE watching: lock after {:?}, screen off after {:?}",
        config.lock_after, config.screen_off_after
    );
    let mut app = App { config };
    loop {
        if queue.blocking_dispatch(&mut app).is_err() {
            eprintln!("IDLE the compositor went away");
            return;
        }
    }
}
