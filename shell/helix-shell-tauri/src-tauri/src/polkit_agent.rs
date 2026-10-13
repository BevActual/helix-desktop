//! Session Polkit authentication agent for the bar (`/org/helix/PolkitAuthenticationAgent`).

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use nix::unistd::Uid;
use serde::{Deserialize, Serialize};
use tauri::AppHandle;
use zbus::zvariant::{OwnedValue, Type, Value};
use zbus_polkit::policykit1::{AuthorityProxy, Subject};

use crate::admin_auth;
use crate::polkit_helper::{HelperError, HelperMessage, HelperSession};

pub const OBJECT_PATH: &str = "/org/helix/PolkitAuthenticationAgent";

#[derive(Clone, Debug, zbus::DBusError)]
#[zbus(prefix = "org.freedesktop.PolicyKit1.Error")]
pub enum PolkitError {
    Failed,
    FailedReason(String),
    Cancelled,
    NotSupported,
    NotAuthorized,
}

#[derive(Debug, Deserialize, Serialize, Type)]
struct PolkitIdentity<'a> {
    identity_kind: &'a str,
    identity_details: HashMap<&'a str, Value<'a>>,
}

struct AgentState {
    app: AppHandle,
    cancel_cookie: Mutex<Option<String>>,
}

struct HelixPolkitAgent {
    state: AgentState,
}

fn locale() -> String {
    std::env::var("LANG")
        .or_else(|_| std::env::var("LC_MESSAGES"))
        .unwrap_or_else(|_| "en_US.UTF-8".to_string())
}

fn session_subject() -> Result<Subject, PolkitError> {
    let session_id = std::env::var("XDG_SESSION_ID")
        .map_err(|_| PolkitError::FailedReason("XDG_SESSION_ID is not set".into()))?;
    let mut details = HashMap::new();
    details.insert(
        "session-id".to_string(),
        OwnedValue::from(zbus::zvariant::Str::from(session_id.as_str())),
    );
    Ok(Subject {
        subject_kind: "unix-session".into(),
        subject_details: details,
    })
}

fn first_unix_user_uid(identities: &[PolkitIdentity<'_>]) -> Result<Uid, PolkitError> {
    for id in identities {
        if id.identity_kind != "unix-user" {
            continue;
        }
        let uid_value = id
            .identity_details
            .get("uid")
            .ok_or(PolkitError::FailedReason("identity missing uid".into()))?;
        let uid: u32 = uid_value
            .try_into()
            .map_err(|_| PolkitError::FailedReason("invalid uid".into()))?;
        return Ok(Uid::from_raw(uid));
    }
    Err(PolkitError::FailedReason("no unix-user identity".into()))
}

async fn run_authentication(
    app: AppHandle,
    message: String,
    cookie: String,
    uid: Uid,
) -> Result<(), PolkitError> {
    let session = Arc::new(Mutex::new(
        HelperSession::new(uid, &cookie).map_err(|error| match error {
            HelperError::NoSocket => PolkitError::NotSupported,
            other => PolkitError::FailedReason(other.to_string()),
        })?,
    ));

    let mut retries = 3;
    while retries >= 0 {
        while !session.lock().expect("polkit session").is_complete() {
            let event = {
                let session = Arc::clone(&session);
                tokio::task::spawn_blocking(move || session.lock().expect("polkit session").dispatch())
                    .await
                    .map_err(|_| PolkitError::FailedReason("polkit helper task failed".into()))?
            }
            .map_err(|error| PolkitError::FailedReason(error.to_string()))?;

            match event {
                HelperMessage::PasswordPrompt { prompt } => {
                    let title = if message.is_empty() {
                        prompt
                    } else if prompt.is_empty() {
                        message.clone()
                    } else {
                        format!("{message}\n\n{prompt}")
                    };
                    let password = match admin_auth::prompt(app.clone(), title).await {
                        Ok(password) => password,
                        Err(error) if error == "cancelled" => return Err(PolkitError::Cancelled),
                        Err(error) => return Err(PolkitError::FailedReason(error)),
                    };
                    {
                        let session = Arc::clone(&session);
                        let password = password;
                        tokio::task::spawn_blocking(move || {
                            session
                                .lock()
                                .expect("polkit session")
                                .respond_password(&password)
                        })
                        .await
                        .map_err(|_| PolkitError::FailedReason("polkit response task failed".into()))?
                    }
                    .map_err(|error| PolkitError::FailedReason(error.to_string()))?;
                }
                HelperMessage::Error(text) => {
                    eprintln!("POLKIT helper error: {text}");
                }
                HelperMessage::Info(text) => {
                    eprintln!("POLKIT helper info: {text}");
                }
                HelperMessage::Complete(ok) => {
                    if ok {
                        return Ok(());
                    }
                    break;
                }
            }
        }
        if session.lock().expect("polkit session").succeeded() {
            return Ok(());
        }
        {
            let session = Arc::clone(&session);
            tokio::task::spawn_blocking(move || session.lock().expect("polkit session").restart())
                .await
                .map_err(|_| PolkitError::FailedReason("polkit restart task failed".into()))?
        }
        .map_err(|error| PolkitError::FailedReason(error.to_string()))?;
        retries -= 1;
    }
    Err(PolkitError::Failed)
}

#[zbus::interface(name = "org.freedesktop.PolicyKit1.AuthenticationAgent")]
impl HelixPolkitAgent {
    async fn begin_authentication(
        &mut self,
        _action_id: &str,
        message: &str,
        _icon_name: &str,
        _details: HashMap<&str, &str>,
        cookie: &str,
        identities: Vec<PolkitIdentity<'_>>,
    ) -> Result<(), PolkitError> {
        *self.state.cancel_cookie.lock().expect("polkit") = Some(cookie.to_string());
        let uid = first_unix_user_uid(&identities)?;
        let app = self.state.app.clone();
        let result = run_authentication(app, message.to_string(), cookie.to_string(), uid).await;
        *self.state.cancel_cookie.lock().expect("polkit") = None;
        result
    }

    async fn cancel_authentication(&mut self, cookie: &str) -> Result<(), PolkitError> {
        let should = self
            .state
            .cancel_cookie
            .lock()
            .expect("polkit")
            .as_deref()
            .is_some_and(|active| active == cookie);
        if should {
            *self.state.cancel_cookie.lock().expect("polkit") = None;
        }
        Ok(())
    }
}

pub fn start(app: AppHandle) {
    if std::env::var_os("HELIX_DISABLE_POLKIT_AGENT").is_some() {
        eprintln!("POLKIT agent disabled (HELIX_DISABLE_POLKIT_AGENT)");
        return;
    }
    tauri::async_runtime::spawn(async move {
        let subject = match session_subject() {
            Ok(subject) => subject,
            Err(error) => {
                eprintln!("POLKIT agent: {error:?}");
                return;
            }
        };
        let agent = HelixPolkitAgent {
            state: AgentState {
                app: app.clone(),
                cancel_cookie: Mutex::new(None),
            },
        };
        let connection = match zbus::connection::Builder::system() {
            Ok(builder) => match builder.serve_at(OBJECT_PATH, agent) {
                Ok(builder) => match builder.build().await {
                    Ok(conn) => conn,
                    Err(error) => {
                        eprintln!("POLKIT agent: build connection: {error}");
                        return;
                    }
                },
                Err(error) => {
                    eprintln!("POLKIT agent: serve agent object: {error}");
                    return;
                }
            },
            Err(error) => {
                eprintln!("POLKIT agent: system bus: {error}");
                return;
            }
        };
        let authority = match AuthorityProxy::new(&connection).await {
            Ok(proxy) => proxy,
            Err(error) => {
                eprintln!("POLKIT agent: authority proxy: {error}");
                return;
            }
        };
        if let Err(error) = authority
            .register_authentication_agent(&subject, &locale(), OBJECT_PATH)
            .await
        {
            eprintln!("POLKIT agent: register failed: {error}");
            return;
        }
        eprintln!("POLKIT agent registered at {OBJECT_PATH}");
        std::future::pending::<()>().await;
    });
}
