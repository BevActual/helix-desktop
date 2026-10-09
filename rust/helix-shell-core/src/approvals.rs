//! The person's choice about approving without protection (`~/.config/helix/approvals.json`): the shell reads it when an approval dialog opens,
//! Settings writes it. A missing, unreadable or odd file means "not allowed": the safe answer is the default, and only a literal `true` changes it.

use serde_json::{json, Value};

/// Whether `text` (the file's content) says approvals without protection are allowed.
pub fn allow_unprotected(text: &str) -> bool {
    serde_json::from_str::<Value>(text)
        .ok()
        .and_then(|value| value.get("allow_unprotected").and_then(Value::as_bool))
        .unwrap_or(false)
}

/// The file's content for the choice.
pub fn render(allow_unprotected: bool) -> String {
    format!("{}\n", json!({"version": 1, "allow_unprotected": allow_unprotected}))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_literal_true_allows() {
        assert!(allow_unprotected(r#"{"allow_unprotected": true}"#));
        for text in [
            "", "{}", "null", "[]", "not json", r#"{"allow_unprotected": false}"#, r#"{"allow_unprotected": "true"}"#,
            r#"{"allow_unprotected": 1}"#, r#"{"allow_unprotected": null}"#, r#"{"Allow_Unprotected": true}"#, r#"{"a": {"allow_unprotected": true}}"#,
        ] {
            assert!(!allow_unprotected(text), "{text:?}");
        }
    }

    #[test]
    fn a_written_choice_reads_back_and_is_valid_json() {
        for choice in [true, false] {
            let text = render(choice);
            assert_eq!(allow_unprotected(&text), choice);
            assert!(serde_json::from_str::<Value>(&text).is_ok());
            assert!(text.ends_with('\n'));
        }
    }
}
