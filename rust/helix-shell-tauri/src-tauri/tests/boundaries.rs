//! The shell binary must not hold Agent1 grants or talk to the model, and must not set the
//! WebKit environment variables that blank layer-shell surfaces.

#[test]
fn shell_does_not_hold_agent_authority() {
    let sources = [
        include_str!("../src/main.rs"),
        include_str!("../src/layer.rs"),
        include_str!("../../dist/bar.js"),
        include_str!("../../dist/panel.js"),
    ]
    .join("\n");
    for forbidden in [
        "org.helix.Desktop.Agent1",
        "RequestAccess",
        "helix-inference",
        "WEBKIT_DISABLE",
        "innerHTML",
        "eval(",
    ] {
        assert!(!sources.contains(forbidden), "{forbidden}");
    }
    assert!(sources.contains("helix-assistant"));
    assert!(sources.contains("textContent"));
}
