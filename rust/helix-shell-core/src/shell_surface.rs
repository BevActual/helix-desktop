//! The one layer surface that hosts the bar and every panel. It covers the whole output, so there is a single WebKit web process
//! however many panels exist. The surface itself never changes size; what changes is the layer it sits on, whether it takes the
//! keyboard, and which part of it accepts the pointer. Everything else passes through to the windows underneath.
//!
//! No GTK and no display: [`Plan`] says what the shell must apply. Verified against a real pointer and keyboard in
//! `tools/nested/single-surface-pointer-test.sh`.

use crate::surfaces::{KeyboardMode, Layer};

/// Space the bar reserves, and the part of the surface that takes the pointer while no panel is open.
pub const BAR_HEIGHT: i32 = 42;

/// Panels the page knows how to show. A command naming anything else is refused.
pub const PANELS: &[&str] = &["assistant"];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InputRegion {
    /// Only the bar. Clicks anywhere else go to the window underneath.
    BarOnly,
    /// The whole surface, so a click outside the panel lands on the scrim and dismisses it.
    Whole,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Plan {
    pub layer: Layer,
    pub keyboard: KeyboardMode,
    pub input: InputRegion,
}

impl Plan {
    const CLOSED: Self = Self {
        // Top sits below fullscreen windows, which is where a bar belongs.
        layer: Layer::Top,
        keyboard: KeyboardMode::None,
        input: InputRegion::BarOnly,
    };
    // A panel must be above everything, take typing, and see every click.
    const OPEN: Self = Self {
        layer: Layer::Overlay,
        keyboard: KeyboardMode::Exclusive,
        input: InputRegion::Whole,
    };
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Escape {
    /// A request is in flight or waiting for an answer: back out of that first, leave the panel open.
    CancelRequest,
    /// Nothing to back out of: the panel closes.
    Close,
    /// No panel is open.
    Nothing,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SurfaceError {
    UnknownPanel,
}

#[derive(Debug, Default)]
pub struct ShellSurface {
    open: Option<String>,
}

impl ShellSurface {
    pub fn new() -> Self {
        Self::default()
    }

    /// What the surface should look like now.
    pub fn plan(&self) -> Plan {
        if self.open.is_some() {
            Plan::OPEN
        } else {
            Plan::CLOSED
        }
    }

    pub fn open_panel_id(&self) -> Option<&str> {
        self.open.as_deref()
    }

    /// Opens `id`, replacing any other panel (there is only ever one). The plan is returned when something changed.
    pub fn open_panel(&mut self, id: &str) -> Result<Option<Plan>, SurfaceError> {
        let panel = PANELS
            .iter()
            .find(|known| **known == id)
            .ok_or(SurfaceError::UnknownPanel)?;
        let was_open = self.open.is_some();
        let same = self.open.as_deref() == Some(*panel);
        self.open = Some((*panel).to_string());
        Ok((!was_open || !same).then(|| self.plan()))
    }

    /// Closes the open panel. `None` when none was open.
    pub fn close(&mut self) -> Option<Plan> {
        self.open.take().map(|_| self.plan())
    }

    /// A click on the scrim, outside the panel. Does nothing unless a panel is open.
    pub fn outside_click(&mut self) -> Option<Plan> {
        self.close()
    }

    /// What Escape should do. `request_active`: the panel has something to back out of (a request in flight or a confirmation
    /// waiting). This only reports; the caller backs out of the request or calls [`ShellSurface::close`], so closing has one path.
    pub fn escape(&self, request_active: bool) -> Escape {
        match (&self.open, request_active) {
            (None, _) => Escape::Nothing,
            (Some(_), true) => Escape::CancelRequest,
            (Some(_), false) => Escape::Close,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn starts_as_a_bar_that_passes_everything_else_through() {
        let surface = ShellSurface::new();
        assert_eq!(
            surface.plan(),
            Plan {
                layer: Layer::Top,
                keyboard: KeyboardMode::None,
                input: InputRegion::BarOnly
            }
        );
        assert_eq!(surface.open_panel_id(), None);
    }

    #[test]
    fn an_open_panel_is_above_everything_and_takes_keyboard_and_clicks() {
        let mut surface = ShellSurface::new();
        let plan = surface.open_panel("assistant").unwrap().expect("changed");
        assert_eq!(plan.layer, Layer::Overlay);
        assert_eq!(plan.keyboard, KeyboardMode::Exclusive);
        assert_eq!(plan.input, InputRegion::Whole);
        assert_eq!(surface.open_panel_id(), Some("assistant"));
    }

    #[test]
    fn closing_restores_the_bar_only_plan_exactly() {
        let mut surface = ShellSurface::new();
        let closed = surface.plan();
        surface.open_panel("assistant").unwrap();
        assert_eq!(surface.close(), Some(closed));
        assert_eq!(surface.plan(), closed);
        assert_eq!(surface.close(), None, "closing a closed surface is a no-op");
    }

    #[test]
    fn opening_the_same_panel_again_changes_nothing() {
        let mut surface = ShellSurface::new();
        assert!(surface.open_panel("assistant").unwrap().is_some());
        assert_eq!(surface.open_panel("assistant").unwrap(), None);
        assert_eq!(surface.open_panel_id(), Some("assistant"));
    }

    #[test]
    fn unknown_or_malformed_panel_ids_are_refused_and_change_nothing() {
        let mut surface = ShellSurface::new();
        for id in [
            "",
            "launcher",
            "Assistant",
            "assistant ",
            "../assistant",
            "assistant\0",
            "a".repeat(10_000).as_str(),
        ] {
            assert_eq!(
                surface.open_panel(id),
                Err(SurfaceError::UnknownPanel),
                "{id:?}"
            );
            assert_eq!(surface.open_panel_id(), None);
        }
        surface.open_panel("assistant").unwrap();
        assert_eq!(surface.open_panel("nope"), Err(SurfaceError::UnknownPanel));
        assert_eq!(
            surface.open_panel_id(),
            Some("assistant"),
            "a refused id must not close what is open"
        );
    }

    #[test]
    fn an_outside_click_closes_only_when_a_panel_is_open() {
        let mut surface = ShellSurface::new();
        assert_eq!(surface.outside_click(), None);
        surface.open_panel("assistant").unwrap();
        assert_eq!(surface.outside_click(), Some(Plan::CLOSED));
        assert_eq!(surface.open_panel_id(), None);
    }

    #[test]
    fn escape_backs_out_of_a_request_before_it_closes_the_panel() {
        let mut surface = ShellSurface::new();
        assert_eq!(surface.escape(true), Escape::Nothing, "nothing is open");
        assert_eq!(surface.escape(false), Escape::Nothing);
        surface.open_panel("assistant").unwrap();
        assert_eq!(surface.escape(true), Escape::CancelRequest);
        assert_eq!(
            surface.open_panel_id(),
            Some("assistant"),
            "backing out leaves the panel open"
        );
        assert_eq!(surface.escape(false), Escape::Close);
        assert_eq!(
            surface.open_panel_id(),
            Some("assistant"),
            "escape only reports; closing is the caller's one path"
        );
        assert_eq!(surface.close(), Some(Plan::CLOSED));
    }

    #[test]
    fn the_whole_cycle_is_repeatable() {
        let mut surface = ShellSurface::new();
        for _ in 0..3 {
            assert_eq!(surface.open_panel("assistant").unwrap(), Some(Plan::OPEN));
            assert_eq!(surface.close(), Some(Plan::CLOSED));
        }
    }
}
