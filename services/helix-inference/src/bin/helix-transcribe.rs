//! Speech-to-text worker: one transcription per process.
//! Protocol (unchanged from the retired C++ worker): `--model` / `--language` / `--threads`,
//! stdin = little-endian `u32` sample count + `i16` PCM @ 16 kHz mono; stdout = one JSON line.

use std::io::{self, Read, Write};
use std::os::raw::{c_char, c_float, c_int};
use std::process::ExitCode;

const MAX_SAMPLES: u32 = 16_000 * 600;
const TEXT_CAP: usize = 256 * 1024;
const LANG_CAP: usize = 16;

#[cfg(helix_whisper)]
unsafe extern "C" {
    fn helix_transcribe_harden();
    fn helix_whisper_transcribe(
        model_path: *const c_char,
        language: *const c_char,
        threads: c_int,
        samples: *const c_float,
        n_samples: c_int,
        text_out: *mut c_char,
        text_cap: usize,
        lang_out: *mut c_char,
        lang_cap: usize,
    ) -> c_int;
}

fn say(value: &serde_json::Value) {
    let mut line = value.to_string();
    line.push('\n');
    let _ = io::stdout().write_all(line.as_bytes());
    let _ = io::stdout().flush();
}

fn fail(why: &str) -> ExitCode {
    say(&serde_json::json!({"ok": false, "error": why}));
    ExitCode::from(1)
}

fn read_exact(buf: &mut [u8]) -> bool {
    let mut filled = 0;
    while filled < buf.len() {
        match io::stdin().read(&mut buf[filled..]) {
            Ok(0) => return false,
            Ok(n) => filled += n,
            Err(_) => return false,
        }
    }
    true
}

fn trim_markers(mut text: String) -> String {
    let trim = |s: &str| s.trim_matches(|c: char| " \t\r\n".contains(c)).to_string();
    text = trim(&text);
    for marker in ["[BLANK_AUDIO]", "(silence)", "[silence]", "[ Silence ]"] {
        while let Some(at) = text.find(marker) {
            text.replace_range(at..at + marker.len(), "");
        }
    }
    trim(&text)
}

fn main() -> ExitCode {
    #[cfg(not(helix_whisper))]
    {
        return fail("speech worker built without whisper (install third_party/whisper-install)");
    }

    #[cfg(helix_whisper)]
    {
        let args: Vec<String> = std::env::args().collect();
        let mut model = String::new();
        let mut language = "auto".to_string();
        let cpus = std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(4);
        let mut threads = (cpus / 2).clamp(2, 8) as i32;
        let mut i = 1;
        while i + 1 < args.len() {
            match args[i].as_str() {
                "--model" => model = args[i + 1].clone(),
                "--language" => language = args[i + 1].clone(),
                "--threads" => {
                    threads = args[i + 1].parse::<i32>().unwrap_or(threads).clamp(1, 16);
                }
                _ => {}
            }
            i += 2;
        }
        if model.is_empty() {
            return fail("no model given");
        }
        let plain = language == "auto"
            || ((2..=3).contains(&language.len())
                && language.bytes().all(|c| (b'a'..=b'z').contains(&c)));
        if !plain {
            return fail("bad language");
        }

        unsafe { helix_transcribe_harden() };

        let mut count_buf = [0u8; 4];
        if !read_exact(&mut count_buf) {
            return fail("bad audio");
        }
        let count = u32::from_le_bytes(count_buf);
        if count == 0 || count > MAX_SAMPLES {
            return fail("bad audio");
        }
        let mut pcm_bytes = vec![0u8; count as usize * 2];
        if !read_exact(&mut pcm_bytes) {
            return fail("short audio");
        }
        let mut samples = Vec::with_capacity(count as usize);
        let mut peak = 0.0f32;
        for chunk in pcm_bytes.chunks_exact(2) {
            let sample = i16::from_le_bytes([chunk[0], chunk[1]]);
            let f = sample as f32 / 32768.0;
            peak = peak.max(f.abs());
            samples.push(f);
        }
        if peak < 0.004 || count < 3200 {
            say(&serde_json::json!({
                "ok": true,
                "text": "",
                "language": if language == "auto" { "" } else { language.as_str() },
            }));
            return ExitCode::SUCCESS;
        }

        let Ok(model_c) = std::ffi::CString::new(model) else {
            return fail("bad model path");
        };
        let Ok(lang_c) = std::ffi::CString::new(language.as_str()) else {
            return fail("bad language");
        };
        let mut text_buf = vec![0u8; TEXT_CAP];
        let mut lang_buf = vec![0u8; LANG_CAP];
        let rc = unsafe {
            helix_whisper_transcribe(
                model_c.as_ptr(),
                lang_c.as_ptr(),
                threads,
                samples.as_ptr(),
                samples.len() as c_int,
                text_buf.as_mut_ptr().cast::<c_char>(),
                TEXT_CAP,
                lang_buf.as_mut_ptr().cast::<c_char>(),
                LANG_CAP,
            )
        };
        if rc != 0 {
            return fail(match rc {
                -2 => "the speech model could not be loaded",
                -3 => "the transcription failed",
                -4 => "the transcription was too long",
                _ => "the transcription failed",
            });
        }
        let text = std::ffi::CStr::from_bytes_until_nul(&text_buf)
            .map(|c| c.to_string_lossy().into_owned())
            .unwrap_or_default();
        let spoken = std::ffi::CStr::from_bytes_until_nul(&lang_buf)
            .map(|c| c.to_string_lossy().into_owned())
            .unwrap_or_default();
        say(&serde_json::json!({
            "ok": true,
            "text": trim_markers(text),
            "language": spoken,
        }));
        ExitCode::SUCCESS
    }
}
