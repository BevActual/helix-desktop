use crate::Policy;
use std::path::Path;

/// Absolute, normal, and not a place that would hand the task the machine (`/`, the home folders, system state, devices).
fn read_only_ok(path: &str) -> bool {
    path.starts_with('/')
        && !path.split('/').any(|p| p == ".." || p == ".")
        && !matches!(
            path.trim_end_matches('/'),
            "" | "/home"
                | "/root"
                | "/etc"
                | "/var"
                | "/run"
                | "/proc"
                | "/sys"
                | "/dev"
                | "/tmp"
                | "/usr"
                | "/bin"
                | "/lib"
                | "/lib64"
        )
        && !["/proc", "/sys", "/dev", "/run", "/root"]
            .iter()
            .any(|p| path == *p || path.starts_with(&format!("{p}/")))
}

/// A relative path inside the folder, with no way out.
fn inside_ok(path: &str) -> bool {
    !path.is_empty()
        && !path.starts_with('/')
        && !path.contains('\0')
        && !path.split('/').any(|p| p == ".." || p == ".")
}

/// The arguments for `bwrap` that run a command under the policy: the folder is the only writable place, the system is read-only, the network is
/// off unless the policy turns it on, and paths the policy protects or hides are read-only or empty. `exists` says which host paths are there
/// (so the result does not depend on the machine it is tested on). The caller adds `-- /bin/bash -c <script>`.
pub fn bwrap_args(
    policy: &Policy,
    folder: &Path,
    exists: &dyn Fn(&Path) -> bool,
    is_dir: &dyn Fn(&Path) -> bool,
) -> Vec<String> {
    let folder_text = folder.to_string_lossy().to_string();
    let mut args: Vec<String> = ["--die-with-parent", "--new-session", "--unshare-all"]
        .map(String::from)
        .to_vec();
    if policy.network {
        args.push("--share-net".into());
    }
    args.extend(["--cap-drop", "ALL"].map(String::from));
    for path in ["/usr", "/bin", "/lib", "/lib64"] {
        if exists(Path::new(path)) {
            args.extend(["--ro-bind", path, path].map(String::from));
        }
    }
    if policy.network {
        // Names and certificates, so a command can look up a host and trust it.
        for path in [
            "/etc/resolv.conf",
            "/etc/hosts",
            "/etc/nsswitch.conf",
            "/etc/ssl",
            "/etc/pki",
            "/etc/ca-certificates",
        ] {
            if exists(Path::new(path)) {
                args.extend(["--ro-bind", path, path].map(String::from));
            }
        }
    }
    for path in policy.read_only.iter().filter(|p| read_only_ok(p)) {
        if exists(Path::new(path)) {
            args.extend(["--ro-bind".to_string(), path.clone(), path.clone()]);
        }
    }
    args.extend(
        [
            "--proc",
            "/proc",
            "--dev",
            "/dev",
            "--tmpfs",
            "/tmp",
            "--dir",
            "/tmp/home",
            "--bind",
        ]
        .map(String::from),
    );
    args.push(folder_text.clone());
    args.push("/workspace".into());
    // These come after the folder is mounted, so they sit on top of it.
    for path in policy.protect.iter().filter(|p| inside_ok(p)) {
        let host = Path::new(&folder_text).join(path);
        if exists(&host) {
            args.extend([
                "--ro-bind".to_string(),
                host.to_string_lossy().to_string(),
                format!("/workspace/{path}"),
            ]);
        }
    }
    for path in policy.hide.iter().filter(|p| inside_ok(p)) {
        let host = Path::new(&folder_text).join(path);
        if exists(&host) {
            if is_dir(&host) {
                args.extend(["--tmpfs".to_string(), format!("/workspace/{path}")]);
            } else {
                args.extend([
                    "--ro-bind".to_string(),
                    "/dev/null".to_string(),
                    format!("/workspace/{path}"),
                ]);
            }
        }
    }
    args.extend(
        [
            "--chdir",
            "/workspace",
            "--clearenv",
            "--setenv",
            "PATH",
            "/usr/local/bin:/usr/bin:/bin",
            "--setenv",
            "HOME",
            "/tmp/home",
            "--setenv",
            "LANG",
            "C.UTF-8",
            "--setenv",
            "TERM",
            "dumb",
        ]
        .map(String::from),
    );
    args
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(policy: &Policy, present: &[&str], dirs: &[&str]) -> Vec<String> {
        let present: Vec<String> = present.iter().map(|s| s.to_string()).collect();
        let dirs: Vec<String> = dirs.iter().map(|s| s.to_string()).collect();
        bwrap_args(
            policy,
            Path::new("/home/me/proj"),
            &|p| present.iter().any(|x| Path::new(x) == p),
            &|p| dirs.iter().any(|x| Path::new(x) == p),
        )
    }
    fn has(args: &[String], seq: &[&str]) -> bool {
        args.windows(seq.len())
            .any(|w| w.iter().zip(seq).all(|(a, b)| a == b))
    }

    #[test]
    fn by_default_there_is_no_network_and_only_the_folder_is_writable() {
        let a = args(&Policy::careful(), &["/usr", "/bin"], &[]);
        assert!(has(&a, &["--unshare-all"]));
        assert!(!a.contains(&"--share-net".to_string()));
        assert!(has(&a, &["--ro-bind", "/usr", "/usr"]));
        assert!(has(&a, &["--bind", "/home/me/proj", "/workspace"]));
        assert_eq!(
            a.iter().filter(|x| *x == "--bind").count(),
            1,
            "one writable mount"
        );
        assert!(has(&a, &["--clearenv"]));
    }

    #[test]
    fn network_brings_name_lookup_and_certificates_with_it() {
        let mut p = Policy::careful();
        p.network = true;
        let a = args(&p, &["/etc/resolv.conf", "/etc/ssl"], &[]);
        assert!(has(&a, &["--share-net"]));
        assert!(has(
            &a,
            &["--ro-bind", "/etc/resolv.conf", "/etc/resolv.conf"]
        ));
        assert!(has(&a, &["--ro-bind", "/etc/ssl", "/etc/ssl"]));
    }

    #[test]
    fn protected_paths_are_read_only_and_hidden_ones_are_empty_on_top_of_the_folder() {
        let p = Policy::balanced("/home/me/proj");
        let a = args(
            &p,
            &[
                "/home/me/proj/.git",
                "/home/me/proj/.env",
                "/home/me/proj/.ssh",
            ],
            &["/home/me/proj/.git", "/home/me/proj/.ssh"],
        );
        assert!(has(
            &a,
            &["--ro-bind", "/home/me/proj/.git", "/workspace/.git"]
        ));
        assert!(has(&a, &["--ro-bind", "/dev/null", "/workspace/.env"]));
        assert!(has(&a, &["--tmpfs", "/workspace/.ssh"]));
        let bind = a.iter().position(|x| x == "--bind").unwrap();
        let protect = a.iter().position(|x| x == "/workspace/.git").unwrap();
        assert!(protect > bind, "mounted after the folder so it sits on top");
        // Not there, not mounted.
        let none = args(&p, &[], &[]);
        assert!(!none.iter().any(|x| x.starts_with("/workspace/.")));
    }

    #[test]
    fn extra_read_only_paths_cannot_open_the_machine_or_escape() {
        let mut p = Policy::careful();
        p.read_only = [
            "/opt/toolchain",
            "/",
            "/home",
            "/etc",
            "/proc/1",
            "/dev",
            "../x",
            "relative",
            "/opt/../etc",
            "/run/user/1000",
        ]
        .map(String::from)
        .to_vec();
        p.protect = ["../escape", "/abs", ""].map(String::from).to_vec();
        p.hide = ["a/../../b"].map(String::from).to_vec();
        let all = [
            "/opt/toolchain",
            "/",
            "/home",
            "/etc",
            "/proc/1",
            "/dev",
            "/opt/../etc",
            "/run/user/1000",
            "/home/me/proj/../escape",
            "/abs",
        ];
        let a = args(&p, &all, &all);
        assert!(has(&a, &["--ro-bind", "/opt/toolchain", "/opt/toolchain"]));
        assert_eq!(
            a.iter().filter(|x| *x == "--ro-bind").count(),
            1,
            "only the toolchain: {a:?}"
        );
        assert!(!a.iter().any(|x| x.contains("escape")));
    }
}
