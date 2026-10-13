//! Talk to polkit's setuid helper over `/run/polkit/agent-helper.socket` (line protocol).

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;

use nix::unistd::{Uid, User};

const SOCKET: &str = "/run/polkit/agent-helper.socket";
const PAM_PROMPT_ECHO_OFF: &str = "PAM_PROMPT_ECHO_OFF";
const PAM_PROMPT_ECHO_ON: &str = "PAM_PROMPT_ECHO_ON";
const PAM_ERROR_MSG: &str = "PAM_ERROR_MSG";
const PAM_TEXT_INFO: &str = "PAM_TEXT_INFO";

#[derive(Debug)]
pub enum HelperError {
    NoSocket,
    UserNotFound,
    Io(std::io::Error),
    Unknown(String),
}

impl std::fmt::Display for HelperError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NoSocket => write!(f, "polkit helper socket missing"),
            Self::UserNotFound => write!(f, "user not found"),
            Self::Io(e) => write!(f, "io: {e}"),
            Self::Unknown(s) => write!(f, "unknown helper message: {s}"),
        }
    }
}

impl std::error::Error for HelperError {}

impl From<std::io::Error> for HelperError {
    fn from(value: std::io::Error) -> Self {
        Self::Io(value)
    }
}

#[derive(Debug, Clone)]
pub enum HelperMessage {
    PasswordPrompt { prompt: String },
    Error(String),
    Info(String),
    Complete(bool),
}

pub struct HelperSession {
    user: User,
    stream: UnixStream,
    cookie: Option<String>,
    complete: bool,
    succeeded: bool,
}

impl HelperSession {
    pub fn new(uid: Uid, cookie: &str) -> Result<Self, HelperError> {
        if !Path::new(SOCKET).exists() {
            return Err(HelperError::NoSocket);
        }
        let user = User::from_uid(uid)
            .map_err(|_| HelperError::UserNotFound)?
            .ok_or(HelperError::UserNotFound)?;
        let mut session = Self {
            user,
            stream: UnixStream::connect(SOCKET)?,
            cookie: Some(cookie.to_string()),
            complete: false,
            succeeded: false,
        };
        session.write_header()?;
        Ok(session)
    }

    pub fn user_name(&self) -> &str {
        self.user.name.as_ref()
    }

    pub fn is_complete(&self) -> bool {
        self.complete
    }

    pub fn succeeded(&self) -> bool {
        self.complete && self.succeeded
    }

    fn write_header(&mut self) -> Result<(), HelperError> {
        self.stream.write_all(self.user.name.as_bytes())?;
        self.stream.write_all(b"\n")?;
        if let Some(cookie) = &self.cookie {
            self.stream.write_all(cookie.as_bytes())?;
            self.stream.write_all(b"\n")?;
        }
        Ok(())
    }

    pub fn restart(&mut self) -> Result<(), HelperError> {
        self.stream = UnixStream::connect(SOCKET)?;
        self.complete = false;
        self.succeeded = false;
        self.write_header()?;
        Ok(())
    }

    pub fn dispatch(&mut self) -> Result<HelperMessage, HelperError> {
        if self.complete {
            return Ok(HelperMessage::Complete(self.succeeded));
        }
        let line = read_line(&mut self.stream)?;
        if let Some(stripped) = line.strip_prefix(PAM_PROMPT_ECHO_OFF) {
            return Ok(HelperMessage::PasswordPrompt {
                prompt: stripped.trim().to_string(),
            });
        }
        if let Some(stripped) = line.strip_prefix(PAM_PROMPT_ECHO_ON) {
            return Ok(HelperMessage::PasswordPrompt {
                prompt: stripped.trim().to_string(),
            });
        }
        if let Some(stripped) = line.strip_prefix(PAM_ERROR_MSG) {
            return Ok(HelperMessage::Error(stripped.trim().to_string()));
        }
        if let Some(stripped) = line.strip_prefix(PAM_TEXT_INFO) {
            return Ok(HelperMessage::Info(stripped.trim().to_string()));
        }
        self.complete = true;
        if line.starts_with("SUCCESS") {
            self.succeeded = true;
            return Ok(HelperMessage::Complete(true));
        }
        if line.starts_with("FAILURE") {
            return Ok(HelperMessage::Complete(false));
        }
        Err(HelperError::Unknown(line))
    }

    pub fn respond_password(&mut self, password: &str) -> Result<(), HelperError> {
        self.stream.write_all(password.as_bytes())?;
        self.stream.write_all(b"\n")?;
        Ok(())
    }
}

fn read_line(stream: &mut UnixStream) -> Result<String, HelperError> {
    let mut data = Vec::new();
    loop {
        let mut byte = [0u8; 1];
        stream.read_exact(&mut byte)?;
        data.push(byte[0]);
        if byte[0] == b'\n' {
            break;
        }
    }
    Ok(String::from_utf8_lossy(&data).into_owned())
}
