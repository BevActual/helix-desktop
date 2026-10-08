//! One question to the compositor over `$NIRI_SOCKET`, answered once (the long-lived event connection lives in `dock.rs`).

use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

use serde_json::Value;

/// `"Layers"` or any other bare request name; the reply's payload, or `None` when niri is not there or says no.
pub fn request(name: &str) -> Option<Value> {
    let path = std::env::var_os("NIRI_SOCKET")?;
    let mut stream = UnixStream::connect(path).ok()?;
    stream.set_read_timeout(Some(Duration::from_secs(1))).ok()?;
    stream.set_write_timeout(Some(Duration::from_secs(1))).ok()?;
    stream.write_all(format!("\"{name}\"\n").as_bytes()).ok()?;
    let mut line = String::new();
    BufReader::new(stream).read_line(&mut line).ok()?;
    let reply: Value = serde_json::from_str(&line).ok()?;
    reply.get("Ok").cloned()
}

/// Whether the compositor treats the layer surface with this namespace as trusted. `None`: it is not on screen (yet).
pub fn layer_is_trusted(namespace: &str) -> Option<bool> {
    let reply = request("Layers")?;
    let layers = reply.get("Layers")?.as_array()?;
    layers
        .iter()
        .find(|layer| layer.get("namespace").and_then(Value::as_str) == Some(namespace))
        .map(|layer| layer.get("trusted").and_then(Value::as_bool).unwrap_or(false))
}
