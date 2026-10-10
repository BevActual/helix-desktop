//! Cross-process contract between `helix-shell-bar` and `helix-shell-panels`.
//!
//! One JSON object per line on a Unix socket (`helix-panels-<WAYLAND_DISPLAY>.sock`, mode 0600).
//! Same trust model as the agent socket: same-user only; Agent Surface still gates desktop change.
//! Pure parse/serialize — no I/O.

use serde::{Deserialize, Serialize};

use crate::shell::shell_surface::PANELS;

/// Hard cap on one request or reply line (bytes).
pub const MAX_LINE: usize = 8 * 1024;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum PanelsRequest {
    Ping,
    Open {
        id: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        mode: Option<String>,
    },
    Close,
    Escape,
    Outside,
    AssistantSubmit {
        request: String,
    },
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum PanelsReply {
    Pong,
    Ready,
    Ok,
    /// Current open panel id, if any (`assistant`, `launcher`, …).
    Status {
        #[serde(default, skip_serializing_if = "Option::is_none")]
        panel: Option<String>,
    },
    Err {
        message: String,
    },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PanelsIpcError {
    Empty,
    TooLong,
    InvalidJson,
    UnknownPanel,
    EmptyRequest,
    ControlChars,
}

impl std::fmt::Display for PanelsIpcError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Empty => write!(f, "empty line"),
            Self::TooLong => write!(f, "line too long"),
            Self::InvalidJson => write!(f, "invalid json"),
            Self::UnknownPanel => write!(f, "unknown panel"),
            Self::EmptyRequest => write!(f, "empty request"),
            Self::ControlChars => write!(f, "control characters"),
        }
    }
}

/// Socket file name fragment: `helix-panels-<display>.sock` under the runtime dir.
pub fn socket_file_name(wayland_display: &str) -> Option<String> {
    let display = wayland_display.trim();
    if display.is_empty() || display.contains('/') || display.contains('\0') {
        return None;
    }
    Some(format!("helix-panels-{display}.sock"))
}

pub fn encode_request(req: &PanelsRequest) -> Result<String, PanelsIpcError> {
    let line = serde_json::to_string(req).map_err(|_| PanelsIpcError::InvalidJson)?;
    if line.len() > MAX_LINE {
        return Err(PanelsIpcError::TooLong);
    }
    Ok(line)
}

pub fn encode_reply(reply: &PanelsReply) -> Result<String, PanelsIpcError> {
    let line = serde_json::to_string(reply).map_err(|_| PanelsIpcError::InvalidJson)?;
    if line.len() > MAX_LINE {
        return Err(PanelsIpcError::TooLong);
    }
    Ok(line)
}

pub fn decode_request(line: &str) -> Result<PanelsRequest, PanelsIpcError> {
    let trimmed = line.trim();
    if trimmed.is_empty() {
        return Err(PanelsIpcError::Empty);
    }
    if trimmed.len() > MAX_LINE {
        return Err(PanelsIpcError::TooLong);
    }
    let req: PanelsRequest =
        serde_json::from_str(trimmed).map_err(|_| PanelsIpcError::InvalidJson)?;
    validate_request(&req)?;
    Ok(req)
}

pub fn decode_reply(line: &str) -> Result<PanelsReply, PanelsIpcError> {
    let trimmed = line.trim();
    if trimmed.is_empty() {
        return Err(PanelsIpcError::Empty);
    }
    if trimmed.len() > MAX_LINE {
        return Err(PanelsIpcError::TooLong);
    }
    serde_json::from_str(trimmed).map_err(|_| PanelsIpcError::InvalidJson)
}

fn validate_request(req: &PanelsRequest) -> Result<(), PanelsIpcError> {
    match req {
        PanelsRequest::Open { id, mode } => {
            if !PANELS.iter().any(|known| *known == id.as_str()) {
                return Err(PanelsIpcError::UnknownPanel);
            }
            if let Some(mode) = mode {
                match mode.as_str() {
                    "talk" | "act" | "agents" | "assistant" => {}
                    _ => return Err(PanelsIpcError::UnknownPanel),
                }
            }
            Ok(())
        }
        PanelsRequest::AssistantSubmit { request } => {
            let trimmed = request.trim_matches(|c: char| c.is_ascii_whitespace());
            if trimmed.is_empty() {
                return Err(PanelsIpcError::EmptyRequest);
            }
            if trimmed
                .chars()
                .any(|c| c.is_control() && c != '\n' && c != '\t')
            {
                return Err(PanelsIpcError::ControlChars);
            }
            Ok(())
        }
        PanelsRequest::Ping
        | PanelsRequest::Close
        | PanelsRequest::Escape
        | PanelsRequest::Outside => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_open_with_mode() {
        let req = PanelsRequest::Open {
            id: "assistant".into(),
            mode: Some("talk".into()),
        };
        let line = encode_request(&req).unwrap();
        assert_eq!(decode_request(&line).unwrap(), req);
    }

    #[test]
    fn refuses_unknown_panel() {
        let line = r#"{"op":"open","id":"settings"}"#;
        assert_eq!(decode_request(line), Err(PanelsIpcError::UnknownPanel));
    }

    #[test]
    fn refuses_empty_assistant_submit() {
        let line = r#"{"op":"assistant_submit","request":"   "}"#;
        assert_eq!(decode_request(line), Err(PanelsIpcError::EmptyRequest));
    }

    #[test]
    fn socket_name_rejects_path_segments() {
        assert!(socket_file_name("wayland-1").is_some());
        assert!(socket_file_name("../x").is_none());
        assert!(socket_file_name("").is_none());
    }

    #[test]
    fn reply_round_trip() {
        let reply = PanelsReply::Status {
            panel: Some("launcher".into()),
        };
        let line = encode_reply(&reply).unwrap();
        assert_eq!(decode_reply(&line).unwrap(), reply);
    }
}
