//! The panels process's Overlay surface: open/close one panel; never owns the bar exclusive zone.

use crate::surfaces::{KeyboardMode, Layer};

use super::shell_surface::{Escape, InputRegion, Plan, SurfaceError, PANELS};

/// Hidden / no panel: panels process may unmap or keep a zero-input Overlay.
pub const PANEL_CLOSED: Plan = Plan {
    layer: Layer::Overlay,
    keyboard: KeyboardMode::None,
    input: InputRegion::BarOnly, // no real bar — means "no hits"; host maps to empty input region
};

pub const PANEL_OPEN: Plan = Plan {
    layer: Layer::Overlay,
    keyboard: KeyboardMode::Exclusive,
    input: InputRegion::Whole,
};

#[derive(Debug, Default)]
pub struct PanelSurface {
    open: Option<String>,
}

impl PanelSurface {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn plan(&self) -> Plan {
        if self.open.is_some() {
            PANEL_OPEN
        } else {
            PANEL_CLOSED
        }
    }

    pub fn open_panel_id(&self) -> Option<&str> {
        self.open.as_deref()
    }

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

    pub fn close(&mut self) -> Option<Plan> {
        self.open.take().map(|_| self.plan())
    }

    pub fn outside_click(&mut self) -> Option<Plan> {
        self.close()
    }

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
    fn closed_panels_surface_is_overlay_without_keyboard() {
        let surface = PanelSurface::new();
        assert_eq!(surface.plan().layer, Layer::Overlay);
        assert_eq!(surface.plan().keyboard, KeyboardMode::None);
        assert_eq!(surface.open_panel_id(), None);
    }

    #[test]
    fn open_panel_takes_exclusive_keyboard() {
        let mut surface = PanelSurface::new();
        let plan = surface.open_panel("assistant").unwrap().unwrap();
        assert_eq!(plan, PANEL_OPEN);
        assert_eq!(surface.open_panel_id(), Some("assistant"));
    }

    #[test]
    fn close_returns_closed_plan() {
        let mut surface = PanelSurface::new();
        surface.open_panel("launcher").unwrap();
        assert_eq!(surface.close(), Some(PANEL_CLOSED));
        assert_eq!(surface.open_panel_id(), None);
    }
}
