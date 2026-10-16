//! Pure peer identity helpers (no D-Bus). Port of `helix::clientIdentity`.

const INTERPRETERS: &[&str] = &["python", "node", "bun", "deno", "ruby", "perl", "bash"];

/// Stable grant key from a verified executable path and command line.
pub fn client_identity(exe_path: &str, cmdline: &[String]) -> String {
    let base = std::path::Path::new(exe_path)
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or_default();
    let interpreter = base == "sh"
        || INTERPRETERS
            .iter()
            .any(|name| base.starts_with(name));
    if interpreter {
        for arg in cmdline.iter().skip(1) {
            if arg.is_empty() || arg.starts_with('-') {
                continue;
            }
            let script = std::fs::canonicalize(arg)
                .map(|path| path.display().to_string())
                .unwrap_or_else(|_| arg.clone());
            return format!("exe:{exe_path}#{script}");
        }
    }
    format!("exe:{exe_path}")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_binary() {
        assert_eq!(client_identity("/usr/bin/helix-assistant", &[]), "exe:/usr/bin/helix-assistant");
    }

    #[test]
    fn python_script() {
        let id = client_identity(
            "/usr/bin/python3",
            &["python3".into(), "/tmp/probe.py".into()],
        );
        assert!(id.starts_with("exe:/usr/bin/python3#"));
        assert!(id.contains("probe.py"));
    }
}
