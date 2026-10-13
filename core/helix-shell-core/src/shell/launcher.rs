//! The application launcher's logic: reading desktop entries, turning an `Exec` line into an argument list, and ranking search
//! results. Nothing here touches the file system, a display or a process. A command line is never built by joining strings and
//! never goes through a shell: [`parse_exec`] produces the argument vector and the caller runs it directly.

use crate::display::sanitize_for_display;

const MAX_NAME_CHARS: usize = 80;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct App {
    /// Desktop file id, e.g. `org.gnome.Calculator.desktop`. What the page sends back to launch it.
    pub id: String,
    pub name: String,
    pub generic_name: String,
    pub keywords: Vec<String>,
    /// The `Icon` value: a theme icon name or an absolute path. Empty when none.
    pub icon: String,
    /// Program and arguments, field codes removed. Never empty.
    pub argv: Vec<String>,
    pub terminal: bool,
    /// `StartupWMClass`: the app id its windows announce, when it differs from the desktop file's name. Empty when unset.
    pub startup_wm_class: String,
    only_show_in: Vec<String>,
    not_show_in: Vec<String>,
}

impl App {
    /// Whether this entry is meant for a session whose `XDG_CURRENT_DESKTOP` names are `desktops`.
    pub fn shown_in(&self, desktops: &[String]) -> bool {
        let named = |list: &[String]| {
            list.iter()
                .any(|entry| desktops.iter().any(|d| d.eq_ignore_ascii_case(entry)))
        };
        if !self.only_show_in.is_empty() && !named(&self.only_show_in) {
            return false;
        }
        !named(&self.not_show_in)
    }
}

fn list(value: &str) -> Vec<String> {
    value
        .split(';')
        .map(str::trim)
        .filter(|item| !item.is_empty())
        .map(str::to_string)
        .collect()
}

/// Parses one `.desktop` file. `None` for anything that should not be listed: not an application, hidden, no name, or no
/// usable command. Only the plain `Name`, not translations, is read.
pub fn parse_desktop_entry(id: &str, text: &str) -> Option<App> {
    let mut in_entry = false;
    let mut fields = std::collections::BTreeMap::<&str, &str>::new();
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            in_entry = line == "[Desktop Entry]";
            continue;
        }
        if !in_entry || line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some((key, value)) = line.split_once('=') {
            // The first value for a key wins; translated keys (`Name[fr]`) are different keys and are ignored.
            fields.entry(key.trim()).or_insert_with(|| value.trim());
        }
    }
    if fields.get("Type").copied() != Some("Application") {
        return None;
    }
    let truthy = |key: &str| fields.get(key).copied() == Some("true");
    if truthy("NoDisplay") || truthy("Hidden") {
        return None;
    }
    let name = sanitize_for_display(fields.get("Name").copied()?, MAX_NAME_CHARS);
    if name.is_empty() {
        return None;
    }
    let argv = parse_exec(fields.get("Exec").copied()?)?;
    Some(App {
        id: id.to_string(),
        name,
        generic_name: sanitize_for_display(
            fields.get("GenericName").copied().unwrap_or(""),
            MAX_NAME_CHARS,
        ),
        icon: fields.get("Icon").copied().unwrap_or("").to_string(),
        keywords: list(fields.get("Keywords").copied().unwrap_or("")),
        argv,
        terminal: truthy("Terminal"),
        startup_wm_class: fields
            .get("StartupWMClass")
            .copied()
            .unwrap_or("")
            .to_string(),
        only_show_in: list(fields.get("OnlyShowIn").copied().unwrap_or("")),
        not_show_in: list(fields.get("NotShowIn").copied().unwrap_or("")),
    })
}

/// Splits an `Exec` value into arguments the way the Desktop Entry spec says: whitespace separates arguments, double quotes group
/// (with `\"`, `` \` ``, `\$` and `\\` escapes inside them), `%%` is a literal percent and every other field code (`%f`, `%U`, ...)
/// is dropped, because the launcher opens no files. `None` when nothing is left to run.
pub fn parse_exec(exec: &str) -> Option<Vec<String>> {
    let mut args: Vec<String> = Vec::new();
    let mut current = String::new();
    let mut started = false;
    let mut quoted = false;
    let mut chars = exec.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '"' => {
                quoted = !quoted;
                started = true;
            }
            '\\' if quoted => match chars.peek().copied() {
                Some(next @ ('"' | '`' | '$' | '\\')) => {
                    current.push(next);
                    chars.next();
                }
                _ => current.push('\\'),
            },
            '%' => {
                // `%%` is a literal percent; every other field code has nothing to substitute and is dropped.
                if chars.next() == Some('%') {
                    current.push('%');
                    started = true;
                }
            }
            c if c.is_whitespace() && !quoted => {
                if started {
                    args.push(std::mem::take(&mut current));
                    started = false;
                }
            }
            c => {
                current.push(c);
                started = true;
            }
        }
    }
    if quoted {
        return None; // an unterminated quote is a broken entry, not something to guess at
    }
    if started {
        args.push(current);
    }
    (!args.is_empty() && !args[0].is_empty()).then_some(args)
}

fn is_subsequence(needle: &str, haystack: &str) -> bool {
    let mut hay = haystack.chars();
    needle.chars().all(|n| hay.any(|h| h == n))
}

/// How well `query` (already lowercase) matches `app`; `None` for no match. Higher is better.
fn score(app: &App, query: &str) -> Option<i32> {
    let name = app.name.to_lowercase();
    if name == query {
        return Some(1000);
    }
    let length_penalty = i32::try_from(name.chars().count())
        .unwrap_or(i32::MAX)
        .min(100);
    if name.starts_with(query) {
        return Some(800 - length_penalty);
    }
    if name
        .split([' ', '-', '_', '.'])
        .any(|word| word.starts_with(query))
    {
        return Some(600 - length_penalty);
    }
    if name.contains(query) {
        return Some(400 - length_penalty);
    }
    if app.generic_name.to_lowercase().contains(query)
        || app
            .keywords
            .iter()
            .any(|k| k.to_lowercase().contains(query))
    {
        return Some(250 - length_penalty);
    }
    is_subsequence(query, &name).then_some(100 - length_penalty)
}

/// What the person has launched before: how often and how recently, by app id. `last` is a launch sequence number, not a clock,
/// so recency needs no time source and tests are deterministic.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct History {
    entries: std::collections::BTreeMap<String, (u32, u64)>,
    next: u64,
}

/// Entries kept; the least recently used are forgotten beyond this.
pub const HISTORY_LIMIT: usize = 200;
/// The most a launch history can add to a score. It breaks ties and reorders near-equal matches, and is kept below the gap
/// between match kinds (a prefix match beats a word match by 200) so what you typed always matters more than what you did.
const MAX_BOOST: i32 = 60;

impl History {
    pub fn from_entries(entries: impl IntoIterator<Item = (String, u32, u64)>) -> Self {
        let mut history = Self::default();
        for (id, count, last) in entries {
            history.next = history.next.max(last);
            history.entries.insert(id, (count, last));
        }
        history
    }

    pub fn entries(&self) -> impl Iterator<Item = (&str, u32, u64)> {
        self.entries
            .iter()
            .map(|(id, (count, last))| (id.as_str(), *count, *last))
    }

    pub fn record(&mut self, id: &str) {
        self.next += 1;
        let entry = self.entries.entry(id.to_string()).or_insert((0, 0));
        entry.0 = entry.0.saturating_add(1);
        entry.1 = self.next;
        while self.entries.len() > HISTORY_LIMIT {
            let Some(oldest) = self
                .entries
                .iter()
                .min_by_key(|(_, (_, last))| *last)
                .map(|(id, _)| id.clone())
            else {
                break;
            };
            self.entries.remove(&oldest);
        }
    }

    fn last(&self, id: &str) -> u64 {
        self.entries.get(id).map_or(0, |(_, last)| *last)
    }

    fn boost(&self, id: &str) -> i32 {
        let Some((count, last)) = self.entries.get(id) else {
            return 0;
        };
        let frequent = i32::try_from((*count).min(5)).unwrap_or(5) * 6; // up to 30
        let recent = if self.next.saturating_sub(*last) < 5 {
            30
        } else {
            0
        }; // among the last five launches
        (frequent + recent).min(MAX_BOOST)
    }
}

/// Indexes into `apps`, best match first, at most `limit`. An empty query lists apps alphabetically.
pub fn search(apps: &[App], query: &str, limit: usize) -> Vec<usize> {
    search_with_history(apps, query, limit, &History::default())
}

/// One application for `app-launch`: exact desktop id, exact name, or a single search hit.
/// Ambiguous search results return `None` so the agent cannot guess.
pub fn resolve_launch<'a>(apps: &'a [App], query: &str) -> Option<&'a App> {
    let query = query.trim();
    if query.is_empty() {
        return None;
    }
    if let Some(app) = apps.iter().find(|app| app.id == query) {
        return Some(app);
    }
    let with_suffix = format!("{query}.desktop");
    if let Some(app) = apps.iter().find(|app| app.id == with_suffix) {
        return Some(app);
    }
    if let Some(app) = apps
        .iter()
        .find(|app| app.name.eq_ignore_ascii_case(query))
    {
        return Some(app);
    }
    let hits = search(apps, query, 2);
    match hits.as_slice() {
        [only] => Some(&apps[*only]),
        _ => None,
    }
}

fn by_name(apps: &[App], a: usize, b: usize) -> std::cmp::Ordering {
    apps[a]
        .name
        .to_lowercase()
        .cmp(&apps[b].name.to_lowercase())
        .then_with(|| a.cmp(&b))
}

/// Like [`search`], but what was launched before counts. With no query, recently launched apps come first (most recent first),
/// then the rest alphabetically. With a query, past use adds a small boost that never outweighs a better kind of text match.
pub fn search_with_history(
    apps: &[App],
    query: &str,
    limit: usize,
    history: &History,
) -> Vec<usize> {
    let query = query.trim().to_lowercase();
    if query.is_empty() {
        let mut all: Vec<usize> = (0..apps.len()).collect();
        all.sort_by(|a, b| {
            history
                .last(&apps[*b].id)
                .cmp(&history.last(&apps[*a].id))
                .then_with(|| by_name(apps, *a, *b))
        });
        all.truncate(limit);
        return all;
    }
    let mut hits: Vec<(i32, usize)> = apps
        .iter()
        .enumerate()
        .filter_map(|(index, app)| score(app, &query).map(|s| (s + history.boost(&app.id), index)))
        .collect();
    hits.sort_by(|(a_score, a), (b_score, b)| {
        b_score.cmp(a_score).then_with(|| by_name(apps, *a, *b))
    });
    hits.into_iter()
        .take(limit)
        .map(|(_, index)| index)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn app(name: &str) -> App {
        parse_desktop_entry(
            &format!("{name}.desktop"),
            &format!(
                "[Desktop Entry]\nType=Application\nName={name}\nExec={}\n",
                name.to_lowercase().replace(' ', "-")
            ),
        )
        .unwrap()
    }
    fn names(apps: &[App], hits: &[usize]) -> Vec<String> {
        hits.iter().map(|&i| apps[i].name.clone()).collect()
    }

    #[test]
    fn resolve_launch_prefers_id_then_name_then_a_single_search_hit() {
        let apps = vec![
            app("Helix Settings"),
            parse_desktop_entry(
                "org.helix.Software.desktop",
                "[Desktop Entry]\nType=Application\nName=Helix Software\nExec=helix-software\n",
            )
            .unwrap(),
            app("Calculator"),
            app("Calendar"),
        ];
        assert_eq!(
            resolve_launch(&apps, "org.helix.Software.desktop").map(|a| a.id.as_str()),
            Some("org.helix.Software.desktop")
        );
        assert_eq!(
            resolve_launch(&apps, "org.helix.Software").map(|a| a.id.as_str()),
            Some("org.helix.Software.desktop")
        );
        assert_eq!(
            resolve_launch(&apps, "helix settings").map(|a| a.name.as_str()),
            Some("Helix Settings")
        );
        assert_eq!(
            resolve_launch(&apps, "calc").map(|a| a.name.as_str()),
            Some("Calculator")
        );
        assert!(resolve_launch(&apps, "cal").is_none(), "Calculator and Calendar both match");
        assert!(resolve_launch(&apps, "").is_none());
        assert!(resolve_launch(&apps, "no-such-app").is_none());
    }

    #[test]
    fn exec_lines_become_argument_lists_without_a_shell() {
        let cases: &[(&str, Option<&[&str]>)] = &[
            ("firefox", Some(&["firefox"])),
            ("firefox %u", Some(&["firefox"])),
            ("code --new-window %F", Some(&["code", "--new-window"])),
            (
                r#""/opt/My App/run" --flag %f"#,
                Some(&["/opt/My App/run", "--flag"]),
            ),
            (
                r#"sh -c "echo \"hi\" \$HOME""#,
                Some(&["sh", "-c", r#"echo "hi" $HOME"#]),
            ),
            ("tool 100%% done", Some(&["tool", "100%", "done"])),
            ("env FOO=bar app", Some(&["env", "FOO=bar", "app"])),
            ("  spaced   out  ", Some(&["spaced", "out"])),
            (r#"app "" x"#, Some(&["app", "", "x"])),
            ("", None),
            ("   ", None),
            ("%U", None),
            (r#""unterminated"#, None),
            (r#""" x"#, None),
        ];
        for (input, expected) in cases {
            let expected =
                expected.map(|args| args.iter().map(|a| a.to_string()).collect::<Vec<_>>());
            assert_eq!(parse_exec(input), expected, "{input:?}");
        }
    }

    #[test]
    fn shell_syntax_is_just_text() {
        // Nothing here is interpreted: it is handed to the program as arguments, never to a shell.
        assert_eq!(
            parse_exec("app; rm -rf ~ && `x` $(y) | z").unwrap(),
            vec!["app;", "rm", "-rf", "~", "&&", "`x`", "$(y)", "|", "z"]
        );
    }

    #[test]
    fn only_real_applications_are_listed() {
        let ok = "[Desktop Entry]\nType=Application\nName=Calc\nExec=calc\n";
        assert!(parse_desktop_entry("calc.desktop", ok).is_some());
        for broken in [
            "[Desktop Entry]\nType=Link\nName=Calc\nExec=calc\n",
            "[Desktop Entry]\nType=Application\nName=Calc\nExec=calc\nNoDisplay=true\n",
            "[Desktop Entry]\nType=Application\nName=Calc\nExec=calc\nHidden=true\n",
            "[Desktop Entry]\nType=Application\nExec=calc\n",
            "[Desktop Entry]\nType=Application\nName=Calc\n",
            "[Desktop Entry]\nType=Application\nName=Calc\nExec=\n",
            "[Desktop Entry]\nName=Calc\nExec=calc\n",
            "[Desktop Action New]\nType=Application\nName=Calc\nExec=calc\n",
            "",
        ] {
            assert!(
                parse_desktop_entry("x.desktop", broken).is_none(),
                "{broken:?}"
            );
        }
    }

    #[test]
    fn translations_comments_and_other_sections_are_ignored() {
        let text = "# comment\n[Desktop Entry]\nType=Application\nName[fr]=Calculatrice\nName=Calc\nExec=calc %U\n\n[Desktop Action Window]\nName=New Window\nExec=other\n";
        let app = parse_desktop_entry("calc.desktop", text).unwrap();
        assert_eq!(app.name, "Calc");
        assert_eq!(app.argv, vec!["calc"]);
    }

    #[test]
    fn names_are_made_display_safe() {
        let text = "[Desktop Entry]\nType=Application\nName=Evil\u{202e}Name\u{7}\nExec=x\n";
        let name = parse_desktop_entry("e.desktop", text).unwrap().name;
        assert!(
            !name.contains('\u{202e}') && !name.contains('\u{7}'),
            "{name:?}"
        );
    }

    #[test]
    fn only_show_in_and_not_show_in_follow_the_session() {
        let text =
            |extra: &str| format!("[Desktop Entry]\nType=Application\nName=X\nExec=x\n{extra}\n");
        let niri = vec!["niri".to_string()];
        let gnome = vec!["GNOME".to_string()];
        let plain = parse_desktop_entry("x.desktop", &text("")).unwrap();
        assert!(plain.shown_in(&niri));
        let only = parse_desktop_entry("x.desktop", &text("OnlyShowIn=GNOME;KDE;")).unwrap();
        assert!(only.shown_in(&gnome) && !only.shown_in(&niri));
        let not = parse_desktop_entry("x.desktop", &text("NotShowIn=niri;")).unwrap();
        assert!(!not.shown_in(&niri) && not.shown_in(&gnome));
        assert!(
            only.shown_in(&["gnome".to_string()]),
            "desktop names compare without case"
        );
    }

    #[test]
    fn better_matches_come_first() {
        let apps: Vec<App> = [
            "Files",
            "Firefox",
            "Calculator",
            "Profile Editor",
            "Fire Tool",
        ]
        .iter()
        .map(|n| app(n))
        .collect();
        assert_eq!(
            names(&apps, &search(&apps, "fire", 10)),
            ["Firefox", "Fire Tool"]
        );
        assert_eq!(
            names(&apps, &search(&apps, "FIREFOX", 10)),
            ["Firefox"],
            "case does not matter"
        );
        assert_eq!(
            names(&apps, &search(&apps, "edit", 10)),
            ["Profile Editor"],
            "a word start beats nothing"
        );
        assert_eq!(
            names(&apps, &search(&apps, "alc", 10)),
            ["Calculator"],
            "a substring matches"
        );
        assert_eq!(
            names(&apps, &search(&apps, "fls", 10)),
            ["Files"],
            "letters in order still match"
        );
        assert!(search(&apps, "zzz", 10).is_empty());
    }

    #[test]
    fn exact_beats_prefix_beats_word_beats_substring() {
        let apps: Vec<App> = ["Notepad", "Pad", "Super Pad", "Keypad Tool", "Pad Pro"]
            .iter()
            .map(|n| app(n))
            .collect();
        assert_eq!(
            names(&apps, &search(&apps, "pad", 10)),
            ["Pad", "Pad Pro", "Super Pad", "Notepad", "Keypad Tool"]
        );
    }

    #[test]
    fn keywords_and_generic_names_find_apps_the_name_does_not() {
        let text = "[Desktop Entry]\nType=Application\nName=GIMP\nGenericName=Image Editor\nKeywords=photo;paint;\nExec=gimp\n";
        let apps = vec![
            parse_desktop_entry("gimp.desktop", text).unwrap(),
            app("Files"),
        ];
        assert_eq!(search(&apps, "photo", 10), vec![0]);
        assert_eq!(search(&apps, "image", 10), vec![0]);
    }

    #[test]
    fn an_empty_query_lists_alphabetically_and_the_limit_applies() {
        let apps: Vec<App> = ["Zed", "alpha", "Beta", "Gamma"]
            .iter()
            .map(|n| app(n))
            .collect();
        assert_eq!(
            names(&apps, &search(&apps, "", 10)),
            ["alpha", "Beta", "Gamma", "Zed"]
        );
        assert_eq!(names(&apps, &search(&apps, "  ", 2)), ["alpha", "Beta"]);
        assert!(search(&apps, "a", 0).is_empty());
    }

    #[test]
    fn with_no_query_recent_apps_come_first_then_alphabetical() {
        let apps: Vec<App> = ["Zed", "alpha", "Beta", "Gamma"]
            .iter()
            .map(|n| app(n))
            .collect();
        let mut history = History::default();
        history.record("Gamma.desktop");
        history.record("Zed.desktop");
        assert_eq!(
            names(&apps, &search_with_history(&apps, "", 10, &history)),
            ["Zed", "Gamma", "alpha", "Beta"]
        );
        history.record("Gamma.desktop");
        assert_eq!(
            names(&apps, &search_with_history(&apps, "", 10, &history)),
            ["Gamma", "Zed", "alpha", "Beta"]
        );
    }

    #[test]
    fn use_breaks_ties_but_never_beats_a_better_match() {
        let apps: Vec<App> = ["Terminal Pro", "Terminator", "Determinant Tool"]
            .iter()
            .map(|n| app(n))
            .collect();
        // Equal kind of match: the one you use more comes first.
        assert_eq!(names(&apps, &search(&apps, "termin", 10))[0], "Terminator");
        let mut history = History::default();
        for _ in 0..9 {
            history.record("Terminal Pro.desktop");
        }
        assert_eq!(
            names(&apps, &search_with_history(&apps, "termin", 10, &history))[0],
            "Terminal Pro"
        );
        // A substring match, however much used, stays below a prefix match.
        for _ in 0..50 {
            history.record("Determinant Tool.desktop");
        }
        let order = names(&apps, &search_with_history(&apps, "termin", 10, &history));
        assert_eq!(order.last().unwrap(), "Determinant Tool", "{order:?}");
    }

    #[test]
    fn history_is_bounded_and_forgets_the_least_recent() {
        let mut history = History::default();
        for i in 0..(HISTORY_LIMIT + 25) {
            history.record(&format!("app{i}.desktop"));
        }
        assert_eq!(history.entries().count(), HISTORY_LIMIT);
        assert!(
            history.entries().all(|(id, _, _)| id != "app0.desktop"),
            "the oldest was dropped"
        );
        assert!(history
            .entries()
            .any(|(id, _, _)| id == format!("app{}.desktop", HISTORY_LIMIT + 24)));
    }

    #[test]
    fn history_round_trips_and_keeps_counting_forward() {
        let mut history = History::default();
        history.record("a.desktop");
        history.record("b.desktop");
        history.record("a.desktop");
        let saved: Vec<(String, u32, u64)> = history
            .entries()
            .map(|(id, c, l)| (id.to_string(), c, l))
            .collect();
        let mut loaded = History::from_entries(saved);
        assert_eq!(loaded, history);
        loaded.record("b.desktop");
        let last = |h: &History, id: &str| h.entries().find(|(i, _, _)| *i == id).unwrap().2;
        assert!(
            last(&loaded, "b.desktop") > last(&loaded, "a.desktop"),
            "a launch after loading is the most recent"
        );
    }

    #[test]
    fn the_icon_is_read_from_the_entry() {
        let text = "[Desktop Entry]\nType=Application\nName=X\nExec=x\nIcon=org.example.X\n";
        assert_eq!(
            parse_desktop_entry("x.desktop", text).unwrap().icon,
            "org.example.X"
        );
        assert_eq!(app("Plain").icon, "");
    }

    #[test]
    fn a_recent_launch_outranks_an_equal_match_used_once_long_ago() {
        let apps: Vec<App> = ["Foo A", "Foo B"].iter().map(|n| app(n)).collect();
        let mut history = History::default();
        history.record("Foo A.desktop");
        for other in 0..6 {
            history.record(&format!("other{other}.desktop"));
        }
        assert_eq!(
            names(&apps, &search_with_history(&apps, "foo", 10, &history)),
            ["Foo A", "Foo B"],
            "A is old: the tie falls back to alphabetical"
        );
        history.record("Foo B.desktop");
        assert_eq!(
            names(&apps, &search_with_history(&apps, "foo", 10, &history)),
            ["Foo B", "Foo A"],
            "B was just launched"
        );
    }
}
