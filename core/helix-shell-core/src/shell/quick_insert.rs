//! The Caps Lock palette as a pure model: emoji, clipboard history and desktop actions behind one search field, plus an "Ask Helix"
//! row when what was typed is better said to the assistant. The page only ever holds display rows and sends back the *position* of
//! the row the person chose; what that position means (text to type, text to paste, an action, a question) comes from the results
//! of the last query kept here, so the page can neither invent text to insert nor name an action it was not offered.

use std::collections::{HashMap, VecDeque};

use serde::{Deserialize, Serialize};

use crate::display::sanitize_for_display;

pub const MAX_RESULTS: usize = 96;
pub const MAX_CLIPS: usize = 50;
/// A clipboard entry larger than this is not kept (a half-copy would surprise more than a missing entry).
pub const MAX_CLIP_BYTES: usize = 64 * 1024;
pub const MAX_QUESTION_CHARS: usize = 500;

/// Text with a line break or a control character: pasted into a terminal that does not guard against it, it runs as commands.
pub fn needs_care(text: &str) -> bool {
    text.chars()
        .any(|c| c == '\n' || c == '\r' || (c.is_control() && c != '\t'))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Source {
    Emoji,
    Clipboard,
    Actions,
}

#[derive(Clone, Debug, Deserialize)]
struct EmojiRecord {
    emoji: String,
    name: String,
    #[serde(default)]
    keywords: Vec<String>,
    #[serde(default)]
    category: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Emoji {
    pub glyph: String,
    pub name: String,
    pub keywords: Vec<String>,
    pub category: String,
}

/// The emoji dataset (a JSON array of `{emoji, name, keywords, category}`). Entries that do not fit are skipped, not guessed at.
pub fn parse_emoji(json: &str) -> Vec<Emoji> {
    let records: Vec<EmojiRecord> = serde_json::from_str(json).unwrap_or_default();
    records
        .into_iter()
        .filter(|r| {
            !r.emoji.is_empty()
                && r.emoji.chars().count() <= 16
                && !r.emoji.chars().any(char::is_control)
        })
        .map(|r| Emoji {
            glyph: r.emoji,
            name: r.name.to_lowercase(),
            keywords: r.keywords.into_iter().map(|k| k.to_lowercase()).collect(),
            category: r.category,
        })
        .collect()
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub enum Action {
    OpenLauncher,
    OpenControlCenter,
    OpenNotifications,
    OpenAgents,
    OpenAssistant,
    ToggleDoNotDisturb,
    ToggleNightLight,
    ToggleDarkTheme,
    LockSession,
}

const ACTIONS: &[(Action, &str, &str, &[&str])] = &[
    (
        Action::OpenLauncher,
        "Open Apps",
        "Search and start applications",
        &["launcher", "applications", "run"],
    ),
    (
        Action::OpenControlCenter,
        "Open Control Center",
        "Wi-Fi, Bluetooth, sound and more",
        &["settings", "quick", "controls", "volume"],
    ),
    (
        Action::OpenNotifications,
        "Open Notifications",
        "What you have missed",
        &["shade", "bell", "messages"],
    ),
    (
        Action::OpenAgents,
        "Open Agents",
        "What agents did and may do",
        &["activity", "ai", "access", "permissions"],
    ),
    (
        Action::OpenAssistant,
        "Ask the Assistant",
        "Tell Helix what to do",
        &["ai", "helix", "chat"],
    ),
    (
        Action::ToggleDoNotDisturb,
        "Toggle Do Not Disturb",
        "Silence banners",
        &["focus", "quiet", "mute notifications"],
    ),
    (
        Action::ToggleNightLight,
        "Toggle Night Light",
        "Warmer screen colours",
        &["blue light", "warm", "evening"],
    ),
    (
        Action::ToggleDarkTheme,
        "Toggle Dark Theme",
        "Switch between dark and light",
        &["appearance", "light", "mode"],
    ),
    (
        Action::LockSession,
        "Lock Screen",
        "Lock this session",
        &["secure", "away", "lock"],
    ),
];

/// What choosing a row does. The host carries it out; this model never touches the system.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Type these characters into the window that had focus.
    Type(String),
    /// Put this text on the clipboard and paste it (so a terminal receives it as a paste, never as keystrokes).
    Paste(String),
    Do(Action),
    /// A sentence for the assistant.
    Ask(String),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Row {
    pub label: String,
    pub detail: String,
    /// An emoji to draw large, for emoji rows.
    pub glyph: Option<String>,
    pub ask: bool,
}

/// One line from the clipboard watcher: the copied bytes as hex, so any content (newlines, binary) fits on one line. Text that is
/// not valid UTF-8, or is empty, is not a clipboard entry.
pub fn decode_clip_hex(line: &str) -> Option<String> {
    let hex = line.trim().as_bytes();
    if hex.is_empty() || !hex.len().is_multiple_of(2) || hex.len() > 2 * (MAX_CLIP_BYTES + 1) {
        return None;
    }
    let nibble = |c: u8| match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        _ => None,
    };
    let mut bytes = Vec::with_capacity(hex.len() / 2);
    for pair in hex.chunks(2) {
        bytes.push(nibble(pair[0])? << 4 | nibble(pair[1])?);
    }
    String::from_utf8(bytes).ok()
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    NoSuchResult,
    /// The rows the person was looking at are not the latest results: what a position means has changed since they were drawn.
    Stale,
}

pub struct QuickInsert {
    emoji: Vec<Emoji>,
    clips: VecDeque<String>,
    used: HashMap<String, u32>,
    last: Vec<Outcome>,
    generation: u64,
}

/// A crude English stem, enough that "smile" finds "smiling" and "laughs" finds "laughing": the longest common suffixes are dropped,
/// never leaving fewer than three letters.
fn stem(word: &str) -> &str {
    for suffix in ["ing", "ed", "es", "s", "e"] {
        if let Some(root) = word.strip_suffix(suffix) {
            if root.chars().count() >= 3 {
                return root;
            }
        }
    }
    word
}

/// Higher is a better match; 0 is no match. `fields` are the lowercase texts to try, the first being the most important.
fn score(query: &str, name: &str, keywords: &[String]) -> u32 {
    if name == query {
        return 100;
    }
    if name.starts_with(query) {
        return 80;
    }
    if name.split(' ').any(|word| word.starts_with(query)) {
        return 60;
    }
    if keywords.iter().any(|k| k == query) {
        return 55;
    }
    if keywords.iter().any(|k| k.starts_with(query)) {
        return 45;
    }
    // Same word under a different ending ("smile" / "smiling"): below an exact or prefix hit, above a stray substring.
    let root = stem(query);
    if root.chars().count() >= 3 && name.split(' ').any(|word| stem(word).starts_with(root)) {
        return 40;
    }
    if root.chars().count() >= 3 && keywords.iter().any(|k| stem(k).starts_with(root)) {
        return 35;
    }
    if name.contains(query) {
        return 30;
    }
    if keywords.iter().any(|k| k.contains(query)) {
        return 20;
    }
    0
}

impl QuickInsert {
    pub fn new(emoji: Vec<Emoji>) -> Self {
        Self {
            emoji,
            clips: VecDeque::new(),
            used: HashMap::new(),
            last: Vec::new(),
            generation: 0,
        }
    }

    /// Something was copied. Blank or oversized text is ignored; copying the same text again moves it to the front.
    pub fn copied(&mut self, text: String) {
        if text.trim().is_empty() || text.len() > MAX_CLIP_BYTES {
            return;
        }
        self.clips.retain(|existing| *existing != text);
        self.clips.push_front(text);
        self.clips.truncate(MAX_CLIPS);
    }

    pub fn forget_clipboard(&mut self) {
        self.clips.clear();
    }

    pub fn clip_count(&self) -> usize {
        self.clips.len()
    }

    /// Identifies the results of the latest query; a row can only be chosen from the results it was drawn from.
    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn query(&mut self, source: Source, text: &str) -> Vec<Row> {
        self.generation += 1;
        let needle = text.trim().to_lowercase();
        let mut rows = Vec::new();
        let mut outcomes = Vec::new();
        match source {
            Source::Emoji => {
                let mut hits: Vec<(u32, u32, usize)> = self
                    .emoji
                    .iter()
                    .enumerate()
                    .filter_map(|(index, e)| {
                        let used = self.used.get(&e.glyph).copied().unwrap_or(0);
                        if needle.is_empty() {
                            (used > 0).then_some((0, used, index))
                        } else {
                            let s = score(&needle, &e.name, &e.keywords);
                            (s > 0).then_some((s, used, index))
                        }
                    })
                    .collect();
                if needle.is_empty() && hits.is_empty() {
                    // Nothing used yet: the first of the dataset (smileys), so the palette is never an empty box.
                    hits = (0..self.emoji.len().min(MAX_RESULTS))
                        .map(|i| (0, 0, i))
                        .collect();
                }
                hits.sort_by(|a, b| b.0.cmp(&a.0).then(b.1.cmp(&a.1)).then(a.2.cmp(&b.2)));
                for (_, _, index) in hits.into_iter().take(MAX_RESULTS) {
                    let e = &self.emoji[index];
                    rows.push(Row {
                        label: sanitize_for_display(&e.name, 60),
                        detail: sanitize_for_display(&e.category, 24),
                        glyph: Some(e.glyph.clone()),
                        ask: false,
                    });
                    outcomes.push(Outcome::Type(e.glyph.clone()));
                }
            }
            Source::Clipboard => {
                for clip in &self.clips {
                    if !needle.is_empty() && !clip.to_lowercase().contains(&needle) {
                        continue;
                    }
                    let first = clip.lines().find(|l| !l.trim().is_empty()).unwrap_or("");
                    let lines = clip.lines().count();
                    rows.push(Row {
                        label: sanitize_for_display(first.trim(), 90),
                        detail: if lines > 1 {
                            format!("{lines} lines")
                        } else {
                            format!("{} characters", clip.chars().count())
                        },
                        glyph: None,
                        ask: false,
                    });
                    outcomes.push(Outcome::Paste(clip.clone()));
                }
            }
            Source::Actions => {
                let mut hits: Vec<(u32, usize)> = ACTIONS
                    .iter()
                    .enumerate()
                    .filter_map(|(index, (_, name, _, keywords))| {
                        if needle.is_empty() {
                            return Some((1, index));
                        }
                        let keywords: Vec<String> =
                            keywords.iter().map(|k| (*k).to_string()).collect();
                        let s = score(&needle, &name.to_lowercase(), &keywords);
                        (s > 0).then_some((s, index))
                    })
                    .collect();
                hits.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
                for (_, index) in hits {
                    let (action, name, detail, _) = ACTIONS[index];
                    rows.push(Row {
                        label: (*name).to_string(),
                        detail: (*detail).to_string(),
                        glyph: None,
                        ask: false,
                    });
                    outcomes.push(Outcome::Do(action));
                }
            }
        }
        // What was typed can always be handed to the assistant, whatever else matched.
        if !needle.is_empty() {
            let question: String = text.trim().chars().take(MAX_QUESTION_CHARS).collect();
            rows.push(Row {
                label: format!("Ask Helix: {}", sanitize_for_display(&question, 80)),
                detail: "Let the assistant do it".to_string(),
                glyph: None,
                ask: true,
            });
            outcomes.push(Outcome::Ask(question));
        }
        self.last = outcomes;
        rows
    }

    /// The person chose the row at `index` of the results of query `generation`.
    pub fn activate(&mut self, generation: u64, index: usize) -> Result<Outcome, RequestError> {
        if generation != self.generation {
            return Err(RequestError::Stale);
        }
        let outcome = self
            .last
            .get(index)
            .cloned()
            .ok_or(RequestError::NoSuchResult)?;
        if let Outcome::Type(glyph) = &outcome {
            *self.used.entry(glyph.clone()).or_insert(0) += 1;
        }
        Ok(outcome)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn palette() -> QuickInsert {
        QuickInsert::new(parse_emoji(
            r#"[
              {"emoji":"😀","name":"grinning face","keywords":["smile","happy"],"category":"people"},
              {"emoji":"😂","name":"face with tears of joy","keywords":["laugh","haha"],"category":"people"},
              {"emoji":"🍕","name":"pizza","keywords":["food","slice"],"category":"food"},
              {"emoji":"🔥","name":"fire","keywords":["hot","flame"],"category":"nature"},
              {"emoji":"","name":"broken","keywords":[],"category":"x"},
              {"emoji":"a\u0007b","name":"control","keywords":[],"category":"x"}
            ]"#,
        ))
    }

    fn labels(rows: &[Row]) -> Vec<String> {
        rows.iter().map(|r| r.label.clone()).collect()
    }

    #[test]
    fn the_dataset_skips_entries_that_cannot_be_inserted_safely() {
        let mut p = palette();
        let all = p.query(Source::Emoji, "");
        assert_eq!(
            all.len(),
            4,
            "empty glyph and control characters are dropped"
        );
    }

    #[test]
    fn an_empty_query_before_any_use_shows_the_start_of_the_dataset() {
        let mut p = palette();
        let rows = p.query(Source::Emoji, "");
        assert_eq!(rows[0].label, "grinning face");
    }

    #[test]
    fn emoji_are_ranked_exact_then_prefix_then_word_then_keyword() {
        let mut p = palette();
        assert_eq!(p.query(Source::Emoji, "pizza")[0].label, "pizza");
        assert_eq!(p.query(Source::Emoji, "gri")[0].label, "grinning face");
        assert_eq!(
            p.query(Source::Emoji, "tears")[0].label,
            "face with tears of joy"
        );
        assert_eq!(p.query(Source::Emoji, "hot")[0].label, "fire");
        // "fa": a prefix of "face with tears of joy" outranks the word match inside "grinning face".
        assert_eq!(
            p.query(Source::Emoji, "fa")[0].label,
            "face with tears of joy"
        );
    }

    #[test]
    fn what_you_use_comes_first_when_you_open_the_palette() {
        let mut p = palette();
        p.query(Source::Emoji, "pizza");
        p.activate(p.generation(), 0).unwrap();
        p.query(Source::Emoji, "pizza");
        p.activate(p.generation(), 0).unwrap();
        p.query(Source::Emoji, "fire");
        p.activate(p.generation(), 0).unwrap();
        let rows = p.query(Source::Emoji, "");
        assert_eq!(
            labels(&rows),
            ["pizza", "fire"],
            "only used ones, most used first"
        );
    }

    #[test]
    fn choosing_an_emoji_types_it() {
        let mut p = palette();
        p.query(Source::Emoji, "pizza");
        assert_eq!(
            p.activate(p.generation(), 0),
            Ok(Outcome::Type("🍕".into()))
        );
    }

    #[test]
    fn the_page_cannot_choose_a_row_that_was_not_offered() {
        let mut p = palette();
        p.query(Source::Emoji, "pizza");
        assert_eq!(
            p.activate(p.generation(), 5),
            Err(RequestError::NoSuchResult)
        );
        let mut fresh = palette();
        assert_eq!(
            fresh.activate(fresh.generation(), 0),
            Err(RequestError::NoSuchResult)
        );
    }

    #[test]
    fn a_row_drawn_from_an_older_query_cannot_be_chosen() {
        let mut p = palette();
        p.query(Source::Emoji, "pizza");
        let drawn = p.generation();
        p.query(Source::Emoji, "fire");
        assert_eq!(p.activate(drawn, 0), Err(RequestError::Stale));
        assert_eq!(
            p.activate(p.generation(), 0),
            Ok(Outcome::Type("🔥".into()))
        );
    }

    #[test]
    fn the_last_query_decides_what_a_position_means() {
        let mut p = palette();
        p.query(Source::Emoji, "pizza");
        p.query(Source::Emoji, "fire");
        assert_eq!(
            p.activate(p.generation(), 0),
            Ok(Outcome::Type("🔥".into()))
        );
    }

    #[test]
    fn clipboard_entries_are_newest_first_deduplicated_and_bounded() {
        let mut p = palette();
        p.copied("one".into());
        p.copied("two".into());
        p.copied("one".into());
        assert_eq!(labels(&p.query(Source::Clipboard, "")), ["one", "two"]);
        for i in 0..100 {
            p.copied(format!("item {i}"));
        }
        assert_eq!(p.clip_count(), MAX_CLIPS);
        assert_eq!(p.query(Source::Clipboard, "")[0].label, "item 99");
    }

    #[test]
    fn blank_and_oversized_copies_are_ignored() {
        let mut p = palette();
        p.copied("   \n ".into());
        p.copied("x".repeat(MAX_CLIP_BYTES + 1));
        assert_eq!(p.clip_count(), 0);
        p.copied("x".repeat(MAX_CLIP_BYTES));
        assert_eq!(p.clip_count(), 1);
    }

    #[test]
    fn a_clipboard_entry_is_pasted_whole_but_shown_as_one_safe_line() {
        let mut p = palette();
        p.copied("first line\u{202e}\nsecond line\nthird".into());
        let rows = p.query(Source::Clipboard, "");
        assert!(!rows[0].label.contains('\u{202e}') && !rows[0].label.contains('\n'));
        assert_eq!(rows[0].detail, "3 lines");
        assert_eq!(
            p.activate(p.generation(), 0),
            Ok(Outcome::Paste(
                "first line\u{202e}\nsecond line\nthird".into()
            ))
        );
    }

    #[test]
    fn clipboard_search_matches_anywhere_ignoring_case() {
        let mut p = palette();
        p.copied("Meeting at NOON".into());
        p.copied("grocery list".into());
        assert_eq!(
            labels(&p.query(Source::Clipboard, "noon"))[0],
            "Meeting at NOON"
        );
        assert_eq!(
            p.query(Source::Clipboard, "noon").len(),
            2,
            "one match plus the Ask row"
        );
    }

    #[test]
    fn forgetting_the_clipboard_removes_everything() {
        let mut p = palette();
        p.copied("secret".into());
        p.forget_clipboard();
        assert!(p.query(Source::Clipboard, "").is_empty());
    }

    #[test]
    fn actions_are_found_by_name_or_keyword_and_carry_their_meaning() {
        let mut p = palette();
        let rows = p.query(Source::Actions, "lock");
        assert_eq!(rows[0].label, "Lock Screen");
        assert_eq!(
            p.activate(p.generation(), 0),
            Ok(Outcome::Do(Action::LockSession))
        );
        p.query(Source::Actions, "focus");
        assert_eq!(
            p.activate(p.generation(), 0),
            Ok(Outcome::Do(Action::ToggleDoNotDisturb))
        );
        assert_eq!(
            p.query(Source::Actions, "").len(),
            ACTIONS.len(),
            "no Ask row without a query"
        );
    }

    #[test]
    fn typing_always_offers_to_ask_the_assistant_last() {
        let mut p = palette();
        let rows = p.query(Source::Emoji, "make the screen dimmer");
        assert_eq!(rows.len(), 1);
        assert!(rows[0].ask && rows[0].label.starts_with("Ask Helix: make the screen"));
        assert_eq!(
            p.activate(p.generation(), 0),
            Ok(Outcome::Ask("make the screen dimmer".into()))
        );
        let rows = p.query(Source::Emoji, "pizza");
        assert!(rows.last().unwrap().ask && rows.len() == 2);
    }

    #[test]
    fn a_long_question_is_bounded_and_its_label_sanitized() {
        let mut p = palette();
        let long = format!("{}\u{202e}", "word ".repeat(400));
        let rows = p.query(Source::Actions, &long);
        let ask = rows.last().unwrap();
        assert!(!ask.label.contains('\u{202e}') && ask.label.chars().count() < 120);
        match p.activate(p.generation(), rows.len() - 1).unwrap() {
            Outcome::Ask(q) => assert!(q.chars().count() <= MAX_QUESTION_CHARS),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn results_are_capped() {
        let json: Vec<String> = (0..300)
            .map(|i| {
                format!(
                    r#"{{"emoji":"{}","name":"smile {i}","keywords":[],"category":"c"}}"#,
                    char::from_u32(0x1F600 + i).unwrap()
                )
            })
            .collect();
        let mut p = QuickInsert::new(parse_emoji(&format!("[{}]", json.join(","))));
        assert_eq!(
            p.query(Source::Emoji, "smile").len(),
            MAX_RESULTS + 1,
            "the cap plus the Ask row"
        );
    }

    #[test]
    fn a_bad_dataset_gives_an_empty_palette_not_a_crash() {
        assert!(parse_emoji("not json").is_empty());
        assert!(parse_emoji("{}").is_empty());
    }

    fn ranked(json: &str, query: &str) -> Vec<String> {
        let mut p = QuickInsert::new(parse_emoji(json));
        p.query(Source::Emoji, query)
            .into_iter()
            .filter(|r| !r.ask)
            .map(|r| r.label)
            .collect()
    }

    #[test]
    fn every_ranking_tier_beats_the_one_below_it() {
        // Listed worst first, so only the ranking (never the dataset order) can put them right.
        let json = r#"[
          {"emoji":"7","name":"xxabxx","keywords":[],"category":"c"},
          {"emoji":"6","name":"zzz","keywords":["xabx"],"category":"c"},
          {"emoji":"5","name":"zzz keyprefix","keywords":["abc"],"category":"c"},
          {"emoji":"4","name":"zzz keyexact","keywords":["ab"],"category":"c"},
          {"emoji":"3","name":"yy abc","keywords":[],"category":"c"},
          {"emoji":"2","name":"abc","keywords":[],"category":"c"},
          {"emoji":"1","name":"ab","keywords":[],"category":"c"}
        ]"#;
        assert_eq!(
            ranked(json, "ab"),
            [
                "ab",
                "abc",
                "yy abc",
                "zzz keyexact",
                "zzz keyprefix",
                "xxabxx",
                "zzz"
            ]
        );
    }

    #[test]
    fn among_equal_matches_the_one_you_use_more_comes_first() {
        let json = r#"[
          {"emoji":"1","name":"smile one","keywords":[],"category":"c"},
          {"emoji":"2","name":"smile two","keywords":[],"category":"c"}
        ]"#;
        let mut p = QuickInsert::new(parse_emoji(json));
        assert_eq!(p.query(Source::Emoji, "smile")[0].label, "smile one");
        p.query(Source::Emoji, "two");
        p.activate(p.generation(), 0).unwrap();
        assert_eq!(p.query(Source::Emoji, "smile")[0].label, "smile two");
    }

    #[test]
    fn a_better_action_match_beats_an_earlier_one() {
        let mut p = palette();
        let rows = p.query(Source::Actions, "t");
        let names = labels(&rows);
        assert!(
            names[..3].iter().all(|n| n.starts_with("Toggle")),
            "{names:?}"
        );
        assert_eq!(names[3], "Ask the Assistant");
    }

    #[test]
    fn a_glyph_longer_than_any_emoji_is_not_inserted() {
        let json = format!(
            r#"[{{"emoji":"{}","name":"too long","keywords":[],"category":"c"}},{{"emoji":"🍕","name":"pizza","keywords":[],"category":"c"}}]"#,
            "a".repeat(40)
        );
        assert_eq!(ranked(&json, ""), ["pizza"]);
    }

    #[test]
    fn emoji_names_are_sanitized_for_display() {
        let json = format!(
            r#"[{{"emoji":"🍕","name":"pi\u202ezza {}","keywords":[],"category":"c"}}]"#,
            "x".repeat(200)
        );
        let name = ranked(&json, "").remove(0);
        assert!(
            !name.contains('\u{202e}') && name.chars().count() <= 61,
            "{name}"
        );
    }

    #[test]
    fn clipboard_lines_decode_only_when_they_are_valid_text() {
        assert_eq!(decode_clip_hex("68690a"), Some("hi\n".into()));
        assert_eq!(decode_clip_hex("  68690a \n"), Some("hi\n".into()));
        assert_eq!(decode_clip_hex(""), None);
        assert_eq!(decode_clip_hex("686"), None, "odd length");
        assert_eq!(decode_clip_hex("6g"), None, "not hex");
        assert_eq!(decode_clip_hex("ff"), None, "not UTF-8");
        assert_eq!(decode_clip_hex("e29c93"), Some("\u{2713}".into()));
        assert_eq!(
            decode_clip_hex(&"61".repeat(MAX_CLIP_BYTES + 2)),
            None,
            "too long to be an entry"
        );
        assert!(decode_clip_hex(&"61".repeat(MAX_CLIP_BYTES)).is_some());
    }

    #[test]
    fn a_word_finds_the_same_word_with_another_ending() {
        let json = r#"[
          {"emoji":"1","name":"smiling face","keywords":[],"category":"c"},
          {"emoji":"2","name":"face with laughing eyes","keywords":["giggles"],"category":"c"}
        ]"#;
        assert_eq!(ranked(json, "smile"), ["smiling face"]);
        assert_eq!(ranked(json, "laughs"), ["face with laughing eyes"]);
        assert_eq!(ranked(json, "giggling"), ["face with laughing eyes"]);
        assert!(ranked(json, "xy").is_empty());
    }

    #[test]
    fn stems_never_shrink_a_short_word_to_nothing() {
        assert_eq!(stem("smiling"), "smil");
        assert_eq!(stem("is"), "is");
        assert_eq!(stem("ies"), "ies");
        assert_eq!(stem("cats"), "cat");
        assert_eq!(stem("dogs"), "dog");
        assert_eq!(stem("bed"), "bed");
    }

    #[test]
    fn text_with_line_breaks_or_control_characters_needs_care() {
        assert!(!needs_care("one line, tabs\tare fine"));
        assert!(needs_care("a\nb"));
        assert!(needs_care("a\rb"));
        assert!(needs_care("a\u{1b}[31mb"));
        assert!(needs_care("a\u{7}b"));
        assert!(!needs_care("unicode ✓ and emoji 🍕"));
    }
}
