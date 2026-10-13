//! helix-wallpaper: paints the desktop background, one layer surface per output, on the background layer (namespace
//! `helix-wallpaper`, which Helix niri overview rules match). It reads the wallpaper choice from the same settings
//! file the shell always used, renders at the output's real (fractional) scale, and keeps no decoded image around: the picture is
//! decoded, cropped and scaled once per draw and dropped, so what stays in memory is the buffers on screen.

use std::fs::File;
use std::os::fd::{AsFd, AsRawFd};
use std::path::PathBuf;

use helix_wallpaper::{
    buffer_size, choice_path, cover_crop, load_config, settings_path, Config, Source, FALLBACK,
};
use memmap2::MmapMut;
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_buffer::{self, WlBuffer},
    wl_compositor::WlCompositor,
    wl_output::{self, WlOutput},
    wl_registry::{self, WlRegistry},
    wl_shm::{self, WlShm},
    wl_shm_pool::WlShmPool,
    wl_surface::WlSurface,
};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols::wp::fractional_scale::v1::client::{
    wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1,
    wp_fractional_scale_v1::{self, WpFractionalScaleV1},
};
use wayland_protocols::wp::viewporter::client::{
    wp_viewport::WpViewport, wp_viewporter::WpViewporter,
};
use wayland_protocols_wlr::layer_shell::v1::client::{
    zwlr_layer_shell_v1::{Layer, ZwlrLayerShellV1},
    zwlr_layer_surface_v1::{self, Anchor, KeyboardInteractivity, ZwlrLayerSurfaceV1},
};

struct Output {
    global: u32,
    output: WlOutput,
    name: Option<String>,
    integer_scale: i32,
    surface: Option<usize>,
}

struct Buffer {
    wl: WlBuffer,
    _map: MmapMut,
}

struct Surface {
    wl: WlSurface,
    layer: ZwlrLayerSurfaceV1,
    viewport: Option<WpViewport>,
    fractional: Option<WpFractionalScaleV1>,
    source: Source,
    logical: (u32, u32),
    scale120: Option<u32>,
    drawn: Option<(u32, u32)>,
    buffers: Vec<Buffer>,
}

struct App {
    config: Config,
    home: String,
    compositor: WlCompositor,
    shm: WlShm,
    layer_shell: ZwlrLayerShellV1,
    viewporter: Option<WpViewporter>,
    fractional: Option<WpFractionalScaleManagerV1>,
    outputs: Vec<Output>,
    surfaces: Vec<Option<Surface>>,
}


/// BGRX bytes (what `wl_shm` calls XRGB8888 on a little-endian machine) of `source` fitted to `w` x `h`.
fn render(source: &Source, w: u32, h: u32) -> Vec<u8> {
    let fill =
        |rgb: [u8; 3]| -> Vec<u8> { [rgb[2], rgb[1], rgb[0], 0xff].repeat((w * h) as usize) };
    let Source::Image(path) = source else {
        let Source::Color(rgb) = source else {
            unreachable!()
        };
        return fill(*rgb);
    };
    let Ok(decoded) = image::open(path) else {
        eprintln!("wallpaper: cannot read {}", path.display());
        return fill(FALLBACK);
    };
    let rgba = decoded.into_rgba8();
    let (x, y, cw, ch) = cover_crop(rgba.width(), rgba.height(), w, h);
    let cropped = image::imageops::crop_imm(&rgba, x, y, cw, ch).to_image();
    drop(rgba);
    let fitted = image::imageops::resize(&cropped, w, h, image::imageops::FilterType::CatmullRom);
    let mut out = Vec::with_capacity((w * h * 4) as usize);
    for pixel in fitted.pixels() {
        out.extend_from_slice(&[pixel[2], pixel[1], pixel[0], 0xff]);
    }
    out
}

impl App {
    fn open_surface(&mut self, output_index: usize, qh: &QueueHandle<App>) {
        let output = &self.outputs[output_index];
        if output.surface.is_some() {
            return;
        }
        let index = self.surfaces.len();
        let wl = self.compositor.create_surface(qh, ());
        let layer = self.layer_shell.get_layer_surface(
            &wl,
            Some(&output.output),
            Layer::Background,
            "helix-wallpaper".into(),
            qh,
            index,
        );
        layer.set_anchor(Anchor::Top | Anchor::Bottom | Anchor::Left | Anchor::Right);
        layer.set_exclusive_zone(-1);
        layer.set_keyboard_interactivity(KeyboardInteractivity::None);
        let viewport = self
            .viewporter
            .as_ref()
            .map(|v| v.get_viewport(&wl, qh, ()));
        let fractional = self
            .fractional
            .as_ref()
            .map(|m| m.get_fractional_scale(&wl, qh, index));
        wl.commit();
        let source = self.config.for_output(output.name.as_deref());
        self.surfaces.push(Some(Surface {
            wl,
            layer,
            viewport,
            fractional,
            source,
            logical: (0, 0),
            scale120: None,
            drawn: None,
            buffers: Vec::new(),
        }));
        self.outputs[output_index].surface = Some(index);
    }

    /// Starts again from a new configuration: every surface takes what the config says for its output and is drawn once more.
    fn reload(&mut self, config: Config, qh: &QueueHandle<App>) {
        self.config = config;
        for index in 0..self.surfaces.len() {
            let name = self
                .outputs
                .iter()
                .find(|o| o.surface == Some(index))
                .and_then(|o| o.name.clone());
            let source = self.config.for_output(name.as_deref());
            if let Some(surface) = self.surfaces[index].as_mut() {
                if surface.source == source {
                    continue;
                }
                surface.source = source;
                surface.drawn = None;
            }
            self.draw(index, qh);
        }
        eprintln!("wallpaper: reloaded");
    }

    /// Draws when the size and scale are known and differ from what is on screen.
    fn draw(&mut self, index: usize, qh: &QueueHandle<App>) {
        let integer = self
            .outputs
            .iter()
            .find(|o| o.surface == Some(index))
            .map_or(1, |o| o.integer_scale.max(1));
        let Some(surface) = self.surfaces[index].as_mut() else {
            return;
        };
        let (lw, lh) = surface.logical;
        if lw == 0 || lh == 0 {
            return;
        }
        // With the fractional-scale protocol the compositor says the scale; wait for it so the first buffer is already the right size.
        let scale120 = match (surface.scale120, surface.fractional.is_some()) {
            (Some(s), _) => s,
            (None, true) => return,
            (None, false) => integer as u32 * 120,
        };
        let (w, h) = (buffer_size(lw, scale120), buffer_size(lh, scale120));
        if surface.drawn == Some((w, h)) {
            return;
        }
        let pixels = render(&surface.source, w, h);
        let Some(buffer) = make_buffer(&self.shm, &pixels, w, h, qh, index) else {
            return;
        };
        surface.wl.attach(Some(&buffer.wl), 0, 0);
        match &surface.viewport {
            Some(viewport) => viewport.set_destination(lw as i32, lh as i32),
            None => surface.wl.set_buffer_scale(integer),
        }
        surface.wl.damage_buffer(0, 0, w as i32, h as i32);
        surface.wl.commit();
        surface.buffers.push(buffer);
        surface.drawn = Some((w, h));
        // The decoder's scratch memory is freed but the allocator keeps it; give it back, so what stays is the buffer on screen.
        // SAFETY: malloc_trim only returns free heap pages to the system.
        unsafe { libc::malloc_trim(0) };
        eprintln!("wallpaper: drew {w}x{h} (logical {lw}x{lh})");
    }
}

fn make_buffer(
    shm: &WlShm,
    pixels: &[u8],
    w: u32,
    h: u32,
    qh: &QueueHandle<App>,
    index: usize,
) -> Option<Buffer> {
    // SAFETY: memfd_create has no preconditions beyond a valid name pointer; the returned descriptor is owned by the File below.
    let fd = unsafe { libc::memfd_create(c"helix-wallpaper".as_ptr(), libc::MFD_CLOEXEC) };
    if fd < 0 {
        return None;
    }
    // SAFETY: `fd` is a freshly created, open descriptor nothing else owns.
    let file = unsafe { <File as std::os::fd::FromRawFd>::from_raw_fd(fd) };
    file.set_len(pixels.len() as u64).ok()?;
    // SAFETY: the file is private to this process and sized above; nothing else truncates it.
    let mut map = unsafe { MmapMut::map_mut(&file) }.ok()?;
    map.copy_from_slice(pixels);
    let pool: WlShmPool = shm.create_pool(file.as_fd(), pixels.len() as i32, qh, ());
    let wl = pool.create_buffer(
        0,
        w as i32,
        h as i32,
        (w * 4) as i32,
        wl_shm::Format::Xrgb8888,
        qh,
        index,
    );
    pool.destroy();
    Some(Buffer { wl, _map: map })
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
            } if interface == "wl_output" => {
                let output =
                    registry.bind::<WlOutput, _, _>(name, version.min(4), qh, app.outputs.len());
                app.outputs.push(Output {
                    global: name,
                    output,
                    name: None,
                    integer_scale: 1,
                    surface: None,
                });
            }
            wl_registry::Event::GlobalRemove { name } => {
                if let Some(position) = app.outputs.iter().position(|o| o.global == name) {
                    if let Some(index) = app.outputs[position].surface {
                        if let Some(surface) = app.surfaces[index].take() {
                            surface.layer.destroy();
                            surface.wl.destroy();
                        }
                    }
                    app.outputs[position].surface = None;
                    app.outputs[position].output.release();
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<WlOutput, usize> for App {
    fn event(
        app: &mut Self,
        _: &WlOutput,
        event: wl_output::Event,
        index: &usize,
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let Some(output) = app.outputs.get_mut(*index) else {
            return;
        };
        match event {
            wl_output::Event::Name { name } => output.name = Some(name),
            wl_output::Event::Scale { factor } => output.integer_scale = factor,
            // `done` ends a batch of property events: the name is known, so the right wallpaper can be chosen.
            wl_output::Event::Done => app.open_surface(*index, qh),
            _ => {}
        }
    }
}

impl Dispatch<ZwlrLayerSurfaceV1, usize> for App {
    fn event(
        app: &mut Self,
        layer: &ZwlrLayerSurfaceV1,
        event: zwlr_layer_surface_v1::Event,
        index: &usize,
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_layer_surface_v1::Event::Configure {
                serial,
                width,
                height,
            } => {
                layer.ack_configure(serial);
                if let Some(Some(surface)) = app.surfaces.get_mut(*index) {
                    surface.logical = (width, height);
                }
                app.draw(*index, qh);
            }
            zwlr_layer_surface_v1::Event::Closed => {
                if let Some(slot) = app.surfaces.get_mut(*index) {
                    *slot = None;
                }
                for output in &mut app.outputs {
                    if output.surface == Some(*index) {
                        output.surface = None;
                    }
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<WpFractionalScaleV1, usize> for App {
    fn event(
        app: &mut Self,
        _: &WpFractionalScaleV1,
        event: wp_fractional_scale_v1::Event,
        index: &usize,
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wp_fractional_scale_v1::Event::PreferredScale { scale } = event {
            if let Some(Some(surface)) = app.surfaces.get_mut(*index) {
                surface.scale120 = Some(scale);
            }
            app.draw(*index, qh);
        }
    }
}

impl Dispatch<WlBuffer, usize> for App {
    fn event(
        app: &mut Self,
        buffer: &WlBuffer,
        event: wl_buffer::Event,
        index: &usize,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // Once the compositor is done with a buffer it is destroyed and its memory returned; only the one on screen stays.
        if let wl_buffer::Event::Release = event {
            if let Some(Some(surface)) = app.surfaces.get_mut(*index) {
                if surface
                    .buffers
                    .last()
                    .is_some_and(|last| last.wl.id() != buffer.id())
                {
                    surface.buffers.retain(|b| b.wl.id() != buffer.id());
                }
            }
            buffer.destroy();
        }
    }
}

macro_rules! ignore_events {
    ($($proxy:ty => $data:ty),* $(,)?) => {$(
        impl Dispatch<$proxy, $data> for App {
            fn event(_: &mut Self, _: &$proxy, _: <$proxy as Proxy>::Event, _: &$data, _: &Connection, _: &QueueHandle<Self>) {}
        }
    )*};
}
ignore_events! {
    WlCompositor => (), WlShm => (), WlShmPool => (), WlSurface => (), ZwlrLayerShellV1 => (),
    WpViewporter => (), WpViewport => (), WpFractionalScaleManagerV1 => (),
}

fn main() {
    let home = std::env::var("HOME").unwrap_or_default();
    let config = load_config(&home);
    let connection = Connection::connect_to_env().expect("no Wayland compositor to paint on");
    let (globals, mut queue) = registry_queue_init::<App>(&connection).expect("registry");
    let qh = queue.handle();
    let compositor = globals.bind(&qh, 4..=6, ()).expect("wl_compositor");
    let shm = globals.bind(&qh, 1..=1, ()).expect("wl_shm");
    let layer_shell = globals
        .bind(&qh, 1..=4, ())
        .expect("zwlr_layer_shell_v1: the compositor cannot show a background");
    let viewporter = globals.bind(&qh, 1..=1, ()).ok();
    let fractional = globals.bind(&qh, 1..=1, ()).ok();
    let mut app = App {
        config,
        home,
        compositor,
        shm,
        layer_shell,
        viewporter,
        fractional,
        outputs: Vec::new(),
        surfaces: Vec::new(),
    };
    let _ = &app.home;
    // The outputs already announced reach `registry` handlers as the queue is first dispatched.
    for global in globals.contents().clone_list() {
        if global.interface == "wl_output" {
            let output = globals.registry().bind::<WlOutput, _, _>(
                global.name,
                global.version.min(4),
                &qh,
                app.outputs.len(),
            );
            app.outputs.push(Output {
                global: global.name,
                output,
                name: None,
                integer_scale: 1,
                surface: None,
            });
        }
    }
    eprintln!("wallpaper: running");
    let watcher = ConfigWatch::new(&[settings_path(&app.home), choice_path(&app.home)]);
    loop {
        if queue.dispatch_pending(&mut app).is_err() || queue.flush().is_err() {
            eprintln!("wallpaper: the compositor went away");
            return;
        }
        let Some(guard) = queue.prepare_read() else {
            continue;
        };
        let mut fds = [
            libc::pollfd {
                fd: guard.connection_fd().as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: watcher.fd,
                events: libc::POLLIN,
                revents: 0,
            },
        ];
        // SAFETY: two valid pollfd structures; the call only reads and writes them.
        let ready = unsafe { libc::poll(fds.as_mut_ptr(), if watcher.fd >= 0 { 2 } else { 1 }, -1) };
        if ready < 0 {
            continue;
        }
        if fds[0].revents & libc::POLLIN != 0 {
            if guard.read().is_err() {
                eprintln!("wallpaper: the compositor went away");
                return;
            }
        } else {
            drop(guard);
        }
        if watcher.fd >= 0 && fds[1].revents & libc::POLLIN != 0 && watcher.changed() {
            let config = load_config(&app.home);
            app.reload(config, &qh);
        }
    }
}

/// Tells when the wallpaper settings files change (inotify on their folders: no polling, nothing runs while they are still).
struct ConfigWatch {
    fd: i32,
    names: Vec<std::ffi::OsString>,
}

impl ConfigWatch {
    fn new(files: &[PathBuf]) -> Self {
        // SAFETY: plain system call; a negative result means no watching, and the wallpaper keeps what it has.
        let fd = unsafe { libc::inotify_init1(libc::IN_NONBLOCK | libc::IN_CLOEXEC) };
        let mut names = Vec::new();
        for file in files {
            let (Some(dir), Some(name)) = (file.parent(), file.file_name()) else {
                continue;
            };
            let _ = std::fs::create_dir_all(dir);
            if let Ok(dir) = std::ffi::CString::new(dir.as_os_str().as_encoded_bytes()) {
                // SAFETY: `dir` is a valid NUL-terminated path for the length of the call.
                unsafe {
                    libc::inotify_add_watch(
                        fd,
                        dir.as_ptr(),
                        libc::IN_CLOSE_WRITE
                            | libc::IN_MOVED_TO
                            | libc::IN_MOVED_FROM
                            | libc::IN_CREATE
                            | libc::IN_DELETE,
                    )
                };
            }
            names.push(name.to_owned());
        }
        ConfigWatch { fd, names }
    }

    /// Reads what happened; true when one of the files was written or replaced.
    fn changed(&self) -> bool {
        let mut buffer = [0u8; 4096];
        let mut changed = false;
        loop {
            // SAFETY: reads at most `buffer.len()` bytes into `buffer`.
            let n = unsafe { libc::read(self.fd, buffer.as_mut_ptr().cast(), buffer.len()) };
            if n <= 0 {
                return changed;
            }
            let mut at = 0usize;
            let n = n as usize;
            while at + 16 <= n {
                let len = u32::from_ne_bytes(buffer[at + 12..at + 16].try_into().unwrap()) as usize;
                let end = (at + 16 + len).min(n);
                let name = &buffer[at + 16..end];
                let name = name.split(|b| *b == 0).next().unwrap_or(&[]);
                changed |= self
                    .names
                    .iter()
                    .any(|wanted| wanted.as_encoded_bytes() == name);
                at = end;
            }
        }
    }
}
