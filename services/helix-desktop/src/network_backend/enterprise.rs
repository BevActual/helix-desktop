//! 802.1X credential validation and NM setting layout (mirrors C++ `network_enterprise`).

use std::collections::HashMap;

use zbus::zvariant::Value as ZValue;

#[derive(Clone, Debug)]
pub struct EnterpriseCredentials {
    pub identity: String,
    pub anonymous_identity: String,
    pub password: String,
    pub ca_cert_path: String,
    pub domain_suffix_match: String,
    pub eap: u8,
    pub phase2: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Validation {
    Ok,
    MissingIdentity,
    MissingPassword,
    UnsupportedPhase2,
    CaCertNotAbsolute,
    UnsupportedAp,
}

pub fn validate(credentials: &EnterpriseCredentials) -> Validation {
    if credentials.identity.is_empty() {
        return Validation::MissingIdentity;
    }
    if credentials.password.is_empty() {
        return Validation::MissingPassword;
    }
    if credentials.eap == 0 && credentials.phase2 != 0 {
        return Validation::UnsupportedPhase2;
    }
    if !credentials.ca_cert_path.is_empty() && !credentials.ca_cert_path.starts_with('/') {
        return Validation::CaCertNotAbsolute;
    }
    Validation::Ok
}

pub fn password_auth_usable(key_management: u32) -> bool {
    key_management == 2
}

fn eap_name(eap: u8) -> &'static str {
    match eap {
        1 => "ttls",
        _ => "peap",
    }
}

fn phase2_name(phase2: u8) -> &'static str {
    match phase2 {
        1 => "pap",
        2 => "mschap",
        3 => "chap",
        _ => "mschapv2",
    }
}

fn ca_cert_uri_bytes(path: &str) -> Vec<u8> {
    let mut bytes = b"file://".to_vec();
    bytes.extend_from_slice(path.as_bytes());
    bytes.push(0);
    bytes
}

pub fn eap_settings(
    credentials: &EnterpriseCredentials,
) -> HashMap<&'static str, ZValue<'_>> {
    let mut settings = HashMap::new();
    settings.insert("eap", ZValue::from(vec![eap_name(credentials.eap)]));
    settings.insert("identity", ZValue::from(credentials.identity.as_str()));
    if !credentials.anonymous_identity.is_empty() {
        settings.insert(
            "anonymous-identity",
            ZValue::from(credentials.anonymous_identity.as_str()),
        );
    }
    settings.insert("phase2-auth", ZValue::from(phase2_name(credentials.phase2)));
    settings.insert("password", ZValue::from(credentials.password.as_str()));
    if !credentials.domain_suffix_match.is_empty() {
        settings.insert(
            "domain-suffix-match",
            ZValue::from(credentials.domain_suffix_match.as_str()),
        );
    }
    if credentials.ca_cert_path.is_empty() {
        settings.insert("system-ca-certs", ZValue::Bool(true));
    } else {
        settings.insert("ca-cert", ZValue::from(ca_cert_uri_bytes(&credentials.ca_cert_path)));
    }
    settings
}
