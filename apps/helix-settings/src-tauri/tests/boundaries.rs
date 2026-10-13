//! Settings holds the Agent Surface *manager* role and nothing else of Agent Surface. It is not an agent: it must never act as a client, never approve
//! anything (the confirmer's job belongs to the shell), never read the activity feed of the registered desktop, and never talk to the model.
//! Page text only ever goes through `textContent`.

#[test]
fn settings_holds_the_manager_role_only() {
    let sources = [
        include_str!("../src/main.rs"),
        include_str!("../../dist/settings.js"),
        include_str!("../../dist/index.html"),
    ]
    .join("\n");
    for forbidden in [
        // Client verbs.
        "RequestAccess",
        "\"Hello\"",
        "ListTools",
        // The confirmer's.
        "RegisterConfirmer",
        ".confirmer()",
        "Confirm\"",
        // The registered desktop's.
        ".activity()",
        "RegisterDesktop",
        "SetEnabled",
        // The model, and unsafe page habits.
        "helix-inference",
        "WEBKIT_DISABLE",
        "innerHTML",
        "eval(",
    ] {
        assert!(!sources.contains(forbidden), "{forbidden}");
    }
    assert!(sources.contains(".manager()"));
    assert!(sources.contains("textContent"));
}

#[test]
fn the_capability_file_names_only_the_settings_window_and_no_filesystem_or_shell() {
    let capability = include_str!("../capabilities/default.json");
    assert!(capability.contains("\"settings\""));
    for forbidden in ["shell:", "fs:", "http:", "dialog:", "process:", "opener:"] {
        assert!(!capability.contains(forbidden), "{forbidden}");
    }
}
