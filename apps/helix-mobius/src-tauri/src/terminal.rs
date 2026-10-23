//! Reviewed project commands. No host home, session sockets, network, or desktop grants.
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;
use tokio::io::AsyncReadExt;
use tokio::sync::{mpsc, watch};

pub fn workspace(raw: &str) -> Result<PathBuf, String> {
    let path = std::fs::canonicalize(raw).map_err(|_| "Choose an existing project folder.")?;
    if !path.is_dir() {
        return Err("Choose a project folder, not a file.".into());
    }
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .and_then(|p| p.canonicalize().ok());
    let protected = [
        "/etc", "/usr", "/proc", "/sys", "/run", "/dev", "/boot", "/var/lib", "/var/log",
    ];
    if path == Path::new("/")
        || home.as_ref() == Some(&path)
        || protected.iter().any(|p| path.starts_with(p))
    {
        return Err(
            "Choose a project folder rather than your whole home or a system directory.".into(),
        );
    }
    if let Some(home) = home {
        if [
            ".config",
            ".ssh",
            ".gnupg",
            ".aws",
            ".kube",
            ".local/share/helix",
            ".local/state/helix",
        ]
        .iter()
        .any(|p| path.starts_with(home.join(p)))
        {
            return Err(
                "Desktop configuration and credential folders are outside project tools.".into(),
            );
        }
    }
    let app_data = crate::data_home().join("helix");
    if path.starts_with(&app_data) || app_data.starts_with(&path) {
        return Err("Choose a folder outside Mobius's application data.".into());
    }
    Ok(path)
}

pub fn safe_input(text: &str, max: usize) -> Result<(), String> {
    if text.trim().is_empty() || text.len() > max {
        return Err(format!("Keep tool input within {max} bytes."));
    }
    if text.chars().any(|c| c != '\n' && c != '\t' && (c.is_control() || matches!(c, '\u{200b}'..='\u{200f}' | '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}' | '\u{feff}'))) {
        return Err("Tool input contains hidden control characters.".into());
    }
    Ok(())
}

pub fn display(text: &str, max: usize) -> String {
    text.chars().filter(|c| *c == '\n' || *c == '\t' || (!c.is_control() && !matches!(*c, '\u{200b}'..='\u{200f}' | '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}'))).take(max).collect()
}

pub struct Output {
    pub text: String,
    pub code: Option<i32>,
    pub stopped: bool,
    pub limited: bool,
}

/// Open a visible terminal on the agent desk with the project folder as cwd (best-effort).
pub fn open_on_agent_desk(folder: &Path) -> Result<(), String> {
    let dir = folder
        .to_str()
        .ok_or("The project folder path is not valid UTF-8.")?;
    helix_compositor::compositor_ipc::place_on_agent_desk(1)?;
    let dir_arg = dir.to_string();
    let attempts: [(&str, Vec<&str>); 5] = [
        ("foot", vec!["-D", &dir_arg]),
        ("ptyxis", vec!["--working-directory", &dir_arg]),
        ("alacritty", vec!["--working-directory", &dir_arg]),
        ("kgx", vec!["--working-directory", &dir_arg]),
        ("gnome-terminal", vec!["--working-directory", &dir_arg]),
    ];
    for (bin, args) in attempts {
        if Command::new(bin)
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .is_ok()
        {
            return Ok(());
        }
    }
    Err(
        "No supported terminal (foot, Ptyxis, Alacritty, Console, or GNOME Terminal) is installed."
            .into(),
    )
}

pub async fn run(
    folder: &Path,
    policy: &helix_task::Policy,
    script: &str,
    mut cancel: watch::Receiver<bool>,
    progress: impl Fn(String),
) -> Result<Output, String> {
    safe_input(script, 2000)?;
    let checked = workspace(
        folder
            .to_str()
            .ok_or("The folder name is not valid UTF-8.")?,
    )?;
    if checked != folder {
        return Err("The project folder changed. Select it again.".into());
    }
    let mut command = tokio::process::Command::new("bwrap");
    command
        .args(helix_task::bwrap_args(
            policy,
            folder,
            &|p| p.exists(),
            &|p| p.is_dir(),
        ))
        .args(["--", "/bin/bash", "--noprofile", "--norc", "-c"])
        .arg(script)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    let mut child = command
        .spawn()
        .map_err(|_| "Project commands need bubblewrap (bwrap).")?;
    let stdout = child.stdout.take().ok_or("No terminal output pipe.")?;
    let stderr = child.stderr.take().ok_or("No terminal error pipe.")?;
    let (sender, mut receiver) = mpsc::channel::<Vec<u8>>(8);
    async fn pump(mut reader: impl tokio::io::AsyncRead + Unpin, sender: mpsc::Sender<Vec<u8>>) {
        let mut bytes = [0; 4096];
        while let Ok(size) = reader.read(&mut bytes).await {
            if size == 0 || sender.send(bytes[..size].to_vec()).await.is_err() {
                break;
            }
        }
    }
    let one = tokio::spawn(pump(stdout, sender.clone()));
    let two = tokio::spawn(pump(stderr, sender));
    let deadline = tokio::time::sleep(Duration::from_secs(30));
    tokio::pin!(deadline);
    let mut bytes = Vec::new();
    let mut stopped = false;
    let mut limited = false;
    loop {
        tokio::select! {
            biased;
            _ = cancel.changed() => { stopped = true; break; },
            _ = &mut deadline => { limited = true; break; },
            chunk = receiver.recv() => match chunk {
                None => break,
                Some(chunk) => {
                    let room = (64 * 1024_usize).saturating_sub(bytes.len());
                    bytes.extend_from_slice(&chunk[..chunk.len().min(room)]);
                    progress(display(&String::from_utf8_lossy(&bytes), 3000));
                    if chunk.len() > room { limited = true; break; }
                }
            }
        }
    }
    if stopped || limited || *cancel.borrow() {
        stopped |= *cancel.borrow();
        let _ = child.kill().await;
    }
    let status = tokio::select! {
        biased;
        _ = cancel.changed() => { stopped = true; let _ = child.kill().await; child.wait().await },
        _ = &mut deadline, if !limited => { limited = true; let _ = child.kill().await; child.wait().await },
        status = child.wait() => status,
    }.map_err(|e| e.to_string())?;
    one.abort();
    two.abort();
    Ok(Output {
        text: display(&String::from_utf8_lossy(&bytes), 12_000),
        code: status.code(),
        stopped,
        limited,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn hidden_inputs_and_system_folders_are_refused() {
        assert!(safe_input("echo hello", 2000).is_ok());
        assert!(safe_input("echo \u{202e}hidden", 2000).is_err());
        assert!(workspace("/").is_err());
        assert!(workspace("/etc").is_err());
    }
    #[tokio::test]
    async fn the_policy_decides_what_a_command_can_see_and_change() {
        let path = std::env::temp_dir().join(format!("mobius-policy-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(path.join(".git")).unwrap();
        std::fs::write(path.join(".env"), "SECRET=1").unwrap();
        std::fs::write(path.join(".git/HEAD"), "ref\n").unwrap();
        let folder = path.canonicalize().unwrap();
        let (_signal, cancel) = watch::channel(false);
        let policy = helix_task::Policy::balanced(folder.to_str().unwrap());
        let output = run(
            &folder,
            &policy,
            "cat .env 2>/dev/null | wc -c; cat .git/HEAD; touch .git/new 2>/dev/null && echo wrote || echo blocked; touch ok.txt && echo fine",
            cancel,
            |_| {},
        )
        .await
        .unwrap();
        assert_eq!(
            output.text.split_whitespace().collect::<Vec<_>>(),
            ["0", "ref", "blocked", "fine"],
            "{}",
            output.text
        );
        assert!(!folder.join(".git/new").exists());
        assert_eq!(
            std::fs::read_to_string(folder.join(".env")).unwrap(),
            "SECRET=1"
        );
    }
    #[tokio::test]
    async fn project_command_is_isolated_and_cancellable() {
        let path = std::env::temp_dir().join(format!("mobius-terminal-{}", std::process::id()));
        std::fs::create_dir_all(&path).unwrap();
        let folder = path.canonicalize().unwrap();
        let (signal, cancel) = watch::channel(false);
        let output = run(
            &folder,
            &helix_task::Policy::careful(),
            "printf 'hello'; printf 'saved' > proof.txt; test ! -e /home",
            cancel.clone(),
            |_| {},
        )
        .await
        .unwrap();
        assert_eq!(output.code, Some(0));
        assert_eq!(output.text, "hello");
        assert_eq!(
            std::fs::read_to_string(folder.join("proof.txt")).unwrap(),
            "saved"
        );
        let careful = helix_task::Policy::careful();
        let (stopped, _) =
            tokio::join!(run(&folder, &careful, "sleep 20", cancel, |_| {}), async {
                tokio::time::sleep(Duration::from_millis(100)).await;
                signal.send(true).unwrap();
            });
        assert!(stopped.unwrap().stopped);
        std::fs::remove_dir_all(folder).unwrap();
    }
}
