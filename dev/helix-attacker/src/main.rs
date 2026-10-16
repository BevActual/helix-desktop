//! `wl-capture <out.ppm>`: asks the compositor for a copy of the first output through wlr-screencopy, exactly as any program of the
//! person's could, and writes it as a PPM. Prints `CAPTURED WxH` on success and `DENIED <reason>` when the compositor refuses.
//! A test tool: it exists to show what an attacker can read.

use std::fs::File;
use std::io::Write;
use std::os::fd::AsFd;

use memmap2::MmapMut;
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_buffer::WlBuffer,
    wl_output::WlOutput,
    wl_registry::WlRegistry,
    wl_shm::{self, WlShm},
    wl_shm_pool::WlShmPool,
};
use wayland_client::{Connection, Dispatch, QueueHandle, WEnum};
use wayland_protocols_wlr::screencopy::v1::client::{
    zwlr_screencopy_frame_v1::{self, ZwlrScreencopyFrameV1},
    zwlr_screencopy_manager_v1::ZwlrScreencopyManagerV1,
};

#[derive(Default)]
struct App {
    format: Option<(u32, u32, u32, u32)>, // format, width, height, stride
    ready: bool,
    failed: bool,
    y_invert: bool,
}

impl Dispatch<WlRegistry, GlobalListContents> for App {
    fn event(
        _: &mut Self,
        _: &WlRegistry,
        _: wayland_client::protocol::wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<WlOutput, ()> for App {
    fn event(
        _: &mut Self,
        _: &WlOutput,
        _: wayland_client::protocol::wl_output::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<WlShm, ()> for App {
    fn event(
        _: &mut Self,
        _: &WlShm,
        _: wl_shm::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<WlShmPool, ()> for App {
    fn event(
        _: &mut Self,
        _: &WlShmPool,
        _: wayland_client::protocol::wl_shm_pool::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<WlBuffer, ()> for App {
    fn event(
        _: &mut Self,
        _: &WlBuffer,
        _: wayland_client::protocol::wl_buffer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<ZwlrScreencopyManagerV1, ()> for App {
    fn event(
        _: &mut Self,
        _: &ZwlrScreencopyManagerV1,
        _: wayland_protocols_wlr::screencopy::v1::client::zwlr_screencopy_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<ZwlrScreencopyFrameV1, ()> for App {
    fn event(
        app: &mut Self,
        _: &ZwlrScreencopyFrameV1,
        event: zwlr_screencopy_frame_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_screencopy_frame_v1::Event::Buffer {
                format: WEnum::Value(format),
                width,
                height,
                stride,
            } => {
                app.format = Some((format as u32, width, height, stride));
            }
            zwlr_screencopy_frame_v1::Event::Flags { flags } => {
                app.y_invert = matches!(flags, WEnum::Value(f) if f.contains(zwlr_screencopy_frame_v1::Flags::YInvert));
            }
            zwlr_screencopy_frame_v1::Event::Ready { .. } => app.ready = true,
            zwlr_screencopy_frame_v1::Event::Failed => app.failed = true,
            _ => {}
        }
    }
}

fn main() {
    let out = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "capture.ppm".into());
    let connection = Connection::connect_to_env().expect("no compositor");
    let (globals, mut queue) = registry_queue_init::<App>(&connection).expect("registry");
    let qh = queue.handle();
    let Ok(manager) = globals.bind::<ZwlrScreencopyManagerV1, _, _>(&qh, 1..=3, ()) else {
        println!("DENIED the compositor does not offer screencopy");
        return;
    };
    let shm: WlShm = globals.bind(&qh, 1..=1, ()).expect("wl_shm");
    let output: WlOutput = globals.bind(&qh, 1..=4, ()).expect("wl_output");
    let mut app = App::default();
    let frame = manager.capture_output(0, &output, &qh, ());
    while app.format.is_none() && !app.failed {
        if queue.blocking_dispatch(&mut app).is_err() {
            println!("DENIED the compositor went away");
            return;
        }
    }
    let Some((format, width, height, stride)) = app.format else {
        println!("DENIED the compositor refused the capture");
        return;
    };
    let size = (stride * height) as usize;
    let file: File = unsafe {
        let fd = libc::memfd_create(c"capture".as_ptr(), 0);
        assert!(fd >= 0);
        std::os::fd::FromRawFd::from_raw_fd(fd)
    };
    file.set_len(size as u64).unwrap();
    let map = unsafe { MmapMut::map_mut(&file) }.unwrap();
    let pool = shm.create_pool(file.as_fd(), size as i32, &qh, ());
    let wl_format = if format == wl_shm::Format::Argb8888 as u32 {
        wl_shm::Format::Argb8888
    } else {
        wl_shm::Format::Xrgb8888
    };
    let buffer = pool.create_buffer(
        0,
        width as i32,
        height as i32,
        stride as i32,
        wl_format,
        &qh,
        (),
    );
    frame.copy(&buffer);
    while !app.ready && !app.failed {
        if queue.blocking_dispatch(&mut app).is_err() {
            break;
        }
    }
    if !app.ready {
        println!("DENIED the compositor failed the capture");
        return;
    }
    let mut ppm = format!("P6\n{width} {height}\n255\n").into_bytes();
    for line in 0..height as usize {
        let row = if app.y_invert {
            height as usize - 1 - line
        } else {
            line
        };
        for column in 0..width as usize {
            let at = row * stride as usize + column * 4;
            ppm.extend_from_slice(&[map[at + 2], map[at + 1], map[at]]);
        }
    }
    File::create(&out)
        .and_then(|mut f| f.write_all(&ppm))
        .expect("write");
    println!("CAPTURED {width}x{height}");
}
