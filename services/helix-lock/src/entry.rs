//! What the person has typed so far, and what the screen says about it. Plain data, no Wayland and no drawing, so it is tested on its own.
//! The password lives only in this buffer, is handed to PAM once, and is overwritten whenever it is cleared or dropped.

use std::time::{Duration, Instant};

/// The longest password accepted. Anything longer is not typed, rather than cut: a cut password would fail in a way nobody could explain.
pub const MAX_BYTES: usize = 512;

/// How long the field shakes after a wrong password.
pub const SHAKE: Duration = Duration::from_millis(520);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    /// Waiting for typing.
    Idle,
    /// PAM has the password.
    Checking,
    /// The last attempt was refused.
    Wrong,
}

pub struct Entry {
    bytes: Vec<u8>,
    chars: usize,
    pub phase: Phase,
    wrong_at: Option<Instant>,
}

/// Overwrites a buffer in a way the compiler may not drop.
fn wipe(buffer: &mut [u8]) {
    for byte in buffer.iter_mut() {
        // SAFETY: a valid, aligned, exclusive pointer into the slice.
        unsafe { std::ptr::write_volatile(byte, 0) };
    }
    std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
}

impl Default for Entry {
    fn default() -> Self {
        Entry {
            bytes: Vec::with_capacity(MAX_BYTES),
            chars: 0,
            phase: Phase::Idle,
            wrong_at: None,
        }
    }
}

impl Drop for Entry {
    fn drop(&mut self) {
        wipe(&mut self.bytes);
    }
}

impl Entry {
    /// Adds a typed character. Control characters and anything past the limit are ignored. Typing while a check runs is ignored too.
    pub fn push(&mut self, c: char) -> bool {
        if self.phase == Phase::Checking
            || c.is_control()
            || self.bytes.len() + c.len_utf8() > MAX_BYTES
        {
            return false;
        }
        let mut utf8 = [0u8; 4];
        self.bytes
            .extend_from_slice(c.encode_utf8(&mut utf8).as_bytes());
        wipe(&mut utf8);
        self.chars += 1;
        if self.phase == Phase::Wrong {
            self.phase = Phase::Idle;
        }
        true
    }

    /// Removes the last character.
    pub fn pop(&mut self) {
        if self.phase == Phase::Checking {
            return;
        }
        if let Some(c) = std::str::from_utf8(&self.bytes)
            .ok()
            .and_then(|s| s.chars().next_back())
        {
            let keep = self.bytes.len() - c.len_utf8();
            wipe(&mut self.bytes[keep..]);
            self.bytes.truncate(keep);
            self.chars -= 1;
        }
        if self.phase == Phase::Wrong {
            self.phase = Phase::Idle;
        }
    }

    pub fn clear(&mut self) {
        if self.phase == Phase::Checking {
            return;
        }
        wipe(&mut self.bytes);
        self.bytes.clear();
        self.chars = 0;
        if self.phase == Phase::Wrong {
            self.phase = Phase::Idle;
        }
    }

    /// How many dots to draw.
    pub fn dots(&self) -> usize {
        self.chars
    }

    pub fn is_empty(&self) -> bool {
        self.chars == 0
    }

    /// Hands the password over for checking (and starts the "Checking" phase). `None` when there is nothing to check or one is already running.
    pub fn begin_check(&mut self) -> Option<String> {
        if self.phase == Phase::Checking || self.bytes.is_empty() {
            return None;
        }
        let text = String::from_utf8(self.bytes.clone()).ok()?;
        self.phase = Phase::Checking;
        Some(text)
    }

    /// The check said no: the field empties and shakes.
    pub fn refused(&mut self, now: Instant) {
        wipe(&mut self.bytes);
        self.bytes.clear();
        self.chars = 0;
        self.phase = Phase::Wrong;
        self.wrong_at = Some(now);
    }

    /// Sideways offset of the field, in logical pixels, a damped wobble for [`SHAKE`] after a refusal.
    pub fn shake(&self, now: Instant) -> f64 {
        let Some(at) = self.wrong_at.filter(|_| self.phase == Phase::Wrong) else {
            return 0.0;
        };
        let t = now.saturating_duration_since(at);
        if t >= SHAKE {
            return 0.0;
        }
        let progress = t.as_secs_f64() / SHAKE.as_secs_f64();
        (progress * std::f64::consts::TAU * 3.0).sin() * 14.0 * (1.0 - progress)
    }

    pub fn shaking(&self, now: Instant) -> bool {
        self.phase == Phase::Wrong
            && self
                .wrong_at
                .is_some_and(|at| now.saturating_duration_since(at) < SHAKE)
    }

    /// The line under the field.
    pub fn message(&self, caps_lock: bool) -> &'static str {
        match self.phase {
            Phase::Checking => "Checking…",
            Phase::Wrong => "That is not right. Try again.",
            Phase::Idle if caps_lock => "Caps Lock is on",
            Phase::Idle => "",
        }
    }
}

/// Up to two capital letters for the avatar: the first and last word of the name.
pub fn initials(name: &str) -> String {
    let words: Vec<&str> = name.split_whitespace().collect();
    let first = words.first().and_then(|w| w.chars().next());
    let last = if words.len() > 1 {
        words.last().and_then(|w| w.chars().next())
    } else {
        None
    };
    let mut out: String = first
        .into_iter()
        .chain(last)
        .flat_map(char::to_uppercase)
        .collect();
    if out.is_empty() {
        out.push('?');
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn type_all(entry: &mut Entry, text: &str) {
        for c in text.chars() {
            entry.push(c);
        }
    }

    #[test]
    fn typing_and_deleting_keep_the_dots_in_step_with_characters_not_bytes() {
        let mut entry = Entry::default();
        type_all(&mut entry, "pä€w");
        assert_eq!(entry.dots(), 4);
        entry.pop();
        entry.pop();
        assert_eq!(entry.dots(), 2);
        assert_eq!(entry.begin_check().as_deref(), Some("pä"));
        entry.phase = Phase::Idle;
        entry.clear();
        assert!(entry.is_empty());
        assert_eq!(entry.begin_check(), None);
    }

    #[test]
    fn controls_and_overlong_input_are_not_typed() {
        let mut entry = Entry::default();
        assert!(!entry.push('\n') && !entry.push('\u{7f}') && !entry.push('\t'));
        for _ in 0..MAX_BYTES + 20 {
            entry.push('a');
        }
        assert_eq!(entry.dots(), MAX_BYTES);
        assert!(
            !entry.push('é'),
            "a character that does not fit is not cut in half"
        );
    }

    #[test]
    fn a_check_blocks_typing_until_it_answers_and_a_refusal_empties_and_shakes() {
        let mut entry = Entry::default();
        type_all(&mut entry, "secret");
        assert_eq!(entry.begin_check().as_deref(), Some("secret"));
        assert_eq!(entry.phase, Phase::Checking);
        assert!(!entry.push('x'));
        entry.pop();
        entry.clear();
        assert_eq!(entry.dots(), 6, "nothing changes while PAM has it");
        assert_eq!(entry.begin_check(), None, "and it cannot be checked twice");
        let now = Instant::now();
        entry.refused(now);
        assert_eq!((entry.dots(), entry.phase), (0, Phase::Wrong));
        assert!(entry.shaking(now) && entry.shake(now + Duration::from_millis(60)).abs() > 0.5);
        assert_eq!(entry.shake(now + SHAKE), 0.0);
        assert_eq!(entry.message(false), "That is not right. Try again.");
        assert!(entry.push('a'));
        assert_eq!(
            entry.phase,
            Phase::Idle,
            "typing again clears the complaint"
        );
        assert_eq!(entry.message(true), "Caps Lock is on");
    }

    #[test]
    fn the_avatar_shows_first_and_last_initial() {
        assert_eq!(initials("Andrew Lance Bevington"), "AB");
        assert_eq!(initials("ada"), "A");
        assert_eq!(initials("  "), "?");
        assert_eq!(initials("élodie dupont"), "ÉD");
    }
}
