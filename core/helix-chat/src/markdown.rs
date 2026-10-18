//! A small, safe Markdown reader for what the model writes. It produces blocks and spans as plain data; the page builds elements from them
//! with `textContent`, so no text from the model is ever interpreted as HTML, and links are shown as text (never made clickable).
//! Unfinished input (a reply still streaming) is fine: an open code fence is simply code so far.

use serde::Serialize;

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Span {
    Text { text: String },
    Code { text: String },
    Bold { text: String },
    Italic { text: String },
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Block {
    Paragraph {
        spans: Vec<Span>,
    },
    Heading {
        level: u8,
        spans: Vec<Span>,
    },
    Code {
        language: String,
        text: String,
    },
    List {
        ordered: bool,
        items: Vec<Vec<Span>>,
    },
    Quote {
        spans: Vec<Span>,
    },
    Table {
        headers: Vec<Vec<Span>>,
        rows: Vec<Vec<Vec<Span>>>,
        alignments: Vec<Alignment>,
    },
    /// A reasoning model's `<think>` text, shown folded. `done` is false while it is still being written.
    Thinking {
        text: String,
        done: bool,
    },
    Rule,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Alignment {
    Left,
    Center,
    Right,
}

/// Split cells without treating escaped pipes or pipes inside inline code as separators.
fn table_cells(line: &str) -> Option<Vec<String>> {
    let mut cells = Vec::new();
    let mut cell = String::new();
    let mut escaped = false;
    let mut code = false;
    let mut separators = 0;
    for c in line.trim().chars() {
        if escaped {
            if c != '|' {
                cell.push('\\');
            }
            cell.push(c);
            escaped = false;
        } else if c == '\\' {
            escaped = true;
        } else if c == '`' {
            code = !code;
            cell.push(c);
        } else if c == '|' && !code {
            cells.push(cell.trim().to_string());
            cell.clear();
            separators += 1;
        } else {
            cell.push(c);
        }
    }
    if escaped {
        cell.push('\\');
    }
    cells.push(cell.trim().to_string());
    if separators == 0 {
        return None;
    }
    if line.trim().starts_with('|') {
        cells.remove(0);
    }
    if line.trim().ends_with('|') && cells.last().is_some_and(String::is_empty) {
        cells.pop();
    }
    if cells.is_empty() {
        None
    } else {
        Some(cells)
    }
}

fn table_alignment(line: &str) -> Option<Vec<Alignment>> {
    table_cells(line)?
        .iter()
        .map(|cell| {
            let dashes = cell.trim_matches(':');
            if dashes.len() < 3 || !dashes.chars().all(|c| c == '-') {
                return None;
            }
            Some(if cell.ends_with(':') {
                if cell.starts_with(':') {
                    Alignment::Center
                } else {
                    Alignment::Right
                }
            } else {
                Alignment::Left
            })
        })
        .collect()
}

fn text(s: &str) -> Span {
    Span::Text {
        text: s.to_string(),
    }
}

/// Inline spans: `code`, **bold**, *italic* / _italic_, and [text](url) shown as `text (url)`.
pub fn spans(input: &str) -> Vec<Span> {
    let chars: Vec<char> = input.chars().collect();
    let mut out: Vec<Span> = Vec::new();
    let mut plain = String::new();
    let flush = |plain: &mut String, out: &mut Vec<Span>| {
        if !plain.is_empty() {
            out.push(text(plain));
            plain.clear();
        }
    };
    let find = |from: usize, pattern: &[char]| -> Option<usize> {
        (from..chars.len().saturating_sub(pattern.len() - 1))
            .find(|&i| chars[i..i + pattern.len()] == *pattern)
    };
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c == '`' {
            if let Some(end) = find(i + 1, &['`']) {
                flush(&mut plain, &mut out);
                out.push(Span::Code {
                    text: chars[i + 1..end].iter().collect(),
                });
                i = end + 1;
                continue;
            }
        } else if c == '*' && chars.get(i + 1) == Some(&'*') {
            if let Some(end) = find(i + 2, &['*', '*']) {
                if end > i + 2 {
                    flush(&mut plain, &mut out);
                    out.push(Span::Bold {
                        text: chars[i + 2..end].iter().collect(),
                    });
                    i = end + 2;
                    continue;
                }
            }
        } else if (c == '*' || c == '_')
            && chars
                .get(i + 1)
                .is_some_and(|n| !n.is_whitespace() && *n != c)
        {
            let before_ok = i == 0 || !chars[i - 1].is_alphanumeric();
            if before_ok {
                if let Some(end) = find(i + 1, &[c]) {
                    if end > i + 1
                        && !chars[end - 1].is_whitespace()
                        && chars.get(end + 1).is_none_or(|n| !n.is_alphanumeric())
                    {
                        flush(&mut plain, &mut out);
                        out.push(Span::Italic {
                            text: chars[i + 1..end].iter().collect(),
                        });
                        i = end + 1;
                        continue;
                    }
                }
            }
        } else if c == '[' {
            if let Some(close) = find(i + 1, &[']']) {
                if chars.get(close + 1) == Some(&'(') {
                    if let Some(end) = find(close + 2, &[')']) {
                        let label: String = chars[i + 1..close].iter().collect();
                        let url: String = chars[close + 2..end].iter().collect();
                        plain.push_str(&format!("{label} ({url})"));
                        i = end + 1;
                        continue;
                    }
                }
            }
        }
        plain.push(c);
        i += 1;
    }
    flush(&mut plain, &mut out);
    out
}

fn list_item(line: &str) -> Option<(bool, &str)> {
    let t = line.trim_start();
    for marker in ["- ", "* ", "+ "] {
        if let Some(rest) = t.strip_prefix(marker) {
            return Some((false, rest));
        }
    }
    let digits = t.chars().take_while(char::is_ascii_digit).count();
    if (1..=3).contains(&digits) {
        if let Some(rest) = t[digits..]
            .strip_prefix(". ")
            .or_else(|| t[digits..].strip_prefix(") "))
        {
            return Some((true, rest));
        }
    }
    None
}

/// The text of a reply that is still being written, with an emphasis or code mark that has not been closed yet closed for it, so the words
/// after `**` show bold as they arrive instead of the two stars showing until the closing pair comes. A mark with nothing after it yet is left out.
pub fn close_open_marks(text: &str) -> String {
    // Only the last line can have an open mark (and only outside a code fence).
    let fences = text
        .lines()
        .filter(|l| l.trim_start().starts_with("```"))
        .count();
    if fences % 2 == 1 {
        return text.to_string();
    }
    let start = text.rfind('\n').map_or(0, |i| i + 1);
    let line: Vec<char> = text[start..].chars().collect();
    let (mut in_code, mut in_bold) = (false, false);
    let (mut open_at, mut i) = (None, 0usize);
    while i < line.len() {
        if line[i] == '`' {
            in_code = !in_code;
            open_at = in_code.then_some(i);
        } else if !in_code && line[i] == '*' && line.get(i + 1) == Some(&'*') {
            in_bold = !in_bold;
            open_at = in_bold.then_some(i);
            i += 1;
        }
        i += 1;
    }
    let mut out = text[..start].to_string();
    let mut kept: String = line.iter().collect();
    if in_code || in_bold {
        let at = open_at.unwrap_or(0);
        let width = if in_code { 1 } else { 2 };
        if at + width >= line.len() {
            // nothing after the mark yet
            kept = line[..at].iter().collect();
        } else {
            kept.push_str(if in_code { "`" } else { "**" });
        }
    }
    out.push_str(&kept);
    out
}

/// Splits the `<think>…</think>` text a reasoning model writes before its answer: the thinking (and whether it has ended) and the answer.
/// Only a tag at the very start counts, so a reply that merely mentions the tag is left alone.
pub fn split_thinking(text: &str) -> (Option<(String, bool)>, &str) {
    let trimmed = text.trim_start();
    let Some(rest) = trimmed.strip_prefix("<think>") else {
        return (None, text);
    };
    match rest.split_once("</think>") {
        Some((thought, answer)) => (
            Some((thought.trim().to_string(), true)),
            answer.trim_start(),
        ),
        None => (Some((rest.trim().to_string(), false)), ""),
    }
}

/// The answer without the thinking: what is shown as the reply and what is sent back to a model as history.
pub fn strip_thinking(text: &str) -> String {
    split_thinking(text).1.to_string()
}

/// A reply as blocks: a folded Thinking block first when the model thought aloud, then the answer.
pub fn parse_reply(text: &str) -> Vec<Block> {
    let (thinking, answer) = split_thinking(text);
    let mut blocks = Vec::new();
    if let Some((text, done)) = thinking {
        if !text.is_empty() || !done {
            blocks.push(Block::Thinking { text, done });
        }
    }
    blocks.extend(parse(answer));
    blocks
}

pub fn parse(input: &str) -> Vec<Block> {
    let mut blocks: Vec<Block> = Vec::new();
    let mut paragraph: Vec<&str> = Vec::new();
    let mut list: Option<(bool, Vec<Vec<Span>>)> = None;
    let mut fence: Option<(String, Vec<&str>)> = None;

    fn end_paragraph(blocks: &mut Vec<Block>, paragraph: &mut Vec<&str>) {
        if !paragraph.is_empty() {
            blocks.push(Block::Paragraph {
                spans: spans(&paragraph.join("\n")),
            });
            paragraph.clear();
        }
    }
    fn end_list(blocks: &mut Vec<Block>, list: &mut Option<(bool, Vec<Vec<Span>>)>) {
        if let Some((ordered, items)) = list.take() {
            blocks.push(Block::List { ordered, items });
        }
    }

    let mut lines = input.lines().peekable();
    while let Some(line) = lines.next() {
        if let Some((language, body)) = &mut fence {
            if line.trim_start().starts_with("```") {
                blocks.push(Block::Code {
                    language: language.clone(),
                    text: body.join("\n"),
                });
                fence = None;
            } else {
                body.push(line);
            }
            continue;
        }
        let trimmed = line.trim_start();
        if let Some(rest) = trimmed.strip_prefix("```") {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            let language: String = match rest
                .trim()
                .get(..9)
                .filter(|p| p.eq_ignore_ascii_case("artifact:"))
            {
                // A document for the stage keeps its title: `artifact:Quarterly plan`.
                Some(_) => {
                    let title: String = rest.trim()[9..]
                        .chars()
                        .filter(|c| c.is_alphanumeric() || " -_.,'()&".contains(*c))
                        .take(80)
                        .collect();
                    format!("artifact:{}", title.trim())
                }
                None => rest
                    .trim()
                    .chars()
                    .filter(|c| c.is_ascii_alphanumeric() || "+-#._".contains(*c))
                    .take(20)
                    .collect(),
            };
            fence = Some((language, Vec::new()));
            continue;
        }
        if trimmed.is_empty() {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            continue;
        }
        if let Some(headers) = table_cells(line) {
            if let Some(alignments) = lines.peek().and_then(|next| table_alignment(next)) {
                if headers.len() == alignments.len() {
                    end_paragraph(&mut blocks, &mut paragraph);
                    end_list(&mut blocks, &mut list);
                    lines.next();
                    let mut rows = Vec::new();
                    while let Some(cells) = lines.peek().and_then(|next| table_cells(next)) {
                        lines.next();
                        let mut row: Vec<Vec<Span>> = cells
                            .iter()
                            .take(headers.len())
                            .map(|cell| spans(cell))
                            .collect();
                        row.resize(headers.len(), Vec::new());
                        rows.push(row);
                    }
                    blocks.push(Block::Table {
                        headers: headers.iter().map(|cell| spans(cell)).collect(),
                        rows,
                        alignments,
                    });
                    continue;
                }
            }
        }
        let hashes = trimmed.chars().take_while(|c| *c == '#').count();
        if (1..=6).contains(&hashes) && trimmed[hashes..].starts_with(' ') {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            blocks.push(Block::Heading {
                level: hashes as u8,
                spans: spans(trimmed[hashes..].trim()),
            });
            continue;
        }
        if trimmed.len() >= 3
            && trimmed.chars().all(|c| c == '-' || c == '*' || c == '_')
            && !trimmed.starts_with("* ")
        {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            blocks.push(Block::Rule);
            continue;
        }
        if let Some(rest) = trimmed
            .strip_prefix("> ")
            .or_else(|| trimmed.strip_prefix('>'))
        {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            blocks.push(Block::Quote {
                spans: spans(rest.trim()),
            });
            continue;
        }
        if let Some((ordered, rest)) = list_item(line) {
            end_paragraph(&mut blocks, &mut paragraph);
            match &mut list {
                Some((o, items)) if *o == ordered => items.push(spans(rest)),
                _ => {
                    end_list(&mut blocks, &mut list);
                    list = Some((ordered, vec![spans(rest)]));
                }
            }
            continue;
        }
        end_list(&mut blocks, &mut list);
        paragraph.push(line.trim_end());
    }
    end_paragraph(&mut blocks, &mut paragraph);
    end_list(&mut blocks, &mut list);
    if let Some((language, body)) = fence {
        blocks.push(Block::Code {
            language,
            text: body.join("\n"),
        });
    }
    blocks
}

/// What to show before the person picks a save location for an assistant reply.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SaveDraft {
    pub body: String,
    pub default_filename: String,
}

fn extension_for_language(language: &str) -> &'static str {
    match language.trim().to_lowercase().as_str() {
        "rust" | "rs" => "rs",
        "python" | "py" => "py",
        "typescript" | "ts" | "tsx" => "ts",
        "javascript" | "js" | "jsx" => "js",
        "bash" | "sh" | "shell" | "zsh" => "sh",
        "json" => "json",
        "yaml" | "yml" => "yaml",
        "toml" => "toml",
        "markdown" | "md" => "md",
        "html" | "htm" => "html",
        "css" => "css",
        "sql" => "sql",
        "go" => "go",
        "java" => "java",
        "kotlin" | "kt" => "kt",
        "c" => "c",
        "cpp" | "c++" | "cc" => "cpp",
        "h" | "hpp" => "h",
        _ => "txt",
    }
}

/// One fenced code block becomes a snippet file; otherwise the full reply is saved as Markdown.
pub fn save_draft(assistant_text: &str) -> SaveDraft {
    let blocks = parse(assistant_text);
    let mut code_blocks: Vec<(&str, &str)> = Vec::new();
    for block in &blocks {
        if let Block::Code { language, text } = block {
            if !text.trim().is_empty() {
                code_blocks.push((language.as_str(), text.as_str()));
            }
        }
    }
    if code_blocks.len() == 1 {
        let (language, text) = code_blocks[0];
        let ext = extension_for_language(language);
        return SaveDraft {
            body: text.to_string(),
            default_filename: format!("mobius-snippet.{ext}"),
        };
    }
    SaveDraft {
        body: assistant_text.to_string(),
        default_filename: "mobius-reply.md".into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn t(s: &str) -> Span {
        text(s)
    }

    #[test]
    fn tables_have_alignment_and_inline_formatting() {
        let blocks = parse(
            "Before\n\n| Name | Cost |\n| :--- | ---: |\n| **Small** | `2|3` |\n| a\\|b |\n\nAfter",
        );
        assert_eq!(blocks.len(), 3);
        assert_eq!(
            blocks[1],
            Block::Table {
                headers: vec![vec![t("Name")], vec![t("Cost")]],
                alignments: vec![Alignment::Left, Alignment::Right],
                rows: vec![
                    vec![
                        vec![Span::Bold {
                            text: "Small".into()
                        }],
                        vec![Span::Code { text: "2|3".into() }]
                    ],
                    vec![vec![t("a|b")], vec![]]
                ],
            }
        );
        assert!(matches!(
            parse("a | b\n--- | :---:\nx | y")[0],
            Block::Table { .. }
        ));
    }

    #[test]
    fn incomplete_and_invalid_tables_stay_text_and_code_stays_code() {
        for input in [
            "a | b",
            "a | b\n-- | ---",
            "a | b\n--- | --- | ---",
            "a | b\n--- | :--",
        ] {
            assert!(
                matches!(parse(input)[0], Block::Paragraph { .. }),
                "{input}"
            );
        }
        assert!(matches!(
            parse("```\na | b\n--- | ---\n```")[0],
            Block::Code { .. }
        ));
        assert!(matches!(parse("a | b\n--- | ---")[0], Block::Table { .. }));
    }

    #[test]
    fn inline_spans() {
        assert_eq!(
            spans("a **b** c"),
            [t("a "), Span::Bold { text: "b".into() }, t(" c")]
        );
        assert_eq!(
            spans("use `ls -la` now"),
            [
                t("use "),
                Span::Code {
                    text: "ls -la".into()
                },
                t(" now")
            ]
        );
        assert_eq!(
            spans("an *italic* word"),
            [
                t("an "),
                Span::Italic {
                    text: "italic".into()
                },
                t(" word")
            ]
        );
        assert_eq!(
            spans("snake_case_name"),
            [t("snake_case_name")],
            "underscores inside words are not emphasis"
        );
        assert_eq!(
            spans("2 * 3 * 4"),
            [t("2 * 3 * 4")],
            "spaced stars are not emphasis"
        );
        assert_eq!(spans("unclosed **bold"), [t("unclosed **bold")]);
        assert_eq!(
            spans("see [the docs](https://x.example/a)"),
            [t("see the docs (https://x.example/a)")],
            "a link is text, not a link"
        );
    }

    #[test]
    fn html_is_just_text() {
        let blocks = parse("<script>alert(1)</script>\n\n<img src=x onerror=y>");
        assert_eq!(blocks.len(), 2);
        assert_eq!(
            blocks[0],
            Block::Paragraph {
                spans: vec![t("<script>alert(1)</script>")]
            }
        );
    }

    #[test]
    fn a_reply_being_written_shows_open_emphasis_closed() {
        assert_eq!(
            close_open_marks("first, a **scheduler"),
            "first, a **scheduler**"
        );
        assert_eq!(close_open_marks("run `cron"), "run `cron`");
        assert_eq!(
            close_open_marks("a **bold** and **mor"),
            "a **bold** and **mor**"
        );
        assert_eq!(
            close_open_marks("a word **"),
            "a word ",
            "a mark with nothing after it yet is held back"
        );
        assert_eq!(close_open_marks("a `"), "a ");
        assert_eq!(close_open_marks("fine **bold** text"), "fine **bold** text");
        assert_eq!(
            close_open_marks("line one\n- item **bo"),
            "line one\n- item **bo**"
        );
        assert_eq!(
            close_open_marks("```sh\nls **x"),
            "```sh\nls **x",
            "inside a code block nothing changes"
        );
        assert_eq!(
            close_open_marks("code `a ** b` end"),
            "code `a ** b` end",
            "stars inside code are not emphasis"
        );
        let blocks = parse(&close_open_marks("first, a **scheduler"));
        match &blocks[0] {
            Block::Paragraph { spans } => assert!(
                spans
                    .iter()
                    .any(|s| matches!(s, Span::Bold { text } if text == "scheduler")),
                "{spans:?}"
            ),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn blocks() {
        let b = parse("# Title\n\nSome *text*.\n\n- one\n- two\n\n1. first\n2. second\n\n> quoted\n\n---\n\n```rust\nfn main() {}\n```\n");
        assert_eq!(
            b[0],
            Block::Heading {
                level: 1,
                spans: vec![t("Title")]
            }
        );
        assert!(matches!(&b[1], Block::Paragraph { .. }));
        assert_eq!(
            b[2],
            Block::List {
                ordered: false,
                items: vec![vec![t("one")], vec![t("two")]]
            }
        );
        assert_eq!(
            b[3],
            Block::List {
                ordered: true,
                items: vec![vec![t("first")], vec![t("second")]]
            }
        );
        assert_eq!(
            b[4],
            Block::Quote {
                spans: vec![t("quoted")]
            }
        );
        assert_eq!(b[5], Block::Rule);
        assert_eq!(
            b[6],
            Block::Code {
                language: "rust".into(),
                text: "fn main() {}".into()
            }
        );
    }

    #[test]
    fn a_streaming_reply_may_end_anywhere() {
        assert_eq!(
            parse("```py\nprint(1)"),
            [Block::Code {
                language: "py".into(),
                text: "print(1)".into()
            }]
        );
        assert_eq!(
            parse("**half"),
            [Block::Paragraph {
                spans: vec![t("**half")]
            }]
        );
        assert!(parse("").is_empty());
        let weird = "```\n```\n- \n#\n> \n";
        let _ = parse(weird);
    }

    #[test]
    fn code_keeps_markdown_characters() {
        let b = parse("```\n**not bold** # not heading\n```");
        assert_eq!(
            b,
            [Block::Code {
                language: String::new(),
                text: "**not bold** # not heading".into()
            }]
        );
    }

    #[test]
    fn the_language_tag_is_tame() {
        let b = parse("```<b onclick=x>\ncode\n```");
        if let Block::Code { language, .. } = &b[0] {
            assert!(
                language
                    .chars()
                    .all(|c| c.is_ascii_alphanumeric() || "+-#._".contains(c)),
                "{language}"
            );
        } else {
            panic!("not code");
        }
    }

    #[test]
    fn save_draft_prefers_a_single_code_block() {
        let draft = save_draft("Here is the script:\n\n```python\nprint('ok')\n```\n");
        assert_eq!(draft.body, "print('ok')");
        assert_eq!(draft.default_filename, "mobius-snippet.py");
        let whole = save_draft("# Title\n\nTwo blocks:\n\n```rs\na\n```\n\n```rs\nb\n```");
        assert!(whole.body.contains("Two blocks"));
        assert_eq!(whole.default_filename, "mobius-reply.md");
    }
}

#[cfg(test)]
mod thinking_tests {
    use super::*;

    #[test]
    fn splits_the_prelude_from_the_answer() {
        let (t, a) = split_thinking("<think>\nhmm, 2+2\n</think>\n\nIt is 4.");
        assert_eq!(t, Some(("hmm, 2+2".to_string(), true)));
        assert_eq!(a, "It is 4.");
        let (t, a) = split_thinking("<think>still thinking");
        assert_eq!(t, Some(("still thinking".to_string(), false)));
        assert_eq!(a, "");
        assert_eq!(split_thinking("Use the <think> tag like this").0, None);
        assert_eq!(strip_thinking("<think>x</think>Hi"), "Hi");
    }

    #[test]
    fn a_reply_has_a_folded_block_then_its_answer() {
        let blocks = parse_reply("<think>why</think>Because.");
        assert!(matches!(&blocks[0], Block::Thinking { text, done: true } if text == "why"));
        assert!(matches!(blocks[1], Block::Paragraph { .. }));
        assert!(matches!(
            parse_reply("<think>x")[0],
            Block::Thinking { done: false, .. }
        ));
        assert_eq!(parse_reply("plain"), parse("plain"));
    }
}

#[cfg(test)]
mod artifact_tests {
    use super::*;

    #[test]
    fn a_document_fence_keeps_its_title() {
        let blocks = parse("```artifact:Quarterly plan\nText\n```");
        assert_eq!(
            blocks,
            vec![Block::Code {
                language: "artifact:Quarterly plan".into(),
                text: "Text".into()
            }]
        );
        let blocks = parse("```rust\nfn x() {}\n```");
        assert!(matches!(&blocks[0], Block::Code { language, .. } if language == "rust"));
    }
}
