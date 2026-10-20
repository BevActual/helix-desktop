//! The Write stage's operations: a small fixed set of edits to a document's text, the same language whether a person's button or the model's
//! `docops` block asks for it. Each is checked against the text (the passage must exist, and be the only one unless "all" is said), applied by
//! plain code, and lands as a new version, so Undo is History. Nothing here runs anything; unknown operations and fields are refused.

use serde::{Deserialize, Serialize};

pub const MAX_OPS: usize = 10;
const MAX_TEXT_CHARS: usize = 20_000;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum DocOp {
    /// Replaces a passage with other text.
    Replace {
        find: String,
        with: String,
        #[serde(default)]
        all: bool,
    },
    /// Puts text after the line that holds the anchor.
    InsertAfter { anchor: String, text: String },
    /// Replaces everything under a heading, up to the next heading of the same or a higher level. The heading stays.
    ReplaceSection { heading: String, text: String },
    /// Adds text at the end.
    Append { text: String },
    /// Removes a passage.
    Delete { find: String },
}

fn check_text(label: &str, text: &str) -> Result<(), String> {
    if text.chars().count() > MAX_TEXT_CHARS {
        return Err(format!("The {label} is too long for one edit."));
    }
    Ok(())
}

fn shorten(text: &str) -> String {
    let one: String = text.split_whitespace().collect::<Vec<_>>().join(" ");
    if one.chars().count() > 40 {
        format!("{}…", one.chars().take(40).collect::<String>())
    } else {
        one
    }
}

/// The one place `find` occurs (or every place with `all`), as byte ranges.
fn locate(body: &str, find: &str, all: bool) -> Result<Vec<(usize, usize)>, String> {
    if find.trim().is_empty() {
        return Err("An edit needs the passage it changes.".into());
    }
    let hits: Vec<(usize, usize)> = body
        .match_indices(find)
        .map(|(i, m)| (i, i + m.len()))
        .collect();
    match hits.len() {
        0 => Err(format!(
            "Could not find “{}” in the document.",
            shorten(find)
        )),
        1 => Ok(hits),
        _ if all => Ok(hits),
        n => Err(format!(
            "“{}” appears {n} times. Say which one, or ask to change all of them.",
            shorten(find)
        )),
    }
}

/// Lines with their byte offsets, and whether each is inside a code fence.
fn lines(body: &str) -> Vec<(usize, &str, bool)> {
    let mut out = Vec::new();
    let mut offset = 0;
    let mut fenced = false;
    for line in body.split_inclusive('\n') {
        let bare = line.trim_end_matches(['\n', '\r']);
        let fence = bare.trim_start().starts_with("```") || bare.trim_start().starts_with("~~~");
        out.push((offset, bare, fenced || fence));
        if fence {
            fenced = !fenced;
        }
        offset += line.len();
    }
    out
}

fn heading_of(line: &str) -> Option<(usize, &str)> {
    let hashes = line.chars().take_while(|c| *c == '#').count();
    if (1..=6).contains(&hashes) && line[hashes..].starts_with(' ') {
        Some((hashes, line[hashes..].trim().trim_end_matches('#').trim()))
    } else {
        None
    }
}

fn apply_one(body: &str, op: &DocOp) -> Result<String, String> {
    match op {
        DocOp::Replace { find, with, all } => {
            check_text("new text", with)?;
            let hits = locate(body, find, *all)?;
            let mut out = String::with_capacity(body.len());
            let mut last = 0;
            for (start, end) in hits {
                out.push_str(&body[last..start]);
                out.push_str(with);
                last = end;
            }
            out.push_str(&body[last..]);
            Ok(out)
        }
        DocOp::Delete { find } => {
            let (start, end) = locate(body, find, false)?[0];
            Ok(format!("{}{}", &body[..start], &body[end..]))
        }
        DocOp::InsertAfter { anchor, text } => {
            check_text("new text", text)?;
            let (start, _) = locate(body, anchor, false)?[0];
            let end = body[start..].find('\n').map_or(body.len(), |i| start + i);
            let line_start = body[..start].rfind('\n').map_or(0, |i| i + 1);
            let list = {
                let l = body[line_start..end].trim_start();
                l.starts_with("- ")
                    || l.starts_with("* ")
                    || l.chars().next().is_some_and(|c| c.is_ascii_digit()) && l.contains(". ")
            };
            let gap = if list { "\n" } else { "\n\n" };
            Ok(format!(
                "{}{gap}{}{}",
                &body[..end],
                text.trim_end(),
                &body[end..]
            ))
        }
        DocOp::Append { text } => {
            check_text("new text", text)?;
            if text.trim().is_empty() {
                return Err("There is nothing to add.".into());
            }
            Ok(format!("{}\n\n{}\n", body.trim_end(), text.trim_end()))
        }
        DocOp::ReplaceSection { heading, text } => {
            check_text("new text", text)?;
            let want = heading.trim().trim_start_matches('#').trim().to_lowercase();
            let all = lines(body);
            let matches: Vec<usize> = all
                .iter()
                .enumerate()
                .filter(|(_, (_, l, fenced))| {
                    !fenced && heading_of(l).is_some_and(|(_, t)| t.to_lowercase() == want)
                })
                .map(|(i, _)| i)
                .collect();
            let at = match matches.as_slice() {
                [] => return Err(format!("There is no heading “{}”.", shorten(heading))),
                [one] => *one,
                _ => {
                    return Err(format!(
                        "More than one heading is called “{}”.",
                        shorten(heading)
                    ))
                }
            };
            let level = heading_of(all[at].1).map_or(1, |(n, _)| n);
            let end = all[at + 1..]
                .iter()
                .find(|(_, l, fenced)| !fenced && heading_of(l).is_some_and(|(n, _)| n <= level))
                .map_or(body.len(), |(o, _, _)| *o);
            let head_end = all.get(at + 1).map_or(body.len(), |(o, _, _)| *o);
            let tail = body[end..].to_string();
            let sep = if tail.is_empty() { "\n" } else { "\n\n" };
            Ok(format!(
                "{}\n{}{sep}{}",
                body[..head_end].trim_end(),
                text.trim(),
                tail
            ))
        }
    }
}

/// Applies the operations in order. All or nothing: one that cannot be done stops the lot, and says which.
pub fn apply(body: &str, ops: &[DocOp]) -> Result<String, String> {
    if ops.is_empty() || ops.len() > MAX_OPS {
        return Err(format!("Apply between one and {MAX_OPS} edits at a time."));
    }
    let mut current = body.to_string();
    for (i, op) in ops.iter().enumerate() {
        current =
            apply_one(&current, op).map_err(|e| format!("Edit {} of {}: {e}", i + 1, ops.len()))?;
    }
    if current.chars().count() > crate::artifacts::MAX_BODY_CHARS {
        return Err("The result is too long for the stage.".into());
    }
    Ok(current)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn op(json: &str) -> DocOp {
        serde_json::from_str(json).unwrap()
    }

    #[test]
    fn replaces_the_one_passage_and_refuses_ambiguity() {
        let body = "Hello world. Bye world.";
        assert_eq!(
            apply(
                body,
                &[op(r#"{"op":"replace","find":"Hello","with":"Hi"}"#)]
            )
            .unwrap(),
            "Hi world. Bye world."
        );
        let e = apply(
            body,
            &[op(r#"{"op":"replace","find":"world","with":"all"}"#)],
        )
        .unwrap_err();
        assert!(e.contains("2 times"), "{e}");
        assert_eq!(
            apply(
                body,
                &[op(
                    r#"{"op":"replace","find":"world","with":"all","all":true}"#
                )]
            )
            .unwrap(),
            "Hello all. Bye all."
        );
        assert!(
            apply(body, &[op(r#"{"op":"replace","find":"nope","with":"x"}"#)])
                .unwrap_err()
                .contains("Could not find")
        );
    }

    #[test]
    fn replaces_a_section_up_to_the_next_heading_of_its_level() {
        let body =
            "# Plan\n\nIntro\n\n## Risks\n\nold one\n\n### Detail\n\nold two\n\n## Next\n\nstay\n";
        let out = apply(
            body,
            &[op(
                r#"{"op":"replace_section","heading":"Risks","text":"new text"}"#,
            )],
        )
        .unwrap();
        assert_eq!(
            out,
            "# Plan\n\nIntro\n\n## Risks\nnew text\n\n## Next\n\nstay\n"
        );
        let last = apply(
            body,
            &[op(
                r###"{"op":"replace_section","heading":"## Next","text":"fresh"}"###,
            )],
        )
        .unwrap();
        assert!(last.ends_with("## Next\nfresh\n"), "{last}");
        assert!(apply(
            body,
            &[op(
                r#"{"op":"replace_section","heading":"Missing","text":"x"}"#
            )]
        )
        .is_err());
    }

    #[test]
    fn headings_inside_a_code_fence_are_not_headings() {
        let body = "## A\n\n```\n## A\n```\n";
        assert!(apply(
            body,
            &[op(r#"{"op":"replace_section","heading":"A","text":"x"}"#)]
        )
        .is_ok());
    }

    #[test]
    fn inserts_after_a_line_and_appends() {
        let body = "One\n\nTwo\n";
        assert_eq!(
            apply(
                body,
                &[op(r#"{"op":"insert_after","anchor":"One","text":"Mid"}"#)]
            )
            .unwrap(),
            "One\n\nMid\n\nTwo\n"
        );
        let list = "- a\n- b\n";
        assert_eq!(
            apply(
                list,
                &[op(r#"{"op":"insert_after","anchor":"- a","text":"- a2"}"#)]
            )
            .unwrap(),
            "- a\n- a2\n- b\n"
        );
        assert_eq!(
            apply(body, &[op(r#"{"op":"append","text":"End"}"#)]).unwrap(),
            "One\n\nTwo\n\nEnd\n"
        );
    }

    #[test]
    fn deletes_and_is_all_or_nothing() {
        let body = "keep this. drop this.";
        assert_eq!(
            apply(body, &[op(r#"{"op":"delete","find":" drop this."}"#)]).unwrap(),
            "keep this."
        );
        let both = [
            op(r#"{"op":"delete","find":"keep"}"#),
            op(r#"{"op":"delete","find":"absent"}"#),
        ];
        let e = apply(body, &both).unwrap_err();
        assert!(e.starts_with("Edit 2 of 2"), "{e}");
    }

    #[test]
    fn refuses_unknown_operations_fields_and_too_many() {
        assert!(serde_json::from_str::<DocOp>(r#"{"op":"run","cmd":"ls"}"#).is_err());
        assert!(
            serde_json::from_str::<DocOp>(r#"{"op":"append","text":"x","path":"/etc"}"#).is_err()
        );
        let many: Vec<DocOp> = (0..11)
            .map(|_| op(r#"{"op":"append","text":"x"}"#))
            .collect();
        assert!(apply("a", &many).is_err());
    }
}
