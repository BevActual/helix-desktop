//! The agents Helix knows how to start, and what each needs: where its program and state live, which environment it is given, how the Helix desktop
//! tools (MCP) and a short note about Helix are put in front of it *for this run only* (their own config files are never edited), and which flags turn
//! their own permission prompts off when the profile says so.

use std::path::{Path, PathBuf};

use serde_json::json;

use crate::profile::Profile;

pub struct Agent {
    pub id: &'static str,
    pub title: &'static str,
    pub program: &'static str,
    /// Under the home folder: where the agent keeps its settings, logins and sessions (writable in the box).
    pub state_dirs: &'static [&'static str],
    pub state_files: &'static [&'static str],
    /// Environment variables passed through by prefix (keys and settings the person exported for this agent).
    pub env_prefixes: &'static [&'static str],
}

pub const AGENTS: [Agent; 3] = [
    Agent {
        id: "claude",
        title: "Claude Code",
        program: "claude",
        state_dirs: &[".claude", ".local/share/claude", ".cache/claude"],
        state_files: &[".claude.json"],
        env_prefixes: &[
            "ANTHROPIC_",
            "CLAUDE_",
            "AWS_BEDROCK",
            "CLOUD_ML_",
            "VERTEX_",
        ],
    },
    Agent {
        id: "opencode",
        title: "OpenCode",
        program: "opencode",
        state_dirs: &[
            ".opencode",
            ".config/opencode",
            ".local/share/opencode",
            ".local/state/opencode",
            ".cache/opencode",
        ],
        state_files: &[],
        env_prefixes: &[
            "OPENCODE_",
            "ANTHROPIC_",
            "OPENAI_",
            "OPENROUTER_",
            "GOOGLE_",
            "GEMINI_",
            "GROQ_",
            "MISTRAL_",
        ],
    },
    Agent {
        id: "codex",
        title: "Codex",
        program: "codex",
        state_dirs: &[".codex"],
        state_files: &[],
        env_prefixes: &["OPENAI_", "CODEX_"],
    },
];

pub fn find(id: &str) -> Option<&'static Agent> {
    AGENTS.iter().find(|a| a.id == id)
}

/// Tool folders under the home that are readable inside the box (compilers, runtimes, package managers).
pub const TOOL_DIRS: [&str; 9] = [
    ".cargo",
    ".rustup",
    ".nvm",
    ".bun",
    ".deno",
    ".local/share/pnpm",
    ".local/share/mise",
    ".pyenv",
    ".local/share/uv",
];

/// Always passed through, whatever the agent: a terminal needs these.
const BASE_ENV: [&str; 11] = [
    "TERM",
    "COLORTERM",
    "LANG",
    "LANGUAGE",
    "TZ",
    "EDITOR",
    "VISUAL",
    "NO_COLOR",
    "FORCE_COLOR",
    "TERM_PROGRAM",
    "COLUMNS",
];
const PROXY_ENV: [&str; 8] = [
    "HTTP_PROXY",
    "HTTPS_PROXY",
    "NO_PROXY",
    "http_proxy",
    "https_proxy",
    "no_proxy",
    "SSL_CERT_FILE",
    "NODE_EXTRA_CA_CERTS",
];

/// The environment an agent is given: a short list, never the whole thing (no D-Bus address, no display, no session tokens).
pub fn environment(agent: &Agent, vars: &[(String, String)], home: &Path) -> Vec<(String, String)> {
    let mut env: Vec<(String, String)> = vec![
        ("HOME".into(), home.to_string_lossy().into_owned()),
        ("PATH".into(), sanitized_path(vars, home)),
    ];
    for (key, value) in vars {
        let wanted = BASE_ENV.contains(&key.as_str())
            || key.starts_with("LC_")
            || PROXY_ENV.contains(&key.as_str())
            || agent.env_prefixes.iter().any(|p| key.starts_with(p));
        if wanted {
            env.push((key.clone(), value.clone()));
        }
    }
    for name in ["USER", "LOGNAME"] {
        if let Some((_, v)) = vars.iter().find(|(k, _)| k == name) {
            env.push((name.into(), v.clone()));
        }
    }
    env.push(("SHELL".into(), "/bin/bash".into()));
    env.push(("HELIX_AGENT".into(), agent.id.into()));
    env
}

/// PATH without the entries that are not here inside the box: the system's, and the ones under the home folder (which are all mounted read-only).
fn sanitized_path(vars: &[(String, String)], home: &Path) -> String {
    let given = vars
        .iter()
        .find(|(k, _)| k == "PATH")
        .map(|(_, v)| v.as_str())
        .unwrap_or("");
    let mut keep: Vec<String> = Vec::new();
    for entry in given.split(':').filter(|e| !e.is_empty()) {
        let path = Path::new(entry);
        let ok = entry.starts_with("/usr/") || entry == "/usr/bin" || path.starts_with(home);
        if ok && !keep.iter().any(|k| k == entry) {
            keep.push(entry.to_string());
        }
    }
    for fallback in ["/usr/local/bin", "/usr/bin"] {
        if !keep.iter().any(|k| k == fallback) {
            keep.push(fallback.into());
        }
    }
    keep.join(":")
}

/// What Helix tells the agent about itself, kept short on purpose.
pub fn primer(profile: Profile, project: &Path, desktop_tools: bool) -> String {
    let mut text = format!(
        "You are running under Helix, an AI-first Linux desktop. Profile: {} ({}). Project folder: {}.\n",
        profile.name(),
        profile.about(),
        project.display()
    );
    if profile.boxed() {
        text.push_str("You are inside a sandbox: you can read and write the project folder, your own settings, and use the network. The rest of the user's home folder (SSH keys, other programs' logins, browsers) is not there, and that is by design: do not try to reach it.\n");
    }
    if desktop_tools {
        text.push_str("The `helix` tool server lets you observe and act on the desktop (windows, panels, settings) through Helix's Agent Surface; every use is audited and some need the user's approval. Prefer it to guessing at the desktop's state, and tell the user what you did with it.\n");
    }
    text.push_str("Change Helix's own settings through its tools or its Settings app, never by editing system files or the compositor's config.kdl. When you are unsure what the user wants, ask. Before saying a task is done, check it.\n");
    text
}

/// What is needed to start one agent.
pub struct Launch {
    pub program: PathBuf,
    pub args: Vec<String>,
    /// More environment, for the agent's own config (OpenCode takes its config through a variable).
    pub extra_env: Vec<(String, String)>,
}

/// The flags and variables for this agent and profile. `mcp` is the command that speaks MCP to the Helix desktop tools (the launcher's own proxy);
/// `primer_file` is a file holding the note for the agent.
pub fn launch(
    agent: &Agent,
    program: &Path,
    profile: Profile,
    mcp: Option<&[String]>,
    primer_file: Option<&Path>,
    passthrough: &[String],
) -> Launch {
    let mut args: Vec<String> = Vec::new();
    let mut extra_env: Vec<(String, String)> = Vec::new();
    match agent.id {
        "claude" => {
            if let Some(command) = mcp {
                let config = json!({"mcpServers": {"helix": {"command": command[0], "args": &command[1..]}}});
                args.extend(["--mcp-config".to_string(), config.to_string()]);
            }
            if let Some(file) = primer_file {
                args.extend([
                    "--append-system-prompt-file".to_string(),
                    file.to_string_lossy().into_owned(),
                ]);
            }
            if profile.skips_prompts() {
                args.push("--dangerously-skip-permissions".into());
            }
        }
        "codex" => {
            if let Some(command) = mcp {
                args.extend([
                    "-c".to_string(),
                    format!("mcp_servers.helix.command={}", json!(command[0])),
                ]);
                args.extend([
                    "-c".to_string(),
                    format!("mcp_servers.helix.args={}", json!(&command[1..])),
                ]);
            }
            if profile.boxed() {
                // Helix's box is the boundary; a second sandbox of Codex's own cannot start inside it.
                args.extend(["-s".to_string(), "danger-full-access".to_string()]);
            }
            if profile.skips_prompts() {
                args.extend(["-a".to_string(), "never".to_string()]);
            } else if profile.boxed() {
                args.extend(["-a".to_string(), "on-request".to_string()]);
            }
        }
        "opencode" => {
            let mut config = json!({});
            if let Some(command) = mcp {
                config["mcp"] =
                    json!({"helix": {"type": "local", "command": command, "enabled": true}});
            }
            if let Some(file) = primer_file {
                config["instructions"] = json!([file.to_string_lossy()]);
            }
            if profile.skips_prompts() {
                config["permission"] =
                    json!({"edit": "allow", "bash": "allow", "webfetch": "allow"});
            }
            if config.as_object().is_some_and(|o| !o.is_empty()) {
                extra_env.push(("OPENCODE_CONFIG_CONTENT".into(), config.to_string()));
            }
        }
        _ => {}
    }
    args.extend(passthrough.iter().cloned());
    Launch {
        program: program.to_path_buf(),
        args,
        extra_env,
    }
}

/// Finds an agent's program: on the PATH, then where installers put them.
pub fn locate(
    agent: &Agent,
    path: &str,
    home: &Path,
    exists: &dyn Fn(&Path) -> bool,
) -> Option<PathBuf> {
    let mut places: Vec<PathBuf> = path
        .split(':')
        .filter(|e| !e.is_empty())
        .map(PathBuf::from)
        .collect();
    places.extend(
        [
            ".local/bin",
            ".opencode/bin",
            ".bun/bin",
            ".npm-global/bin",
            ".cargo/bin",
        ]
        .map(|p| home.join(p)),
    );
    places.extend(["/usr/local/bin", "/usr/bin"].map(PathBuf::from));
    places
        .into_iter()
        .map(|d| d.join(agent.program))
        .find(|p| exists(p))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vars() -> Vec<(String, String)> {
        [
            ("TERM", "xterm-256color"),
            ("LANG", "en_US.UTF-8"),
            (
                "PATH",
                "/home/a/.local/bin:/usr/local/bin:/usr/bin:/opt/other/bin:/home/a/.cargo/bin",
            ),
            ("ANTHROPIC_API_KEY", "sk-ant"),
            ("OPENAI_API_KEY", "sk-oai"),
            ("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus"),
            ("WAYLAND_DISPLAY", "wayland-1"),
            ("SSH_AUTH_SOCK", "/run/user/1000/keyring/ssh"),
            ("GITHUB_TOKEN", "ghp_x"),
            ("USER", "a"),
        ]
        .iter()
        .map(|(k, v)| (k.to_string(), v.to_string()))
        .collect()
    }

    #[test]
    fn an_agent_gets_its_own_keys_and_nothing_that_reaches_the_desktop() {
        let env = environment(find("claude").unwrap(), &vars(), Path::new("/home/a"));
        let has = |k: &str| env.iter().any(|(key, _)| key == k);
        assert!(has("ANTHROPIC_API_KEY") && has("TERM") && has("USER") && has("HOME"));
        assert!(!has("OPENAI_API_KEY"), "another agent's key is not passed");
        for secret in [
            "DBUS_SESSION_BUS_ADDRESS",
            "WAYLAND_DISPLAY",
            "SSH_AUTH_SOCK",
            "GITHUB_TOKEN",
        ] {
            assert!(!has(secret), "{secret}");
        }
        let path = &env.iter().find(|(k, _)| k == "PATH").unwrap().1;
        assert_eq!(
            path,
            "/home/a/.local/bin:/usr/local/bin:/usr/bin:/home/a/.cargo/bin"
        );
        let codex = environment(find("codex").unwrap(), &vars(), Path::new("/home/a"));
        assert!(
            codex.iter().any(|(k, _)| k == "OPENAI_API_KEY")
                && !codex.iter().any(|(k, _)| k == "ANTHROPIC_API_KEY")
        );
    }

    #[test]
    fn each_agent_is_given_the_desktop_tools_and_the_profiles_prompt_setting() {
        let mcp = vec![
            "/opt/helix/helix-agents".to_string(),
            "mcp-proxy".to_string(),
            "/run/x.sock".to_string(),
        ];
        let primer = Path::new("/tmp/primer.md");
        let program = Path::new("/usr/bin/x");
        let claude = launch(
            find("claude").unwrap(),
            program,
            Profile::Auto,
            Some(&mcp),
            Some(primer),
            &["--model".into(), "opus".into()],
        );
        assert!(claude.args.windows(2).any(|w| w[0] == "--mcp-config"
            && w[1].contains("mcp-proxy")
            && w[1].contains("\"helix\"")));
        assert!(claude
            .args
            .contains(&"--dangerously-skip-permissions".to_string()));
        assert!(
            claude
                .args
                .ends_with(&["--model".to_string(), "opus".to_string()]),
            "the person's own flags come last"
        );
        let ask = launch(
            find("claude").unwrap(),
            program,
            Profile::Ask,
            Some(&mcp),
            Some(primer),
            &[],
        );
        assert!(
            !ask.args
                .contains(&"--dangerously-skip-permissions".to_string()),
            "ask keeps the agent's prompts"
        );
        let codex = launch(
            find("codex").unwrap(),
            program,
            Profile::Auto,
            Some(&mcp),
            None,
            &[],
        );
        assert!(codex
            .args
            .iter()
            .any(|a| a.starts_with("mcp_servers.helix.command=\"/opt/helix")));
        assert!(
            codex
                .args
                .windows(2)
                .any(|w| w == ["-s", "danger-full-access"])
                && codex.args.windows(2).any(|w| w == ["-a", "never"])
        );
        let open = launch(
            find("codex").unwrap(),
            program,
            Profile::Open,
            Some(&mcp),
            None,
            &[],
        );
        assert!(
            !open.args.contains(&"-s".to_string()) && !open.args.contains(&"-a".to_string()),
            "unboxed: Codex keeps its own sandbox and prompts"
        );
        let opencode = launch(
            find("opencode").unwrap(),
            program,
            Profile::Auto,
            Some(&mcp),
            Some(primer),
            &[],
        );
        let config: serde_json::Value = serde_json::from_str(&opencode.extra_env[0].1).unwrap();
        assert_eq!(config["mcp"]["helix"]["command"][1], "mcp-proxy");
        assert_eq!(config["permission"]["bash"], "allow");
        assert_eq!(config["instructions"][0], "/tmp/primer.md");
        let plain = launch(
            find("opencode").unwrap(),
            program,
            Profile::Ask,
            None,
            None,
            &[],
        );
        assert!(plain.extra_env.is_empty());
    }

    #[test]
    fn agents_are_found_on_the_path_or_where_installers_put_them() {
        let exists = |p: &Path| {
            p == Path::new("/home/a/.opencode/bin/opencode") || p == Path::new("/usr/bin/claude")
        };
        let a = Path::new("/home/a");
        assert_eq!(
            locate(find("opencode").unwrap(), "/usr/bin", a, &exists),
            Some(PathBuf::from("/home/a/.opencode/bin/opencode"))
        );
        assert_eq!(
            locate(find("claude").unwrap(), "/usr/bin", a, &exists),
            Some(PathBuf::from("/usr/bin/claude"))
        );
        assert_eq!(locate(find("codex").unwrap(), "/usr/bin", a, &exists), None);
    }

    #[test]
    fn the_primer_says_where_the_agent_is_and_what_it_may_do() {
        let text = primer(Profile::Auto, Path::new("/home/a/app"), true);
        assert!(
            text.contains("auto")
                && text.contains("/home/a/app")
                && text.contains("sandbox")
                && text.contains("Agent Surface")
        );
        assert!(!primer(Profile::Open, Path::new("/x"), false).contains("sandbox"));
    }
}
