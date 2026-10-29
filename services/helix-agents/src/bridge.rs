//! The way an agent in the box reaches the Helix desktop tools without being given the D-Bus session bus. The launcher listens on a Unix socket in a
//! folder that is mounted into the box; the agent's MCP config runs `helix-agents mcp-proxy <socket>` (this program, inside the box), which copies the
//! agent's stdin and stdout to the socket; for each connection the launcher, outside the box, starts the real MCP adapter (`tools/helix-agent/run.sh`)
//! and connects it to the socket. The adapter talks to Agent Surface as its own client, with its own grants and audit; the agent never touches the bus.

use std::io::{Read, Write};
use std::os::unix::fs::DirBuilderExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// Where the MCP adapter is, if this machine has it: `HELIX_MCP_COMMAND`, else next to the sources or in the usual install places.
pub fn adapter(
    env_command: Option<&str>,
    root: Option<&Path>,
    exists: &dyn Fn(&Path) -> bool,
) -> Option<Vec<String>> {
    if let Some(command) = env_command.filter(|c| !c.is_empty()) {
        return Some(command.split_whitespace().map(String::from).collect());
    }
    let mut places: Vec<PathBuf> = Vec::new();
    if let Some(root) = root {
        places.push(root.join("tools/helix-agent/run.sh"));
    }
    places.extend(
        [
            "/usr/share/helix/helix-agent/run.sh",
            "/usr/libexec/helix/helix-mcp",
        ]
        .map(PathBuf::from),
    );
    places
        .into_iter()
        .find(|p| exists(p))
        .map(|p| vec![p.to_string_lossy().into_owned()])
}

/// A folder only this user can enter, for the socket.
pub fn private_folder(base: &Path, name: &str) -> std::io::Result<PathBuf> {
    let dir = base.join(name);
    match std::fs::DirBuilder::new()
        .mode(0o700)
        .recursive(true)
        .create(&dir)
    {
        Ok(()) => Ok(dir),
        Err(e) => Err(e),
    }
}

/// Listens on `socket` and serves each connection with a fresh adapter process, until the process ends.
pub fn serve(socket: &Path, command: Vec<String>) -> std::io::Result<()> {
    let _ = std::fs::remove_file(socket);
    let listener = UnixListener::bind(socket)?;
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            let command = command.clone();
            std::thread::spawn(move || connect_adapter(stream, &command));
        }
    });
    Ok(())
}

fn connect_adapter(stream: UnixStream, command: &[String]) {
    let Some((program, args)) = command.split_first() else {
        return;
    };
    let mut child = match Command::new(program)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
    {
        Ok(child) => child,
        Err(error) => {
            eprintln!("helix-agents: could not start the Helix desktop tools ({program}): {error}");
            return;
        }
    };
    let (Some(mut to_adapter), Some(mut from_adapter)) = (child.stdin.take(), child.stdout.take())
    else {
        return;
    };
    let (mut read_half, mut write_half) = match (stream.try_clone(), stream) {
        (Ok(a), b) => (a, b),
        _ => return,
    };
    let up = std::thread::spawn(move || {
        // The agent closed its end: the adapter then sees end of input (`to_adapter` is dropped here).
        pump(&mut read_half, &mut to_adapter);
    });
    pump(&mut from_adapter, &mut write_half);
    let _ = write_half.shutdown(std::net::Shutdown::Both);
    let _ = child.kill();
    let _ = child.wait();
    let _ = up.join();
}

/// Copies what arrives to where it goes, as it arrives. (`io::copy` may hand a pipe to the kernel to move in large pieces and wait to fill them, which holds a
/// message back until the connection closes: not what a conversation over MCP can have.)
fn pump(from: &mut dyn Read, to: &mut dyn Write) {
    let mut buffer = [0u8; 16 * 1024];
    while let Ok(n) = from.read(&mut buffer) {
        if n == 0 || to.write_all(&buffer[..n]).is_err() || to.flush().is_err() {
            break;
        }
    }
}

/// Inside the box: copies stdin to the socket and the socket to stdout.
pub fn proxy(socket: &Path) -> i32 {
    let Ok(stream) = UnixStream::connect(socket) else {
        eprintln!(
            "helix-agents: the Helix desktop tools are not available ({})",
            socket.display()
        );
        return 1;
    };
    let Ok(mut reader) = stream.try_clone() else {
        return 1;
    };
    let mut writer = stream;
    let upstream = std::thread::spawn(move || {
        let mut stdin = std::io::stdin().lock();
        let mut buffer = [0u8; 16 * 1024];
        while let Ok(n) = stdin.read(&mut buffer) {
            if n == 0 || writer.write_all(&buffer[..n]).is_err() {
                break;
            }
        }
        let _ = writer.shutdown(std::net::Shutdown::Write);
    });
    let mut stdout = std::io::stdout().lock();
    let mut buffer = [0u8; 16 * 1024];
    while let Ok(n) = reader.read(&mut buffer) {
        if n == 0 || stdout.write_all(&buffer[..n]).is_err() || stdout.flush().is_err() {
            break;
        }
    }
    drop(upstream);
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_adapter_is_found_where_it_is() {
        let exists = |p: &Path| p == Path::new("/src/helix/tools/helix-agent/run.sh");
        assert_eq!(
            adapter(None, Some(Path::new("/src/helix")), &exists),
            Some(vec!["/src/helix/tools/helix-agent/run.sh".to_string()])
        );
        assert_eq!(
            adapter(Some("python3 /x/server.py"), None, &exists),
            Some(vec!["python3".to_string(), "/x/server.py".to_string()])
        );
        assert_eq!(adapter(None, None, &exists), None);
    }

    #[test]
    fn a_message_goes_through_the_socket_to_a_separate_process_and_back() {
        let base = std::env::temp_dir().join(format!("helix-bridge-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let dir = private_folder(&base, "mcp").unwrap();
        assert_eq!(
            std::fs::metadata(&dir).unwrap().permissions().mode_bits() & 0o777,
            0o700
        );
        let socket = dir.join("mcp.sock");
        // `cat` stands in for the adapter: it echoes what the agent sends.
        serve(&socket, vec!["/usr/bin/cat".to_string()]).unwrap();
        let mut stream = UnixStream::connect(&socket).unwrap();
        stream
            .write_all(b"{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}\n")
            .unwrap();
        stream.shutdown(std::net::Shutdown::Write).unwrap();
        let mut answer = String::new();
        stream.read_to_string(&mut answer).unwrap();
        assert!(answer.contains("\"ping\""), "{answer}");
        // Each connection is its own adapter: a second one works the same.
        let mut second = UnixStream::connect(&socket).unwrap();
        second.write_all(b"two\n").unwrap();
        second.shutdown(std::net::Shutdown::Write).unwrap();
        let mut again = String::new();
        second.read_to_string(&mut again).unwrap();
        assert_eq!(again, "two\n");
        let _ = std::fs::remove_dir_all(&base);
    }

    trait ModeBits {
        fn mode_bits(&self) -> u32;
    }
    impl ModeBits for std::fs::Permissions {
        fn mode_bits(&self) -> u32 {
            use std::os::unix::fs::PermissionsExt;
            self.mode()
        }
    }
}
