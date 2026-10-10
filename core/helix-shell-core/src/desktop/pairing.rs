//! Following one Bluetooth pairing from start to finish. The service asks questions through a `Prompt` signal (a PIN to type, a passkey to
//! type, a code to compare, a yes/no, or a code to show); this keeps the question, turns it into what the page shows, and is the only place
//! that builds the answer: a PIN is 1 to 16 printable characters, a passkey is at most six digits, a yes/no is only offered for a yes/no
//! question, and nothing is answered when nothing was asked or the question was about a device that was not the one being paired.

use serde::Serialize;
use serde_json::Value;

use crate::bluetooth::{Bluetooth, Request, RequestError};
use crate::display::sanitize_for_display;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    Pin,
    Passkey,
    DisplayPin,
    DisplayPasskey,
    Confirm,
    Authorize,
    AuthorizeService,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Prompt {
    kind: Kind,
    passkey: u32,
    pin: String,
    entered: u16,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Pairing {
    device: Option<String>,
    prompt: Option<Prompt>,
    /// The service's id for the pair operation; only its completion ends this pairing (a scan finishing must not).
    operation: Option<u64>,
    /// Operations that completed before `operation` was known.
    ended: Vec<u64>,
}

/// What to send to Bluetooth1.Reply.
#[derive(Clone, PartialEq, Eq)]
pub struct Reply {
    pub response: String,
    pub cancel: bool,
}

/// A reply never prints what was typed.
impl std::fmt::Debug for Reply {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Reply").field("response", &"<hidden>").field("cancel", &self.cancel).finish()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PairingError {
    /// Nothing is being paired, or nothing has been asked yet.
    NoQuestion,
    /// The answer does not fit the question (a yes for a PIN, text for a yes/no).
    WrongKind,
    BadPin,
    BadPasskey,
    Request(RequestError),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct PairingView {
    pub active: bool,
    pub device: String,
    /// `waiting` (no question yet), `pin`, `passkey` (type it), `display` (type it on the device), `confirm` (compare), `authorize` (yes/no).
    pub stage: &'static str,
    pub code: String,
    /// How many digits of a displayed passkey the device has taken so far.
    pub entered: u16,
}

fn kind_of(number: i64) -> Option<Kind> {
    Some(match number {
        1 => Kind::Pin,
        2 => Kind::Passkey,
        3 => Kind::DisplayPin,
        4 => Kind::DisplayPasskey,
        5 => Kind::Confirm,
        6 => Kind::Authorize,
        7 => Kind::AuthorizeService,
        _ => return None,
    })
}

impl Pairing {
    /// Begins pairing `path`: the request to send, and from now on prompts about that device are followed.
    pub fn begin(&mut self, bluetooth: &Bluetooth, path: &str) -> Result<Request, PairingError> {
        if self.device.is_some() {
            return Err(PairingError::Request(RequestError::Busy));
        }
        let request = bluetooth.pair(path).map_err(PairingError::Request)?;
        self.device = Some(path.to_string());
        self.prompt = None;
        self.operation = None;
        self.ended.clear();
        Ok(request)
    }

    /// A `Prompt` signal. Ignored unless a pairing is under way and it is about that device.
    pub fn on_prompt(&mut self, prompt: &Value) {
        let Some(device) = &self.device else { return };
        if prompt.get("device").and_then(Value::as_str) != Some(device.as_str()) {
            return;
        }
        let Some(kind) = prompt.get("kind").and_then(Value::as_i64).and_then(kind_of) else { return };
        self.prompt = Some(Prompt {
            kind,
            passkey: prompt.get("passkey").and_then(Value::as_u64).unwrap_or(0).min(999_999) as u32,
            pin: sanitize_for_display(prompt.get("pin").and_then(Value::as_str).unwrap_or(""), 16),
            entered: prompt.get("entered").and_then(Value::as_u64).unwrap_or(0).min(6) as u16,
        });
    }

    /// The service accepted the pair request as operation `id`. If that operation already completed, the pairing is over.
    pub fn set_operation(&mut self, id: u64) {
        if self.device.is_none() {
            return;
        }
        if self.ended.contains(&id) {
            self.finish();
        } else {
            self.operation = Some(id);
        }
    }

    /// A `Completed` signal for operation `id`: ends the pairing only when it is the pair operation.
    pub fn operation_done(&mut self, id: u64) {
        if self.device.is_none() {
            return;
        }
        if self.operation == Some(id) {
            self.finish();
        } else if self.operation.is_none() && self.ended.len() < 16 {
            self.ended.push(id);
        }
    }

    /// Pairing is over (finished, failed or cancelled): forget the question.
    pub fn finish(&mut self) {
        *self = Self::default();
    }

    pub fn is_active(&self) -> bool {
        self.device.is_some()
    }

    /// Done when the device being paired shows as paired.
    pub fn follow(&mut self, bluetooth: &Bluetooth) {
        if let Some(path) = &self.device {
            if bluetooth.is_paired(path) {
                self.finish();
            }
        }
    }

    pub fn view(&self, bluetooth: &Bluetooth) -> PairingView {
        let Some(path) = &self.device else {
            return PairingView { active: false, device: String::new(), stage: "waiting", code: String::new(), entered: 0 };
        };
        let device = bluetooth.name_of(path).unwrap_or_default();
        let (stage, code, entered) = match &self.prompt {
            None => ("waiting", String::new(), 0),
            Some(p) => match p.kind {
                Kind::Pin => ("pin", String::new(), 0),
                Kind::Passkey => ("passkey", String::new(), 0),
                Kind::DisplayPin => ("display", p.pin.clone(), 0),
                Kind::DisplayPasskey => ("display", format!("{:06}", p.passkey), p.entered),
                Kind::Confirm => ("confirm", format!("{:06}", p.passkey), 0),
                Kind::Authorize | Kind::AuthorizeService => ("authorize", String::new(), 0),
            },
        };
        PairingView { active: true, device, stage, code, entered }
    }

    fn question(&self) -> Result<&Prompt, PairingError> {
        self.prompt.as_ref().ok_or(PairingError::NoQuestion)
    }

    /// The answer to a PIN or passkey question.
    pub fn answer_text(&self, text: &str) -> Result<Reply, PairingError> {
        match self.question()?.kind {
            Kind::Pin => {
                if (1..=16).contains(&text.len()) && text.chars().all(|c| (' '..='~').contains(&c)) {
                    Ok(Reply { response: text.to_string(), cancel: false })
                } else {
                    Err(PairingError::BadPin)
                }
            }
            Kind::Passkey => {
                if (1..=6).contains(&text.len()) && text.chars().all(|c| c.is_ascii_digit()) {
                    Ok(Reply { response: text.to_string(), cancel: false })
                } else {
                    Err(PairingError::BadPasskey)
                }
            }
            _ => Err(PairingError::WrongKind),
        }
    }

    /// "Yes" to a comparison or an authorisation.
    pub fn accept(&self) -> Result<Reply, PairingError> {
        match self.question()?.kind {
            Kind::Confirm | Kind::Authorize | Kind::AuthorizeService => Ok(Reply { response: String::new(), cancel: false }),
            _ => Err(PairingError::WrongKind),
        }
    }

    /// Stops pairing: asks the service to cancel it, and forgets the question.
    pub fn cancel(&mut self) -> Result<Request, PairingError> {
        let path = self.device.clone().ok_or(PairingError::NoQuestion)?;
        self.finish();
        Ok(Request { action: "cancel-pair", args: serde_json::json!({"path": path}) })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn bluetooth() -> Bluetooth {
        Bluetooth::from_snapshot(&json!({
            "available": true, "enabled": true, "scanning": true,
            "devices": [
                {"path": "/d/new", "name": "Speaker", "paired": false, "connected": false},
                {"path": "/d/old", "name": "Phone", "paired": true, "connected": false},
            ]
        }))
    }

    fn prompt(kind: i64, passkey: u64, pin: &str, entered: u64) -> Value {
        json!({"kind": kind, "device": "/d/new", "passkey": passkey, "pin": pin, "entered": entered})
    }

    fn started() -> Pairing {
        let mut pairing = Pairing::default();
        pairing.begin(&bluetooth(), "/d/new").unwrap();
        pairing
    }

    #[test]
    fn only_an_unpaired_listed_device_can_be_paired_and_only_one_at_a_time() {
        let bt = bluetooth();
        let mut pairing = Pairing::default();
        assert_eq!(pairing.begin(&bt, "/d/old").unwrap_err(), PairingError::Request(RequestError::AlreadyPaired));
        assert_eq!(pairing.begin(&bt, "/d/none").unwrap_err(), PairingError::Request(RequestError::UnknownDevice));
        assert!(!pairing.is_active(), "a refused start leaves nothing under way");
        let request = pairing.begin(&bt, "/d/new").unwrap();
        assert_eq!(request, Request { action: "pair", args: json!({"path": "/d/new"}) });
        assert_eq!(pairing.begin(&bt, "/d/new").unwrap_err(), PairingError::Request(RequestError::Busy));
        let off = Bluetooth::from_snapshot(&json!({"available": true, "enabled": false, "devices": []}));
        assert!(Pairing::default().begin(&off, "/d/new").is_err());
    }

    #[test]
    fn only_the_pair_operation_ending_ends_the_pairing() {
        let mut p = started();
        p.set_operation(7);
        p.operation_done(3);
        assert!(p.is_active(), "a scan finishing is not the pairing finishing");
        p.operation_done(7);
        assert!(!p.is_active());
        let mut early = started();
        early.operation_done(9);
        early.operation_done(4);
        assert!(early.is_active());
        early.set_operation(9);
        assert!(!early.is_active(), "the pair operation finished before its id arrived");
        let mut other = started();
        other.operation_done(4);
        other.set_operation(5);
        assert!(other.is_active());
    }

    #[test]
    fn a_question_is_shown_for_what_it_is() {
        let bt = bluetooth();
        let mut p = started();
        assert_eq!(p.view(&bt).stage, "waiting");
        assert_eq!(p.view(&bt).device, "Speaker");
        p.on_prompt(&prompt(5, 42, "", 0));
        let v = p.view(&bt);
        assert_eq!((v.stage, v.code.as_str()), ("confirm", "000042"), "a code is padded to six digits");
        p.on_prompt(&prompt(4, 123456, "", 3));
        let v = p.view(&bt);
        assert_eq!((v.stage, v.code.as_str(), v.entered), ("display", "123456", 3));
        p.on_prompt(&prompt(3, 0, "0000", 0));
        assert_eq!(p.view(&bt).code, "0000");
        p.on_prompt(&prompt(1, 0, "", 0));
        assert_eq!(p.view(&bt).stage, "pin");
        p.on_prompt(&prompt(2, 0, "", 0));
        assert_eq!(p.view(&bt).stage, "passkey");
        p.on_prompt(&prompt(6, 0, "", 0));
        assert_eq!(p.view(&bt).stage, "authorize");
        p.on_prompt(&prompt(7, 0, "", 0));
        assert_eq!(p.view(&bt).stage, "authorize");
    }

    #[test]
    fn prompts_that_are_not_for_this_pairing_are_ignored() {
        let bt = bluetooth();
        let mut idle = Pairing::default();
        idle.on_prompt(&prompt(5, 1, "", 0));
        assert!(!idle.view(&bt).active, "nothing was started");
        let mut p = started();
        p.on_prompt(&json!({"kind": 5, "device": "/d/other", "passkey": 1}));
        assert_eq!(p.view(&bt).stage, "waiting", "another device");
        p.on_prompt(&prompt(99, 1, "", 0));
        p.on_prompt(&prompt(0, 1, "", 0));
        p.on_prompt(&json!({"device": "/d/new"}));
        p.on_prompt(&json!("junk"));
        assert_eq!(p.view(&bt).stage, "waiting", "unknown or missing kinds");
    }

    #[test]
    fn a_pin_is_one_to_sixteen_printable_characters() {
        let mut p = started();
        assert_eq!(p.answer_text("1234"), Err(PairingError::NoQuestion), "nothing was asked yet");
        p.on_prompt(&prompt(1, 0, "", 0));
        assert_eq!(p.answer_text("1234"), Ok(Reply { response: "1234".into(), cancel: false }));
        assert!(p.answer_text(&"a".repeat(16)).is_ok());
        for bad in ["", &"a".repeat(17), "tab\t", "né", "line\n"] {
            assert_eq!(p.answer_text(bad), Err(PairingError::BadPin), "{bad:?}");
        }
    }

    #[test]
    fn a_passkey_is_at_most_six_digits() {
        let mut p = started();
        p.on_prompt(&prompt(2, 0, "", 0));
        assert!(p.answer_text("0").is_ok());
        assert!(p.answer_text("123456").is_ok());
        for bad in ["", "1234567", "12a4", "-1", " 12", "1.5"] {
            assert_eq!(p.answer_text(bad), Err(PairingError::BadPasskey), "{bad:?}");
        }
    }

    #[test]
    fn an_answer_has_to_fit_the_question() {
        let mut p = started();
        p.on_prompt(&prompt(5, 7, "", 0));
        assert!(p.accept().is_ok());
        assert_eq!(p.answer_text("123456"), Err(PairingError::WrongKind), "text for a yes/no");
        p.on_prompt(&prompt(1, 0, "", 0));
        assert_eq!(p.accept(), Err(PairingError::WrongKind), "a yes for a PIN");
        p.on_prompt(&prompt(4, 5, "", 0));
        assert_eq!(p.accept(), Err(PairingError::WrongKind), "nothing to answer for a displayed code");
        assert_eq!(p.answer_text("5"), Err(PairingError::WrongKind));
        p.on_prompt(&prompt(6, 0, "", 0));
        assert_eq!(p.accept(), Ok(Reply { response: String::new(), cancel: false }));
    }

    #[test]
    fn finishing_or_cancelling_forgets_everything() {
        let bt = bluetooth();
        let mut p = started();
        p.on_prompt(&prompt(5, 7, "", 0));
        let cancel = p.cancel().unwrap();
        assert_eq!(cancel, Request { action: "cancel-pair", args: json!({"path": "/d/new"}) });
        assert!(!p.is_active() && p.accept() == Err(PairingError::NoQuestion));
        assert_eq!(p.cancel(), Err(PairingError::NoQuestion), "nothing left to cancel");
        let mut p = started();
        p.on_prompt(&prompt(5, 7, "", 0));
        p.follow(&bt);
        assert!(p.is_active(), "still unpaired: still going");
        let paired = Bluetooth::from_snapshot(&json!({"available": true, "enabled": true, "devices": [{"path": "/d/new", "name": "Speaker", "paired": true}]}));
        p.follow(&paired);
        assert!(!p.is_active() && p.accept() == Err(PairingError::NoQuestion), "paired: done");
    }

    #[test]
    fn what_was_typed_is_never_printed() {
        let mut p = started();
        p.on_prompt(&prompt(1, 0, "", 0));
        let reply = p.answer_text("s3cretpin").unwrap();
        assert!(!format!("{reply:?}").contains("s3cretpin"));
        assert_eq!(reply.response, "s3cretpin");
    }

    #[test]
    fn a_displayed_pin_is_clean_text() {
        let bt = bluetooth();
        let mut p = started();
        p.on_prompt(&json!({"kind": 3, "device": "/d/new", "pin": "12\u{202e}34\n"}));
        let code = p.view(&bt).code;
        assert!(!code.contains('\u{202e}') && !code.contains('\n'), "{code:?}");
    }
}
