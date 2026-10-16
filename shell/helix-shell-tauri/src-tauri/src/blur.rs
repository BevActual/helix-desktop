//! Glass: asks the compositor to blur what is behind the parts of the shell surface that are drawn as glass, through
//! `ext-background-effect-v1`. The shell is one surface covering the whole output, so the blur region has to follow whatever is open
//! (the bar, a panel, toasts); the page reports those rectangles and `helix_shell_model::glass` turns them into the region.
//! GTK owns the surface and its connection. This module makes its own event queue on that same connection, so nothing here reads
//! or dispatches GTK's events, and it only ever adds the blur region (never attaches buffers or touches the surface otherwise).

use std::cell::RefCell;

use gtk::glib::translate::ToGlibPtr;
use gtk::prelude::*;
use helix_shell_model::glass::Rect;
use wayland_backend::client::{Backend, ObjectId};
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_compositor::WlCompositor, wl_region::WlRegion, wl_registry::WlRegistry,
    wl_surface::WlSurface,
};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols::ext::background_effect::v1::client::ext_background_effect_manager_v1::{
    self, Capability, ExtBackgroundEffectManagerV1,
};
use wayland_protocols::ext::background_effect::v1::client::ext_background_effect_surface_v1::ExtBackgroundEffectSurfaceV1;

#[derive(Default)]
struct State {
    blur: bool,
}

impl Dispatch<WlRegistry, GlobalListContents> for State {
    fn event(
        _: &mut Self,
        _: &WlRegistry,
        _: <WlRegistry as Proxy>::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<WlCompositor, ()> for State {
    fn event(
        _: &mut Self,
        _: &WlCompositor,
        _: <WlCompositor as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<WlRegion, ()> for State {
    fn event(
        _: &mut Self,
        _: &WlRegion,
        _: <WlRegion as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<WlSurface, ()> for State {
    fn event(
        _: &mut Self,
        _: &WlSurface,
        _: <WlSurface as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<ExtBackgroundEffectSurfaceV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &ExtBackgroundEffectSurfaceV1,
        _: <ExtBackgroundEffectSurfaceV1 as Proxy>::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl Dispatch<ExtBackgroundEffectManagerV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &ExtBackgroundEffectManagerV1,
        event: ext_background_effect_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let ext_background_effect_manager_v1::Event::Capabilities { flags } = event {
            state.blur = flags
                .into_result()
                .is_ok_and(|caps| caps.contains(Capability::Blur));
        }
    }
}

pub struct Blur {
    connection: Connection,
    qh: QueueHandle<State>,
    surface: WlSurface,
    compositor: WlCompositor,
    effect: ExtBackgroundEffectSurfaceV1,
}

thread_local! {
    /// GTK and Wayland objects belong to the main thread; so does this.
    static BLUR: RefCell<Option<Option<Blur>>> = const { RefCell::new(None) };
}

impl Blur {
    /// Connects to the compositor through GTK's own connection for `window`'s surface. `None` when the session is not Wayland or the
    /// compositor offers no blur.
    fn new(window: &gtk::ApplicationWindow) -> Option<Self> {
        let gdk_window = window.window()?;
        let display = gdk_window.display();
        if display.type_().name() != "GdkWaylandDisplay" {
            return None;
        }
        // SAFETY: both pointers come from GDK for a live Wayland window on the main thread, and the surface and display outlive this
        // object (it is dropped with the process; the window is never destroyed while the shell runs).
        let (display_ptr, surface_ptr) = unsafe {
            (
                gdk_wayland_sys::gdk_wayland_display_get_wl_display(
                    ToGlibPtr::<*mut gtk::gdk::ffi::GdkDisplay>::to_glib_none(&display).0 as *mut _,
                ),
                gdk_wayland_sys::gdk_wayland_window_get_wl_surface(
                    ToGlibPtr::<*mut gtk::gdk::ffi::GdkWindow>::to_glib_none(&gdk_window).0
                        as *mut _,
                ),
            )
        };
        if display_ptr.is_null() || surface_ptr.is_null() {
            return None;
        }
        // SAFETY: `display_ptr` is the live wl_display GTK is using; the backend only wraps it and never closes it.
        let backend = unsafe { Backend::from_foreign_display(display_ptr as *mut _) };
        let connection = Connection::from_backend(backend);
        let (globals, mut queue) = registry_queue_init::<State>(&connection).ok()?;
        let qh = queue.handle();
        let compositor: WlCompositor = globals.bind(&qh, 1..=6, ()).ok()?;
        let manager: ExtBackgroundEffectManagerV1 = globals.bind(&qh, 1..=1, ()).ok()?;
        // SAFETY: the pointer is GTK's live wl_surface proxy; wrapping it adds a handle on this queue, not a second owner.
        let surface_id =
            unsafe { ObjectId::from_ptr(WlSurface::interface(), surface_ptr as *mut _) }.ok()?;
        let surface = WlSurface::from_id(&connection, surface_id).ok()?;
        let mut state = State::default();
        queue.roundtrip(&mut state).ok()?; // the compositor says what it can do
        if !state.blur {
            return None;
        }
        let effect = manager.get_background_effect(&surface, &qh, ());
        Some(Self {
            connection,
            qh,
            surface,
            compositor,
            effect,
        })
    }

    fn set(&self, rects: &[Rect]) {
        if rects.is_empty() {
            self.effect.set_blur_region(None);
        } else {
            let region = self.compositor.create_region(&self.qh, ());
            for rect in rects {
                region.add(rect.x, rect.y, rect.w, rect.h);
            }
            self.effect.set_blur_region(Some(&region));
            region.destroy();
        }
        // The region is applied on the surface's next commit; GTK commits when it draws, so make sure one happens now.
        self.surface.commit();
        let _ = self.connection.flush();
    }
}

/// Sets the blur region of the shell surface. Returns whether blur is available at all (and so whether the page should draw its glass
/// more transparent). Must be called on the main thread.
pub fn apply(window: &gtk::ApplicationWindow, rects: &[Rect]) -> bool {
    BLUR.with(|cell| {
        let mut slot = cell.borrow_mut();
        let blur = slot.get_or_insert_with(|| {
            let made = Blur::new(window);
            eprintln!(
                "GLASS blur {}",
                if made.is_some() {
                    "available"
                } else {
                    "unavailable"
                }
            );
            made
        });
        match blur {
            Some(blur) => {
                blur.set(rects);
                true
            }
            None => false,
        }
    })
}
