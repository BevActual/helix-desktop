//! Resolve the D-Bus caller to a grant key (exe path + cmdline).

use helix_shell_core::client_identity;
use zbus::Connection;

#[derive(Clone, Debug)]
pub struct Peer {
    pub client: String,
    #[allow(dead_code)]
    pub exe: String,
    pub unique_name: String,
}

pub async fn resolve_peer(connection: &Connection, sender: &str) -> zbus::fdo::Result<Peer> {
    let dbus = zbus::fdo::DBusProxy::new(connection).await?;
    let bus_name = zbus::names::BusName::try_from(sender)
        .map_err(|_| zbus::fdo::Error::InvalidArgs("bad sender name".into()))?;
    let creds = dbus.get_connection_credentials(bus_name).await?;
    let pid = creds
        .process_id()
        .ok_or_else(|| zbus::fdo::Error::Failed("the bus did not report the caller's process".into()))?;
    let uid = creds
        .unix_user_id()
        .ok_or_else(|| zbus::fdo::Error::Failed("the bus did not report the caller's user".into()))?;
    if uid != unsafe { libc::geteuid() } {
        return Err(zbus::fdo::Error::AccessDenied(
            "callers must run as the same user".into(),
        ));
    }
    let exe_link = format!("/proc/{pid}/exe");
    let exe = std::fs::read_link(&exe_link)
        .map_err(|_| zbus::fdo::Error::Failed("could not read the caller's executable".into()))?
        .display()
        .to_string();
    let cmdline = read_cmdline(pid);
    Ok(Peer {
        client: client_identity(&exe, &cmdline),
        exe,
        unique_name: sender.to_string(),
    })
}

fn read_cmdline(pid: u32) -> Vec<String> {
    let Ok(bytes) = std::fs::read(format!("/proc/{pid}/cmdline")) else {
        return Vec::new();
    };
    bytes
        .split(|byte| *byte == 0)
        .filter(|part| !part.is_empty())
        .map(|part| String::from_utf8_lossy(part).into_owned())
        .collect()
}
