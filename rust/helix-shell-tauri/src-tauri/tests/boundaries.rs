//! The shell binary must not hold Agent1 *client* or *manager* authority, and must not talk to
//! the model. The confirmer role is allowed: RegisterConfirmer, Confirm, ConfirmationRequested,
//! ConfirmationEnded (see helix_desktop_api::CONFIRMER_METHODS / CONFIRMER_SIGNALS). It must not
//! set the WebKit environment variables that blank layer-shell surfaces.

#[test]
fn shell_does_not_hold_agent_authority() {
    let sources = [
        include_str!("../src/main.rs"),
        include_str!("../src/layer.rs"),
        include_str!("../../dist/shell.js"),
        include_str!("../../dist/index.html"),
    ]
    .join("\n");
    for forbidden in [
        // Client / manager Agent1 verbs. Confirmer verbs are allowed when the Overlay surface lands.
        "RequestAccess",
        "RegisterManager",
        "GetGrants",
        "SetGrant",
        "RevokeClient",
        "GetAudit",
        "helix-inference",
        "WEBKIT_DISABLE",
        "innerHTML",
        "eval(",
    ] {
        assert!(!sources.contains(forbidden), "{forbidden}");
    }
    // Still must not embed the interface name as a client grant path; confirmer goes through
    // helix_desktop_api::ConfirmerClient only.
    assert!(!sources.contains("Hello\""));
    assert!(sources.contains("helix-assistant"));
    assert!(sources.contains("textContent"));
}
