//! Which layer surfaces exist, and which panel is open. No GTK and no display: the shell applies
//! [`Step`]s in order. Size is requested before layer-shell init, and nothing is shown until a
//! later show step. At most one panel is open on a monitor.

use std::collections::BTreeMap;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Layer {
    Top,
    Overlay,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyboardMode {
    None,
    Exclusive,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExclusiveZone {
    /// Reserve the surface's own size, the way a bar does.
    Auto,
    /// Overlay. Do not push other windows aside.
    None,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Edge {
    Top,
    Bottom,
    Left,
    Right,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SurfaceSpec {
    pub id: String,
    /// Empty means the default output. Exclusivity is per monitor.
    pub monitor: String,
    pub width: i32,
    pub height: i32,
    pub layer: Layer,
    pub anchors: Vec<Edge>,
    pub keyboard: KeyboardMode,
    pub exclusive: ExclusiveZone,
    pub namespace: String,
    pub panel: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Step {
    CreateHidden {
        id: String,
    },
    SetSizeRequest {
        id: String,
        width: i32,
        height: i32,
    },
    InitLayerShell {
        id: String,
    },
    SetLayer {
        id: String,
        layer: Layer,
    },
    SetAnchors {
        id: String,
        edges: Vec<Edge>,
    },
    SetExclusiveZone {
        id: String,
        zone: ExclusiveZone,
    },
    SetKeyboardMode {
        id: String,
        mode: KeyboardMode,
    },
    SetNamespace {
        id: String,
        namespace: String,
    },
    Show {
        id: String,
    },
    Hide {
        id: String,
    },
    /// Drop the GTK/WebKit window. The surface is gone from the manager; add it again to reopen.
    Destroy {
        id: String,
    },
}

impl Step {
    pub fn id(&self) -> &str {
        match self {
            Self::CreateHidden { id }
            | Self::SetSizeRequest { id, .. }
            | Self::InitLayerShell { id }
            | Self::SetLayer { id, .. }
            | Self::SetAnchors { id, .. }
            | Self::SetExclusiveZone { id, .. }
            | Self::SetKeyboardMode { id, .. }
            | Self::SetNamespace { id, .. }
            | Self::Show { id }
            | Self::Hide { id }
            | Self::Destroy { id } => id,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SurfaceError {
    DuplicateId,
    Unknown,
    NotPanel,
    NotBar,
}

struct Record {
    spec: SurfaceSpec,
    visible: bool,
}

#[derive(Default)]
pub struct SurfaceManager {
    surfaces: BTreeMap<String, Record>,
    /// monitor -> open panel id
    open: BTreeMap<String, String>,
}

impl SurfaceManager {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn open_panel_on(&self, monitor: &str) -> Option<&str> {
        self.open.get(monitor).map(String::as_str)
    }

    pub fn is_visible(&self, id: &str) -> bool {
        self.surfaces.get(id).is_some_and(|record| record.visible)
    }

    /// Create the surface hidden. The returned steps are the whole layer-shell setup, in the
    /// order they must be applied. There is no Show.
    pub fn add(&mut self, spec: SurfaceSpec) -> Result<Vec<Step>, SurfaceError> {
        if self.surfaces.contains_key(&spec.id) {
            return Err(SurfaceError::DuplicateId);
        }
        let steps = setup_steps(&spec);
        self.surfaces.insert(
            spec.id.clone(),
            Record {
                spec,
                visible: false,
            },
        );
        Ok(steps)
    }

    pub fn show_bar(&mut self, id: &str) -> Result<Vec<Step>, SurfaceError> {
        let record = self.surfaces.get_mut(id).ok_or(SurfaceError::Unknown)?;
        if record.spec.panel {
            return Err(SurfaceError::NotBar);
        }
        record.visible = true;
        Ok(vec![Step::Show { id: id.to_string() }])
    }

    /// Show `id`, hiding the panel already open on that monitor.
    pub fn open_panel(&mut self, id: &str) -> Result<Vec<Step>, SurfaceError> {
        let record = self.surfaces.get(id).ok_or(SurfaceError::Unknown)?;
        if !record.spec.panel {
            return Err(SurfaceError::NotPanel);
        }
        let monitor = record.spec.monitor.clone();
        let mut steps = Vec::new();
        if let Some(previous) = self.open.get(&monitor).cloned() {
            if previous == id {
                return Ok(steps);
            }
            steps.extend(self.hide(&previous)?);
        }
        steps.extend(self.show(id)?);
        self.open.insert(monitor, id.to_string());
        Ok(steps)
    }

    pub fn close_panel(&mut self, id: &str) -> Result<Vec<Step>, SurfaceError> {
        let record = self.surfaces.get(id).ok_or(SurfaceError::Unknown)?;
        if !record.spec.panel {
            return Err(SurfaceError::NotPanel);
        }
        if !record.visible {
            return Ok(Vec::new());
        }
        let monitor = record.spec.monitor.clone();
        let steps = self.hide(id)?;
        if self.open.get(&monitor).is_some_and(|open| open == id) {
            self.open.remove(&monitor);
        }
        Ok(steps)
    }

    /// Forget the surface so its WebKit window can be destroyed. Bars stay. Emits Destroy only
    /// (no Hide): hide-then-destroy kept a second WebKitWebProcess alive in nested measurements.
    pub fn destroy_panel(&mut self, id: &str) -> Result<Vec<Step>, SurfaceError> {
        let (is_panel, monitor, visible) = {
            let record = self.surfaces.get(id).ok_or(SurfaceError::Unknown)?;
            (
                record.spec.panel,
                record.spec.monitor.clone(),
                record.visible,
            )
        };
        if !is_panel {
            return Err(SurfaceError::NotPanel);
        }
        if visible {
            if let Some(record) = self.surfaces.get_mut(id) {
                record.visible = false;
            }
            if self.open.get(&monitor).is_some_and(|open| open == id) {
                self.open.remove(&monitor);
            }
        }
        self.surfaces.remove(id);
        Ok(vec![Step::Destroy { id: id.to_string() }])
    }

    pub fn escape(&mut self, monitor: &str) -> Result<Vec<Step>, SurfaceError> {
        self.dismiss(monitor)
    }

    pub fn outside_click(&mut self, monitor: &str) -> Result<Vec<Step>, SurfaceError> {
        self.dismiss(monitor)
    }

    fn dismiss(&mut self, monitor: &str) -> Result<Vec<Step>, SurfaceError> {
        let Some(id) = self.open.get(monitor).cloned() else {
            return Ok(Vec::new());
        };
        self.close_panel(&id)
    }

    fn show(&mut self, id: &str) -> Result<Vec<Step>, SurfaceError> {
        let record = self.surfaces.get_mut(id).ok_or(SurfaceError::Unknown)?;
        record.visible = true;
        Ok(vec![Step::Show { id: id.to_string() }])
    }

    fn hide(&mut self, id: &str) -> Result<Vec<Step>, SurfaceError> {
        let record = self.surfaces.get_mut(id).ok_or(SurfaceError::Unknown)?;
        record.visible = false;
        Ok(vec![Step::Hide { id: id.to_string() }])
    }
}

fn setup_steps(spec: &SurfaceSpec) -> Vec<Step> {
    let id = spec.id.clone();
    vec![
        Step::CreateHidden { id: id.clone() },
        Step::SetSizeRequest {
            id: id.clone(),
            width: spec.width,
            height: spec.height,
        },
        Step::InitLayerShell { id: id.clone() },
        Step::SetLayer {
            id: id.clone(),
            layer: spec.layer,
        },
        Step::SetAnchors {
            id: id.clone(),
            edges: spec.anchors.clone(),
        },
        Step::SetExclusiveZone {
            id: id.clone(),
            zone: spec.exclusive,
        },
        Step::SetKeyboardMode {
            id: id.clone(),
            mode: spec.keyboard,
        },
        Step::SetNamespace {
            id,
            namespace: spec.namespace.clone(),
        },
    ]
}

/// Size is requested before layer-shell init, init happens before any show, and a setup does
/// not show the surface. The shell checks this before touching GTK.
pub fn validate_setup(steps: &[Step]) -> Result<(), &'static str> {
    let mut size_at = BTreeMap::<&str, usize>::new();
    let mut init_at = BTreeMap::<&str, usize>::new();
    for (index, step) in steps.iter().enumerate() {
        match step {
            Step::SetSizeRequest { id, .. } => {
                size_at.insert(id.as_str(), index);
            }
            Step::InitLayerShell { id } if !size_at.contains_key(id.as_str()) => {
                return Err("init_layer_shell before set_size_request");
            }
            Step::InitLayerShell { id } => {
                init_at.insert(id.as_str(), index);
            }
            Step::Show { id } if !init_at.contains_key(id.as_str()) => {
                return Err("show before init_layer_shell");
            }
            _ => {}
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{
        validate_setup, Edge, ExclusiveZone, KeyboardMode, Layer, Step, SurfaceError,
        SurfaceManager, SurfaceSpec,
    };

    fn panel(id: &str, monitor: &str) -> SurfaceSpec {
        SurfaceSpec {
            id: id.to_string(),
            monitor: monitor.to_string(),
            width: 520,
            height: 260,
            layer: Layer::Overlay,
            anchors: Vec::new(),
            keyboard: KeyboardMode::Exclusive,
            exclusive: ExclusiveZone::None,
            namespace: format!("helix-tauri-{id}"),
            panel: true,
        }
    }

    fn bar() -> SurfaceSpec {
        SurfaceSpec {
            id: "bar".to_string(),
            monitor: String::new(),
            width: -1,
            height: 42,
            layer: Layer::Top,
            anchors: vec![Edge::Top, Edge::Left, Edge::Right],
            keyboard: KeyboardMode::None,
            exclusive: ExclusiveZone::Auto,
            namespace: "helix-tauri-bar".to_string(),
            panel: false,
        }
    }

    #[test]
    fn setup_is_hidden_and_sizes_before_layer_shell() {
        let mut manager = SurfaceManager::new();
        let steps = manager.add(bar()).unwrap();
        assert!(validate_setup(&steps).is_ok());
        assert!(steps.iter().all(|step| !matches!(step, Step::Show { .. })));
        let size = steps
            .iter()
            .position(|step| matches!(step, Step::SetSizeRequest { .. }))
            .unwrap();
        let init = steps
            .iter()
            .position(|step| matches!(step, Step::InitLayerShell { .. }))
            .unwrap();
        assert!(size < init);
        assert!(!manager.is_visible("bar"));
        let shown = manager.show_bar("bar").unwrap();
        assert_eq!(
            shown,
            vec![Step::Show {
                id: "bar".to_string()
            }]
        );
        assert!(manager.is_visible("bar"));
    }

    #[test]
    fn swapped_size_and_init_is_rejected() {
        let steps = vec![
            Step::CreateHidden {
                id: "bar".to_string(),
            },
            Step::InitLayerShell {
                id: "bar".to_string(),
            },
            Step::SetSizeRequest {
                id: "bar".to_string(),
                width: -1,
                height: 42,
            },
        ];
        assert_eq!(
            validate_setup(&steps),
            Err("init_layer_shell before set_size_request")
        );
    }

    #[test]
    fn one_panel_per_monitor_and_escape_and_outside_click_close_it() {
        let mut manager = SurfaceManager::new();
        manager.add(panel("assistant", "")).unwrap();
        manager.add(panel("launcher", "")).unwrap();
        manager.add(panel("other", "DP-1")).unwrap();

        let open = manager.open_panel("assistant").unwrap();
        assert_eq!(
            open,
            vec![Step::Show {
                id: "assistant".to_string()
            }]
        );
        assert_eq!(manager.open_panel_on(""), Some("assistant"));

        let replaced = manager.open_panel("launcher").unwrap();
        assert_eq!(
            replaced,
            vec![
                Step::Hide {
                    id: "assistant".to_string()
                },
                Step::Show {
                    id: "launcher".to_string()
                }
            ]
        );
        assert!(!manager.is_visible("assistant"));
        assert!(manager.is_visible("launcher"));
        assert_eq!(manager.open_panel_on(""), Some("launcher"));

        manager.open_panel("other").unwrap();
        assert!(manager.is_visible("launcher"));
        assert_eq!(manager.open_panel_on("DP-1"), Some("other"));

        assert_eq!(
            manager.escape("").unwrap(),
            vec![Step::Hide {
                id: "launcher".to_string()
            }]
        );
        assert_eq!(manager.open_panel_on(""), None);
        assert!(manager.is_visible("other"));
        assert!(manager.escape("").unwrap().is_empty());

        assert_eq!(
            manager.outside_click("DP-1").unwrap(),
            vec![Step::Hide {
                id: "other".to_string()
            }]
        );
        assert!(!manager.is_visible("other"));
    }

    #[test]
    fn escape_does_not_hide_the_bar() {
        let mut manager = SurfaceManager::new();
        manager.add(bar()).unwrap();
        manager.show_bar("bar").unwrap();
        manager.add(panel("assistant", "")).unwrap();
        manager.open_panel("assistant").unwrap();
        manager.escape("").unwrap();
        assert!(manager.is_visible("bar"));
        assert!(!manager.is_visible("assistant"));
    }

    #[test]
    fn bars_and_panels_cannot_borrow_each_others_verbs() {
        let mut manager = SurfaceManager::new();
        manager.add(bar()).unwrap();
        manager.add(panel("assistant", "")).unwrap();
        assert_eq!(manager.open_panel("bar"), Err(SurfaceError::NotPanel));
        assert_eq!(manager.show_bar("assistant"), Err(SurfaceError::NotBar));
        assert_eq!(manager.open_panel("missing"), Err(SurfaceError::Unknown));
    }

    #[test]
    fn destroy_panel_forgets_it_so_it_can_be_added_again() {
        let mut manager = SurfaceManager::new();
        manager.add(panel("assistant", "")).unwrap();
        manager.open_panel("assistant").unwrap();
        let steps = manager.destroy_panel("assistant").unwrap();
        assert_eq!(
            steps,
            vec![Step::Destroy {
                id: "assistant".to_string()
            }]
        );
        assert!(!manager.is_visible("assistant"));
        assert_eq!(manager.open_panel_on(""), None);
        assert_eq!(manager.open_panel("assistant"), Err(SurfaceError::Unknown));
        manager.add(panel("assistant", "")).unwrap();
        manager.open_panel("assistant").unwrap();
        assert!(manager.is_visible("assistant"));
        assert_eq!(manager.destroy_panel("bar"), Err(SurfaceError::Unknown));
        manager.add(bar()).unwrap();
        assert_eq!(manager.destroy_panel("bar"), Err(SurfaceError::NotPanel));
    }
}
