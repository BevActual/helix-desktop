//! One local diffusion request per process.
//! Protocol (unchanged from the retired C++ worker): `--model PATH`, one JSON line on stdin,
//! JSONL progress, then `{"ok":true,"png":"<base64>"}`.

use std::io::{self, BufRead, Write};
use std::os::raw::{c_char, c_int};
use std::process::ExitCode;

#[cfg(helix_sd)]
unsafe extern "C" {
    fn helix_image_generate(
        model_path: *const c_char,
        prompt: *const c_char,
        width: u32,
        height: u32,
        steps: u32,
        seed: u64,
        progress: Option<unsafe extern "C" fn(f64, *const c_char)>,
        rgb_out: *mut *mut u8,
        rgb_len: *mut usize,
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

fn validate(prompt: &str, width: u32, height: u32, steps: u32, seed: u64) -> Result<(), &'static str> {
    if prompt.is_empty() || prompt.len() > 4000 || prompt.contains('\0') {
        return Err("image prompt must be UTF-8 text up to 4000 bytes");
    }
    let side_ok = |v: u32| (256..=768).contains(&v) && v.is_multiple_of(64);
    if !side_ok(width) || !side_ok(height) {
        return Err("image dimensions must be multiples of 64 between 256 and 768");
    }
    if !(1..=8).contains(&steps) {
        return Err("image steps must be between 1 and 8");
    }
    if seed > 2_147_483_647 {
        return Err("image seed must be between 0 and 2147483647");
    }
    Ok(())
}

fn encode_png(rgb: &[u8], width: u32, height: u32) -> Option<String> {
    if rgb.len() != width as usize * height as usize * 3 || width > 768 || height > 768 {
        return None;
    }
    let mut bytes = Vec::new();
    let encoder = image::codecs::png::PngEncoder::new(&mut bytes);
    use image::ImageEncoder;
    encoder
        .write_image(rgb, width, height, image::ExtendedColorType::Rgb8)
        .ok()?;
    if bytes.len() > 4 * 1024 * 1024 {
        return None;
    }
    Some(base64::Engine::encode(
        &base64::engine::general_purpose::STANDARD,
        bytes,
    ))
}

#[cfg(helix_sd)]
unsafe extern "C" fn on_progress(fraction: f64, stage: *const c_char) {
    let stage = if stage.is_null() {
        ""
    } else {
        unsafe { std::ffi::CStr::from_ptr(stage) }.to_str().unwrap_or("")
    };
    say(&serde_json::json!({"progress": fraction, "stage": stage}));
}

fn main() -> ExitCode {
    #[cfg(not(helix_sd))]
    {
        return fail("image worker built without stable-diffusion (install third_party/sd-install)");
    }
    #[cfg(helix_sd)]
    {
        let args: Vec<String> = std::env::args().collect();
        if args.len() != 3 || args[1] != "--model" {
            return fail("an installed model is required");
        }
        let model = &args[2];
        let mut line = String::new();
        if io::stdin().lock().read_line(&mut line).is_err() || line.len() > 16384 {
            return fail("image request is too large");
        }
        let Ok(json) = serde_json::from_str::<serde_json::Value>(line.trim_end()) else {
            return fail("invalid image request or engine failure");
        };
        let Some(prompt) = json.get("prompt").and_then(|v| v.as_str()) else {
            return fail("invalid image request or engine failure");
        };
        let width = json.get("width").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
        let height = json.get("height").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
        let steps = json.get("steps").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
        let seed = json.get("seed").and_then(|v| v.as_u64()).unwrap_or(u64::MAX);
        if let Err(error) = validate(prompt, width, height, steps, seed) {
            return fail(error);
        }
        let Ok(model_c) = std::ffi::CString::new(model.as_str()) else {
            return fail("an installed model is required");
        };
        let Ok(prompt_c) = std::ffi::CString::new(prompt) else {
            return fail("image prompt contains an invalid character");
        };
        let mut rgb: *mut u8 = std::ptr::null_mut();
        let mut rgb_len = 0usize;
        let rc = unsafe {
            helix_image_generate(
                model_c.as_ptr(),
                prompt_c.as_ptr(),
                width,
                height,
                steps,
                seed,
                Some(on_progress),
                &mut rgb,
                &mut rgb_len,
            )
        };
        if rc != 0 {
            return fail(match rc {
                -2 => "the image model could not be loaded",
                -3 => "image generation failed",
                _ => "invalid image request or engine failure",
            });
        }
        let pixels = unsafe { std::slice::from_raw_parts(rgb, rgb_len) };
        let png = encode_png(pixels, width, height);
        unsafe { libc::free(rgb.cast()) };
        let Some(png) = png else {
            return fail("could not encode the image");
        };
        say(&serde_json::json!({"ok": true, "png": png}));
        ExitCode::SUCCESS
    }
}
