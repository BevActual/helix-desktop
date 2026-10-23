//! The engine's own sandbox. `llama-server` parses model files, which are untrusted binary input to C++ code, so it runs in a `bwrap` box of its own:
//! no network, no home folder, the system read-only, the models read-only, only its own socket folder writable, the GPU's device nodes and nothing
//! else, and every capability dropped. This is what lets the service that supervises it have network access (for LAN and cloud backends) without the
//! parser having any: the boundary is the box around the engine, not the service's.

use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// Never (tests, machines without bubblewrap and a unit that already boxes the service).
    Off,
    /// When bubblewrap works here, else run unboxed and say so.
    Auto,
    /// Refuse to start the engine without the box.
    Require,
}

impl Mode {
    pub fn from_env(value: &str) -> Self {
        match value {
            "off" => Self::Off,
            "require" => Self::Require,
            _ => Self::Auto,
        }
    }
}

pub struct Spec<'a> {
    /// The engine program and the folders holding its libraries (read-only).
    pub binary: &'a Path,
    pub library_dirs: &'a [PathBuf],
    pub model_dir: &'a Path,
    /// The only writable place: the engine's socket and its caches.
    pub work_dir: &'a Path,
    /// Device nodes the engine may use (GPU).
    pub devices: &'a [PathBuf],
    /// Environment the engine is given (the box starts with none).
    pub env: &'a [(String, String)],
}

/// The GPU's device nodes that exist here: NVIDIA's, the render nodes, and AMD's compute node.
pub fn gpu_devices() -> Vec<PathBuf> {
    let mut out: Vec<PathBuf> = std::fs::read_dir("/dev")
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .is_some_and(|n| n.to_string_lossy().starts_with("nvidia"))
        })
        .collect();
    for fixed in ["/dev/dri", "/dev/kfd"] {
        if Path::new(fixed).exists() {
            out.push(PathBuf::from(fixed));
        }
    }
    out.sort();
    out
}

/// The `bwrap` arguments (before `--` and the engine). `exists` says which host paths are there, so the result does not depend on the machine.
pub fn bwrap_args(spec: &Spec<'_>, exists: &dyn Fn(&Path) -> bool) -> Vec<String> {
    let text = |p: &Path| p.to_string_lossy().into_owned();
    let mut args: Vec<String> = [
        "--die-with-parent",
        "--new-session",
        "--unshare-all",
        "--cap-drop",
        "ALL",
        "--clearenv",
    ]
    .map(String::from)
    .to_vec();
    // The system, read-only: programs, libraries and the GPU drivers' user-space parts (Vulkan and CUDA files live under /usr and /etc).
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
        ]
        .map(String::from),
    );
    for path in [
        "/etc/ld.so.cache",
        "/etc/ld.so.conf",
        "/etc/ld.so.conf.d",
        "/etc/vulkan",
        "/etc/OpenCL",
        "/etc/glvnd",
    ] {
        if exists(Path::new(path)) {
            args.extend(["--ro-bind", path, path].map(String::from));
        }
    }
    // Device discovery reads /sys and /proc.
    args.extend(
        [
            "--ro-bind",
            "/sys",
            "/sys",
            "--proc",
            "/proc",
            "--dev",
            "/dev",
            "--tmpfs",
            "/tmp",
        ]
        .map(String::from),
    );
    for device in spec.devices.iter().filter(|d| exists(d)) {
        args.extend(["--dev-bind".to_string(), text(device), text(device)]);
    }
    // The engine and its libraries, and the models: read-only, at the same paths, so nothing else has to change.
    let mut readable: Vec<PathBuf> = vec![spec.model_dir.to_path_buf()];
    readable.extend(spec.binary.parent().map(Path::to_path_buf));
    readable.extend(spec.library_dirs.iter().cloned());
    readable.dedup();
    for path in readable.iter().filter(|p| exists(p)) {
        args.extend(["--ro-bind".to_string(), text(path), text(path)]);
    }
    args.extend([
        "--bind".to_string(),
        text(spec.work_dir),
        text(spec.work_dir),
    ]);
    for (key, value) in spec.env {
        args.extend(["--setenv".to_string(), key.clone(), value.clone()]);
    }
    args.extend(["--setenv", "HOME", "/tmp", "--setenv", "PATH", "/usr/bin"].map(String::from));
    args
}

/// Whether bubblewrap can make a box here at all (it cannot inside a unit that forbids namespaces, for instance). Cached.
pub fn works() -> Result<(), String> {
    use std::sync::OnceLock;
    static RESULT: OnceLock<Result<(), String>> = OnceLock::new();
    RESULT
        .get_or_init(|| {
            let out = std::process::Command::new("bwrap")
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
                .stdin(std::process::Stdio::null())
                .output()
                .map_err(|e| format!("bubblewrap is not available: {e}"))?;
            if out.status.success() {
                Ok(())
            } else {
                Err(format!(
                    "bubblewrap cannot make a box here: {}",
                    String::from_utf8_lossy(&out.stderr).trim()
                ))
            }
        })
        .clone()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec<'a>(
        devices: &'a [PathBuf],
        env: &'a [(String, String)],
        libs: &'a [PathBuf],
    ) -> Spec<'a> {
        Spec {
            binary: Path::new("/opt/engine/bin/llama-server"),
            library_dirs: libs,
            model_dir: Path::new("/home/a/.local/share/helix/models"),
            work_dir: Path::new("/tmp/helix-inference-1"),
            devices,
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
    fn the_box_has_no_network_no_home_and_one_writable_place() {
        let devices = [
            PathBuf::from("/dev/nvidia0"),
            PathBuf::from("/dev/dri"),
            PathBuf::from("/dev/gone"),
        ];
        let libs = [PathBuf::from("/home/a/.local/cuda/lib")];
        let env = [("LD_LIBRARY_PATH".to_string(), "/opt/engine/bin".to_string())];
        let exists = |p: &Path| p != Path::new("/dev/gone") && p != Path::new("/etc/OpenCL");
        let args = bwrap_args(&spec(&devices, &env, &libs), &exists);
        for flag in [
            "--unshare-all",
            "--die-with-parent",
            "--new-session",
            "--clearenv",
        ] {
            assert!(args.contains(&flag.to_string()), "{flag}");
        }
        assert!(!args.contains(&"--share-net".to_string()), "no network");
        assert!(args.windows(2).any(|w| w == ["--cap-drop", "ALL"]));
        // Read-only: the system, the models, the engine and its libraries. Writable: only the work folder.
        let ro: Vec<String> = pairs(&args, "--ro-bind").into_iter().map(|p| p.0).collect();
        for path in [
            "/usr",
            "/home/a/.local/share/helix/models",
            "/opt/engine/bin",
            "/home/a/.local/cuda/lib",
        ] {
            assert!(ro.contains(&path.to_string()), "{path} read-only");
        }
        assert!(
            !ro.contains(&"/etc/OpenCL".to_string()),
            "paths that are not there are skipped"
        );
        assert_eq!(
            pairs(&args, "--bind"),
            [(
                "/tmp/helix-inference-1".to_string(),
                "/tmp/helix-inference-1".to_string()
            )]
        );
        assert!(
            !args.iter().any(|a| a == "/home/a" || a == "/home"),
            "the home folder is not mounted"
        );
        // Only the devices that exist.
        let dev: Vec<String> = pairs(&args, "--dev-bind")
            .into_iter()
            .map(|p| p.0)
            .collect();
        assert_eq!(dev, ["/dev/nvidia0", "/dev/dri"]);
        // The environment is what was given, nothing inherited.
        assert!(pairs(&args, "--setenv")
            .contains(&("LD_LIBRARY_PATH".to_string(), "/opt/engine/bin".to_string())));
        assert!(pairs(&args, "--setenv").contains(&("HOME".to_string(), "/tmp".to_string())));
    }

    #[test]
    fn modes_are_read_from_a_word() {
        assert_eq!(Mode::from_env("off"), Mode::Off);
        assert_eq!(Mode::from_env("require"), Mode::Require);
        assert_eq!(Mode::from_env(""), Mode::Auto);
        assert_eq!(Mode::from_env("whatever"), Mode::Auto);
    }

    /// Runs a real program in the box and reports what it could do. Skipped where bubblewrap cannot make a box.
    #[test]
    fn the_real_box_cannot_see_home_or_reach_the_network() {
        if works().is_err() {
            return;
        }
        let work = std::env::temp_dir().join(format!("helix-box-test-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&work);
        let secret = std::env::temp_dir().join(format!("helix-box-secret-{}", std::process::id()));
        std::fs::write(&secret, "x").unwrap();
        let models = std::env::temp_dir().join(format!("helix-box-models-{}", std::process::id()));
        std::fs::create_dir_all(&models).unwrap();
        let spec = Spec {
            binary: Path::new("/usr/bin/python3"),
            library_dirs: &[],
            model_dir: &models,
            work_dir: &work,
            devices: &[],
            env: &[],
        };
        let mut args = bwrap_args(&spec, &|p| p.exists());
        args.extend(["--", "/usr/bin/python3", "-c"].map(String::from));
        let script = format!(
            "import os, socket, json\n\
             out = {{}}\n\
             out['home'] = os.path.exists(os.path.expanduser('~/.ssh')) or os.path.exists('/home')\n\
             out['secret'] = os.path.exists({secret:?})\n\
             try:\n    socket.create_connection(('1.1.1.1', 53), 2); out['net'] = True\n\
             except OSError: out['net'] = False\n\
             try:\n    open({models:?} + '/x', 'w'); out['write_models'] = True\n\
             except OSError: out['write_models'] = False\n\
             open({work:?} + '/ok', 'w').write('1'); out['write_work'] = True\n\
             out['env'] = sorted(os.environ)\n\
             print(json.dumps(out))",
            secret = secret.to_string_lossy(),
            models = models.to_string_lossy(),
            work = work.to_string_lossy()
        );
        args.push(script);
        let output = std::process::Command::new("bwrap")
            .args(&args)
            .output()
            .unwrap();
        let _ = std::fs::remove_file(&secret);
        let seen: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap_or_else(|_| {
            panic!(
                "the box did not run: {}",
                String::from_utf8_lossy(&output.stderr)
            )
        });
        let _ = std::fs::remove_dir_all(&work);
        let _ = std::fs::remove_dir_all(&models);
        assert_eq!(seen["home"], false, "no home folder");
        assert_eq!(seen["secret"], false, "the host's /tmp is not the box's");
        assert_eq!(seen["net"], false, "no network");
        assert_eq!(seen["write_models"], false, "models are read-only");
        assert_eq!(seen["write_work"], true, "its own folder is writable");
        let env: Vec<String> = serde_json::from_value(seen["env"].clone()).unwrap();
        assert!(
            !env.iter()
                .any(|k| k.contains("DBUS") || k == "XDG_RUNTIME_DIR" || k == "WAYLAND_DISPLAY"),
            "nothing inherited: {env:?}"
        );
    }
}
