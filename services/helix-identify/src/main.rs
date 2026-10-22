//! helix-identify: a big number in the middle of each screen for a few seconds, so Settings can say which screen is which.
//!
//!   helix-identify [--seconds N] OUTPUT=LABEL ...      e.g. `helix-identify DP-1=1 HDMI-A-1=2`
//!
//! One overlay layer surface per named output (namespace `helix-identify`), no keyboard and no pointer: it never takes input. Outputs not named are left alone.

use cairo::{Context, Format, ImageSurface};
use smithay_client_toolkit::{
    compositor::{CompositorHandler, CompositorState},
    delegate_registry,
    output::{OutputHandler, OutputState},
    registry::{ProvidesRegistryState, RegistryState},
    registry_handlers,
    shell::{
        wlr_layer::{
            Anchor, KeyboardInteractivity, Layer, LayerShell, LayerShellHandler, LayerSurface,
            LayerSurfaceConfigure,
        },
        WaylandSurface,
    },
    shm::{slot::SlotPool, Shm, ShmHandler},
};
use wayland_client::{
    globals::registry_queue_init,
    protocol::{wl_output, wl_shm, wl_surface},
    Connection, QueueHandle,
};

const SIZE: u32 = 280;

struct Badge {
    layer: LayerSurface,
    label: String,
    output_name: String,
    scale: i32,
    drawn: bool,
}

struct App {
    registry_state: RegistryState,
    output_state: OutputState,
    compositor: CompositorState,
    layer_shell: LayerShell,
    shm: Shm,
    pool: SlotPool,
    wanted: Vec<(String, String)>,
    badges: Vec<Badge>,
}

/// The badge: a rounded dark square, the label large, the output's own name small underneath.
fn paint(label: &str, name: &str, px: i32) -> ImageSurface {
    let surface = ImageSurface::create(Format::ARgb32, px, px).expect("surface");
    let c = Context::new(&surface).expect("context");
    let s = px as f64;
    let r = s * 0.12;
    c.new_sub_path();
    c.arc(s - r, r, r, -std::f64::consts::FRAC_PI_2, 0.0);
    c.arc(s - r, s - r, r, 0.0, std::f64::consts::FRAC_PI_2);
    c.arc(
        r,
        s - r,
        r,
        std::f64::consts::FRAC_PI_2,
        std::f64::consts::PI,
    );
    c.arc(
        r,
        r,
        r,
        std::f64::consts::PI,
        3.0 * std::f64::consts::FRAC_PI_2,
    );
    c.close_path();
    c.set_source_rgba(0.09, 0.09, 0.11, 0.9);
    let _ = c.fill_preserve();
    c.set_source_rgba(1.0, 1.0, 1.0, 0.18);
    c.set_line_width(s * 0.008);
    let _ = c.stroke();
    let layout = pangocairo::functions::create_layout(&c);
    let centred = |text: &str, font: &str, top: f64, alpha: f64| {
        let mut description = pango::FontDescription::from_string(font);
        description.set_absolute_size(match font {
            _ if font.contains("Bold") => s * 0.52 * pango::SCALE as f64,
            _ => s * 0.085 * pango::SCALE as f64,
        });
        layout.set_font_description(Some(&description));
        layout.set_text(text);
        let (w, _) = layout.pixel_size();
        c.set_source_rgba(1.0, 1.0, 1.0, alpha);
        c.move_to((s - w as f64) / 2.0, top);
        pangocairo::functions::show_layout(&c, &layout);
    };
    centred(label, "Sans Bold", s * 0.06, 1.0);
    centred(name, "Sans", s * 0.82, 0.65);
    surface
}

impl App {
    fn open(&mut self, qh: &QueueHandle<Self>, output: &wl_output::WlOutput) {
        let Some(info) = self.output_state.info(output) else {
            return;
        };
        let Some(name) = info.name.clone() else {
            return;
        };
        let Some((_, label)) = self.wanted.iter().find(|(n, _)| *n == name).cloned() else {
            return;
        };
        if self.badges.iter().any(|b| b.output_name == name) {
            return;
        }
        let surface = self.compositor.create_surface(qh);
        let layer = self.layer_shell.create_layer_surface(
            qh,
            surface,
            Layer::Overlay,
            Some("helix-identify"),
            Some(output),
        );
        layer.set_anchor(Anchor::empty());
        layer.set_exclusive_zone(-1);
        layer.set_keyboard_interactivity(KeyboardInteractivity::None);
        layer.set_size(SIZE, SIZE);
        layer.commit();
        self.badges.push(Badge {
            layer,
            label,
            output_name: name,
            scale: info.scale_factor.max(1),
            drawn: false,
        });
    }

    fn draw(&mut self, index: usize) {
        let badge = &mut self.badges[index];
        if badge.drawn {
            return;
        }
        badge.drawn = true;
        let px = (SIZE as i32) * badge.scale;
        let mut image = paint(&badge.label, &badge.output_name, px);
        let stride = px * 4;
        let (buffer, canvas) = self
            .pool
            .create_buffer(px, px, stride, wl_shm::Format::Argb8888)
            .expect("create buffer");
        if let Ok(data) = image.data() {
            let n = canvas.len().min(data.len());
            canvas[..n].copy_from_slice(&data[..n]);
        }
        let wl = badge.layer.wl_surface();
        wl.set_buffer_scale(badge.scale);
        wl.damage_buffer(0, 0, px, px);
        buffer.attach_to(wl).expect("attach");
        badge.layer.commit();
    }
}

impl CompositorHandler for App {
    fn scale_factor_changed(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &wl_surface::WlSurface,
        _: i32,
    ) {
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
        self.open(qh, &output);
    }
    fn update_output(
        &mut self,
        _: &Connection,
        qh: &QueueHandle<Self>,
        output: wl_output::WlOutput,
    ) {
        self.open(qh, &output);
    }
    fn output_destroyed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: wl_output::WlOutput) {}
}

impl LayerShellHandler for App {
    fn closed(&mut self, _: &Connection, _: &QueueHandle<Self>, _: &LayerSurface) {
        std::process::exit(0);
    }
    fn configure(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        layer: &LayerSurface,
        _: LayerSurfaceConfigure,
        _: u32,
    ) {
        if let Some(index) = self
            .badges
            .iter()
            .position(|b| b.layer.wl_surface() == layer.wl_surface())
        {
            self.draw(index);
        }
    }
}

impl ShmHandler for App {
    fn shm_state(&mut self) -> &mut Shm {
        &mut self.shm
    }
}

impl ProvidesRegistryState for App {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    registry_handlers![OutputState];
}

delegate_registry!(App);
smithay_client_toolkit::delegate_dispatch2!(App);

fn main() {
    let mut seconds = 3u64;
    let mut wanted: Vec<(String, String)> = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--seconds" {
            seconds = args
                .next()
                .and_then(|s| s.parse().ok())
                .unwrap_or(3)
                .clamp(1, 30);
        } else if let Some((name, label)) = arg.split_once('=') {
            let label: String = label.chars().filter(|c| !c.is_control()).take(3).collect();
            if !name.is_empty() && !label.is_empty() && wanted.len() < 16 {
                wanted.push((name.to_string(), label));
            }
        }
    }
    if wanted.is_empty() {
        eprintln!("usage: helix-identify [--seconds N] OUTPUT=LABEL ...");
        std::process::exit(2);
    }
    let conn = Connection::connect_to_env().expect("no Wayland display");
    let (globals, mut queue) = registry_queue_init(&conn).expect("registry");
    let qh = queue.handle();
    let compositor = CompositorState::bind(&globals, &qh).expect("wl_compositor");
    let layer_shell = LayerShell::bind(&globals, &qh).expect("the compositor has no layer shell");
    let shm = Shm::bind(&globals, &qh).expect("wl_shm");
    let pool = SlotPool::new((SIZE * SIZE * 4 * 4) as usize, &shm).expect("shm pool");
    let mut app = App {
        registry_state: RegistryState::new(&globals),
        output_state: OutputState::new(&globals, &qh),
        compositor,
        layer_shell,
        shm,
        pool,
        wanted,
        badges: Vec::new(),
    };
    // However long the compositor takes, the badges leave on time.
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(seconds));
        std::process::exit(0);
    });
    loop {
        if queue.blocking_dispatch(&mut app).is_err() {
            std::process::exit(1);
        }
    }
}
