//! A task's policy as a Microsoft Execution Containers configuration (`mxc-config` schema 1.0.0, MIT, github.com/microsoft/mxc), so the same
//! containment can be run by MXC's own sandbox or handed to another agent that speaks it. Helix's own sandbox ([`crate::bwrap_args`]) enforces the
//! same policy today; this is the interchange form. The schema is young (its own alpha versions precede 1.0.0), so only the plain, stable parts are
//! used: paths, a network default, the command and a time limit.

use crate::Policy;
use serde_json::{json, Value};
use std::path::Path;

const SYSTEM_READ_ONLY: [&str; 4] = ["/usr", "/bin", "/lib", "/lib64"];

/// The configuration that runs `command` under `policy` in `folder`.
pub fn to_mxc(policy: &Policy, folder: &Path, command: &str, timeout_ms: u32) -> Value {
    let folder_text = folder.to_string_lossy().to_string();
    let inside = |p: &String| {
        Path::new(&folder_text)
            .join(p)
            .to_string_lossy()
            .to_string()
    };
    let mut readonly: Vec<String> = SYSTEM_READ_ONLY.iter().map(|s| s.to_string()).collect();
    readonly.extend(
        policy
            .read_only
            .iter()
            .filter(|p| p.starts_with('/') && !p.contains(".."))
            .cloned(),
    );
    readonly.extend(
        policy
            .protect
            .iter()
            .filter(|p| !p.starts_with('/') && !p.contains(".."))
            .map(inside),
    );
    let denied: Vec<String> = policy
        .hide
        .iter()
        .filter(|p| !p.starts_with('/') && !p.contains(".."))
        .map(inside)
        .collect();
    json!({
        "version": "1.0.0",
        "containment": "bubblewrap",
        "process": {
            "commandLine": command,
            "cwd": folder_text,
            "env": ["PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "TERM=dumb"],
            "inheritDefaultEnv": false,
            "timeout": timeout_ms,
        },
        "filesystem": {
            "readonlyPaths": readonly,
            "readwritePaths": [folder_text],
            "deniedPaths": denied,
        },
        "network": {
            "egress": { "default": if policy.network { "allow" } else { "deny" } },
            "ingress": { "default": "deny", "hostLoopback": false },
        },
        "ui": { "clipboard": "none", "disable": true, "injection": false },
    })
}

/// What an MXC configuration asks for, read as far as a Helix policy can honour it: extra read-only paths, whether the network is open, and nothing
/// that would widen what is writable (the folder is the only writable place).
pub fn read_only_and_network(config: &Value) -> (Vec<String>, bool) {
    let paths = config["filesystem"]["readonlyPaths"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|p| p.as_str())
        .filter(|p| p.starts_with('/') && !SYSTEM_READ_ONLY.contains(p))
        .map(String::from)
        .collect();
    (paths, config["network"]["egress"]["default"] == "allow")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_balanced_policy_becomes_an_mxc_config() {
        let policy = Policy::balanced("/home/me/proj");
        let config = to_mxc(&policy, Path::new("/home/me/proj"), "cargo test", 30_000);
        assert_eq!(config["version"], "1.0.0");
        assert_eq!(config["containment"], "bubblewrap");
        assert_eq!(config["process"]["commandLine"], "cargo test");
        assert_eq!(config["process"]["inheritDefaultEnv"], false);
        assert_eq!(
            config["filesystem"]["readwritePaths"],
            json!(["/home/me/proj"])
        );
        let readonly = config["filesystem"]["readonlyPaths"].as_array().unwrap();
        assert!(
            readonly.contains(&json!("/usr")) && readonly.contains(&json!("/home/me/proj/.git"))
        );
        let denied = config["filesystem"]["deniedPaths"].as_array().unwrap();
        assert!(
            denied.contains(&json!("/home/me/proj/.env"))
                && denied.contains(&json!("/home/me/proj/.ssh"))
        );
        assert_eq!(config["network"]["egress"]["default"], "deny");
        assert_eq!(config["ui"]["disable"], true);
    }

    #[test]
    fn network_is_open_only_when_the_policy_says_so() {
        let mut policy = Policy::careful();
        let off = to_mxc(&policy, Path::new("/p"), "ls", 1000);
        policy.network = true;
        let on = to_mxc(&policy, Path::new("/p"), "ls", 1000);
        assert_eq!(off["network"]["egress"]["default"], "deny");
        assert_eq!(on["network"]["egress"]["default"], "allow");
    }

    #[test]
    fn paths_that_escape_the_folder_are_left_out() {
        let mut policy = Policy::careful();
        policy.protect = vec!["../x".into(), "/abs".into(), "ok".into()];
        policy.hide = vec!["a/../b".into()];
        policy.read_only = vec!["/opt/tool".into(), "rel".into(), "/a/../b".into()];
        let config = to_mxc(&policy, Path::new("/p"), "ls", 1);
        let readonly: Vec<&str> = config["filesystem"]["readonlyPaths"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|v| v.as_str())
            .collect();
        assert!(readonly.contains(&"/p/ok") && readonly.contains(&"/opt/tool"));
        assert!(!readonly
            .iter()
            .any(|p| p.contains("..") || *p == "rel" || *p == "/abs"));
        assert_eq!(config["filesystem"]["deniedPaths"], json!([]));
    }

    #[test]
    fn an_incoming_config_is_read_only_as_far_as_it_narrows() {
        let config = json!({"filesystem": {"readonlyPaths": ["/usr", "/opt/tool", "rel"], "readwritePaths": ["/"]}, "network": {"egress": {"default": "allow"}}});
        let (paths, network) = read_only_and_network(&config);
        assert_eq!(
            paths,
            ["/opt/tool"],
            "system paths are always there; a writable root is ignored"
        );
        assert!(network);
        assert!(!read_only_and_network(&json!({})).1);
    }
}
