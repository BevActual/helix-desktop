//! The live-status pill in the middle of the bar: one thing at a time, chosen by importance. Something waiting on the person beats the
//! assistant working, which beats music playing; with none of them it is not shown and the clock stands alone. Titles of what is playing
//! come from other programs, so they are sanitized and bounded here.

use serde::Serialize;

use crate::agent_desk::AgentDeskGlance;
use crate::display::sanitize_for_display;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Media {
    pub playing: bool,
    pub artist: String,
    pub title: String,
}

/// One line of `playerctl --follow metadata --format '{{status}}<TAB>{{artist}}<TAB>{{title}}'`. A paused or stopped player is still
/// returned (so the pill can fade out rather than flicker); an empty line means no player at all.
pub fn parse_media_line(line: &str) -> Option<Media> {
    let line = line.trim_end_matches(['\n', '\r']);
    let mut fields = line.splitn(3, '\t');
    let status = fields.next()?;
    let (artist, title) = (fields.next().unwrap_or(""), fields.next().unwrap_or(""));
    if status.is_empty() || !matches!(status, "Playing" | "Paused" | "Stopped") {
        return None;
    }
    Some(Media {
        playing: status == "Playing",
        artist: sanitize_for_display(artist, 40),
        title: sanitize_for_display(title, 60),
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    None,
    /// Agents are waiting for the person to decide.
    Approval,
    Assistant,
    /// Agent desk has windows while the person is on another workspace.
    AgentDesk,
    Media,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct IslandView {
    pub kind: Kind,
    pub text: String,
    pub detail: String,
}

impl IslandView {
    fn none() -> Self {
        Self {
            kind: Kind::None,
            text: String::new(),
            detail: String::new(),
        }
    }
}

/// `assistant_phase` is the assistant session's phase name (`idle`, `thinking`, `confirm`, `acting`, `result`).
pub fn choose(
    pending_approvals: usize,
    agents_enabled: bool,
    assistant_phase: &str,
    agent_desk: Option<&AgentDeskGlance>,
    media: Option<&Media>,
) -> IslandView {
    if agents_enabled && pending_approvals > 0 {
        return IslandView {
            kind: Kind::Approval,
            text: if pending_approvals == 1 {
                "Waiting for you".into()
            } else {
                format!("{pending_approvals} waiting for you")
            },
            detail: String::new(),
        };
    }
    match assistant_phase {
        "thinking" => {
            return IslandView {
                kind: Kind::Assistant,
                text: "Thinking…".into(),
                detail: String::new(),
            }
        }
        "acting" => {
            return IslandView {
                kind: Kind::Assistant,
                text: "Working…".into(),
                detail: String::new(),
            }
        }
        "confirm" => {
            return IslandView {
                kind: Kind::Assistant,
                text: "Your answer needed".into(),
                detail: String::new(),
            }
        }
        _ => {}
    }
    if let Some(desk) = agent_desk.filter(|d| d.windows > 0) {
        let text = if desk.windows == 1 {
            "1 window on agent desk".into()
        } else {
            format!("{} windows on agent desk", desk.windows)
        };
        return IslandView {
            kind: Kind::AgentDesk,
            text,
            detail: desk.label.clone(),
        };
    }
    if let Some(m) = media.filter(|m| m.playing && !m.title.is_empty()) {
        return IslandView {
            kind: Kind::Media,
            text: m.title.clone(),
            detail: m.artist.clone(),
        };
    }
    IslandView::none()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn playing(title: &str) -> Media {
        Media {
            playing: true,
            artist: "Artist".into(),
            title: title.into(),
        }
    }

    #[test]
    fn nothing_going_on_shows_nothing() {
        assert_eq!(choose(0, true, "idle", None, None).kind, Kind::None);
        assert_eq!(
            choose(0, true, "result", None, None).kind,
            Kind::None,
            "a finished request is not live status"
        );
    }

    #[test]
    fn what_waits_on_the_person_wins_over_everything() {
        let m = playing("Song");
        assert_eq!(
            choose(1, true, "thinking", None, Some(&m)).kind,
            Kind::Approval
        );
        assert_eq!(choose(1, true, "idle", None, None).text, "Waiting for you");
        assert_eq!(
            choose(3, true, "idle", None, None).text,
            "3 waiting for you"
        );
    }

    #[test]
    fn with_the_agents_switched_off_their_approvals_are_not_announced() {
        assert_eq!(choose(2, false, "idle", None, None).kind, Kind::None);
    }

    #[test]
    fn the_assistant_beats_music() {
        let m = playing("Song");
        let v = choose(0, true, "thinking", None, Some(&m));
        assert_eq!((v.kind, v.text.as_str()), (Kind::Assistant, "Thinking…"));
        assert_eq!(choose(0, true, "acting", None, Some(&m)).text, "Working…");
        assert_eq!(
            choose(0, true, "confirm", None, None).text,
            "Your answer needed"
        );
    }

    #[test]
    fn agent_desk_beats_music_but_not_assistant_work() {
        let desk = AgentDeskGlance {
            windows: 2,
            label: "Helix Agent".into(),
        };
        let m = playing("Song");
        assert_eq!(
            choose(0, true, "idle", Some(&desk), Some(&m)).kind,
            Kind::AgentDesk
        );
        assert_eq!(
            choose(0, true, "thinking", Some(&desk), None).kind,
            Kind::Assistant
        );
    }

    #[test]
    fn music_shows_only_while_playing_and_with_a_title() {
        let v = choose(0, true, "idle", None, Some(&playing("Song")));
        assert_eq!(
            (v.kind, v.text.as_str(), v.detail.as_str()),
            (Kind::Media, "Song", "Artist")
        );
        let paused = Media {
            playing: false,
            ..playing("Song")
        };
        assert_eq!(
            choose(0, true, "idle", None, Some(&paused)).kind,
            Kind::None
        );
        assert_eq!(
            choose(0, true, "idle", None, Some(&playing(""))).kind,
            Kind::None
        );
    }

    #[test]
    fn player_lines_are_parsed_and_sanitized() {
        let m = parse_media_line("Playing\tSome Artist\tA Title\n").unwrap();
        assert_eq!(
            (m.playing, m.artist.as_str(), m.title.as_str()),
            (true, "Some Artist", "A Title")
        );
        assert!(!parse_media_line("Paused\ta\tb").unwrap().playing);
        assert!(!parse_media_line("Stopped\t\t").unwrap().playing);
        let hostile =
            parse_media_line(&format!("Playing\tPay\u{202e}pal\t{}", "t".repeat(500))).unwrap();
        assert!(!hostile.artist.contains('\u{202e}') && hostile.title.chars().count() <= 61);
        assert_eq!(
            parse_media_line("Playing\tartist only").unwrap().title,
            "",
            "a missing field is empty, not an error"
        );
    }

    #[test]
    fn lines_that_are_not_player_status_are_ignored() {
        for bad in [
            "",
            "\n",
            "No players found",
            "playing\ta\tb",
            "Playing now\ta\tb",
        ] {
            assert!(parse_media_line(bad).is_none(), "{bad:?}");
        }
    }

    #[test]
    fn a_tab_inside_a_title_does_not_split_it() {
        let m = parse_media_line("Playing\tArtist\tTitle\twith a tab").unwrap();
        assert_eq!(m.artist, "Artist");
        assert_eq!(
            m.title, "Title with a tab",
            "the third field is everything after the second tab, the tab shown as a space"
        );
    }
}
