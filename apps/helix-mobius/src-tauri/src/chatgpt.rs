//! Using a ChatGPT plan in Mobius, through OpenAI's "Sign in with ChatGPT".
//!
//! The person signs in on OpenAI's own page in their browser; Mobius never sees a password. OpenAI then issues Mobius tokens that let it send
//! requests which count against the person's ChatGPT plan instead of an API key. This is the route OpenAI offers open-source apps that run on
//! a user's own computer (Helix is public and MIT-licensed), written here from OpenAI's published protocol and not from its example code, whose
//! licence is noncommercial.
//!
//! What it does and does not do:
//! * Only requests for the signed-in person, made from this computer. Nothing is proxied or shared.
//! * It does not give access to the person's ChatGPT conversations.
//! * The tokens are kept in the Secret Service (or a private file where there is none), never in a settings file or a transcript.
//! * Nothing here reads another program's stored login.

use std::path::PathBuf;
use std::time::Duration;

use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

pub const BACKEND_ID: &str = "chatgpt";
pub const USAGE_URL: &str = "https://chatgpt.com/settings/usage";
const ISSUER: &str = "https://auth.openai.com";
const API: &str = "https://api.openai.com/v1";
/// Identity, a refresh token, and permission to use the plan (`chatgpt.tokens.use.direct`) with OpenAI's API.
const SCOPES: &str =
    "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct";
const SHARING_SCOPE: &str = "chatgpt.tokens.use.direct";
const SECRET_REF: &str = "chatgpt-plan";
const APP_NAME: &str = "Helix Mobius";
const CALLBACK_PATH: &str = "/callback";
const DYNAMIC_CLIENT: &str = "dynamic_agent_client";

/// Tests point the sign-in and the API at a pretend server on this computer. Only a loopback address is accepted, so the setting cannot send
/// a real sign-in anywhere else.
fn endpoint(variable: &str, real: &str) -> String {
    match std::env::var(variable) {
        Ok(value) if value.starts_with("http://127.0.0.1:") => {
            value.trim_end_matches('/').to_string()
        }
        _ => real.to_string(),
    }
}

fn issuer() -> String {
    endpoint("HELIX_CHATGPT_ISSUER", ISSUER)
}

fn api() -> String {
    endpoint("HELIX_CHATGPT_API", API)
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

// ---- small pure pieces -------------------------------------------------------------------------------------------------------------

fn random_bytes(count: usize) -> Result<Vec<u8>, String> {
    use std::io::Read;
    let mut bytes = vec![0u8; count];
    std::fs::File::open("/dev/urandom")
        .and_then(|mut f| f.read_exact(&mut bytes))
        .map_err(|_| "Could not get random numbers for the sign-in.".to_string())?;
    Ok(bytes)
}

/// Random text for a PKCE verifier, a state or a nonce: URL-safe base64 of `count` random bytes.
fn random_token(count: usize) -> Result<String, String> {
    Ok(URL_SAFE_NO_PAD.encode(random_bytes(count)?))
}

/// The PKCE challenge for a verifier (RFC 7636, method S256).
pub fn code_challenge(verifier: &str) -> String {
    URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()))
}

/// A random identifier for this installation, `urn:uuid:` and a version 4 UUID. It is not a credential.
pub fn new_host_id() -> Result<String, String> {
    let mut b = random_bytes(16)?;
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    let hex: String = b.iter().map(|x| format!("{x:02x}")).collect();
    Ok(format!(
        "urn:uuid:{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    ))
}

fn constant_time_eq(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// The address the person's browser is sent to. The first sign-in registers this app (`dynamic_agent_client` plus a name); later ones use
/// the client id OpenAI issued.
#[allow(clippy::too_many_arguments)]
pub fn authorization_url(
    authorization_endpoint: &str,
    saved_client: Option<&str>,
    redirect_uri: &str,
    state: &str,
    nonce: &str,
    challenge: &str,
    host_id: &str,
    login_hint: Option<&str>,
) -> Result<String, String> {
    let mut url = url::Url::parse(authorization_endpoint)
        .map_err(|_| "OpenAI's sign-in address could not be read.".to_string())?;
    {
        let mut query = url.query_pairs_mut();
        query
            .append_pair("response_type", "code")
            .append_pair("client_id", saved_client.unwrap_or(DYNAMIC_CLIENT))
            .append_pair("redirect_uri", redirect_uri)
            .append_pair("scope", SCOPES)
            .append_pair("resource", API)
            .append_pair("state", state)
            .append_pair("nonce", nonce)
            .append_pair("code_challenge_method", "S256")
            .append_pair("code_challenge", challenge)
            .append_pair("ext_agent_host_id", host_id);
        if saved_client.is_none() {
            query.append_pair("agent_name_hint", APP_NAME);
        }
        if let Some(hint) = login_hint.filter(|h| !h.is_empty()) {
            query.append_pair("login_hint", hint);
        }
    }
    Ok(url.to_string())
}

#[derive(Debug, PartialEq, Eq)]
pub enum Callback {
    /// Not the sign-in's own request (wrong path, method or state): answered with an error and the wait goes on.
    Ignore(u16),
    /// OpenAI or the person ended the sign-in.
    Failed(String),
    Done {
        code: String,
        client_id: String,
    },
}

fn client_id_ok(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 200
        && id != DYNAMIC_CLIENT
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

/// Reads the browser's request to the local callback. Only a `GET` of the callback path with this sign-in's own `state` is believed; anything
/// else on that port (another program, a stray page) cannot end or steer the sign-in.
pub fn parse_callback(
    request: &str,
    port: u16,
    expected_state: &str,
    saved_client: Option<&str>,
) -> Callback {
    let mut lines = request.lines();
    let Some(first) = lines.next() else {
        return Callback::Ignore(400);
    };
    let mut parts = first.split_whitespace();
    let (Some(method), Some(target)) = (parts.next(), parts.next()) else {
        return Callback::Ignore(400);
    };
    let host_ok = lines.take_while(|l| !l.is_empty()).any(|l| {
        l.to_ascii_lowercase()
            .strip_prefix("host:")
            .is_some_and(|h| h.trim() == format!("127.0.0.1:{port}"))
    });
    let Ok(url) = url::Url::parse(&format!("http://127.0.0.1:{port}{target}")) else {
        return Callback::Ignore(400);
    };
    if method != "GET" || !host_ok || url.path() != CALLBACK_PATH {
        return Callback::Ignore(404);
    }
    let all = |name: &str| -> Vec<String> {
        url.query_pairs()
            .filter(|(k, _)| k == name)
            .map(|(_, v)| v.into_owned())
            .collect()
    };
    let states = all("state");
    if states.len() != 1 || !constant_time_eq(&states[0], expected_state) {
        return Callback::Ignore(400);
    }
    if let Some(error) = all("error").first() {
        return Callback::Failed(match error.as_str() {
            "access_denied" => "Sign-in was cancelled.".to_string(),
            _ => "ChatGPT did not complete the sign-in. Please try again.".to_string(),
        });
    }
    let (codes, clients) = (all("code"), all("client_id"));
    let client = clients
        .first()
        .cloned()
        .or_else(|| saved_client.map(str::to_string));
    let ok = codes.len() == 1
        && clients.len() <= 1
        && client.as_deref().is_some_and(client_id_ok)
        && !(saved_client.is_some()
            && clients
                .first()
                .is_some_and(|c| Some(c.as_str()) != saved_client));
    match (ok, client) {
        (true, Some(client_id)) => Callback::Done {
            code: codes[0].clone(),
            client_id,
        },
        _ => Callback::Failed(
            "ChatGPT did not complete app registration. Please try signing in again.".into(),
        ),
    }
}

// ---- the ID token ------------------------------------------------------------------------------------------------------------------

fn der_length(len: usize) -> Vec<u8> {
    if len < 0x80 {
        vec![len as u8]
    } else if len < 0x100 {
        vec![0x81, len as u8]
    } else {
        vec![0x82, (len >> 8) as u8, len as u8]
    }
}

fn der_integer(bytes: &[u8]) -> Vec<u8> {
    let trimmed: &[u8] = match bytes.iter().position(|b| *b != 0) {
        Some(i) => &bytes[i..],
        None => &[0],
    };
    let mut body = Vec::new();
    if trimmed[0] & 0x80 != 0 {
        body.push(0);
    }
    body.extend_from_slice(trimmed);
    let mut out = vec![0x02];
    out.extend(der_length(body.len()));
    out.extend(body);
    out
}

/// An RSA public key (modulus and exponent) in the DER form `ring` reads.
fn rsa_public_key_der(modulus: &[u8], exponent: &[u8]) -> Vec<u8> {
    let mut body = der_integer(modulus);
    body.extend(der_integer(exponent));
    let mut out = vec![0x30];
    out.extend(der_length(body.len()));
    out.extend(body);
    out
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Identity {
    pub subject: String,
    pub email: String,
    pub name: String,
}

/// Checks the identity OpenAI returned: the signature (RS256, against OpenAI's published keys), who issued it, that it is for this client,
/// that it answers this sign-in's nonce, and that it has not expired. Returns who signed in.
pub fn verify_id_token(
    token: &str,
    jwks: &Value,
    issuer: &str,
    audience: &str,
    nonce: &str,
    at: u64,
) -> Result<Identity, String> {
    let bad = || "The ChatGPT identity could not be verified. Please sign in again.".to_string();
    let mut parts = token.split('.');
    let (Some(header), Some(payload), Some(signature), None) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return Err(bad());
    };
    let decode = |text: &str| URL_SAFE_NO_PAD.decode(text).map_err(|_| bad());
    let header: Value = serde_json::from_slice(&decode(header)?).map_err(|_| bad())?;
    if header.get("alg").and_then(Value::as_str) != Some("RS256") {
        return Err(bad());
    }
    let kid = header.get("kid").and_then(Value::as_str).ok_or_else(bad)?;
    let key = jwks
        .get("keys")
        .and_then(Value::as_array)
        .and_then(|keys| {
            keys.iter().find(|k| {
                k.get("kid").and_then(Value::as_str) == Some(kid)
                    && k.get("kty").and_then(Value::as_str) == Some("RSA")
            })
        })
        .ok_or_else(bad)?;
    let n = decode(key.get("n").and_then(Value::as_str).ok_or_else(bad)?)?;
    let e = decode(key.get("e").and_then(Value::as_str).ok_or_else(bad)?)?;
    let public = ring::signature::UnparsedPublicKey::new(
        &ring::signature::RSA_PKCS1_2048_8192_SHA256,
        rsa_public_key_der(&n, &e),
    );
    let signed = token
        .rsplit_once('.')
        .map(|(head, _)| head)
        .ok_or_else(bad)?;
    public
        .verify(signed.as_bytes(), &decode(signature)?)
        .map_err(|_| bad())?;

    let claims: Value = serde_json::from_slice(&decode(payload)?).map_err(|_| bad())?;
    let text = |name: &str| claims.get(name).and_then(Value::as_str);
    if text("iss") != Some(issuer) {
        return Err(bad());
    }
    let audiences: Vec<&str> = match claims.get("aud") {
        Some(Value::String(a)) => vec![a.as_str()],
        Some(Value::Array(list)) => list.iter().filter_map(Value::as_str).collect(),
        _ => Vec::new(),
    };
    if !audiences.contains(&audience)
        || (audiences.len() > 1 && text("azp") != Some(audience))
        || text("azp").is_some_and(|a| a != audience)
    {
        return Err(bad());
    }
    let exp = claims.get("exp").and_then(Value::as_u64).ok_or_else(bad)?;
    if exp + 5 < at || claims.get("iat").and_then(Value::as_u64).is_none() {
        return Err(bad());
    }
    let subject = text("sub")
        .filter(|s| !s.is_empty())
        .ok_or_else(bad)?
        .to_string();
    if text("nonce") != Some(nonce) {
        return Err(bad());
    }
    Ok(Identity {
        subject,
        email: text("email").unwrap_or("").to_string(),
        name: text("name").unwrap_or("").to_string(),
    })
}

// ---- what is kept ------------------------------------------------------------------------------------------------------------------

/// Who is connected. Not secret; the tokens are kept apart, in the Secret Service.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct Profile {
    pub client_id: String,
    pub subject: String,
    pub email: String,
    pub name: String,
    pub scopes: Vec<String>,
    pub connected_at: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
struct Credentials {
    access_token: String,
    refresh_token: String,
    /// Unix seconds.
    expires_at: u64,
}

fn data_dir() -> PathBuf {
    std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
        .unwrap_or_else(|| PathBuf::from("."))
        .join("helix/mobius")
}

fn profile_path() -> PathBuf {
    data_dir().join("chatgpt-profile.json")
}

fn host_path() -> PathBuf {
    data_dir().join("chatgpt-host.json")
}

fn write_private(path: &PathBuf, text: &str) -> Result<(), String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temporary = path.with_extension("tmp");
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&temporary)
        .map_err(|e| e.to_string())?;
    file.write_all(text.as_bytes()).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, path).map_err(|e| e.to_string())
}

pub fn profile() -> Option<Profile> {
    std::fs::read_to_string(profile_path())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
}

fn save_profile(profile: &Profile) -> Result<(), String> {
    write_private(
        &profile_path(),
        &serde_json::to_string(profile).map_err(|e| e.to_string())?,
    )
}

fn host_id() -> Result<String, String> {
    if let Some(id) = std::fs::read_to_string(host_path())
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .and_then(|v| v.get("host_id").and_then(Value::as_str).map(str::to_string))
        .filter(|id| id.starts_with("urn:uuid:"))
    {
        return Ok(id);
    }
    let id = new_host_id()?;
    write_private(&host_path(), &json!({"host_id": id}).to_string())?;
    Ok(id)
}

fn load_credentials() -> Option<Credentials> {
    crate::secrets::get(SECRET_REF).and_then(|t| serde_json::from_str(&t).ok())
}

fn save_credentials(credentials: &Credentials) -> Result<(), String> {
    crate::secrets::set(
        SECRET_REF,
        &serde_json::to_string(credentials).map_err(|e| e.to_string())?,
    )
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Status {
    pub connected: bool,
    pub email: String,
    pub name: String,
    pub usage_url: &'static str,
}

pub fn status() -> Status {
    let connected = profile().is_some_and(|p| p.scopes.iter().any(|s| s == SHARING_SCOPE))
        && load_credentials().is_some();
    let profile = profile().unwrap_or_default();
    Status {
        connected,
        email: profile.email,
        name: profile.name,
        usage_url: USAGE_URL,
    }
}

// ---- talking to OpenAI -------------------------------------------------------------------------------------------------------------

fn http() -> Result<reqwest::Client, String> {
    reqwest::Client::builder()
        .timeout(Duration::from_secs(30))
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .map_err(|e| e.to_string())
}

struct Discovery {
    authorization_endpoint: String,
    token_endpoint: String,
    jwks_uri: String,
    revocation_endpoint: Option<String>,
}

fn same_origin(a: &str, b: &str) -> bool {
    match (url::Url::parse(a), url::Url::parse(b)) {
        (Ok(a), Ok(b)) => a.origin() == b.origin(),
        _ => false,
    }
}

async fn discover(client: &reqwest::Client) -> Result<Discovery, String> {
    let issuer = issuer();
    let reply = client
        .get(format!("{issuer}/.well-known/openid-configuration"))
        .send()
        .await
        .map_err(|_| "Could not reach OpenAI to sign in.".to_string())?;
    let data: Value = reply
        .json()
        .await
        .map_err(|_| "OpenAI's sign-in settings could not be read.".to_string())?;
    if data.get("issuer").and_then(Value::as_str) != Some(issuer.as_str()) {
        return Err(
            "OpenAI's sign-in settings were not what was expected, so the sign-in was stopped."
                .into(),
        );
    }
    // Every address must belong to the issuer, so a tampered answer cannot send the person or a code elsewhere.
    let field = |name: &str| -> Result<String, String> {
        let value = data
            .get(name)
            .and_then(Value::as_str)
            .ok_or_else(|| "OpenAI's sign-in settings were incomplete.".to_string())?;
        if same_origin(value, &issuer) {
            Ok(value.to_string())
        } else {
            Err("OpenAI's sign-in settings pointed somewhere unexpected, so the sign-in was stopped.".into())
        }
    };
    let revocation_endpoint = data
        .get("revocation_endpoint")
        .and_then(Value::as_str)
        .filter(|v| same_origin(v, &issuer))
        .map(str::to_string);
    Ok(Discovery {
        authorization_endpoint: field("authorization_endpoint")?,
        token_endpoint: field("token_endpoint")?,
        jwks_uri: field("jwks_uri")?,
        revocation_endpoint,
    })
}

struct Tokens {
    access_token: String,
    refresh_token: Option<String>,
    expires_in: u64,
    scopes: Vec<String>,
    id_token: Option<String>,
}

fn parse_tokens(data: &Value, previous_scopes: Option<&[String]>) -> Result<Tokens, String> {
    let access_token = data
        .get("access_token")
        .and_then(Value::as_str)
        .filter(|t| !t.is_empty())
        .ok_or("OpenAI did not return a token.")?
        .to_string();
    let scopes = match data.get("scope").and_then(Value::as_str) {
        Some(s) => s.split_whitespace().map(str::to_string).collect(),
        None => previous_scopes
            .map(<[String]>::to_vec)
            .ok_or("OpenAI did not say what the sign-in allows.")?,
    };
    Ok(Tokens {
        access_token,
        refresh_token: data
            .get("refresh_token")
            .and_then(Value::as_str)
            .filter(|t| !t.is_empty())
            .map(str::to_string),
        expires_in: data
            .get("expires_in")
            .and_then(Value::as_u64)
            .unwrap_or(3600)
            .clamp(60, 86_400),
        scopes,
        id_token: data
            .get("id_token")
            .and_then(Value::as_str)
            .map(str::to_string),
    })
}

async fn token_request(
    client: &reqwest::Client,
    endpoint: &str,
    form: &[(&str, &str)],
    previous_scopes: Option<&[String]>,
) -> Result<Tokens, String> {
    let reply = client
        .post(endpoint)
        .header("accept", "application/json")
        .form(form)
        .send()
        .await
        .map_err(|_| "Could not reach OpenAI to sign in.".to_string())?;
    let status = reply.status();
    let data: Value = reply.json().await.unwrap_or(Value::Null);
    if !status.is_success() {
        let error = data.get("error").and_then(Value::as_str).unwrap_or("");
        return Err(if error == "invalid_grant" {
            "The ChatGPT sign-in is no longer valid. Continue with ChatGPT again in Settings → Providers.".to_string()
        } else {
            format!("ChatGPT sign-in failed ({}).", status.as_u16())
        });
    }
    parse_tokens(&data, previous_scopes)
}

static CANCEL: tokio::sync::Notify = tokio::sync::Notify::const_new();

pub fn cancel_sign_in() {
    CANCEL.notify_waiters();
}

async fn wait_for_callback(
    listener: tokio::net::TcpListener,
    port: u16,
    state: &str,
    saved_client: Option<&str>,
) -> Result<(String, String), String> {
    let waiting = async {
        loop {
            let (mut stream, _) = listener
                .accept()
                .await
                .map_err(|_| "The local sign-in listener stopped. Please try again.".to_string())?;
            let mut buffer = vec![0u8; 8192];
            let read = match tokio::time::timeout(Duration::from_secs(10), stream.read(&mut buffer))
                .await
            {
                Ok(Ok(n)) => n,
                _ => continue,
            };
            let request = String::from_utf8_lossy(&buffer[..read]).to_string();
            let (status, body, outcome) = match parse_callback(&request, port, state, saved_client)
            {
                Callback::Ignore(code) => (code, "Not found".to_string(), None),
                Callback::Failed(message) => {
                    (200, "Return to Mobius.".to_string(), Some(Err(message)))
                }
                Callback::Done { code, client_id } => (
                    200,
                    "You are signed in. You can close this tab and return to Mobius.".to_string(),
                    Some(Ok((code, client_id))),
                ),
            };
            let page = format!("<!doctype html><meta charset=\"utf-8\"><title>Return to Mobius</title><style>body{{font:17px system-ui;max-width:32rem;margin:18vh auto;padding:24px}}</style><p>{body}</p>");
            let reply = format!(
                "HTTP/1.1 {status} OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{page}",
                page.len()
            );
            let _ = stream.write_all(reply.as_bytes()).await;
            let _ = stream.shutdown().await;
            if let Some(result) = outcome {
                return result;
            }
        }
    };
    tokio::select! {
        result = waiting => result,
        _ = CANCEL.notified() => Err("Sign-in was cancelled.".to_string()),
        _ = tokio::time::sleep(Duration::from_secs(300)) => Err("Sign-in took too long. Try again when you are ready.".to_string()),
    }
}

/// The whole sign-in: the person's browser is sent to OpenAI (`open` does that), the browser comes back to a one-time local address with a
/// code, the code is exchanged for tokens, and the identity is checked before anything is kept.
pub async fn sign_in(open: impl FnOnce(&str) -> Result<(), String>) -> Result<Profile, String> {
    let client = http()?;
    let discovery = discover(&client).await?;
    let previous = profile();
    let saved_client = previous
        .as_ref()
        .map(|p| p.client_id.clone())
        .filter(|c| client_id_ok(c));
    let (verifier, state, nonce) = (random_token(64)?, random_token(32)?, random_token(32)?);
    let host = host_id()?;
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .map_err(|_| "The local sign-in port is unavailable.".to_string())?;
    let port = listener.local_addr().map_err(|e| e.to_string())?.port();
    let redirect = format!("http://127.0.0.1:{port}{CALLBACK_PATH}");
    let login_hint = previous.as_ref().map(|p| p.email.clone());
    let url = authorization_url(
        &discovery.authorization_endpoint,
        saved_client.as_deref(),
        &redirect,
        &state,
        &nonce,
        &code_challenge(&verifier),
        &host,
        login_hint.as_deref(),
    )?;
    open(&url)?;
    let (code, client_id) =
        wait_for_callback(listener, port, &state, saved_client.as_deref()).await?;
    let tokens = token_request(
        &client,
        &discovery.token_endpoint,
        &[
            ("grant_type", "authorization_code"),
            ("client_id", &client_id),
            ("code", &code),
            ("code_verifier", &verifier),
            ("redirect_uri", &redirect),
            ("resource", API),
        ],
        None,
    )
    .await?;
    let id_token = tokens
        .id_token
        .as_deref()
        .ok_or("ChatGPT did not return a verifiable identity. Please try signing in again.")?;
    let jwks: Value = client
        .get(&discovery.jwks_uri)
        .send()
        .await
        .map_err(|_| "Could not reach OpenAI to check the sign-in.".to_string())?
        .json()
        .await
        .map_err(|_| "OpenAI's signing keys could not be read.".to_string())?;
    let identity = verify_id_token(id_token, &jwks, &issuer(), &client_id, &nonce, now())?;
    if previous
        .as_ref()
        .is_some_and(|p| !p.subject.is_empty() && p.subject != identity.subject)
    {
        return Err("This sign-in was for a different ChatGPT account than the one connected before. Disconnect first to switch.".into());
    }
    if !tokens.scopes.iter().any(|s| s == SHARING_SCOPE) {
        return Err("You are signed in, but ChatGPT plan usage is not turned on for this app. Turn it on in your ChatGPT settings, then try again.".into());
    }
    let refresh_token = tokens
        .refresh_token
        .clone()
        .ok_or("ChatGPT did not allow staying signed in. Please try again.")?;
    save_credentials(&Credentials {
        access_token: tokens.access_token,
        refresh_token,
        expires_at: now() + tokens.expires_in,
    })?;
    let profile = Profile {
        client_id,
        subject: identity.subject,
        email: identity.email,
        name: identity.name,
        scopes: tokens.scopes,
        connected_at: now(),
    };
    save_profile(&profile)?;
    Ok(profile)
}

/// A token that is good for at least another minute, refreshed first when it is not.
pub async fn access_token() -> Result<String, String> {
    let profile = profile()
        .ok_or("Not signed in with ChatGPT. Continue with ChatGPT in Settings → Providers.")?;
    let credentials = load_credentials()
        .ok_or("Not signed in with ChatGPT. Continue with ChatGPT in Settings → Providers.")?;
    if credentials.expires_at > now() + 60 {
        return Ok(credentials.access_token);
    }
    let client = http()?;
    let discovery = discover(&client).await?;
    let refreshed = token_request(
        &client,
        &discovery.token_endpoint,
        &[
            ("grant_type", "refresh_token"),
            ("client_id", &profile.client_id),
            ("refresh_token", &credentials.refresh_token),
            ("resource", API),
        ],
        Some(&profile.scopes),
    )
    .await;
    match refreshed {
        Ok(tokens) => {
            save_credentials(&Credentials {
                access_token: tokens.access_token.clone(),
                refresh_token: tokens.refresh_token.unwrap_or(credentials.refresh_token),
                expires_at: now() + tokens.expires_in,
            })?;
            Ok(tokens.access_token)
        }
        Err(message) => {
            if message.contains("no longer valid") {
                let _ = crate::secrets::clear(SECRET_REF);
            }
            Err(message)
        }
    }
}

/// Signs out: asks OpenAI to end the sign-in (best effort) and removes everything kept here.
pub async fn disconnect() -> Result<(), String> {
    if let (Some(profile), Some(credentials)) = (profile(), load_credentials()) {
        if let Ok(client) = http() {
            if let Ok(discovery) = discover(&client).await {
                if let Some(endpoint) = discovery.revocation_endpoint {
                    let _ = client
                        .post(endpoint)
                        .form(&[
                            ("token", credentials.refresh_token.as_str()),
                            ("token_type_hint", "refresh_token"),
                            ("client_id", profile.client_id.as_str()),
                        ])
                        .send()
                        .await;
                }
            }
        }
    }
    crate::secrets::clear(SECRET_REF)?;
    let _ = std::fs::remove_file(profile_path());
    let _ = std::fs::remove_file(models_path());
    Ok(())
}

// ---- requests ----------------------------------------------------------------------------------------------------------------------

/// A model that can chat, from the list OpenAI returns (which also holds embeddings, speech, images and moderation).
pub fn chat_model(id: &str) -> bool {
    let lower = id.to_ascii_lowercase();
    let family = lower.starts_with("gpt-")
        || lower.starts_with("chatgpt-")
        || (lower.starts_with('o') && lower.chars().nth(1).is_some_and(|c| c.is_ascii_digit()))
        || lower.starts_with("codex");
    family
        && ![
            "embedding",
            "tts",
            "whisper",
            "transcribe",
            "image",
            "audio",
            "realtime",
            "search-preview",
            "moderation",
            "instruct",
            "davinci",
            "babbage",
        ]
        .iter()
        .any(|w| lower.contains(w))
}

fn models_path() -> PathBuf {
    data_dir().join("chatgpt-models.json")
}

/// The models last fetched, so menus can list them without a request.
pub fn cached_models() -> Vec<String> {
    std::fs::read_to_string(models_path())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

pub async fn models() -> Result<Vec<String>, String> {
    let ids = fetch_models().await?;
    let _ = write_private(
        &models_path(),
        &serde_json::to_string(&ids).unwrap_or_default(),
    );
    Ok(ids)
}

async fn fetch_models() -> Result<Vec<String>, String> {
    let token = access_token().await?;
    let reply = http()?
        .get(format!("{}/models", api()))
        .bearer_auth(token)
        .header("accept", "application/json")
        .send()
        .await
        .map_err(|_| "Could not reach OpenAI.".to_string())?;
    if !reply.status().is_success() {
        return Err(explain_failure(
            reply.status().as_u16(),
            &reply.text().await.unwrap_or_default(),
        ));
    }
    let data: Value = reply
        .json()
        .await
        .map_err(|_| "OpenAI's model list could not be read.".to_string())?;
    let mut ids: Vec<String> = data
        .get("data")
        .and_then(Value::as_array)
        .map(|list| {
            list.iter()
                .filter_map(|m| m.get("id").and_then(Value::as_str))
                .filter(|id| chat_model(id))
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default();
    ids.sort_by(|a, b| b.cmp(a));
    ids.dedup();
    Ok(ids)
}

/// The request for the Responses API. System messages become `instructions`; nothing is stored on OpenAI's side (`store: false`).
pub fn build_request(model: &str, messages: &[(&str, String)], max_tokens: u32) -> Value {
    let system: Vec<&str> = messages
        .iter()
        .filter(|(role, _)| *role == "system")
        .map(|(_, text)| text.as_str())
        .collect();
    let input: Vec<Value> = messages
        .iter()
        .filter(|(role, _)| *role != "system")
        .map(|(role, text)| json!({"role": role, "content": text}))
        .collect();
    let mut body = json!({"model": model, "input": input, "max_output_tokens": max_tokens.max(64), "stream": false, "store": false});
    if !system.is_empty() {
        body["instructions"] = json!(system.join("\n\n"));
    }
    body
}

/// The reply text, the model that answered and how many tokens it wrote. The plan pays, so there is no cost to report.
pub fn parse_response(value: &Value, asked: &str) -> Result<crate::lan::Reply, String> {
    if let Some(message) = value
        .get("error")
        .and_then(|e| e.get("message"))
        .and_then(Value::as_str)
    {
        return Err(format!("ChatGPT said: {message}"));
    }
    let mut text = String::new();
    for item in value
        .get("output")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        if item.get("type").and_then(Value::as_str) != Some("message") {
            continue;
        }
        for part in item
            .get("content")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            if part.get("type").and_then(Value::as_str) == Some("output_text") {
                text.push_str(part.get("text").and_then(Value::as_str).unwrap_or(""));
            }
        }
    }
    let text = text.trim().to_string();
    if text.is_empty() {
        return Err("The model returned nothing.".into());
    }
    Ok(crate::lan::Reply {
        text,
        model: value
            .get("model")
            .and_then(Value::as_str)
            .filter(|m| !m.is_empty())
            .unwrap_or(asked)
            .to_string(),
        cost_micro: None,
        completion_tokens: value
            .get("usage")
            .and_then(|u| u.get("output_tokens"))
            .and_then(Value::as_u64)
            .map(|t| t.min(u64::from(u32::MAX)) as u32),
    })
}

/// What a refusal means to the person, in their words.
pub fn explain_failure(status: u16, detail: &str) -> String {
    let lower = detail.to_lowercase();
    match status {
        401 => "ChatGPT did not accept the sign-in. Continue with ChatGPT again in Settings → Providers.".into(),
        403 if lower.contains("sharing") || lower.contains("scope") || lower.contains("permission") => "ChatGPT plan usage is not turned on for this app. Turn it on in your ChatGPT settings.".into(),
        402 | 429 => "Your ChatGPT plan's usage limit was reached. Open Settings → Providers → ChatGPT plan and choose Manage usage.".into(),
        404 => "ChatGPT does not offer that model to your plan. Choose another in Settings → Models.".into(),
        code => {
            let short: String = detail.chars().take(160).collect();
            format!("ChatGPT returned {code}: {short}")
        }
    }
}

/// Sends a conversation to a model with the person's ChatGPT plan. A token that has just expired is refreshed once.
pub async fn respond(
    model: &str,
    messages: &[(&str, String)],
    max_tokens: u32,
    timeout_secs: u64,
) -> Result<crate::lan::Reply, String> {
    let body = build_request(model, messages, max_tokens);
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(timeout_secs.clamp(15, 600)))
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .map_err(|e| e.to_string())?;
    let mut refreshed = false;
    loop {
        let token = access_token().await?;
        let reply = client
            .post(format!("{}/responses", api()))
            .bearer_auth(token)
            .json(&body)
            .send()
            .await
            .map_err(|_| "Could not reach OpenAI.".to_string())?;
        let status = reply.status().as_u16();
        if status == 401 && !refreshed {
            // The token was refused though it looked fresh: expire it once and ask for a new one.
            refreshed = true;
            if let Some(mut credentials) = load_credentials() {
                credentials.expires_at = 0;
                let _ = save_credentials(&credentials);
            }
            continue;
        }
        if !reply.status().is_success() {
            return Err(explain_failure(
                status,
                &reply.text().await.unwrap_or_default(),
            ));
        }
        let value: Value = reply
            .json()
            .await
            .map_err(|_| "OpenAI's reply could not be read.".to_string())?;
        return parse_response(&value, model);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::engine::general_purpose::STANDARD;
    use ring::signature::{RsaKeyPair, RSA_PKCS1_SHA256};

    // A throwaway key made only for these tests; it signs nothing real.
    const TEST_KEY: &str = "MIIEowIBAAKCAQEAlgaC6tWBluuxwKEk1aXBHwBc0nqL3kfLPWSjq7ZxRlzbcS+XL7NXFMkf793YLOv3/VoSI41bUgIE6v9V9YvWRGGftvbell3z0r0ZFbv02BtElvzQHQif8PqCW3GasHhVj+mMUih9HcwCqVvDskFDPGQPfZ048xrpCXsiqF7hbZToDlZkD1SL+cT4LPnbHMyJGXpEitBtuvYGq34EcTQ8873yN5vGYNQ0XWuD37WfnGRoTNqOQCHTJ4s818WJCAcT8HAjJ3meyc+Os5701hgHX/zcKzZMStdSZmrQpV7IYGHDF0Plh+v+6Si+XO4vzAm3x+lmLomfizpNVcukxIUCGQIDAQABAoIBADD1rAjOJ43kmiw2iopMnyMqX6ByzK18OYfuDXnOvtXLHPQpp0myWcNXRZxmhhNW6KXeHJhMm9Y+AXPdYySQf5ruTwyLMSXaQGVwP7z3i1ZxQ6eQlqw68dNXNW55OxXHF4kxGxF+1CICv4WchFdqc/JD50hDpF+10wI8lJlk6WV0mHHSOCTbP3y4Ff5aOUoDvziHPRsxrFe0emOnaCzeCFlBfWyvgms86TRrdsATVZVu+6y2ioShyvDmCkg0hDRodGS9xdgnk4RNS1hqQaU+RO7DYafQMeBj/16TtoGXB9RafpCalvMIobJwTHKsz4jGVEBldxL6q43JQbrQ0wZOCx8CgYEAymqIPE+dVcrib4vhaqveWyuCal5KF8rxgpBybNu+nHKY2FWk6hm3tT5mUxF1GcpmXhgKgbbdFS0y6n3Mt+O6oYLahbJI1YqGa2VrtsDWFy9I8w8a/GxCHoaACgovVuOc6XQ23G4iyBRy3/AiR04vTsSkcO1zuQQ1BHwP2kuaecsCgYEAvb2IMX3CKOQrczRUvYvnSb/nU5s9gxzBljtmzXY0T+13QJotZ5tnQOGYoVfs60oYN1LJuet+Tx58ynm6zzpBRQ4PGX5j4eBKiV4pEU+LauVzuwMzIRXRbjtKxuz9vY/F1q9rbughDOaIgWEqOebh0zdmgNIO+06j25XIDLj6BysCgYALPtnsx9uM3aWw3suaOowy8nrfX/er1LmQMGg7BgZT+MXnFvL/kogzSJE0wj6IPUdsqNdEl502xPkUMAMEoGFJAdUUInCLdJO9Obg/Qf1i8lySLF+qcHVH7t8EL8arhB++lJdLrc6+4Ov05wlwfraAIMql/3szZzlu01mqHhMp+wKBgBfgXN4VfDRXGhyiLjO4Aj/mlT6RH3l7SeUUzof2YvjX4hTQEfyisqiYevN6tAabyVZU5auutbNRG9UC4n4nT4VYEpBbtkf7SIXmwnY6M0odW6Xh7jIwo60iavODaR7Mk9HDO1q2fUo5TF4zkvO8WRiSNSTqyhXXvIjqXUU0lcoHAoGBAMlph4557eDud2wgUuVz+A/0rL+6dhCSKJDaZvP+vtLb/1HxuQEFRf978XvekQR3+oE6/CfE+NLP2XI9Y1TO99n6EzNByeqIECTWJ4ZZ7OZxDSV78NB2QcaDUq6TsPDzE1EcYwtvkI7BYojv2FQ97feV/FDl3CRj9glO6okU9z87";

    fn key_pair() -> RsaKeyPair {
        RsaKeyPair::from_der(&STANDARD.decode(TEST_KEY).unwrap()).unwrap()
    }

    fn jwks(kid: &str) -> Value {
        let public = ring::rsa::PublicKeyComponents::<Vec<u8>>::from(key_pair().public());
        json!({"keys": [{"kty": "RSA", "kid": kid, "alg": "RS256", "n": URL_SAFE_NO_PAD.encode(&public.n), "e": URL_SAFE_NO_PAD.encode(&public.e)}]})
    }

    fn sign(claims: &Value, kid: &str) -> String {
        let header =
            URL_SAFE_NO_PAD.encode(json!({"alg": "RS256", "kid": kid, "typ": "JWT"}).to_string());
        let payload = URL_SAFE_NO_PAD.encode(claims.to_string());
        let signed = format!("{header}.{payload}");
        let mut signature = vec![0u8; key_pair().public().modulus_len()];
        key_pair()
            .sign(
                &RSA_PKCS1_SHA256,
                &ring::rand::SystemRandom::new(),
                signed.as_bytes(),
                &mut signature,
            )
            .unwrap();
        format!("{signed}.{}", URL_SAFE_NO_PAD.encode(signature))
    }

    fn claims() -> Value {
        json!({"iss": "https://auth.openai.com", "aud": "client_abc", "sub": "user-1", "exp": 2_000_000_100u64, "iat": 2_000_000_000u64, "nonce": "N", "email": "me@example.com", "name": "Me"})
    }

    #[test]
    fn the_pkce_challenge_matches_the_published_example() {
        assert_eq!(
            code_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
            "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
        );
    }

    #[test]
    fn random_values_are_long_distinct_and_the_host_id_is_a_version_4_uuid() {
        assert_ne!(random_token(32).unwrap(), random_token(32).unwrap());
        assert!(random_token(64).unwrap().len() >= 43);
        let id = new_host_id().unwrap();
        assert!(
            id.starts_with("urn:uuid:") && id.len() == "urn:uuid:".len() + 36,
            "{id}"
        );
        assert_eq!(id.as_bytes()["urn:uuid:".len() + 14], b'4');
    }

    #[test]
    fn the_sign_in_address_registers_first_and_reuses_the_client_after() {
        let first = authorization_url(
            "https://auth.example/authorize",
            None,
            "http://127.0.0.1:1234/callback",
            "S",
            "N",
            "C",
            "urn:uuid:h",
            None,
        )
        .unwrap();
        let url = url::Url::parse(&first).unwrap();
        let get = |k: &str| {
            url.query_pairs()
                .find(|(n, _)| n == k)
                .map(|(_, v)| v.into_owned())
        };
        assert_eq!(get("client_id").as_deref(), Some("dynamic_agent_client"));
        assert_eq!(get("agent_name_hint").as_deref(), Some("Helix Mobius"));
        assert_eq!(get("code_challenge_method").as_deref(), Some("S256"));
        assert_eq!(
            get("resource").as_deref(),
            Some("https://api.openai.com/v1")
        );
        assert_eq!(get("scope").as_deref(), Some(SCOPES));
        assert_eq!(
            (
                get("state").as_deref(),
                get("nonce").as_deref(),
                get("ext_agent_host_id").as_deref()
            ),
            (Some("S"), Some("N"), Some("urn:uuid:h"))
        );
        let again = authorization_url(
            "https://auth.example/authorize",
            Some("client_abc"),
            "http://127.0.0.1:1234/callback",
            "S",
            "N",
            "C",
            "urn:uuid:h",
            Some("me@example.com"),
        )
        .unwrap();
        let url = url::Url::parse(&again).unwrap();
        let get = |k: &str| {
            url.query_pairs()
                .find(|(n, _)| n == k)
                .map(|(_, v)| v.into_owned())
        };
        assert_eq!(get("client_id").as_deref(), Some("client_abc"));
        assert!(get("agent_name_hint").is_none());
        assert_eq!(get("login_hint").as_deref(), Some("me@example.com"));
    }

    fn request(target: &str, host: &str) -> String {
        format!("GET {target} HTTP/1.1\r\nHost: {host}\r\n\r\n")
    }

    #[test]
    fn only_this_sign_ins_own_callback_is_believed() {
        let p = 4000;
        let good = request(
            "/callback?code=abc&state=STATE&client_id=client_abc",
            "127.0.0.1:4000",
        );
        assert_eq!(
            parse_callback(&good, p, "STATE", None),
            Callback::Done {
                code: "abc".into(),
                client_id: "client_abc".into()
            }
        );
        // wrong state, wrong path, wrong host and a second state are all ignored, so a stray request cannot end the wait
        assert_eq!(
            parse_callback(
                &request(
                    "/callback?code=abc&state=OTHER&client_id=client_abc",
                    "127.0.0.1:4000"
                ),
                p,
                "STATE",
                None
            ),
            Callback::Ignore(400)
        );
        assert_eq!(
            parse_callback(
                &request(
                    "/other?code=abc&state=STATE&client_id=client_abc",
                    "127.0.0.1:4000"
                ),
                p,
                "STATE",
                None
            ),
            Callback::Ignore(404)
        );
        assert_eq!(
            parse_callback(
                &request(
                    "/callback?code=abc&state=STATE&client_id=client_abc",
                    "evil.example"
                ),
                p,
                "STATE",
                None
            ),
            Callback::Ignore(404)
        );
        assert_eq!(
            parse_callback(
                &request(
                    "/callback?code=abc&state=STATE&state=STATE&client_id=client_abc",
                    "127.0.0.1:4000"
                ),
                p,
                "STATE",
                None
            ),
            Callback::Ignore(400)
        );
        assert_eq!(
            parse_callback(
                "POST /callback?state=STATE HTTP/1.1\r\nHost: 127.0.0.1:4000\r\n\r\n",
                p,
                "STATE",
                None
            ),
            Callback::Ignore(404)
        );
        // the person said no, or the registration came back unfinished
        assert_eq!(
            parse_callback(
                &request(
                    "/callback?error=access_denied&state=STATE",
                    "127.0.0.1:4000"
                ),
                p,
                "STATE",
                None
            ),
            Callback::Failed("Sign-in was cancelled.".into())
        );
        assert!(matches!(
            parse_callback(
                &request("/callback?code=abc&state=STATE", "127.0.0.1:4000"),
                p,
                "STATE",
                None
            ),
            Callback::Failed(_)
        ));
        assert!(matches!(
            parse_callback(
                &request(
                    "/callback?code=abc&state=STATE&client_id=dynamic_agent_client",
                    "127.0.0.1:4000"
                ),
                p,
                "STATE",
                None
            ),
            Callback::Failed(_)
        ));
        // a saved client is used when none is returned, and a different one is refused
        assert_eq!(
            parse_callback(
                &request("/callback?code=abc&state=STATE", "127.0.0.1:4000"),
                p,
                "STATE",
                Some("client_abc")
            ),
            Callback::Done {
                code: "abc".into(),
                client_id: "client_abc".into()
            }
        );
        assert!(matches!(
            parse_callback(
                &request(
                    "/callback?code=abc&state=STATE&client_id=client_zzz",
                    "127.0.0.1:4000"
                ),
                p,
                "STATE",
                Some("client_abc")
            ),
            Callback::Failed(_)
        ));
    }

    #[test]
    fn a_signed_identity_is_accepted_and_every_way_of_forging_one_is_refused() {
        let at = 2_000_000_050u64;
        let good = sign(&claims(), "k1");
        let who = verify_id_token(
            &good,
            &jwks("k1"),
            "https://auth.openai.com",
            "client_abc",
            "N",
            at,
        )
        .unwrap();
        assert_eq!(
            (who.subject.as_str(), who.email.as_str()),
            ("user-1", "me@example.com")
        );
        let check = |token: &str, kid: &str, issuer: &str, audience: &str, nonce: &str, at: u64| {
            verify_id_token(token, &jwks(kid), issuer, audience, nonce, at)
        };
        assert!(
            check(
                &good,
                "k1",
                "https://auth.openai.com",
                "client_abc",
                "WRONG",
                at
            )
            .is_err(),
            "another sign-in's nonce"
        );
        assert!(
            check(
                &good,
                "k1",
                "https://auth.openai.com",
                "other_client",
                "N",
                at
            )
            .is_err(),
            "someone else's client"
        );
        assert!(
            check(&good, "k1", "https://evil.example", "client_abc", "N", at).is_err(),
            "another issuer"
        );
        assert!(
            check(
                &good,
                "k1",
                "https://auth.openai.com",
                "client_abc",
                "N",
                2_000_001_000
            )
            .is_err(),
            "expired"
        );
        assert!(
            check(
                &good,
                "other",
                "https://auth.openai.com",
                "client_abc",
                "N",
                at
            )
            .is_err(),
            "no such key"
        );
        // a token whose body was changed after signing
        let parts: Vec<&str> = good.split('.').collect();
        let mut changed = claims();
        changed["sub"] = json!("admin");
        let forged = format!(
            "{}.{}.{}",
            parts[0],
            URL_SAFE_NO_PAD.encode(changed.to_string()),
            parts[2]
        );
        assert!(
            check(
                &forged,
                "k1",
                "https://auth.openai.com",
                "client_abc",
                "N",
                at
            )
            .is_err(),
            "body changed"
        );
        // a token that claims no signature at all
        let none = format!(
            "{}.{}.",
            URL_SAFE_NO_PAD.encode(json!({"alg": "none"}).to_string()),
            parts[1]
        );
        assert!(check(
            &none,
            "k1",
            "https://auth.openai.com",
            "client_abc",
            "N",
            at
        )
        .is_err());
        assert!(check(
            "not.a.jwt.at.all",
            "k1",
            "https://auth.openai.com",
            "client_abc",
            "N",
            at
        )
        .is_err());
    }

    #[test]
    fn a_token_for_several_audiences_must_name_this_client_as_its_authorised_party() {
        let mut c = claims();
        c["aud"] = json!(["client_abc", "someone_else"]);
        assert!(verify_id_token(
            &sign(&c, "k1"),
            &jwks("k1"),
            "https://auth.openai.com",
            "client_abc",
            "N",
            2_000_000_050
        )
        .is_err());
        c["azp"] = json!("client_abc");
        assert!(verify_id_token(
            &sign(&c, "k1"),
            &jwks("k1"),
            "https://auth.openai.com",
            "client_abc",
            "N",
            2_000_000_050
        )
        .is_ok());
    }

    #[test]
    fn the_request_puts_system_text_in_instructions_and_stores_nothing() {
        let messages = vec![
            ("system", "Be brief.".to_string()),
            ("user", "hi".to_string()),
            ("assistant", "hello".to_string()),
            ("user", "again".to_string()),
        ];
        let body = build_request("gpt-x", &messages, 512);
        assert_eq!(body["instructions"], "Be brief.");
        assert_eq!(body["input"].as_array().unwrap().len(), 3);
        assert_eq!(
            (
                body["store"].clone(),
                body["stream"].clone(),
                body["model"].clone()
            ),
            (json!(false), json!(false), json!("gpt-x"))
        );
        assert!(build_request("gpt-x", &[("user", "hi".to_string())], 512)
            .get("instructions")
            .is_none());
    }

    #[test]
    fn the_reply_is_read_from_the_output_and_failures_are_said_plainly() {
        let value = json!({"model": "gpt-x-2026", "output": [{"type": "reasoning"}, {"type": "message", "content": [{"type": "output_text", "text": "Hello "}, {"type": "output_text", "text": "there."}]}], "usage": {"output_tokens": 9}});
        let reply = parse_response(&value, "gpt-x").unwrap();
        assert_eq!(
            (
                reply.text.as_str(),
                reply.model.as_str(),
                reply.completion_tokens,
                reply.cost_micro
            ),
            ("Hello there.", "gpt-x-2026", Some(9), None)
        );
        assert!(parse_response(&json!({"output": []}), "m").is_err());
        assert!(parse_response(&json!({"error": {"message": "nope"}}), "m")
            .unwrap_err()
            .contains("nope"));
        assert!(explain_failure(429, "").contains("Manage usage"));
        assert!(explain_failure(401, "").contains("Continue with ChatGPT"));
        assert!(explain_failure(403, "token sharing disabled").contains("not turned on"));
        assert!(explain_failure(404, "").contains("does not offer"));
    }

    #[test]
    fn only_models_that_chat_are_listed() {
        for yes in [
            "gpt-5",
            "gpt-5.1-mini",
            "o3",
            "o4-mini",
            "chatgpt-4o-latest",
            "codex-mini",
        ] {
            assert!(chat_model(yes), "{yes}");
        }
        for no in [
            "text-embedding-3-small",
            "whisper-1",
            "tts-1",
            "gpt-image-1",
            "gpt-4o-transcribe",
            "gpt-4o-realtime-preview",
            "omni-moderation-latest",
            "dall-e-3",
            "gpt-3.5-turbo-instruct",
            "gpt-4o-audio-preview",
            "davinci-002",
        ] {
            assert!(!chat_model(no), "{no}");
        }
    }

    #[test]
    fn the_test_overrides_accept_only_this_computer() {
        std::env::set_var("HELIX_CHATGPT_TEST_A", "https://evil.example");
        std::env::set_var("HELIX_CHATGPT_TEST_B", "http://127.0.0.1:9999/");
        assert_eq!(endpoint("HELIX_CHATGPT_TEST_A", ISSUER), ISSUER);
        assert_eq!(
            endpoint("HELIX_CHATGPT_TEST_B", ISSUER),
            "http://127.0.0.1:9999"
        );
        assert_eq!(endpoint("HELIX_CHATGPT_TEST_UNSET", ISSUER), ISSUER);
    }

    #[test]
    fn an_der_public_key_round_trips_through_the_verifier() {
        // covered by the signed-identity test; this pins the encoding of an integer whose top bit is set
        assert_eq!(
            der_integer(&[0x80, 0x01]),
            vec![0x02, 0x03, 0x00, 0x80, 0x01]
        );
        assert_eq!(der_integer(&[0x00, 0x00, 0x7f]), vec![0x02, 0x01, 0x7f]);
        assert_eq!(der_length(300), vec![0x82, 0x01, 0x2c]);
    }
}
