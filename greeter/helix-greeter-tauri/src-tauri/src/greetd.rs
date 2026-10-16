//! The greetd IPC protocol: one length-prefixed JSON message each way over `$GREETD_SOCK`. Pure framing and
//! interpretation live here so they can be tested against a fake greetd; nothing here knows about the window.

use serde::{Deserialize, Serialize};
use std::io::{self, Read, Write};

/// A request is never larger than this; greetd's own messages are tiny. A longer prefix is a corrupt stream, not a message to buffer.
const MAX_MESSAGE: usize = 64 * 1024;

#[derive(Debug, Serialize, PartialEq, Eq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Request<'a> {
    CreateSession {
        username: &'a str,
    },
    PostAuthMessageResponse {
        response: Option<&'a str>,
    },
    StartSession {
        cmd: &'a [String],
        env: &'a [String],
    },
    CancelSession,
}

#[derive(Debug, Deserialize, PartialEq, Eq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Response {
    Success,
    Error {
        error_type: String,
        description: String,
    },
    AuthMessage {
        auth_message_type: String,
        auth_message: String,
    },
}

/// What the person sees next. Only these four shapes ever reach the page, so a hostile or odd greetd cannot make it draw anything else.
#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Step {
    /// A question: `secret` is true for a password (typed text is hidden).
    Prompt { secret: bool, message: String },
    /// A line to show while authentication continues (PAM `info` / `error` text).
    Info { message: String, error: bool },
    /// Authentication finished; a session can be started.
    Ready,
    /// This attempt failed; the person may start again.
    Failed { message: String },
}

pub fn write_request(out: &mut impl Write, request: &Request) -> io::Result<()> {
    let body = serde_json::to_vec(request).map_err(io::Error::other)?;
    let len = u32::try_from(body.len()).map_err(|_| io::Error::other("request too large"))?;
    out.write_all(&len.to_ne_bytes())?;
    out.write_all(&body)?;
    out.flush()
}

pub fn read_response(input: &mut impl Read) -> io::Result<Response> {
    let mut len = [0u8; 4];
    input.read_exact(&mut len)?;
    let len = u32::from_ne_bytes(len) as usize;
    if len > MAX_MESSAGE {
        return Err(io::Error::other("greetd message too large"));
    }
    let mut body = vec![0u8; len];
    input.read_exact(&mut body)?;
    serde_json::from_slice(&body).map_err(io::Error::other)
}

/// Text from greetd or PAM is bounded and stripped of control characters before it is shown.
pub fn display_text(text: &str) -> String {
    text.chars().filter(|c| !c.is_control()).take(200).collect()
}

impl Response {
    /// The step after an authentication-phase request.
    pub fn into_step(self) -> Step {
        match self {
            Response::Success => Step::Ready,
            Response::Error {
                error_type,
                description,
            } => Step::Failed {
                message: if error_type == "auth_error" {
                    "That didn’t work. Try again.".into()
                } else {
                    display_text(&description)
                },
            },
            Response::AuthMessage {
                auth_message_type,
                auth_message,
            } => {
                let message = display_text(&auth_message);
                match auth_message_type.as_str() {
                    "secret" => Step::Prompt {
                        secret: true,
                        message,
                    },
                    "visible" => Step::Prompt {
                        secret: false,
                        message,
                    },
                    "error" => Step::Info {
                        message,
                        error: true,
                    },
                    _ => Step::Info {
                        message,
                        error: false,
                    },
                }
            }
        }
    }
}

/// One conversation with greetd over any stream.
pub struct Conversation<S: Read + Write> {
    stream: S,
}

impl<S: Read + Write> Conversation<S> {
    pub fn new(stream: S) -> Self {
        Self { stream }
    }

    fn exchange(&mut self, request: &Request) -> io::Result<Response> {
        write_request(&mut self.stream, request)?;
        read_response(&mut self.stream)
    }

    pub fn begin(&mut self, username: &str) -> io::Result<Step> {
        Ok(self
            .exchange(&Request::CreateSession { username })?
            .into_step())
    }

    pub fn respond(&mut self, response: Option<&str>) -> io::Result<Step> {
        Ok(self
            .exchange(&Request::PostAuthMessageResponse { response })?
            .into_step())
    }

    pub fn cancel(&mut self) -> io::Result<()> {
        self.exchange(&Request::CancelSession).map(|_| ())
    }

    /// Success means greetd will end this greeter and start the session.
    pub fn start(&mut self, cmd: &[String]) -> io::Result<Result<(), String>> {
        Ok(
            match self.exchange(&Request::StartSession { cmd, env: &[] })? {
                Response::Success => Ok(()),
                Response::Error { description, .. } => Err(display_text(&description)),
                Response::AuthMessage { .. } => {
                    Err("unexpected request from the login service".into())
                }
            },
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::net::UnixStream;
    use std::thread;

    fn frame(value: &str) -> Vec<u8> {
        let mut out = (value.len() as u32).to_ne_bytes().to_vec();
        out.extend(value.as_bytes());
        out
    }

    #[test]
    fn requests_use_the_greetd_wire_shape() {
        let mut out = Vec::new();
        write_request(&mut out, &Request::CreateSession { username: "ada" }).unwrap();
        assert_eq!(&out[4..], br#"{"type":"create_session","username":"ada"}"#);
        let mut out = Vec::new();
        write_request(
            &mut out,
            &Request::PostAuthMessageResponse { response: None },
        )
        .unwrap();
        assert_eq!(
            &out[4..],
            br#"{"type":"post_auth_message_response","response":null}"#
        );
    }

    #[test]
    fn auth_messages_become_prompts_and_info() {
        let secret = Response::AuthMessage {
            auth_message_type: "secret".into(),
            auth_message: "Password: ".into(),
        };
        assert_eq!(
            secret.into_step(),
            Step::Prompt {
                secret: true,
                message: "Password: ".into()
            }
        );
        let error = Response::AuthMessage {
            auth_message_type: "error".into(),
            auth_message: "Locked\u{7}".into(),
        };
        assert_eq!(
            error.into_step(),
            Step::Info {
                message: "Locked".into(),
                error: true
            }
        );
    }

    #[test]
    fn a_wrong_password_reads_as_a_plain_retry() {
        let failed = Response::Error {
            error_type: "auth_error".into(),
            description: "pam says no\nat /etc/pam.d".into(),
        };
        assert_eq!(
            failed.into_step(),
            Step::Failed {
                message: "That didn’t work. Try again.".into()
            }
        );
    }

    #[test]
    fn an_oversized_message_is_refused() {
        let mut bytes = ((MAX_MESSAGE + 1) as u32).to_ne_bytes().to_vec();
        bytes.extend([0u8; 8]);
        assert!(read_response(&mut bytes.as_slice()).is_err());
    }

    #[test]
    fn a_full_login_against_a_fake_greetd() {
        let (client, mut server) = UnixStream::pair().unwrap();
        let fake = thread::spawn(move || {
            let mut seen = Vec::new();
            let take = |server: &mut UnixStream| {
                let mut len = [0u8; 4];
                server.read_exact(&mut len).unwrap();
                let mut body = vec![0u8; u32::from_ne_bytes(len) as usize];
                server.read_exact(&mut body).unwrap();
                String::from_utf8(body).unwrap()
            };
            seen.push(take(&mut server));
            server.write_all(&frame(r#"{"type":"auth_message","auth_message_type":"secret","auth_message":"Password:"}"#)).unwrap();
            seen.push(take(&mut server));
            server.write_all(&frame(r#"{"type":"success"}"#)).unwrap();
            seen.push(take(&mut server));
            server.write_all(&frame(r#"{"type":"success"}"#)).unwrap();
            seen
        });
        let mut talk = Conversation::new(client);
        assert_eq!(
            talk.begin("ada").unwrap(),
            Step::Prompt {
                secret: true,
                message: "Password:".into()
            }
        );
        assert_eq!(talk.respond(Some("hunter2")).unwrap(), Step::Ready);
        assert_eq!(talk.start(&["niri-session".to_string()]).unwrap(), Ok(()));
        let seen = fake.join().unwrap();
        assert!(seen[0].contains("create_session") && seen[0].contains("ada"));
        assert!(seen[1].contains("hunter2"));
        assert!(seen[2].contains("start_session") && seen[2].contains("niri-session"));
    }
}
