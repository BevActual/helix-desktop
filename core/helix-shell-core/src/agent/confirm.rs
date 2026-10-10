//! Confirmation arming. Enter never confirms. Do it is ignored until [`ARMING_MS`] after the
//! prompt is shown. Escape denies.

pub const ARMING_MS: u64 = 700;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Decision {
    Pending,
    Confirmed,
    Denied,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Confirmation {
    shown_at: Option<u64>,
    decision: Decision,
}

impl Default for Confirmation {
    fn default() -> Self {
        Self::new()
    }
}

impl Confirmation {
    pub fn new() -> Self {
        Self {
            shown_at: None,
            decision: Decision::Pending,
        }
    }

    pub fn show(&mut self, now_ms: u64) {
        self.shown_at = Some(now_ms);
        self.decision = Decision::Pending;
    }

    pub fn is_pending(&self) -> bool {
        self.shown_at.is_some() && self.decision == Decision::Pending
    }

    pub fn armed(&self, now_ms: u64) -> bool {
        match self.shown_at {
            Some(shown) if self.decision == Decision::Pending => {
                now_ms.saturating_sub(shown) >= ARMING_MS
            }
            _ => false,
        }
    }

    /// Enter never confirms, before or after the prompt is armed.
    pub fn enter(&mut self, _now_ms: u64) -> bool {
        false
    }

    /// Do it. Ignored until the arming delay has elapsed, and only once.
    pub fn activate(&mut self, now_ms: u64) -> bool {
        if !self.armed(now_ms) {
            return false;
        }
        self.decision = Decision::Confirmed;
        true
    }

    /// Escape denies a prompt that is still waiting. Returns whether this call denied it.
    pub fn escape(&mut self) -> bool {
        if !self.is_pending() {
            return false;
        }
        self.decision = Decision::Denied;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::{Confirmation, ARMING_MS};

    #[test]
    fn enter_never_confirms() {
        let mut prompt = Confirmation::new();
        prompt.show(0);
        assert!(!prompt.enter(0));
        assert!(!prompt.enter(ARMING_MS));
        assert!(prompt.is_pending());
        assert!(!prompt.activate(ARMING_MS - 1));
        assert!(prompt.activate(ARMING_MS));
        assert!(!prompt.enter(ARMING_MS + 1));
    }

    #[test]
    fn do_it_arms_at_700_ms_and_not_before() {
        let mut prompt = Confirmation::new();
        assert!(!prompt.activate(ARMING_MS));
        prompt.show(1_000);
        assert!(!prompt.armed(1_000 + ARMING_MS - 1));
        assert!(!prompt.activate(1_000 + ARMING_MS - 1));
        assert!(prompt.is_pending());
        assert!(prompt.armed(1_000 + ARMING_MS));
        assert!(prompt.activate(1_000 + ARMING_MS));
        assert!(!prompt.activate(1_000 + ARMING_MS + 5));
    }

    #[test]
    fn escape_denies_before_and_after_arming() {
        let mut early = Confirmation::new();
        early.show(0);
        assert!(early.escape());
        assert!(!early.activate(ARMING_MS));
        assert!(!early.escape());

        let mut armed = Confirmation::new();
        armed.show(0);
        assert!(armed.armed(ARMING_MS));
        assert!(armed.escape());
        assert!(!armed.activate(ARMING_MS + 1));
    }

    #[test]
    fn escape_with_nothing_showing_does_nothing() {
        let mut prompt = Confirmation::new();
        assert!(!prompt.escape());
        assert!(!prompt.activate(ARMING_MS));
    }
}
