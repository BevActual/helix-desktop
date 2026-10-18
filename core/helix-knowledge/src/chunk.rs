//! Splits a Markdown page into passages that can be searched and embedded on their own.
//!
//! A passage follows the page's headings, so a retrieved passage reads as a section and carries its heading path. Long sections are
//! split on paragraph breaks, and very short ones are joined with the section that follows so no passage is a stub.

/// Passages longer than this are split (characters, not bytes).
pub const MAX_CHARS: usize = 1200;
/// A passage shorter than this takes the next section along with it.
pub const MIN_CHARS: usize = 240;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Passage {
    /// Heading path such as `Design > Storage`, empty before the first heading.
    pub heading: String,
    pub text: String,
}

pub fn chunk(body: &str) -> Vec<Passage> {
    let mut sections: Vec<(String, String)> = Vec::new();
    let mut path: Vec<(usize, String)> = Vec::new();
    let mut heading = String::new();
    let mut text = String::new();
    let mut fenced = false;
    let flush = |sections: &mut Vec<(String, String)>, heading: &str, text: &mut String| {
        let trimmed = text.trim();
        if !trimmed.is_empty() {
            sections.push((heading.to_string(), trimmed.to_string()));
        }
        text.clear();
    };
    for line in body.lines() {
        if line.trim_start().starts_with("```") {
            fenced = !fenced;
        }
        if !fenced {
            if let Some((level, title)) = heading_of(line) {
                flush(&mut sections, &heading, &mut text);
                while path.last().is_some_and(|(l, _)| *l >= level) {
                    path.pop();
                }
                path.push((level, title));
                heading = path
                    .iter()
                    .map(|(_, t)| t.as_str())
                    .collect::<Vec<_>>()
                    .join(" > ");
                continue;
            }
        }
        text.push_str(line);
        text.push('\n');
    }
    flush(&mut sections, &heading, &mut text);

    let mut out: Vec<Passage> = Vec::new();
    let mut pending: Option<Passage> = None;
    for (heading, text) in sections {
        for piece in split_long(&text) {
            match pending.take() {
                Some(mut held) if held.text.chars().count() < MIN_CHARS => {
                    held.text.push_str("\n\n");
                    // a section carried along keeps its own heading, so its text is not read as belonging to the first
                    if heading != held.heading && !heading.is_empty() {
                        held.text.push_str(&heading);
                        held.text.push('\n');
                    }
                    held.text.push_str(&piece);
                    pending = Some(held);
                }
                Some(held) => {
                    out.push(held);
                    pending = Some(Passage {
                        heading: heading.clone(),
                        text: piece,
                    });
                }
                None => {
                    pending = Some(Passage {
                        heading: heading.clone(),
                        text: piece,
                    })
                }
            }
        }
    }
    out.extend(pending);
    out
}

fn heading_of(line: &str) -> Option<(usize, String)> {
    let trimmed = line.trim_start();
    let level = trimmed.chars().take_while(|c| *c == '#').count();
    if level == 0 || level > 6 {
        return None;
    }
    let rest = trimmed[level..].strip_prefix(' ')?;
    let title = rest.trim().trim_end_matches('#').trim();
    (!title.is_empty()).then(|| (level, title.to_string()))
}

fn split_long(text: &str) -> Vec<String> {
    if text.chars().count() <= MAX_CHARS {
        return vec![text.to_string()];
    }
    let mut pieces = Vec::new();
    let mut current = String::new();
    for paragraph in text.split("\n\n") {
        let paragraph = paragraph.trim();
        if paragraph.is_empty() {
            continue;
        }
        if !current.is_empty()
            && current.chars().count() + paragraph.chars().count() + 2 > MAX_CHARS
        {
            pieces.push(std::mem::take(&mut current));
        }
        if paragraph.chars().count() > MAX_CHARS {
            if !current.is_empty() {
                pieces.push(std::mem::take(&mut current));
            }
            pieces.extend(split_words(paragraph));
            continue;
        }
        if !current.is_empty() {
            current.push_str("\n\n");
        }
        current.push_str(paragraph);
    }
    if !current.is_empty() {
        pieces.push(current);
    }
    pieces
}

/// A paragraph with no break to split on: cut at whitespace under the limit.
fn split_words(paragraph: &str) -> Vec<String> {
    let mut pieces = Vec::new();
    let mut current = String::new();
    for word in paragraph.split_whitespace() {
        if !current.is_empty() && current.chars().count() + word.chars().count() + 1 > MAX_CHARS {
            pieces.push(std::mem::take(&mut current));
        }
        if !current.is_empty() {
            current.push(' ');
        }
        current.push_str(word);
    }
    if !current.is_empty() {
        pieces.push(current);
    }
    pieces
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn follows_headings_and_joins_stubs() {
        let long = "word ".repeat(100);
        let body = format!("intro\n\n# A\n\nshort\n\n## B\n\n{long}\n\n# C\n\n{long}");
        let passages = chunk(&body);
        // "intro" and "short" are stubs, so they travel with the section after them, which keeps its heading in the text
        assert_eq!(passages.len(), 2);
        assert_eq!(passages[0].heading, "");
        assert!(passages[0].text.contains("intro") && passages[0].text.contains("short"));
        assert!(passages[0].text.contains("A > B\nword"));
        assert_eq!(passages[1].heading, "C");
    }

    #[test]
    fn headings_inside_code_fences_are_text() {
        let body = "# Real\n\n```\n# not a heading\n```\n";
        let passages = chunk(body);
        assert_eq!(passages.len(), 1);
        assert!(passages[0].text.contains("# not a heading"));
    }

    #[test]
    fn long_sections_split_under_the_limit() {
        let paragraph = "sentence here. ".repeat(40);
        let body = format!("# T\n\n{paragraph}\n\n{paragraph}\n\n{paragraph}");
        let passages = chunk(&body);
        assert!(passages.len() >= 2);
        assert!(passages.iter().all(|p| p.text.chars().count() <= MAX_CHARS));
    }
}
