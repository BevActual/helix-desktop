//! DNS profile parsing (mirrors `helix::validatedDnsUpdate`).

use std::collections::HashMap;

use zbus::fdo;
use zbus::zvariant::{OwnedValue, Value};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DnsOverride {
    pub profile: u8,
    pub servers: Vec<String>,
    pub nextdns_id: String,
}

pub fn parse_dns_args(args: &HashMap<String, OwnedValue>) -> fdo::Result<DnsOverride> {
    let profile_value = args
        .get("profile")
        .ok_or_else(|| fdo::Error::InvalidArgs("Choose a DNS profile".into()))?;
    let profile = match &**profile_value {
        Value::U8(number) => *number,
        Value::U16(number) => u8::try_from(*number).unwrap_or(255),
        Value::U32(number) => u8::try_from(*number).unwrap_or(255),
        Value::U64(number) => u8::try_from(*number).unwrap_or(255),
        Value::I32(number) if *number >= 0 => *number as u8,
        _ => return Err(fdo::Error::InvalidArgs("Choose a DNS profile".into())),
    };
    if profile > 4 {
        return Err(fdo::Error::InvalidArgs("Unknown DNS profile".into()));
    }
    let mut servers = Vec::new();
    if let Some(value) = args.get("servers") {
        match &**value {
            Value::Array(items) => {
                for item in items.iter() {
                    match item {
                        Value::Str(text) => servers.push(text.to_string()),
                        _ => {
                            return Err(fdo::Error::InvalidArgs(
                                "DNS servers must be text addresses".into(),
                            ));
                        }
                    }
                }
            }
            _ => {
                return Err(fdo::Error::InvalidArgs(
                    "DNS servers must be a list of addresses".into(),
                ));
            }
        }
    }
    let nextdns_id = args
        .get("nextdns")
        .map(|value| match &**value {
            Value::Str(text) => Ok(text.to_string()),
            _ => Err(fdo::Error::InvalidArgs("NextDNS ID must be text".into())),
        })
        .transpose()?
        .unwrap_or_default();
    if profile == 4 && servers.is_empty() {
        return Err(fdo::Error::InvalidArgs(
            "Enter at least one DNS server address".into(),
        ));
    }
    if profile == 4 {
        for server in &servers {
            if !parse_server(server).is_some() {
                return Err(fdo::Error::InvalidArgs(
                    "Enter valid IPv4 or IPv6 DNS addresses".into(),
                ));
            }
        }
    }
    if profile == 3 && !valid_nextdns_id(&nextdns_id) {
        return Err(fdo::Error::InvalidArgs(
            "NextDNS ID may contain only letters, numbers, hyphens and underscores".into(),
        ));
    }
    Ok(DnsOverride {
        profile,
        servers,
        nextdns_id,
    })
}

pub fn valid_nextdns_id(text: &str) -> bool {
    !text.is_empty()
        && text
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || ch == '-' || ch == '_')
}

fn parse_server(token: &str) -> Option<bool> {
    let host = token.split('#').next().unwrap_or(token).trim();
    if host.is_empty() {
        return None;
    }
    if host.parse::<std::net::Ipv4Addr>().is_ok() || host.parse::<std::net::Ipv6Addr>().is_ok() {
        return Some(true);
    }
    None
}

pub fn builtin_servers(profile: u8, nextdns_id: &str) -> Vec<String> {
    match profile {
        1 => vec!["8.8.8.8".into(), "8.8.4.4".into()],
        2 => vec!["1.1.1.1".into(), "1.0.0.1".into()],
        3 if !nextdns_id.is_empty() => {
            vec![
                format!("45.90.28.0#{}.dns.nextdns.io", nextdns_id),
                format!("45.90.30.0#{}.dns.nextdns.io", nextdns_id),
            ]
        }
        _ => Vec::new(),
    }
}
