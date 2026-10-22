//! helix-agent-input: the hands of the agent display. It connects to the display's compositor (`WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` are the display's own,
//! set by the shell that starts it) and turns JSON-line commands on stdin into virtual pointer and virtual keyboard events there, answering `ok` or
//! `error: …` on stdout. It can reach nothing but the display it was started for; every command is checked in `lib.rs` before anything is sent.

use std::io::{BufRead, Write};
use std::os::fd::{AsFd, FromRawFd, OwnedFd};
use std::time::{Duration, Instant};

use helix_agent_input::{evdev_button, key_events, keymap_text, parse_command, Command};
use wayland_client::{
    globals::{registry_queue_init, GlobalListContents},
    protocol::{wl_pointer, wl_registry, wl_seat},
    Connection, Dispatch, QueueHandle,
};
use wayland_protocols_misc::zwp_virtual_keyboard_v1::client::{
    zwp_virtual_keyboard_manager_v1::ZwpVirtualKeyboardManagerV1,
    zwp_virtual_keyboard_v1::ZwpVirtualKeyboardV1,
};
use wayland_protocols_wlr::virtual_pointer::v1::client::{
    zwlr_virtual_pointer_manager_v1::ZwlrVirtualPointerManagerV1,
    zwlr_virtual_pointer_v1::ZwlrVirtualPointerV1,
};

struct State;

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
    fn event(
        _: &mut Self,
        _: &wl_registry::WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<wl_seat::WlSeat, ()> for State {
    fn event(
        _: &mut Self,
        _: &wl_seat::WlSeat,
        _: wl_seat::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<ZwlrVirtualPointerManagerV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &ZwlrVirtualPointerManagerV1,
        _: <ZwlrVirtualPointerManagerV1 as wayland_client::Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<ZwlrVirtualPointerV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &ZwlrVirtualPointerV1,
        _: <ZwlrVirtualPointerV1 as wayland_client::Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<ZwpVirtualKeyboardManagerV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &ZwpVirtualKeyboardManagerV1,
        _: <ZwpVirtualKeyboardManagerV1 as wayland_client::Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<ZwpVirtualKeyboardV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &ZwpVirtualKeyboardV1,
        _: <ZwpVirtualKeyboardV1 as wayland_client::Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

struct Hands {
    connection: Connection,
    queue: wayland_client::EventQueue<State>,
    pointer: ZwlrVirtualPointerV1,
    keyboard: ZwpVirtualKeyboardV1,
    size: (u32, u32),
    started: Instant,
}

impl Hands {
    fn time(&self) -> u32 {
        self.started.elapsed().as_millis() as u32
    }

    fn settle(&mut self) -> Result<(), String> {
        self.queue
            .roundtrip(&mut State)
            .map(|_| ())
            .map_err(|e| format!("the display stopped answering: {e}"))
    }

    fn move_to(&mut self, x: f64, y: f64) {
        let (w, h) = self.size;
        let x = x.clamp(0.0, f64::from(w - 1)).round() as u32;
        let y = y.clamp(0.0, f64::from(h - 1)).round() as u32;
        self.pointer.motion_absolute(self.time(), x, y, w, h);
        self.pointer.frame();
    }

    fn button(&mut self, button: &str, down: bool) {
        let Some(code) = evdev_button(button) else {
            return;
        };
        let state = if down {
            wl_pointer::ButtonState::Pressed
        } else {
            wl_pointer::ButtonState::Released
        };
        self.pointer.button(self.time(), code, state);
        self.pointer.frame();
    }

    fn scroll(&mut self, dx: f64, dy: f64) {
        self.pointer.axis_source(wl_pointer::AxisSource::Wheel);
        for (axis, steps) in [
            (wl_pointer::Axis::HorizontalScroll, dx),
            (wl_pointer::Axis::VerticalScroll, dy),
        ] {
            if steps != 0.0 {
                self.pointer
                    .axis_discrete(self.time(), axis, steps * 15.0, steps.round() as i32);
            }
        }
        self.pointer.frame();
    }

    /// Sends a keymap made for this command, then its key events.
    fn keys(&mut self, command: &Command) -> Result<(), String> {
        let (symbols, events) = key_events(command).ok_or("that cannot be typed")?;
        let text = keymap_text(&symbols);
        let mut bytes = text.into_bytes();
        bytes.push(0);
        // SAFETY: `memfd_create` returns a new descriptor (or -1); it is owned here and closed by `OwnedFd`.
        let raw = unsafe { libc::memfd_create(c"helix-agent-keymap".as_ptr(), libc::MFD_CLOEXEC) };
        if raw < 0 {
            return Err("cannot make a keymap".into());
        }
        // SAFETY: `raw` is a valid descriptor this function now owns.
        let fd = unsafe { OwnedFd::from_raw_fd(raw) };
        let mut file = std::fs::File::from(fd.try_clone().map_err(|e| e.to_string())?);
        file.write_all(&bytes).map_err(|e| e.to_string())?;
        self.keyboard.keymap(1, fd.as_fd(), bytes.len() as u32);
        self.settle()?;
        for (code, down) in events {
            self.keyboard.key(self.time(), code, u32::from(down));
            self.connection.flush().map_err(|e| e.to_string())?;
            std::thread::sleep(Duration::from_millis(4));
        }
        self.settle()
    }

    fn run(&mut self, command: Command) -> Result<(), String> {
        match command {
            Command::Size { w, h } => self.size = (w, h),
            Command::Move { x, y } => self.move_to(x, y),
            Command::Button { button, down } => self.button(&button, down),
            Command::Click { button, x, y } => {
                self.move_to(x, y);
                self.settle()?;
                // The pointer has to be seen arriving before the press: the program under it learns it was entered, then pressed.
                std::thread::sleep(Duration::from_millis(150));
                self.button(&button, true);
                self.settle()?;
                std::thread::sleep(Duration::from_millis(40));
                self.button(&button, false);
            }
            Command::Scroll { dx, dy } => self.scroll(dx, dy),
            command @ (Command::Key { .. } | Command::Text { .. } | Command::Combo { .. }) => {
                return self.keys(&command)
            }
        }
        self.settle()
    }
}

fn main() {
    let connection = match Connection::connect_to_env() {
        Ok(c) => c,
        Err(error) => {
            eprintln!("helix-agent-input: no display: {error}");
            std::process::exit(2);
        }
    };
    let (globals, mut queue) = match registry_queue_init::<State>(&connection) {
        Ok(pair) => pair,
        Err(error) => {
            eprintln!("helix-agent-input: {error}");
            std::process::exit(2);
        }
    };
    let qh = queue.handle();
    let seat: wl_seat::WlSeat = match globals.bind(&qh, 1..=1, ()) {
        Ok(seat) => seat,
        Err(_) => {
            eprintln!("helix-agent-input: the display has no seat");
            std::process::exit(2);
        }
    };
    let pointer_manager: ZwlrVirtualPointerManagerV1 = match globals.bind(&qh, 1..=1, ()) {
        Ok(m) => m,
        Err(_) => {
            eprintln!("helix-agent-input: the display has no virtual pointer");
            std::process::exit(2);
        }
    };
    let keyboard_manager: ZwpVirtualKeyboardManagerV1 = match globals.bind(&qh, 1..=1, ()) {
        Ok(m) => m,
        Err(_) => {
            eprintln!("helix-agent-input: the display has no virtual keyboard");
            std::process::exit(2);
        }
    };
    let pointer = pointer_manager.create_virtual_pointer(Some(&seat), &qh, ());
    let keyboard = keyboard_manager.create_virtual_keyboard(&seat, &qh, ());
    let _ = queue.roundtrip(&mut State);
    let mut hands = Hands {
        connection,
        queue,
        pointer,
        keyboard,
        size: (1280, 720),
        started: Instant::now(),
    };
    let mut out = std::io::stdout();
    let _ = writeln!(out, "ready");
    let _ = out.flush();
    for line in std::io::stdin().lock().lines().map_while(Result::ok) {
        let answer = match parse_command(&line).and_then(|c| hands.run(c)) {
            Ok(()) => "ok".to_string(),
            Err(error) => format!("error: {}", error.replace('\n', " ")),
        };
        let _ = writeln!(out, "{answer}");
        let _ = out.flush();
    }
}
