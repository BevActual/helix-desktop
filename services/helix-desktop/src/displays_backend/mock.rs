//! Mock Displays1 inventory for nested tests.

use serde_json::{json, Value};

pub fn force_mock() -> bool {
    matches!(
        std::env::var("HELIX_DESKTOP_MOCK_DISPLAYS").ok().as_deref(),
        Some("1") | Some("true") | Some("yes")
    )
}

pub fn unavailable_snapshot() -> Value {
    json!({
        "available": false,
        "outputs": [],
        "capabilities": [],
        "reason": "Display inventory requires an active Helix desktop and its compositor connection.",
    })
}

pub fn mock_snapshot() -> Value {
    json!({
        "available": true,
        "outputs": [{
            "name": "eDP-1",
            "label": "Built-in Display",
            "enabled": true,
            "width": 1920,
            "height": 1080,
            "refresh-millihertz": 60000,
            "scale": 1.0,
            "x": 0,
            "y": 0,
            "logical-width": 1920,
            "logical-height": 1080,
            "transform": "Normal",
            "vrr-supported": false,
            "vrr-enabled": false,
        }],
        "capabilities": ["inventory"],
    })
}
