//! Who owns a window. A window announces an app id, and any program can announce any app id, so the app id alone must not decide which
//! application the dock credits (a program calling itself Firefox would light up Firefox's running mark and borrow its icon). The
//! compositor reports the real process of each window (from the connection, not from the program), and the host reads what that process
//! really is from `/proc`. This model says whether that process fits the installed application the app id names.
//!
//! "Fits" is deliberately about names, not paths: launchers and wrappers are common (`brave-browser-stable` starts
//! `/opt/brave.com/brave/brave`), so the program's own words (the desktop entry's id, window class and program, against the real
//! executable and, for an interpreter, the script it runs) must share a distinctive word. A program that merely claims an id it has
//! nothing to do with shares none. A sandboxed (Flatpak) process is checked by the application id the sandbox itself records.

use std::path::Path;

use crate::display::sanitize_for_display;
use crate::launcher::App;

/// What the host read about a window's process.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Process {
    /// `/proc/<pid>/exe`, resolved.
    pub exe: String,
    /// `/proc/<pid>/cmdline`, split.
    pub cmdline: Vec<String>,
    /// The Flatpak application id the sandbox records for the process, when it is sandboxed.
    pub flatpak: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Identity {
    Verified,
    Unverified,
}

/// Words too common to tell programs apart.
const STOP: &[&str] = &[
    "org", "com", "net", "io", "dev", "app", "apps", "desktop", "bin", "usr", "opt", "lib", "lib64", "libexec", "share", "local", "home",
    "browser", "stable", "gnome", "kde", "gtk", "qt", "linux", "x86", "the", "env", "exe", "main", "run", "start", "launcher", "bash",
    "python", "python3", "node", "java", "perl", "ruby", "sh", "flatpak", "snap", "appimage", "electron", "chromium",
];

const INTERPRETERS: &[&str] = &["python", "python3", "node", "nodejs", "java", "perl", "ruby", "sh", "bash", "dash", "zsh", "env", "deno", "bun", "lua", "wish"];

fn words(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut current = String::new();
    // Split on anything that is not a letter or digit, and between a lower-case letter and a capital (`HelixSettings`).
    let mut previous_lower = false;
    for c in text.chars() {
        if c.is_alphanumeric() {
            if c.is_uppercase() && previous_lower && !current.is_empty() {
                out.push(std::mem::take(&mut current));
            }
            previous_lower = c.is_lowercase();
            current.extend(c.to_lowercase());
        } else {
            previous_lower = false;
            if !current.is_empty() {
                out.push(std::mem::take(&mut current));
            }
        }
    }
    if !current.is_empty() {
        out.push(current);
    }
    out.retain(|w| w.chars().count() >= 3 && !STOP.contains(&w.as_str()) && !w.chars().all(|c| c.is_ascii_digit()));
    out
}

fn basename(path: &str) -> &str {
    Path::new(path).file_name().and_then(|n| n.to_str()).unwrap_or(path)
}

/// The program an `Exec` line really runs: past `env VAR=value` and `flatpak run`.
fn program_of(argv: &[String]) -> Option<&str> {
    let mut it = argv.iter().map(String::as_str).peekable();
    while let Some(word) = it.next() {
        if word == "env" || word.contains('=') && !word.starts_with('/') {
            continue;
        }
        if basename(word) == "flatpak" && it.peek() == Some(&"run") {
            it.next();
            return it.find(|w| !w.starts_with('-'));
        }
        return Some(word);
    }
    None
}

/// The application's own words: from its desktop id, its window class and the program it runs.
fn app_words(app: &App) -> Vec<String> {
    let mut all = words(app.id.strip_suffix(".desktop").unwrap_or(&app.id));
    all.extend(words(&app.startup_wm_class));
    if let Some(program) = program_of(&app.argv) {
        all.extend(words(basename(program)));
    }
    all.sort();
    all.dedup();
    all
}

/// The process's own words: its executable's name and folder, and (for an interpreter) the script it runs.
fn process_words(process: &Process) -> Vec<String> {
    let mut all = words(basename(&process.exe));
    if let Some(parent) = Path::new(&process.exe).parent().and_then(Path::file_name).and_then(|n| n.to_str()) {
        all.extend(words(parent));
    }
    let program = process.cmdline.first().map(|c| basename(c)).unwrap_or("");
    all.extend(words(program));
    let interpreter = |name: &str| {
        let name = name.trim_end_matches(|c: char| c.is_ascii_digit() || c == '.');
        INTERPRETERS.contains(&name)
    };
    if interpreter(basename(&process.exe)) || interpreter(program) {
        for argument in process.cmdline.iter().skip(1) {
            // The script is the first argument that is a path and not an option.
            if !argument.starts_with('-') && argument.contains('/') {
                all.extend(words(basename(argument)));
                break;
            }
        }
    }
    all.sort();
    all.dedup();
    all
}

/// Whether `process` is plausibly the installed application `app`.
pub fn verify(app: &App, process: &Process) -> Identity {
    if let Some(sandbox) = &process.flatpak {
        let id = app.id.strip_suffix(".desktop").unwrap_or(&app.id);
        return if sandbox == id { Identity::Verified } else { Identity::Unverified };
    }
    let ours = app_words(app);
    let theirs = process_words(process);
    if ours.iter().any(|w| theirs.contains(w)) {
        Identity::Verified
    } else {
        Identity::Unverified
    }
}

/// What a person sees for a process: its program's name.
pub fn program_label(process: &Process) -> String {
    let name = basename(&process.exe);
    sanitize_for_display(if name.is_empty() { "unknown program" } else { name }, 40)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::launcher::parse_desktop_entry;

    fn app(id: &str, exec: &str, wm_class: &str) -> App {
        let text = format!("[Desktop Entry]\nType=Application\nName=X\nExec={exec}\nStartupWMClass={wm_class}\n");
        parse_desktop_entry(id, &text).unwrap()
    }

    fn process(exe: &str, cmdline: &[&str]) -> Process {
        Process { exe: exe.into(), cmdline: cmdline.iter().map(|s| s.to_string()).collect(), flatpak: None }
    }

    #[test]
    fn real_applications_are_verified_including_wrapped_ones() {
        let cases = [
            (app("brave-browser.desktop", "brave-browser-stable %U", ""), process("/opt/brave.com/brave/brave", &["/opt/brave.com/brave/brave"])),
            (app("org.gnome.Nautilus.desktop", "nautilus --new-window %U", ""), process("/usr/bin/nautilus", &["nautilus"])),
            (app("cursor.desktop", "/opt/Cursor/cursor %F", "Cursor"), process("/opt/Cursor/cursor", &["/opt/Cursor/cursor"])),
            (app("dev.warp.Warp.desktop", "warp-terminal %U", ""), process("/opt/warpdotdev/warp-terminal/warp", &["warp"])),
            (app("org.helix.Settings.desktop", "helix-settings", "org.helix.Settings"), process("/usr/local/bin/helix-settings", &["helix-settings"])),
            (app("com.anthropic.Claude.desktop", "claude-desktop %U", ""), process("/usr/lib/claude-desktop/claude-desktop", &["claude-desktop"])),
            (app("foo.desktop", "env FOO=1 foo-app --x", ""), process("/usr/bin/foo-app", &["foo-app"])),
            (app("calc.desktop", "python3 /usr/share/calc/calc-main.py", "calc"), process("/usr/bin/python3.13", &["python3", "/usr/share/calc/calc.py"])),
            (app("HelixThing.desktop", "ht", ""), process("/usr/bin/helix-thing", &["helix-thing"])),
        ];
        for (application, running) in cases {
            assert_eq!(verify(&application, &running), Identity::Verified, "{} vs {:?}", application.id, running);
        }
    }

    #[test]
    fn a_program_claiming_an_application_it_is_not_is_unverified() {
        let firefox = app("firefox.desktop", "firefox %u", "firefox");
        for fake in [
            process("/usr/bin/python3", &["python3", "/tmp/evil.py"]),
            process("/tmp/x/payload", &["./payload"]),
            process("/usr/bin/bash", &["bash", "-c", "sleep 100"]),
            process("/usr/bin/node", &["node", "server.js"]),
            process("", &[]),
        ] {
            assert_eq!(verify(&firefox, &fake), Identity::Unverified, "{fake:?}");
        }
        let brave = app("brave-browser.desktop", "brave-browser-stable", "");
        assert_eq!(verify(&brave, &process("/usr/bin/firefox", &["firefox"])), Identity::Unverified, "another real application");
    }

    #[test]
    fn common_words_alone_never_verify() {
        // Two different gnome programs, two different org.* ids: shared boilerplate is not shared identity.
        let calculator = app("org.gnome.Calculator.desktop", "gnome-calculator", "");
        assert_eq!(verify(&calculator, &process("/usr/bin/gnome-terminal-server", &["gnome-terminal-server"])), Identity::Unverified);
        assert_eq!(verify(&calculator, &process("/usr/bin/gnome-calculator", &["gnome-calculator"])), Identity::Verified);
        let app_one = app("com.example.Alpha.desktop", "alpha", "");
        assert_eq!(verify(&app_one, &process("/usr/bin/beta-thing", &["x"])), Identity::Unverified);
        // A limit, stated: two programs of one vendor share the vendor's name, so each can pass for the other.
        let vendor_a = app("org.helix.Settings.desktop", "helix-settings", "");
        assert_eq!(verify(&vendor_a, &process("/usr/bin/helix-terminal", &["helix-terminal"])), Identity::Verified);
    }

    #[test]
    fn a_script_run_by_an_interpreter_is_judged_by_the_script() {
        let tool = app("pdftool.desktop", "python3 /usr/bin/pdftool", "");
        assert_eq!(verify(&tool, &process("/usr/bin/python3", &["python3", "/usr/bin/pdftool", "--gui"])), Identity::Verified);
        assert_eq!(verify(&tool, &process("/usr/bin/python3", &["python3", "-u", "/usr/bin/pdftool"])), Identity::Verified, "options before the script");
        assert_eq!(verify(&tool, &process("/usr/bin/python3", &["python3", "/tmp/other.py"])), Identity::Unverified);
        assert_eq!(verify(&tool, &process("/usr/bin/python3", &["python3", "-c", "pdftool"])), Identity::Unverified, "a word in -c code is not a script");
    }

    #[test]
    fn a_sandbox_is_judged_by_the_id_it_records() {
        let editor = app("org.gnome.TextEditor.desktop", "flatpak run org.gnome.TextEditor", "");
        let mut sandboxed = process("/app/bin/gnome-text-editor", &["gnome-text-editor"]);
        sandboxed.flatpak = Some("org.gnome.TextEditor".into());
        assert_eq!(verify(&editor, &sandboxed), Identity::Verified);
        sandboxed.flatpak = Some("org.evil.Thing".into());
        assert_eq!(verify(&editor, &sandboxed), Identity::Unverified, "a sandbox of another application cannot be the editor, whatever it is called");
        let mut named_alike = process("/app/bin/org-gnome-texteditor", &["x"]);
        named_alike.flatpak = Some("org.evil.Thing".into());
        assert_eq!(verify(&editor, &named_alike), Identity::Unverified);
    }

    #[test]
    fn words_are_split_filtered_and_lowercased() {
        assert_eq!(words("org.gnome.Nautilus"), ["nautilus"]);
        assert_eq!(words("HelixSettings"), ["helix", "settings"]);
        assert_eq!(words("brave-browser-stable"), ["brave"]);
        assert_eq!(words("a-bc-12345-x_y"), Vec::<String>::new(), "short, numeric and common words are dropped");
        assert_eq!(program_of(&["env".into(), "A=1".into(), "foo".into()]), Some("foo"));
        assert_eq!(program_of(&["flatpak".into(), "run".into(), "--branch=x".into(), "org.a.B".into()]), Some("org.a.B"));
        assert_eq!(program_of(&[]), None);
    }

    #[test]
    fn labels_are_clean_and_never_empty() {
        assert_eq!(program_label(&process("/usr/bin/firefox", &[])), "firefox");
        assert_eq!(program_label(&process("", &[])), "unknown program");
        let label = program_label(&process("/tmp/pay\u{202e}pal\n", &[]));
        assert!(!label.contains('\u{202e}') && !label.contains('\n'), "{label:?}");
    }
}
