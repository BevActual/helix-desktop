//! helix-nightlight: warms the screen in the evening. The compositor already lets one client at a time own an output's gamma table
//! (`wlr-gamma-control`), so this is a client like any other: it reads `~/.config/helix/nightlight.json` (Settings writes it), works out from the
//! clock how warm the screen should be, and sets a colour ramp on every output. While the screen should be neutral it lets go of the tables
//! altogether, so another tool can have them and the compositor restores the ordinary colours. If the compositor says an output's table cannot be
//! set (a nested window, a screen without gamma support, another client holding it) that output is left alone and tried again later.

use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};
use std::time::{Duration, Instant};

use helix_nightlight_model::{ramp, white_point, NightLight, NEUTRAL};
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_output::WlOutput,
    wl_registry::{self, WlRegistry},
};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols_wlr::gamma_control::v1::client::{
    zwlr_gamma_control_manager_v1::ZwlrGammaControlManagerV1,
    zwlr_gamma_control_v1::{self, ZwlrGammaControlV1},
};

/// How long an output that refused its table is left alone before it is asked again.
const RETRY: Duration = Duration::from_secs(30);

struct Output {
    /// The registry name of the `wl_output`, which is also what its gamma control carries as user data.
    name: u32,
    wl: WlOutput,
    control: Option<ZwlrGammaControlV1>,
    size: Option<u32>,
    /// The warmth last set on this output, so a table is sent when it changes and not every tick.
    shown: Option<[f64; 3]>,
    refused_at: Option<Instant>,
}

struct App {
    manager: ZwlrGammaControlManagerV1,
    outputs: Vec<Output>,
    /// What the screens should show now; `None` means ordinary colours (no tables held).
    want: Option<[f64; 3]>,
}

/// Minutes since local midnight.
fn local_minute() -> u32 {
    // SAFETY: `localtime_r` is given a valid time and a zeroed `tm` it fills in.
    unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&now, &mut tm).is_null() {
            return 0;
        }
        (tm.tm_hour.clamp(0, 23) * 60 + tm.tm_min.clamp(0, 59)) as u32
    }
}

/// An in-memory file holding the ramp, in the native byte order the protocol asks for, positioned at its start.
fn table_fd(table: &[u16]) -> std::io::Result<OwnedFd> {
    let bytes: Vec<u8> = table.iter().flat_map(|v| v.to_ne_bytes()).collect();
    // SAFETY: memfd_create returns a new descriptor we own or -1.
    let raw = unsafe { libc::memfd_create(c"helix-nightlight".as_ptr(), libc::MFD_CLOEXEC) };
    if raw < 0 {
        return Err(std::io::Error::last_os_error());
    }
    // SAFETY: `raw` is a fresh, valid descriptor nothing else owns.
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    let mut written = 0;
    while written < bytes.len() {
        // SAFETY: the pointer and length describe the unwritten tail of `bytes`.
        let n = unsafe {
            libc::write(
                fd.as_raw_fd(),
                bytes[written..].as_ptr().cast(),
                bytes.len() - written,
            )
        };
        if n < 0 {
            return Err(std::io::Error::last_os_error());
        }
        written += n as usize;
    }
    // The compositor reads from the descriptor's own position, so it has to be back at the start.
    // SAFETY: seeking a descriptor we own.
    if unsafe { libc::lseek(fd.as_raw_fd(), 0, libc::SEEK_SET) } < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(fd)
}

impl App {
    fn open(&mut self, index: usize, qh: &QueueHandle<App>) {
        let output = &mut self.outputs[index];
        if output.control.is_none() {
            output.control = Some(self.manager.get_gamma_control(&output.wl, qh, output.name));
            output.size = None;
            output.shown = None;
        }
    }

    fn close(&mut self, index: usize) {
        let output = &mut self.outputs[index];
        if let Some(control) = output.control.take() {
            control.destroy();
        }
        output.size = None;
        output.shown = None;
    }

    fn apply(&mut self, index: usize) {
        let want = self.want;
        let output = &mut self.outputs[index];
        let (Some(control), Some(size), Some(want)) = (&output.control, output.size, want) else {
            return;
        };
        if output.shown == Some(want) {
            return;
        }
        match table_fd(&ramp(size as usize, want)) {
            Ok(fd) => {
                control.set_gamma(fd.as_fd());
                output.shown = Some(want);
            }
            Err(error) => eprintln!("NIGHTLIGHT could not make a table: {error}"),
        }
    }

    /// Brings the outputs in line with `want`: tables held and set while warm, released while neutral.
    fn settle(&mut self, qh: &QueueHandle<App>) {
        for index in 0..self.outputs.len() {
            match self.want {
                None => self.close(index),
                Some(_) => {
                    let retry = self.outputs[index]
                        .refused_at
                        .is_none_or(|at| at.elapsed() >= RETRY);
                    if self.outputs[index].control.is_none() && retry {
                        self.outputs[index].refused_at = None;
                        self.open(index, qh);
                    } else {
                        self.apply(index);
                    }
                }
            }
        }
    }
}

impl Dispatch<WlRegistry, GlobalListContents> for App {
    fn event(
        app: &mut Self,
        registry: &WlRegistry,
        event: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        match event {
            wl_registry::Event::Global {
                name,
                interface,
                version,
            } if interface == WlOutput::interface().name => {
                let wl = registry.bind::<WlOutput, _, _>(name, version.min(4), qh, ());
                app.outputs.push(Output {
                    name,
                    wl,
                    control: None,
                    size: None,
                    shown: None,
                    refused_at: None,
                });
                if app.want.is_some() {
                    let last = app.outputs.len() - 1;
                    app.open(last, qh);
                }
            }
            wl_registry::Event::GlobalRemove { name } => {
                if let Some(at) = app.outputs.iter().position(|o| o.name == name) {
                    app.close(at);
                    app.outputs.remove(at);
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<ZwlrGammaControlV1, u32> for App {
    fn event(
        app: &mut Self,
        _: &ZwlrGammaControlV1,
        event: zwlr_gamma_control_v1::Event,
        name: &u32,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let Some(index) = app.outputs.iter().position(|o| o.name == *name) else {
            return;
        };
        match event {
            zwlr_gamma_control_v1::Event::GammaSize { size } => {
                app.outputs[index].size = Some(size);
                app.outputs[index].shown = None;
                app.apply(index);
            }
            zwlr_gamma_control_v1::Event::Failed => {
                eprintln!("NIGHTLIGHT output {name} cannot take a colour table now");
                app.close(index);
                app.outputs[index].refused_at = Some(Instant::now());
            }
            _ => {}
        }
    }
}

macro_rules! ignore_events {
    ($($proxy:ty),* $(,)?) => {$(
        impl Dispatch<$proxy, ()> for App {
            fn event(_: &mut Self, _: &$proxy, _: <$proxy as Proxy>::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
        }
    )*};
}
ignore_events!(WlOutput, ZwlrGammaControlManagerV1);

fn main() {
    let connection = Connection::connect_to_env().expect("no Wayland compositor");
    let (globals, mut queue) = registry_queue_init::<App>(&connection).expect("registry");
    let qh = queue.handle();
    let Ok(manager) = globals.bind::<ZwlrGammaControlManagerV1, _, _>(&qh, 1..=1, ()) else {
        // A nested window, a sandboxed session or a compositor without gamma support. Not an error to restart over.
        eprintln!("NIGHTLIGHT this compositor does not offer colour tables; nothing to do");
        return;
    };
    let mut app = App {
        manager,
        outputs: Vec::new(),
        want: None,
    };
    // Outputs present before we started arrive as globals of the registry the queue was made with.
    for global in globals.contents().clone_list() {
        if global.interface == WlOutput::interface().name {
            let wl = globals.registry().bind::<WlOutput, _, _>(
                global.name,
                global.version.min(4),
                &qh,
                (),
            );
            app.outputs.push(Output {
                name: global.name,
                wl,
                control: None,
                size: None,
                shown: None,
                refused_at: None,
            });
        }
    }
    let path = helix_nightlight_model::config_path();
    let mut stamp = None;
    let mut config = NightLight::default();
    eprintln!("NIGHTLIGHT started with {} output(s)", app.outputs.len());
    loop {
        let now = std::fs::metadata(&path).and_then(|m| m.modified()).ok();
        if now != stamp {
            stamp = now;
            config = helix_nightlight_model::load();
            eprintln!(
                "NIGHTLIGHT settings enabled={} warmth={} scheduled={} {}-{}",
                config.enabled, config.warmth, config.scheduled, config.start, config.end
            );
        }
        let kelvin = config.temperature_at(local_minute());
        let want = (kelvin != NEUTRAL).then(|| white_point(kelvin));
        if want != app.want {
            if want.is_some() != app.want.is_some() {
                eprintln!(
                    "NIGHTLIGHT {}",
                    if want.is_some() { "warm" } else { "neutral" }
                );
            }
            app.want = want;
        }
        app.settle(&qh);
        let _ = queue.dispatch_pending(&mut app);
        let _ = connection.flush();
        // Wait for the compositor, or a second, whichever is first; then look at the clock and the file again.
        if let Some(guard) = queue.prepare_read() {
            let mut pollfd = libc::pollfd {
                fd: guard.connection_fd().as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            };
            // SAFETY: one valid pollfd, and the descriptor outlives the call.
            let ready = unsafe { libc::poll(&mut pollfd, 1, 1000) };
            if ready > 0 {
                let _ = guard.read();
            }
        }
        if queue.dispatch_pending(&mut app).is_err() {
            eprintln!("NIGHTLIGHT lost the compositor");
            return;
        }
    }
}
