//! The box an agent runs in: the project folder and the agent's own state are writable, the network works (agents call their APIs), the system and the
//! person's tools are read-only, and everything else of the home folder (`~/.ssh`, `~/.aws`, browsers, password stores, other programs' logins) simply is
//! not there. Only the one MCP socket reaches the desktop (never the D-Bus session bus, which would reach every service with it).

use std::path::{Path, PathBuf};

pub struct Spec<'a> {
    pub home: &'a Path,
    /// The project folder, writable, at the same path inside.
    pub project: &'a Path,
    /// What the agent runs: its program (and the folder it lives in).
    pub program: &'a Path,
    /// Folders under the home that hold the agent's own state (writable), and files (writable).
    pub state_dirs: &'a [&'a str],
    pub state_files: &'a [&'a str],
    /// Folders under the home that hold tools and toolchains (read-only): `.cargo`, `.nvm`, …
    pub tool_dirs: &'a [&'a str],
    /// Other folders to make readable (the launcher, the MCP adapter).
    pub readable: &'a [PathBuf],
    /// Folders to make writable (the MCP socket's folder).
    pub writable: &'a [PathBuf],
    /// The environment the agent is given.
    pub env: &'a [(String, String)],
}

const SYSTEM: [&str; 3] = ["/usr", "/etc/ld.so.cache", "/etc/ld.so.conf.d"];
/// Names, certificates, users and time: what a program needs to reach an API and run git.
const ETC: [&str; 11] = [
    "/etc/resolv.conf",
    "/etc/hosts",
    "/etc/nsswitch.conf",
    "/etc/ssl",
    "/etc/pki",
    "/etc/ca-certificates",
    "/etc/passwd",
    "/etc/group",
    "/etc/localtime",
    "/etc/os-release",
    "/etc/gitconfig",
];

/// The `bwrap` arguments, before `--` and the program. `exists` says which host paths are there (so the result does not depend on the machine).
pub fn bwrap_args(spec: &Spec<'_>, exists: &dyn Fn(&Path) -> bool) -> Vec<String> {
    let text = |p: &Path| p.to_string_lossy().into_owned();
    let mut args: Vec<String> = [
        "--die-with-parent",
        "--unshare-all",
        "--share-net",
        "--cap-drop",
        "ALL",
        "--clearenv",
    ]
    .map(String::from)
    .to_vec();
    args.extend(
        [
            "--ro-bind",
            "/usr",
            "/usr",
            "--symlink",
            "usr/lib64",
            "/lib64",
            "--symlink",
            "usr/lib",
            "/lib",
            "--symlink",
            "usr/bin",
            "/bin",
            "--symlink",
            "usr/sbin",
            "/sbin",
        ]
        .map(String::from),
    );
    for path in SYSTEM.iter().skip(1).chain(ETC.iter()) {
        if exists(Path::new(path)) {
            args.extend(["--ro-bind", path, path].map(String::from));
        }
    }
    // systemd-resolved's stub file is what /etc/resolv.conf points at.
    if exists(Path::new("/run/systemd/resolve")) {
        args.extend(
            ["--ro-bind", "/run/systemd/resolve", "/run/systemd/resolve"].map(String::from),
        );
    }
    args.extend(["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"].map(String::from));
    // The home folder is empty, then given back piece by piece.
    args.extend(["--tmpfs".to_string(), text(spec.home)]);
    let mut shown: Vec<PathBuf> = Vec::new();
    let mut readonly = |path: PathBuf, args: &mut Vec<String>| {
        if exists(&path) && !shown.contains(&path) {
            args.extend(["--ro-bind".to_string(), text(&path), text(&path)]);
            shown.push(path);
        }
    };
    for dir in spec.tool_dirs {
        readonly(spec.home.join(dir), &mut args);
    }
    readonly(spec.home.join(".gitconfig"), &mut args);
    readonly(spec.home.join(".config/git"), &mut args);
    // The agent's program, and the folder it was found in.
    if let Some(folder) = spec.program.parent() {
        readonly(folder.to_path_buf(), &mut args);
    }
    for path in spec.readable {
        readonly(path.clone(), &mut args);
    }
    // Writable: the agent's own state, then the project, then the socket folder.
    for dir in spec.state_dirs {
        let path = spec.home.join(dir);
        if exists(&path) {
            args.extend(["--bind".to_string(), text(&path), text(&path)]);
        } else {
            args.extend(["--dir".to_string(), text(&path)]);
        }
    }
    for file in spec.state_files {
        let path = spec.home.join(file);
        if exists(&path) {
            args.extend(["--bind".to_string(), text(&path), text(&path)]);
        }
    }
    args.extend(["--bind".to_string(), text(spec.project), text(spec.project)]);
    for path in spec.writable {
        if exists(path) {
            args.extend(["--bind".to_string(), text(path), text(path)]);
        }
    }
    for (key, value) in spec.env {
        args.extend(["--setenv".to_string(), key.clone(), value.clone()]);
    }
    args.extend(["--chdir".to_string(), text(spec.project)]);
    args
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec<'a>(
        env: &'a [(String, String)],
        readable: &'a [PathBuf],
        writable: &'a [PathBuf],
    ) -> Spec<'a> {
        Spec {
            home: Path::new("/home/a"),
            project: Path::new("/home/a/work/app"),
            program: Path::new("/home/a/.local/bin/claude"),
            state_dirs: &[".claude"],
            state_files: &[".claude.json"],
            tool_dirs: &[".cargo", ".nvm"],
            readable,
            writable,
            env,
        }
    }

    fn pairs(args: &[String], flag: &str) -> Vec<(String, String)> {
        args.windows(3)
            .filter(|w| w[0] == flag)
            .map(|w| (w[1].clone(), w[2].clone()))
            .collect()
    }

    #[test]
    fn the_home_folder_is_empty_except_what_the_agent_needs() {
        let env = [("TERM".to_string(), "xterm".to_string())];
        let readable = [PathBuf::from("/opt/helix/bin")];
        let writable = [PathBuf::from("/run/user/1000/helix-agents/1")];
        let exists = |p: &Path| {
            p != Path::new("/home/a/.nvm")
                && p != Path::new("/etc/pki")
                && p != Path::new("/home/a/.claude")
        };
        let args = bwrap_args(&spec(&env, &readable, &writable), &exists);
        assert!(
            args.contains(&"--share-net".to_string()),
            "agents call their APIs"
        );
        assert!(
            args.windows(2).any(|w| w == ["--tmpfs", "/home/a"]),
            "the home folder starts empty"
        );
        let ro: Vec<String> = pairs(&args, "--ro-bind").into_iter().map(|p| p.0).collect();
        for path in [
            "/usr",
            "/home/a/.cargo",
            "/home/a/.local/bin",
            "/opt/helix/bin",
            "/etc/ssl",
        ] {
            assert!(ro.contains(&path.to_string()), "{path} is readable");
        }
        assert!(
            !ro.contains(&"/home/a/.nvm".to_string()) && !ro.contains(&"/etc/pki".to_string()),
            "what is not there is skipped"
        );
        let rw: Vec<String> = pairs(&args, "--bind").into_iter().map(|p| p.0).collect();
        assert_eq!(
            rw,
            [
                "/home/a/.claude.json",
                "/home/a/work/app",
                "/run/user/1000/helix-agents/1"
            ]
        );
        assert!(
            args.windows(2).any(|w| w == ["--dir", "/home/a/.claude"]),
            "the agent's state folder is created if it is new"
        );
        // Nothing secret is mounted, and the session bus is not.
        for secret in [
            ".ssh",
            ".aws",
            ".gnupg",
            ".config/gh",
            ".mozilla",
            ".password-store",
            "/run/user/1000/bus",
        ] {
            assert!(
                !args.iter().any(|a| a.contains(secret)),
                "{secret} must not be mounted"
            );
        }
        assert!(args
            .windows(2)
            .any(|w| w == ["--chdir", "/home/a/work/app"]));
        assert!(pairs(&args, "--setenv").contains(&("TERM".to_string(), "xterm".to_string())));
    }
}
