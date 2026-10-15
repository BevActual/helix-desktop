// Display-safe text for anything an agent, app or window can influence. Port of
// helix::agent::sanitizeForDisplay: control characters and bidi/zero-width controls removed,
// invalid UTF-8 replaced with '?', shortened to `max_codepoints` with an ellipsis. Newlines
// become spaces so one value cannot fake extra lines.

const ELLIPSIS: &str = "…";

fn sequence_length(lead: u8) -> usize {
    if lead < 0x80 {
        1
    } else if (0xC2..=0xDF).contains(&lead) {
        2
    } else if (0xE0..=0xEF).contains(&lead) {
        3
    } else if (0xF0..=0xF4).contains(&lead) {
        4
    } else {
        0
    }
}

fn decode(bytes: &[u8], length: usize) -> u32 {
    let mut value = match length {
        2 => u32::from(bytes[0] & 0x1F),
        3 => u32::from(bytes[0] & 0x0F),
        _ => u32::from(bytes[0] & 0x07),
    };
    for &next in &bytes[1..length] {
        if next & 0xC0 != 0x80 {
            return 0xFFFF_FFFF;
        }
        value = (value << 6) | u32::from(next & 0x3F);
    }
    value
}

fn is_hidden_formatting(cp: u32) -> bool {
    (0x200B..=0x200F).contains(&cp)
        || (0x202A..=0x202E).contains(&cp)
        || (0x2060..=0x2064).contains(&cp)
        || (0x2066..=0x2069).contains(&cp)
        || cp == 0xFEFF
        || (0x80..0xA0).contains(&cp)
        || cp == 0x2028
        || cp == 0x2029
}

/// `text` may contain invalid UTF-8. Callers that already hold a `str` use [`sanitize_for_display`].
pub fn sanitize_bytes(text: &[u8], max_codepoints: usize) -> String {
    let mut out = String::new();
    let mut count = 0;
    let mut i = 0;
    while i < text.len() {
        let lead = text[i];
        let mut length = sequence_length(lead);
        let mut cp = 0xFFFF_FFFF;
        if length != 0 && i + length <= text.len() {
            cp = if length == 1 {
                u32::from(lead)
            } else {
                decode(&text[i..], length)
            };
            let overlong = (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000);
            if cp == 0xFFFF_FFFF || overlong || cp > 0x10FFFF || (0xD800..=0xDFFF).contains(&cp) {
                cp = 0xFFFF_FFFF;
            }
        } else {
            length = 1;
        }

        let piece: &[u8] = if cp == 0xFFFF_FFFF {
            b"?"
        } else if cp == u32::from(b'\n') || cp == u32::from(b'\r') || cp == u32::from(b'\t') {
            b" "
        } else if cp < 0x20 || cp == 0x7F || is_hidden_formatting(cp) {
            b""
        } else {
            &text[i..i + length]
        };
        i += length;
        if piece.is_empty() {
            continue;
        }
        if count == max_codepoints {
            out.push_str(ELLIPSIS);
            return out;
        }
        out.push_str(std::str::from_utf8(piece).unwrap_or("?"));
        count += 1;
    }
    out
}

pub fn sanitize_for_display(text: &str, max_codepoints: usize) -> String {
    sanitize_bytes(text.as_bytes(), max_codepoints)
}

/// Short name for a verified Agent Surface client identity. Taken from the script after `#` when
/// present, otherwise the path after `exe:`. Only ASCII `[A-Za-z0-9._+-]` survive; everything
/// else becomes `_`. Empty → `"unknown"`. Does not run through sanitize first: control bytes in
/// the path must not be treated as ordinary text by a filesystem helper.
pub fn friendly_client_name(client: &str, max_chars: usize) -> String {
    let path = if let Some(script) = client.split_once('#').map(|(_, rest)| rest) {
        script
    } else if let Some(rest) = client.strip_prefix("exe:") {
        rest
    } else {
        client
    };
    let name = path.rsplit('/').next().unwrap_or(path);
    let mut out = String::new();
    for &ch in name.as_bytes() {
        if out.len() == max_chars {
            out.push_str("...");
            break;
        }
        let plain =
            ch.is_ascii_alphanumeric() || ch == b'.' || ch == b'_' || ch == b'+' || ch == b'-';
        out.push(if plain { char::from(ch) } else { '_' });
    }
    if out.is_empty() {
        "unknown".to_string()
    } else {
        out
    }
}

#[cfg(test)]
mod tests {
    use super::{friendly_client_name, sanitize_bytes, sanitize_for_display};

    #[test]
    fn keeps_plain_text_and_real_unicode() {
        assert_eq!(sanitize_for_display("plain text", 50), "plain text");
        assert_eq!(sanitize_for_display("café ✓", 50), "café ✓");
    }

    #[test]
    fn newlines_become_spaces_and_controls_drop() {
        assert_eq!(
            sanitize_for_display("two\nlines\tand\rmore", 50),
            "two lines and more"
        );
        assert_eq!(sanitize_for_display("a\u{1b}[31mb\u{7}", 50), "a[31mb");
        assert_eq!(
            sanitize_for_display("safe\u{202e}txt.exe", 50),
            "safetxt.exe"
        );
        assert_eq!(
            sanitize_for_display("zero\u{200b}width\u{feff}", 50),
            "zerowidth"
        );
    }

    #[test]
    fn invalid_bytes_cannot_hide_and_length_counts_characters() {
        assert_eq!(sanitize_bytes(b"bad\xffbyte", 50), "bad?byte");
        assert!(!sanitize_bytes(b"over\xc0\xaflong", 50).contains('/'));
        assert_eq!(sanitize_bytes(b"\xed\xa0\x80", 50), "?");
        assert_eq!(sanitize_bytes(b"truncated\xe2\x82", 50), "truncated??");
        assert_eq!(sanitize_for_display("abcdefghij", 4), "abcd…");
        assert_eq!(sanitize_for_display("ééé", 2), "éé…");
    }

    #[test]
    fn friendly_name_takes_the_script_or_program_and_strips_hostility() {
        assert_eq!(
            friendly_client_name("exe:/usr/bin/helix-settings", 40),
            "helix-settings"
        );
        assert_eq!(
            friendly_client_name("exe:/usr/bin/python3.14#/home/u/tools/server.py", 40),
            "server.py"
        );
        let hostile = "exe:/tmp/evil\u{202e}exe.txt\nSettings: all access granted";
        assert_eq!(
            friendly_client_name(hostile, 40),
            "evil___exe.txt_Settings__all_access_gran..."
        );
        assert_eq!(
            friendly_client_name("exe:/tmp/Settings (trusted)", 40),
            "Settings__trusted_"
        );
        // Cyrillic 'С' is two bytes, both become '_'.
        assert_eq!(
            friendly_client_name("exe:/tmp/\u{0421}ontrol", 40),
            "__ontrol"
        );
        let long = format!("exe:/usr/bin/{}", "a".repeat(100));
        assert!(friendly_client_name(&long, 40).len() <= 43);
        assert_eq!(friendly_client_name("", 40), "unknown");
        assert_eq!(friendly_client_name("exe:/", 40), "unknown");
    }
}
