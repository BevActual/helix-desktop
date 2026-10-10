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
    Paragraph { spans: Vec<Span> },
    Heading { level: u8, spans: Vec<Span> },
    Code { language: String, text: String },
    List { ordered: bool, items: Vec<Vec<Span>> },
    Quote { spans: Vec<Span> },
    Rule,
}

fn text(s: &str) -> Span {
    Span::Text { text: s.to_string() }
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
        (from..chars.len().saturating_sub(pattern.len() - 1)).find(|&i| chars[i..i + pattern.len()] == *pattern)
    };
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c == '`' {
            if let Some(end) = find(i + 1, &['`']) {
                flush(&mut plain, &mut out);
                out.push(Span::Code { text: chars[i + 1..end].iter().collect() });
                i = end + 1;
                continue;
            }
        } else if c == '*' && chars.get(i + 1) == Some(&'*') {
            if let Some(end) = find(i + 2, &['*', '*']) {
                if end > i + 2 {
                    flush(&mut plain, &mut out);
                    out.push(Span::Bold { text: chars[i + 2..end].iter().collect() });
                    i = end + 2;
                    continue;
                }
            }
        } else if (c == '*' || c == '_') && chars.get(i + 1).is_some_and(|n| !n.is_whitespace() && *n != c) {
            let before_ok = i == 0 || !chars[i - 1].is_alphanumeric();
            if before_ok {
                if let Some(end) = find(i + 1, &[c]) {
                    if end > i + 1 && !chars[end - 1].is_whitespace() && chars.get(end + 1).is_none_or(|n| !n.is_alphanumeric()) {
                        flush(&mut plain, &mut out);
                        out.push(Span::Italic { text: chars[i + 1..end].iter().collect() });
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
        if let Some(rest) = t[digits..].strip_prefix(". ").or_else(|| t[digits..].strip_prefix(") ")) {
            return Some((true, rest));
        }
    }
    None
}

pub fn parse(input: &str) -> Vec<Block> {
    let mut blocks: Vec<Block> = Vec::new();
    let mut paragraph: Vec<&str> = Vec::new();
    let mut list: Option<(bool, Vec<Vec<Span>>)> = None;
    let mut fence: Option<(String, Vec<&str>)> = None;

    fn end_paragraph(blocks: &mut Vec<Block>, paragraph: &mut Vec<&str>) {
        if !paragraph.is_empty() {
            blocks.push(Block::Paragraph { spans: spans(&paragraph.join("\n")) });
            paragraph.clear();
        }
    }
    fn end_list(blocks: &mut Vec<Block>, list: &mut Option<(bool, Vec<Vec<Span>>)>) {
        if let Some((ordered, items)) = list.take() {
            blocks.push(Block::List { ordered, items });
        }
    }

    for line in input.lines() {
        if let Some((language, body)) = &mut fence {
            if line.trim_start().starts_with("```") {
                blocks.push(Block::Code { language: language.clone(), text: body.join("\n") });
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
            let language: String = rest.trim().chars().filter(|c| c.is_ascii_alphanumeric() || "+-#._".contains(*c)).take(20).collect();
            fence = Some((language, Vec::new()));
            continue;
        }
        if trimmed.is_empty() {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            continue;
        }
        let hashes = trimmed.chars().take_while(|c| *c == '#').count();
        if (1..=6).contains(&hashes) && trimmed[hashes..].starts_with(' ') {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            blocks.push(Block::Heading { level: hashes as u8, spans: spans(trimmed[hashes..].trim()) });
            continue;
        }
        if trimmed.len() >= 3 && trimmed.chars().all(|c| c == '-' || c == '*' || c == '_') && !trimmed.starts_with("* ") {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            blocks.push(Block::Rule);
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix("> ").or_else(|| trimmed.strip_prefix('>')) {
            end_paragraph(&mut blocks, &mut paragraph);
            end_list(&mut blocks, &mut list);
            blocks.push(Block::Quote { spans: spans(rest.trim()) });
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
        blocks.push(Block::Code { language, text: body.join("\n") });
    }
    blocks
}

#[cfg(test)]
mod tests {
    use super::*;

    fn t(s: &str) -> Span {
        text(s)
    }

    #[test]
    fn inline_spans() {
        assert_eq!(spans("a **b** c"), [t("a "), Span::Bold { text: "b".into() }, t(" c")]);
        assert_eq!(spans("use `ls -la` now"), [t("use "), Span::Code { text: "ls -la".into() }, t(" now")]);
        assert_eq!(spans("an *italic* word"), [t("an "), Span::Italic { text: "italic".into() }, t(" word")]);
        assert_eq!(spans("snake_case_name"), [t("snake_case_name")], "underscores inside words are not emphasis");
        assert_eq!(spans("2 * 3 * 4"), [t("2 * 3 * 4")], "spaced stars are not emphasis");
        assert_eq!(spans("unclosed **bold"), [t("unclosed **bold")]);
        assert_eq!(spans("see [the docs](https://x.example/a)"), [t("see the docs (https://x.example/a)")], "a link is text, not a link");
    }

    #[test]
    fn html_is_just_text() {
        let blocks = parse("<script>alert(1)</script>\n\n<img src=x onerror=y>");
        assert_eq!(blocks.len(), 2);
        assert_eq!(blocks[0], Block::Paragraph { spans: vec![t("<script>alert(1)</script>")] });
    }

    #[test]
    fn blocks() {
        let b = parse("# Title\n\nSome *text*.\n\n- one\n- two\n\n1. first\n2. second\n\n> quoted\n\n---\n\n```rust\nfn main() {}\n```\n");
        assert_eq!(b[0], Block::Heading { level: 1, spans: vec![t("Title")] });
        assert!(matches!(&b[1], Block::Paragraph { .. }));
        assert_eq!(b[2], Block::List { ordered: false, items: vec![vec![t("one")], vec![t("two")]] });
        assert_eq!(b[3], Block::List { ordered: true, items: vec![vec![t("first")], vec![t("second")]] });
        assert_eq!(b[4], Block::Quote { spans: vec![t("quoted")] });
        assert_eq!(b[5], Block::Rule);
        assert_eq!(b[6], Block::Code { language: "rust".into(), text: "fn main() {}".into() });
    }

    #[test]
    fn a_streaming_reply_may_end_anywhere() {
        assert_eq!(parse("```py\nprint(1)"), [Block::Code { language: "py".into(), text: "print(1)".into() }]);
        assert_eq!(parse("**half"), [Block::Paragraph { spans: vec![t("**half")] }]);
        assert!(parse("").is_empty());
        let weird = "```\n```\n- \n#\n> \n";
        let _ = parse(weird);
    }

    #[test]
    fn code_keeps_markdown_characters() {
        let b = parse("```\n**not bold** # not heading\n```");
        assert_eq!(b, [Block::Code { language: String::new(), text: "**not bold** # not heading".into() }]);
    }

    #[test]
    fn the_language_tag_is_tame() {
        let b = parse("```<b onclick=x>\ncode\n```");
        if let Block::Code { language, .. } = &b[0] {
            assert!(language.chars().all(|c| c.is_ascii_alphanumeric() || "+-#._".contains(c)), "{language}");
        } else {
            panic!("not code");
        }
    }
}
