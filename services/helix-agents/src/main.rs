//! helix-agents: start an AI coding agent (Claude Code, OpenCode, Codex) the Helix way. The agent runs in a profile (see `profile.rs`), in a bubblewrap
//! box that holds the project folder and the agent's own state but not the rest of the home folder, with the Helix desktop tools (MCP) and a short note
//! about Helix put in front of it for this run only. Every launch is logged. Nothing in the agent's own config files is edited.
//!
//!   helix-agents run [AGENT] [--profile ask|auto|open|danger] [--dir PATH|focused] [--for 30m] [-- agent flags…]
//!   helix-agents terminal [AGENT] [--profile …]     open a terminal in the focused window's folder running the agent (the global key)
//!   helix-agents default [AGENT [PROFILE]]          show or set the default agent and profile
//!   helix-agents list | doctor | install-links
//!
//! Started as `helix-claude`, `helix-opencode`, `helix-codex` or `ha` (the default agent) it is `run` for that agent (`install-links` makes those names).

mod agents;
mod bridge;
mod focus;
mod profile;
mod sandbox;

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use agents::{Agent, AGENTS, TOOL_DIRS};
use profile::Profile;
use serde_json::{json, Value};

fn home() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn config_path() -> PathBuf {
    std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".config"))
        .join("helix/agents.json")
}

fn state_dir() -> PathBuf {
    std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".local/state"))
        .join("helix/agents")
}

/// The default agent and profile: `{"default":"claude","profile":"ask"}`.
fn defaults() -> (String, Profile) {
    let value: Value = std::fs::read_to_string(config_path())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or(Value::Null);
    let agent = value["default"]
        .as_str()
        .filter(|a| agents::find(a).is_some())
        .unwrap_or("claude")
        .to_string();
    let profile = value["profile"]
        .as_str()
        .and_then(Profile::parse)
        .unwrap_or(Profile::Ask);
    (agent, profile)
}

fn set_defaults(agent: &str, profile: Profile) -> Result<(), String> {
    let path = config_path();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    std::fs::write(
        &path,
        json!({"default": agent, "profile": profile.name()}).to_string() + "\n",
    )
    .map_err(|e| e.to_string())
}

fn exists(path: &Path) -> bool {
    path.exists()
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

/// One line per launch and per end, in `<state>/helix/agents/launches.jsonl`.
fn log(entry: Value) {
    let dir = state_dir();
    let _ = std::fs::create_dir_all(&dir);
    if let Ok(mut file) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(dir.join("launches.jsonl"))
    {
        use std::io::Write;
        let _ = writeln!(file, "{entry}");
    }
}

struct Options {
    agent: Option<String>,
    profile: Option<Profile>,
    dir: Option<String>,
    expiry: Option<u64>,
    pass: Vec<String>,
}

fn parse_options(args: &[String], take_agent: bool) -> Result<Options, String> {
    let mut o = Options {
        agent: None,
        profile: None,
        dir: None,
        expiry: None,
        pass: Vec::new(),
    };
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        let mut value = |name: &str| -> Result<String, String> {
            i += 1;
            args.get(i).cloned().ok_or(format!("{name} needs a value"))
        };
        match a {
            "--" => {
                o.pass.extend(args[i + 1..].iter().cloned());
                break;
            }
            "--profile" => {
                let v = value("--profile")?;
                o.profile = Some(Profile::parse(&v).ok_or(format!(
                    "the profiles are ask, auto, open and danger, not {v}"
                ))?);
            }
            "--dir" => o.dir = Some(value("--dir")?),
            "--here" => o.dir = Some(".".into()),
            "--for" => {
                let v = value("--for")?;
                o.expiry = Some(
                    profile::parse_duration(&v)
                        .ok_or(format!("a time like 30m or 2h (up to 24h), not {v}"))?,
                );
            }
            other if take_agent && o.agent.is_none() && !other.starts_with('-') => {
                o.agent = Some(other.to_string())
            }
            other => o.pass.push(other.to_string()),
        }
        i += 1;
    }
    Ok(o)
}

fn run(program_name: Option<&str>, args: &[String]) -> i32 {
    let o = match parse_options(args, program_name.is_none()) {
        Ok(o) => o,
        Err(e) => return fail(&e),
    };
    let (default_agent, default_profile) = defaults();
    let id = program_name
        .map(str::to_string)
        .or(o.agent.clone())
        .unwrap_or(default_agent);
    let Some(agent) = agents::find(&id) else {
        return fail(&format!(
            "I do not know the agent {id}. Known: {}",
            AGENTS.iter().map(|a| a.id).collect::<Vec<_>>().join(", ")
        ));
    };
    let profile = o.profile.unwrap_or(default_profile);
    if profile.needs_expiry() && o.expiry.is_none() {
        return fail("the danger profile is for a limited time: add --for 30m (or 1h, up to 24h)");
    }
    let home = home();
    let project = match resolve_dir(o.dir.as_deref(), &home) {
        Ok(p) => p,
        Err(e) => return fail(&e),
    };
    if profile.boxed() && !focus::usable(&project, &home) {
        return fail(&format!("{} is not a place to start an agent (it must be under your home folder or /tmp, /srv, /mnt, /media, /opt)", project.display()));
    }
    let path_var = std::env::var("PATH").unwrap_or_default();
    let Some(program) = agents::locate(agent, &path_var, &home, &exists) else {
        return fail(&format!("{} is not installed ({} is not on the PATH or in the usual places). Install it with its own installer, then run this again.", agent.title, agent.program));
    };
    let mut exit = launch(agent, &program, profile, &project, &home, o.expiry, &o.pass);
    if exit < 0 {
        exit = 1;
    }
    exit
}

fn resolve_dir(arg: Option<&str>, home: &Path) -> Result<PathBuf, String> {
    let dir = match arg {
        Some("focused") => focus::focused_pid()
            .and_then(|pid| focus::working_directory(Path::new("/proc"), pid))
            .filter(|d| focus::usable(d, home))
            .or_else(|| std::env::current_dir().ok())
            .ok_or("could not tell which folder the focused window is in")?,
        Some(path) => PathBuf::from(path),
        None => std::env::current_dir().map_err(|e| e.to_string())?,
    };
    let dir = std::fs::canonicalize(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    dir.is_dir()
        .then_some(dir.clone())
        .ok_or(format!("{} is not a folder", dir.display()))
}

fn fail(message: &str) -> i32 {
    eprintln!("helix-agents: {message}");
    2
}

/// The folder the Helix sources are in, when this is a checkout (the MCP adapter lives there).
fn source_root() -> Option<PathBuf> {
    if let Some(root) = std::env::var_os("HELIX_ROOT") {
        return Some(PathBuf::from(root));
    }
    let exe = std::env::current_exe().ok()?;
    exe.ancestors()
        .find(|a| a.join("tools/helix-agent/run.sh").exists())
        .map(Path::to_path_buf)
}

extern "C" fn ignore(_: libc::c_int) {}

fn launch(
    agent: &Agent,
    program: &Path,
    profile: Profile,
    project: &Path,
    home: &Path,
    expiry: Option<u64>,
    pass: &[String],
) -> i32 {
    let vars: Vec<(String, String)> = std::env::vars().collect();
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    let session = format!("{}-{}", std::process::id(), now());
    // The desktop tools: a socket the launcher serves, and a note for the agent, in a private folder that is mounted into the box.
    let mut mcp: Option<Vec<String>> = None;
    let mut private: Option<PathBuf> = None;
    let mut primer_file: Option<PathBuf> = None;
    let exe = std::env::current_exe().unwrap_or_else(|_| PathBuf::from("helix-agents"));
    if let Some(command) = bridge::adapter(
        std::env::var("HELIX_MCP_COMMAND").ok().as_deref(),
        source_root().as_deref(),
        &exists,
    ) {
        if let Ok(folder) = bridge::private_folder(&runtime.join("helix-agents"), &session) {
            let socket = folder.join("mcp.sock");
            if bridge::serve(&socket, command).is_ok() {
                mcp = Some(vec![
                    exe.to_string_lossy().into_owned(),
                    "mcp-proxy".into(),
                    socket.to_string_lossy().into_owned(),
                ]);
            }
            private = Some(folder);
        }
    }
    if private.is_none() {
        private = bridge::private_folder(&runtime.join("helix-agents"), &session).ok();
    }
    if let Some(folder) = &private {
        let file = folder.join("helix.md");
        if std::fs::write(&file, agents::primer(profile, project, mcp.is_some())).is_ok() {
            primer_file = Some(file);
        }
    }
    let launch = agents::launch(
        agent,
        program,
        profile,
        mcp.as_deref(),
        primer_file.as_deref(),
        pass,
    );
    let mut env = agents::environment(agent, &vars, home);
    env.extend(launch.extra_env.iter().cloned());
    let mut command;
    if profile.boxed() {
        // The launcher itself (the MCP proxy runs inside), and where the agent's program really is when what was found is a link.
        let mut readable = vec![exe.parent().map(Path::to_path_buf).unwrap_or_default()];
        readable.extend(fs_canonical(program).parent().map(Path::to_path_buf));
        let writable: Vec<PathBuf> = private.iter().cloned().collect();
        let spec = sandbox::Spec {
            home,
            project,
            program,
            state_dirs: agent.state_dirs,
            state_files: agent.state_files,
            tool_dirs: &TOOL_DIRS,
            readable: &readable,
            writable: &writable,
            env: &env,
        };
        command = Command::new("bwrap");
        command
            .args(sandbox::bwrap_args(&spec, &exists))
            .arg("--")
            .arg(&launch.program)
            .args(&launch.args);
    } else {
        // Unboxed, it is the plain agent with the person's own environment, launched and logged by Helix.
        command = Command::new(&launch.program);
        command
            .args(&launch.args)
            .current_dir(project)
            .envs(launch.extra_env.iter().cloned());
    }
    eprintln!("{}", banner(agent, profile, project, expiry));
    log(
        json!({"event": "start", "session": session, "pid": std::process::id(), "time": now(), "agent": agent.id, "title": agent.title, "profile": profile.name(), "project": project, "boxed": profile.boxed(), "expires_in": expiry, "desktop_tools": mcp.is_some()}),
    );
    // Ctrl-C belongs to the agent: this process only waits.
    // SAFETY: installing a handler that does nothing; the child resets it on exec.
    unsafe { libc::signal(libc::SIGINT, ignore as *const () as libc::sighandler_t) };
    let mut child = match command.spawn() {
        Ok(c) => c,
        Err(e) => return fail(&format!("could not start {}: {e}", agent.title)),
    };
    let pid = child.id();
    if let Some(seconds) = expiry {
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_secs(seconds.saturating_sub(60)));
            eprintln!(
                "\nhelix-agents: the {} profile ends in one minute.",
                "time-limited"
            );
            std::thread::sleep(Duration::from_secs(seconds.min(60)));
            // SAFETY: signalling our own child.
            unsafe { libc::kill(pid as i32, libc::SIGTERM) };
        });
    }
    let status = child.wait();
    let code = status.as_ref().ok().and_then(|s| s.code()).unwrap_or(1);
    log(
        json!({"event": "end", "session": session, "pid": std::process::id(), "time": now(), "agent": agent.id, "profile": profile.name(), "project": project, "exit": code}),
    );
    if let Some(folder) = private {
        let _ = std::fs::remove_dir_all(folder);
    }
    code
}

fn fs_canonical(path: &Path) -> PathBuf {
    std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf())
}

fn banner(agent: &Agent, profile: Profile, project: &Path, expiry: Option<u64>) -> String {
    let shown = project.strip_prefix(home()).map_or_else(
        |_| project.display().to_string(),
        |p| format!("~/{}", p.display()),
    );
    let line = format!(
        "Helix · {} · {} ({}) · {shown}",
        agent.title,
        profile.name(),
        profile.about()
    );
    match (profile, expiry) {
        (Profile::Danger, Some(s)) => format!("\x1b[1;31m{line} · ends in {} min\x1b[0m", s / 60),
        (Profile::Auto, _) => format!("\x1b[33m{line}\x1b[0m"),
        _ => format!("\x1b[2m{line}\x1b[0m"),
    }
}

// --- terminals ------------------------------------------------------------------------------------------------------------------------

/// `(program, what comes before the command)`.
const TERMINALS: [(&str, &[&str]); 9] = [
    ("foot", &[]),
    ("kitty", &[]),
    ("alacritty", &["-e"]),
    ("ghostty", &["-e"]),
    ("wezterm", &["start", "--"]),
    ("gnome-terminal", &["--"]),
    ("ptyxis", &["--"]),
    ("konsole", &["-e"]),
    ("xterm", &["-e"]),
];

fn find_terminal(path: &str, preferred: Option<&str>) -> Option<(String, Vec<String>)> {
    let on_path = |name: &str| path.split(':').any(|d| Path::new(d).join(name).exists());
    let known = |name: &str| TERMINALS.iter().find(|(n, _)| *n == name);
    let chosen = preferred
        .and_then(|p| known(p))
        .filter(|(n, _)| on_path(n))
        .or_else(|| TERMINALS.iter().find(|(n, _)| on_path(n)))?;
    Some((
        chosen.0.to_string(),
        chosen.1.iter().map(|s| s.to_string()).collect(),
    ))
}

fn terminal(args: &[String]) -> i32 {
    let o = match parse_options(args, true) {
        Ok(o) => o,
        Err(e) => return fail(&e),
    };
    let exe = std::env::current_exe().unwrap_or_else(|_| PathBuf::from("helix-agents"));
    let path = std::env::var("PATH").unwrap_or_default();
    let preferred = std::env::var("HELIX_TERMINAL")
        .ok()
        .or_else(|| std::env::var("TERMINAL").ok());
    let Some((program, before)) = find_terminal(&path, preferred.as_deref()) else {
        return fail("no terminal found (set HELIX_TERMINAL to one of: foot, kitty, alacritty, ghostty, wezterm, gnome-terminal, ptyxis, konsole, xterm)");
    };
    let mut run_args = vec!["run".to_string()];
    run_args.extend(o.agent);
    run_args.extend(["--dir".to_string(), "focused".to_string()]);
    if let Some(p) = o.profile {
        run_args.extend(["--profile".to_string(), p.name().to_string()]);
    }
    if let Some(e) = o.expiry {
        run_args.extend(["--for".to_string(), format!("{}s", e)]);
    }
    // The terminal opens in the folder: the launcher resolves `focused` itself, but the window that has focus when it starts is the new terminal,
    // so it is resolved here first and passed as a path.
    let dir = resolve_dir(Some("focused"), &home())
        .map(|d| d.to_string_lossy().into_owned())
        .unwrap_or_else(|_| ".".into());
    if let Some(at) = run_args.iter().position(|a| a == "focused") {
        run_args[at] = dir.clone();
    }
    match Command::new(&program)
        .args(&before)
        .arg(&exe)
        .args(&run_args)
        .current_dir(&dir)
        .spawn()
    {
        Ok(_) => 0,
        Err(e) => fail(&format!("could not start {program}: {e}")),
    }
}

// --- the rest ---------------------------------------------------------------------------------------------------------------------------

fn list() -> i32 {
    let (default, profile) = defaults();
    let path = std::env::var("PATH").unwrap_or_default();
    for agent in &AGENTS {
        let found = agents::locate(agent, &path, &home(), &exists);
        println!(
            "{:<9} {:<12} {}{}",
            agent.id,
            agent.title,
            found.map_or("not installed".to_string(), |p| p.display().to_string()),
            if agent.id == default {
                format!("   (default, profile {})", profile.name())
            } else {
                String::new()
            }
        );
    }
    println!("\nprofiles:");
    for p in [Profile::Ask, Profile::Auto, Profile::Open, Profile::Danger] {
        println!("  {:<7} {}", p.name(), p.about());
    }
    0
}

fn doctor() -> i32 {
    let ok = |b: bool| if b { "ok     " } else { "MISSING" };
    let bwrap = Command::new("bwrap")
        .arg("--version")
        .output()
        .ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string());
    println!(
        "{}  bubblewrap: {}",
        ok(bwrap.is_some()),
        bwrap.unwrap_or_else(|| "not installed (the boxed profiles need it)".into())
    );
    let boxed = Command::new("bwrap")
        .args([
            "--unshare-all",
            "--ro-bind",
            "/usr",
            "/usr",
            "--symlink",
            "usr/lib64",
            "/lib64",
            "--proc",
            "/proc",
            "--dev",
            "/dev",
            "/usr/bin/true",
        ])
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false);
    println!("{}  a box can be made here", ok(boxed));
    let tiocsti = std::fs::read_to_string("/proc/sys/dev/tty/legacy_tiocsti")
        .map(|t| t.trim() == "0")
        .unwrap_or(true);
    println!(
        "{}  the kernel refuses keystroke injection into a terminal (TIOCSTI)",
        ok(tiocsti)
    );
    let mcp = bridge::adapter(
        std::env::var("HELIX_MCP_COMMAND").ok().as_deref(),
        source_root().as_deref(),
        &exists,
    );
    println!(
        "{}  Helix desktop tools (MCP adapter): {}",
        ok(mcp.is_some()),
        mcp.map_or(
            "not found (set HELIX_ROOT or HELIX_MCP_COMMAND)".into(),
            |c| c.join(" ")
        )
    );
    let path = std::env::var("PATH").unwrap_or_default();
    let term = find_terminal(&path, None);
    println!(
        "{}  a terminal for the global key: {}",
        ok(term.is_some()),
        term.map_or("none found".into(), |t| t.0)
    );
    list();
    0
}

fn install_links() -> i32 {
    let exe = std::env::current_exe().unwrap_or_default();
    let dir = home().join(".local/bin");
    let _ = std::fs::create_dir_all(&dir);
    for name in ["helix-claude", "helix-opencode", "helix-codex", "ha"] {
        let link = dir.join(name);
        let _ = std::fs::remove_file(&link);
        match std::os::unix::fs::symlink(&exe, &link) {
            Ok(()) => println!("{} -> {}", link.display(), exe.display()),
            Err(e) => eprintln!("{}: {e}", link.display()),
        }
    }
    0
}

fn default_command(args: &[String]) -> i32 {
    match args {
        [] => {
            let (a, p) = defaults();
            println!("{a} {}", p.name());
            0
        }
        [agent, rest @ ..] => {
            if agents::find(agent).is_none() {
                return fail(&format!("I do not know the agent {agent}"));
            }
            let profile = match rest.first() {
                Some(word) => match Profile::parse(word) {
                    Some(p) if p != Profile::Danger => p,
                    _ => return fail("the default profile is ask, auto or open (danger is always for a limited time)"),
                },
                None => defaults().1,
            };
            match set_defaults(agent, profile) {
                Ok(()) => {
                    println!("default: {agent} {}", profile.name());
                    0
                }
                Err(e) => fail(&e),
            }
        }
    }
}

fn main() {
    let argv: Vec<OsString> = std::env::args_os().collect();
    let name = argv
        .first()
        .and_then(|a| Path::new(a).file_name())
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let rest: Vec<String> = argv
        .iter()
        .skip(1)
        .map(|a| a.to_string_lossy().into_owned())
        .collect();
    let code = match name.as_str() {
        "helix-claude" => run(Some("claude"), &rest),
        "helix-opencode" => run(Some("opencode"), &rest),
        "helix-codex" => run(Some("codex"), &rest),
        "ha" => run(Some(&defaults().0), &rest),
        _ => match rest.split_first() {
            Some((c, args)) if c == "run" => run(None, args),
            Some((c, args)) if c == "terminal" => terminal(args),
            Some((c, args)) if c == "default" => default_command(args),
            Some((c, args)) if c == "mcp-proxy" && args.len() == 1 => {
                bridge::proxy(Path::new(&args[0]))
            }
            Some((c, _)) if c == "list" => list(),
            Some((c, _)) if c == "doctor" => doctor(),
            Some((c, _)) if c == "install-links" => install_links(),
            _ => {
                eprintln!("usage: helix-agents run [AGENT] [--profile ask|auto|open|danger] [--dir PATH|focused] [--for 30m] [-- agent flags]\n       helix-agents terminal | default | list | doctor | install-links");
                2
            }
        },
    };
    std::process::exit(code);
}
