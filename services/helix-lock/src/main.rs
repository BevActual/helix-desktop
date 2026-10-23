//! helix-lock: the session lock screen. It asks the compositor to lock (ext-session-lock-v1), puts one lock surface on every output, draws the Helix
//! lock screen on each, and lets the session back in only when PAM accepts the account's password. If this process dies the compositor keeps the
//! session locked, so a crash shows a locked screen, never the desktop.
//!
//!   helix-lock [--daemonize]     --daemonize returns once the lock is in place (what the idle service and "lock before sleep" wait for)

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use cairo::{Context, Format, ImageSurface};
use helix_lock::entry::{initials, Entry, Phase};
use helix_lock::facts;
use helix_lock::paint::{self, Frame};
use helix_lock::pam::{self, Outcome};
use helix_wallpaper_model::Source;
use smithay_client_toolkit::{
    compositor::{CompositorHandler, CompositorState},
    delegate_registry,
    output::{OutputHandler, OutputState},
    reexports::{
        calloop::{
            channel,
            timer::{TimeoutAction, Timer},
            EventLoop, LoopHandle,
        },
        calloop_wayland_source::WaylandSource,
    },
    registry::{ProvidesRegistryState, RegistryState},
    registry_handlers,
    seat::{
        keyboard::{KeyEvent, KeyboardHandler, Keysym, Modifiers, RawModifiers},
        Capability, SeatHandler, SeatState,
    },
    session_lock::{
        SessionLock, SessionLockHandler, SessionLockState, SessionLockSurface,
        SessionLockSurfaceConfigure,
    },
    shm::{raw::RawPool, Shm, ShmHandler},
};
use wayland_client::{
    globals::registry_queue_init,
    protocol::{wl_buffer, wl_keyboard, wl_output, wl_seat, wl_shm, wl_surface},
    Connection, QueueHandle,
};

/// One lock surface and what is known about the output under it.
struct Lockable {
    surface: SessionLockSurface,
    output: wl_output::WlOutput,
    /// Logical size from the compositor's configure, `None` until then.
    size: Option<(u32, u32)>,
    scale: i32,
    source: Source,
    /// The blurred background at the physical size, made when the picture is ready or the size changes.
    background: Option<(ImageSurface, i32, i32)>,
}

struct App {
    conn: Connection,
    loop_handle: LoopHandle<'static, Self>,
    compositor_state: CompositorState,
    output_state: OutputState,
    registry_state: RegistryState,
    seat_state: SeatState,
    shm: Shm,
    session_lock_state: SessionLockState,
    session_lock: Option<SessionLock>,
    locked: bool,
    notify_fd: Option<i32>,
    lockables: Vec<Lockable>,
    keyboard: Option<wl_keyboard::WlKeyboard>,
    caps: bool,
    ctrl: bool,
    entry: Entry,
    pictures: HashMap<PathBuf, Option<ImageSurface>>,
    login: String,
    name: String,
    /// The account's picture, square and sized for the card, when it has one.
    avatar: Option<ImageSurface>,
    accent: [u8; 3],
    service: &'static str,
    twelve: bool,
    last_minute: String,
    answers: channel::Sender<Outcome>,
    exit_code: Option<i32>,
    /// Something changed that the screen shows: drawn at the end of the current loop turn.
    want_redraw: bool,
}

fn now_parts() -> (u32, u32, String, String, u32) {
    // SAFETY: `localtime_r` and `strftime` write only into the zeroed `tm` and the local buffers, at most their length.
    unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&now, &mut tm);
        let text = |format: &std::ffi::CStr| {
            let mut buffer = [0 as libc::c_char; 64];
            let n = libc::strftime(buffer.as_mut_ptr(), buffer.len(), format.as_ptr(), &tm);
            let bytes: Vec<u8> = buffer[..n].iter().map(|&b| b as u8).collect();
            String::from_utf8_lossy(&bytes).into_owned()
        };
        (
            tm.tm_hour as u32,
            tm.tm_min as u32,
            text(c"%A"),
            text(c"%B"),
            tm.tm_mday as u32,
        )
    }
}

/// Where the battery files are (`HELIX_LOCK_POWER_DIR` lets a test supply its own).
fn power_dir() -> PathBuf {
    std::env::var_os("HELIX_LOCK_POWER_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/sys/class/power_supply"))
}

/// Whether the clock is written 12-hour: the person's choice in Settings, else the language's habit.
fn twelve_hour() -> bool {
    let home = std::env::var("HOME").unwrap_or_default();
    let config = std::env::var("XDG_CONFIG_HOME").unwrap_or_else(|_| format!("{home}/.config"));
    let text =
        std::fs::read_to_string(format!("{config}/helix/clock-settings.json")).unwrap_or_default();
    let compact: String = text.chars().filter(|c| !c.is_whitespace()).collect();
    if compact.contains("\"format\":\"12\"") {
        return true;
    }
    if compact.contains("\"format\":\"24\"") {
        return false;
    }
    // SAFETY: `nl_langinfo` returns a pointer to static text, read at once.
    unsafe {
        let format = libc::nl_langinfo(libc::T_FMT);
        !format.is_null()
            && std::ffi::CStr::from_ptr(format)
                .to_string_lossy()
                .contains("%p")
    }
}

impl App {
    fn clock(&self) -> (String, String) {
        let (hour, minute, weekday, month, day) = now_parts();
        facts::clock_text(hour, minute, self.twelve, &weekday, &month, day)
    }

    fn picture(&mut self, path: &Path) -> Option<&ImageSurface> {
        self.pictures
            .entry(path.to_path_buf())
            .or_insert_with(|| paint::load_picture(path, 960))
            .as_ref()
    }

    /// Makes sure every surface has its background for its current size, loading a picture the first time it is wanted.
    fn prepare_backgrounds(&mut self) {
        for index in 0..self.lockables.len() {
            let Some((lw, lh)) = self.lockables[index].size else {
                continue;
            };
            let scale = self.lockables[index].scale;
            let (pw, ph) = ((lw as i32) * scale, (lh as i32) * scale);
            if self.lockables[index]
                .background
                .as_ref()
                .is_some_and(|(_, w, h)| (*w, *h) == (pw, ph))
            {
                continue;
            }
            let source = self.lockables[index].source.clone();
            let picture = match &source {
                Source::Image(path) => self.picture(path).cloned(),
                Source::Color(_) => None,
            };
            if let Some(surface) = paint::background(&source, picture.as_ref(), pw, ph) {
                self.lockables[index].background = Some((surface, pw, ph));
            }
        }
    }

    fn draw_all(&mut self, qh: &QueueHandle<Self>) {
        let now = Instant::now();
        let (time, date) = self.clock();
        self.last_minute = time.clone();
        let message = self.entry.message(self.caps);
        let complaint = self.entry.phase == Phase::Wrong;
        let dots = self.entry.dots();
        let shake = self.entry.shake(now);
        let checking = self.entry.phase == Phase::Checking;
        let initials = initials(&self.name);
        let battery = facts::battery(&power_dir());
        for lockable in &self.lockables {
            let Some((lw, lh)) = lockable.size else {
                continue;
            };
            let scale = lockable.scale.max(1);
            let (pw, ph) = ((lw as i32) * scale, (lh as i32) * scale);
            // The interface scale shrinks the logical screen the picture is laid out on, so everything on it is larger.
            let zoom = facts::ui_scale();
            let Ok(mut canvas) = ImageSurface::create(Format::ARgb32, pw, ph) else {
                continue;
            };
            {
                let Ok(c) = Context::new(&canvas) else {
                    continue;
                };
                match &lockable.background {
                    Some((background, w, h)) if (*w, *h) == (pw, ph) => {
                        let _ = c.set_source_surface(background, 0.0, 0.0);
                        let _ = c.paint();
                    }
                    _ => {
                        c.set_source_rgb(0.055, 0.063, 0.157);
                        let _ = c.paint();
                    }
                }
                c.scale(f64::from(scale) * zoom, f64::from(scale) * zoom);
                paint::draw(
                    &c,
                    f64::from(lw) / zoom,
                    f64::from(lh) / zoom,
                    &Frame {
                        time: &time,
                        date: &date,
                        name: &self.name,
                        initials: &initials,
                        avatar: self.avatar.as_ref(),
                        dots,
                        message,
                        complaint,
                        shake,
                        accent: self.accent,
                        checking,
                        battery,
                    },
                );
            }
            canvas.flush();
            // A test cannot screenshot a locked session (the compositor refuses), so it can ask for the picture itself. It shows dots, never text.
            if let Some(dir) = std::env::var_os("HELIX_LOCK_SNAPSHOT") {
                if let Ok(mut file) = std::fs::File::create(PathBuf::from(dir).join("lock.png")) {
                    let _ = canvas.write_to_png(&mut file);
                }
            }
            let stride = canvas.stride() as usize;
            let Ok(data) = canvas.data() else { continue };
            let Ok(mut pool) = RawPool::new(stride * ph as usize, &self.shm) else {
                continue;
            };
            pool.mmap()[..stride * ph as usize].copy_from_slice(&data[..stride * ph as usize]);
            let buffer =
                pool.create_buffer(0, pw, ph, stride as i32, wl_shm::Format::Argb8888, (), qh);
            let surface = lockable.surface.wl_surface();
            surface.set_buffer_scale(scale);
            surface.attach(Some(&buffer), 0, 0);
            surface.damage_buffer(0, 0, pw, ph);
            surface.commit();
            buffer.destroy();
        }
    }

    fn add_surface(&mut self, output: wl_output::WlOutput, qh: &QueueHandle<Self>) {
        let Some(lock) = self.session_lock.as_ref() else {
            return;
        };
        if self.lockables.iter().any(|l| l.output == output) {
            return;
        }
        let info = self.output_state.info(&output);
        let name = info.as_ref().and_then(|i| i.name.clone());
        let scale = info.as_ref().map(|i| i.scale_factor).unwrap_or(1).max(1);
        let home = std::env::var("HOME").unwrap_or_default();
        let source = helix_wallpaper_model::load_config(&home).for_output(name.as_deref());
        let surface = self.compositor_state.create_surface(qh);
        let lock_surface = lock.create_lock_surface(surface, &output, qh);
        self.lockables.push(Lockable {
            surface: lock_surface,
            output,
            size: None,
            scale,
            source,
            background: None,
        });
    }

    fn accept(&mut self) {
        if let Some(lock) = self.session_lock.take() {
            lock.unlock();
            // The compositor has to see the unlock before this process goes away.
            let _ = self.conn.roundtrip();
        }
        self.exit_code = Some(0);
    }

    fn submit(&mut self, qh: &QueueHandle<Self>) {
        let Some(password) = self.entry.begin_check() else {
            return;
        };
        let (login, service, answers) = (self.login.clone(), self.service, self.answers.clone());
        let confdir = std::env::var("HELIX_LOCK_PAM_DIR")
            .ok()
            .filter(|d| !d.is_empty());
        std::thread::spawn(move || {
            let _ = answers.send(pam::authenticate_in(
                confdir.as_deref(),
                service,
                &login,
                &password,
            ));
        });
        self.draw_all(qh);
    }

    fn shake_until_done(&mut self) {
        let _ = self.loop_handle.insert_source(
            Timer::from_duration(Duration::from_millis(16)),
            |_, _, app| {
                // The redraw needs a queue handle; the app keeps its own through the connection's event queue.
                app.want_redraw = true;
                if app.entry.shaking(Instant::now()) {
                    TimeoutAction::ToDuration(Duration::from_millis(16))
                } else {
                    TimeoutAction::Drop
                }
            },
        );
    }
}

impl SessionLockHandler for App {
    fn locked(&mut self, _conn: &Connection, qh: &QueueHandle<Self>, _lock: SessionLock) {
        self.locked = true;
        self.prepare_backgrounds();
        self.draw_all(qh);
        if let Some(fd) = self.notify_fd.take() {
            // The waiting `--daemonize` process is told, and then goes: everything that waits for "the lock is in place" (before sleep) can go on.
            // SAFETY: `fd` is the write end of a pipe this process was started with and owns; it is written once and closed.
            unsafe {
                libc::write(fd, b"1".as_ptr().cast(), 1);
                libc::close(fd);
            }
        }
    }

    fn finished(&mut self, _conn: &Connection, _qh: &QueueHandle<Self>, _lock: SessionLock) {
        // The compositor refused (the session is already locked by another locker) or ended our lock.
        self.exit_code = Some(if self.locked { 0 } else { 1 });
    }

    fn configure(
        &mut self,
        _conn: &Connection,
        qh: &QueueHandle<Self>,
        surface: SessionLockSurface,
        configure: SessionLockSurfaceConfigure,
        _serial: u32,
    ) {
        if let Some(lockable) = self
            .lockables
            .iter_mut()
            .find(|l| l.surface.wl_surface() == surface.wl_surface())
        {
            lockable.size = Some(configure.new_size);
        }
        self.prepare_backgrounds();
        self.draw_all(qh);
    }
}

impl CompositorHandler for App {
    fn scale_factor_changed(
        &mut self,
        _: &Connection,
        qh: &QueueHandle<Self>,
        surface: &wl_surface::WlSurface,
        factor: i32,
    ) {
        if let Some(lockable) = self
            .lockables
            .iter_mut()
            .find(|l| l.surface.wl_surface() == surface)
        {
            lockable.scale = factor.max(1);
        }
        self.prepare_backgrounds();
        self.draw_all(qh);
    }
    fn transform_changed(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &wl_surface::WlSurface,
        _: wl_output::Transform,
    ) {
    }
    fn frame(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &wl_surface::WlSurface, _: u32) {}
    fn surface_enter(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &wl_surface::WlSurface,
        _: &wl_output::WlOutput,
    ) {
    }
    fn surface_leave(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &wl_surface::WlSurface,
        _: &wl_output::WlOutput,
    ) {
    }
}

impl OutputHandler for App {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }
    fn new_output(&mut self, _: &Connection, qh: &QueueHandle<Self>, output: wl_output::WlOutput) {
        // A display plugged in while locked gets a lock surface too, or it would show the desktop.
        self.add_surface(output, qh);
    }
    fn update_output(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
    fn output_destroyed(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        output: wl_output::WlOutput,
    ) {
        self.lockables.retain(|l| l.output != output);
    }
}

impl SeatHandler for App {
    fn seat_state(&mut self) -> &mut SeatState {
        &mut self.seat_state
    }
    fn new_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_seat::WlSeat) {}
    fn new_capability(
        &mut self,
        _: &Connection,
        qh: &QueueHandle<Self>,
        seat: wl_seat::WlSeat,
        capability: Capability,
    ) {
        if capability == Capability::Keyboard && self.keyboard.is_none() {
            self.keyboard = self.seat_state.get_keyboard(qh, &seat, None).ok();
        }
    }
    fn remove_capability(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: wl_seat::WlSeat,
        capability: Capability,
    ) {
        if capability == Capability::Keyboard {
            if let Some(keyboard) = self.keyboard.take() {
                keyboard.release();
            }
        }
    }
    fn remove_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_seat::WlSeat) {}
}

impl KeyboardHandler for App {
    fn enter(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &wl_keyboard::WlKeyboard,
        _: &wl_surface::WlSurface,
        _: u32,
        _: &[u32],
        _: &[Keysym],
    ) {
    }
    fn leave(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &wl_keyboard::WlKeyboard,
        _: &wl_surface::WlSurface,
        _: u32,
    ) {
    }

    fn press_key(
        &mut self,
        _: &Connection,
        qh: &QueueHandle<Self>,
        _: &wl_keyboard::WlKeyboard,
        _: u32,
        event: KeyEvent,
    ) {
        match event.keysym {
            Keysym::Return | Keysym::KP_Enter => return self.submit(qh),
            Keysym::BackSpace => self.entry.pop(),
            Keysym::Escape => self.entry.clear(),
            Keysym::u | Keysym::U if self.ctrl => self.entry.clear(),
            _ => {
                if self.ctrl {
                    return;
                }
                if let Some(text) = &event.utf8 {
                    for c in text.chars() {
                        self.entry.push(c);
                    }
                }
            }
        }
        self.draw_all(qh);
    }

    fn repeat_key(
        &mut self,
        conn: &Connection,
        qh: &QueueHandle<Self>,
        keyboard: &wl_keyboard::WlKeyboard,
        serial: u32,
        event: KeyEvent,
    ) {
        // Holding a key types more and deletes more; holding Return does not check again.
        if !matches!(event.keysym, Keysym::Return | Keysym::KP_Enter) {
            self.press_key(conn, qh, keyboard, serial, event);
        }
    }

    fn release_key(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &wl_keyboard::WlKeyboard,
        _: u32,
        _: KeyEvent,
    ) {
    }

    fn update_modifiers(
        &mut self,
        _: &Connection,
        qh: &QueueHandle<Self>,
        _: &wl_keyboard::WlKeyboard,
        _: u32,
        modifiers: Modifiers,
        _: RawModifiers,
        _: u32,
    ) {
        self.ctrl = modifiers.ctrl;
        if self.caps != modifiers.caps_lock {
            self.caps = modifiers.caps_lock;
            self.draw_all(qh);
        }
    }
}

impl ProvidesRegistryState for App {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState, SeatState];
}

impl ShmHandler for App {
    fn shm_state(&mut self) -> &mut Shm {
        &mut self.shm
    }
}

delegate_registry!(App);
wayland_client::delegate_noop!(App: ignore wl_buffer::WlBuffer);
smithay_client_toolkit::delegate_dispatch2!(App);

/// `--daemonize`: starts the locker as its own process and returns once it says the lock is in place (0), or it gave up (1). A fork after the
/// display and fonts are set up is not safe, so the locker is started fresh and tells this process through a pipe.
fn daemonize(args: &[String]) -> i32 {
    use std::os::unix::process::CommandExt;
    let mut fds = [0 as libc::c_int; 2];
    // SAFETY: `fds` has room for the two descriptors `pipe` writes.
    if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
        eprintln!("helix-lock: cannot make a pipe");
        return 2;
    }
    let (read_end, write_end) = (fds[0], fds[1]);
    // The reading end must not leak into the locker; the writing end must (a plain `pipe` descriptor is inherited across `exec`).
    // SAFETY: both descriptors are ours.
    unsafe {
        libc::fcntl(read_end, libc::F_SETFD, libc::FD_CLOEXEC);
    }
    let me = match std::env::current_exe() {
        Ok(path) => path,
        Err(error) => {
            eprintln!("helix-lock: {error}");
            return 2;
        }
    };
    let mut command = std::process::Command::new(me);
    command.args(args.iter().skip(1).filter(|a| *a != "--daemonize"));
    command.arg(format!("--notify-fd={write_end}"));
    // SAFETY: only async-signal-safe calls run between fork and exec: a new session so the locker outlives whatever started it.
    unsafe {
        command.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    let started = command.spawn();
    // SAFETY: the writing end now belongs to the locker (or nobody, when it did not start): closing ours makes end-of-file mean "it is gone".
    unsafe { libc::close(write_end) };
    if let Err(error) = started {
        eprintln!("helix-lock: cannot start the locker: {error}");
        return 2;
    }
    let mut byte = [0u8; 1];
    // SAFETY: `read_end` is ours and `byte` has room for one byte.
    let n = unsafe { libc::read(read_end, byte.as_mut_ptr().cast(), 1) };
    if n == 1 {
        0
    } else {
        1
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|a| a == "--daemonize") {
        std::process::exit(daemonize(&args));
    }
    let notify_fd: Option<i32> = args
        .iter()
        .find_map(|a| a.strip_prefix("--notify-fd=")?.parse().ok());
    let conn = match Connection::connect_to_env() {
        Ok(conn) => conn,
        Err(error) => {
            eprintln!("helix-lock: no Wayland display: {error}");
            std::process::exit(2);
        }
    };
    // SAFETY: called once at start-up, before any other thread exists.
    unsafe { libc::setlocale(libc::LC_ALL, c"".as_ptr()) };
    let (globals, event_queue) = match registry_queue_init(&conn) {
        Ok(pair) => pair,
        Err(error) => {
            eprintln!("helix-lock: cannot read the display's globals: {error}");
            std::process::exit(2);
        }
    };
    let qh: QueueHandle<App> = event_queue.handle();
    let mut event_loop: EventLoop<App> = EventLoop::try_new().expect("event loop");
    let (answers, receiver) = channel::channel::<Outcome>();
    let (login, name) = facts::who();
    let home = std::env::var("HOME").unwrap_or_default();
    let accent = facts::accent_rgb(&facts::accent_name(&helix_wallpaper_model::settings_path(
        &home,
    )));
    let mut app = App {
        conn: conn.clone(),
        loop_handle: event_loop.handle(),
        compositor_state: CompositorState::bind(&globals, &qh).expect("wl_compositor"),
        output_state: OutputState::new(&globals, &qh),
        registry_state: RegistryState::new(&globals),
        seat_state: SeatState::new(&globals, &qh),
        shm: Shm::bind(&globals, &qh).expect("wl_shm"),
        session_lock_state: SessionLockState::new(&globals, &qh),
        session_lock: None,
        locked: false,
        notify_fd,
        lockables: Vec::new(),
        keyboard: None,
        caps: false,
        ctrl: false,
        entry: Entry::default(),
        pictures: HashMap::new(),
        avatar: facts::avatar_path(&login, &home)
            .and_then(|path| paint::load_picture(&path, 256))
            .and_then(|picture| paint::square_avatar(&picture, 152)),
        login,
        name,
        accent,
        service: facts::pam_service(|s| {
            let folder = std::env::var("HELIX_LOCK_PAM_DIR")
                .ok()
                .filter(|d| !d.is_empty())
                .unwrap_or_else(|| "/etc/pam.d".into());
            std::path::Path::new(&folder).join(s).exists()
        }),
        twelve: twelve_hour(),
        last_minute: String::new(),
        answers,
        exit_code: None,
        want_redraw: false,
    };
    app.session_lock = match app.session_lock_state.lock(&qh) {
        Ok(lock) => Some(lock),
        Err(_) => {
            eprintln!("helix-lock: this compositor does not support ext-session-lock");
            std::process::exit(2);
        }
    };
    for output in app.output_state.outputs().collect::<Vec<_>>() {
        app.add_surface(output, &qh);
    }
    WaylandSource::new(conn.clone(), event_queue)
        .insert(event_loop.handle())
        .expect("wayland source");
    event_loop
        .handle()
        .insert_source(receiver, |event, _, app| {
            if let channel::Event::Msg(outcome) = event {
                match outcome {
                    Outcome::Accepted => app.accept(),
                    Outcome::Refused | Outcome::Broken(_) => {
                        if let Outcome::Broken(why) = &outcome {
                            eprintln!("helix-lock: {why}");
                        }
                        app.entry.refused(Instant::now());
                        app.want_redraw = true;
                        app.shake_until_done();
                    }
                }
            }
        })
        .expect("channel");
    // The clock: look once a second, draw only when the text changed.
    event_loop
        .handle()
        .insert_source(Timer::from_duration(Duration::from_secs(1)), |_, _, app| {
            if app.clock().0 != app.last_minute {
                app.want_redraw = true;
            }
            TimeoutAction::ToDuration(Duration::from_secs(1))
        })
        .expect("timer");
    loop {
        if let Err(error) = event_loop.dispatch(Duration::from_millis(250), &mut app) {
            eprintln!("helix-lock: {error}");
            break;
        }
        if app.want_redraw {
            app.want_redraw = false;
            app.draw_all(&qh);
        }
        if let Some(code) = app.exit_code {
            std::process::exit(code);
        }
    }
}
