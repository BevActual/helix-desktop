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
    /// Program and arguments, field codes removed. Never empty.
    pub argv: Vec<String>,
    pub terminal: bool,
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
        keywords: list(fields.get("Keywords").copied().unwrap_or("")),
        argv,
        terminal: truthy("Terminal"),
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

/// Indexes into `apps`, best match first, at most `limit`. An empty query lists apps alphabetically.
pub fn search(apps: &[App], query: &str, limit: usize) -> Vec<usize> {
    let query = query.trim().to_lowercase();
    let mut hits: Vec<(i32, usize)> = apps
        .iter()
        .enumerate()
        .filter_map(|(index, app)| {
            if query.is_empty() {
                Some((0, index))
            } else {
                score(app, &query).map(|s| (s, index))
            }
        })
        .collect();
    hits.sort_by(|(a_score, a), (b_score, b)| {
        b_score
            .cmp(a_score)
            .then_with(|| {
                apps[*a]
                    .name
                    .to_lowercase()
                    .cmp(&apps[*b].name.to_lowercase())
            })
            .then_with(|| a.cmp(b))
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
}
