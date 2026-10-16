//! Pictures in and out. Bytes a client sends are untrusted: they are sniffed, bounded and decoded with memory-safe Rust
//! decoders before any engine sees them (port of C++ `image_input.cpp`; the PNG encoder is `image_generator.cpp`).

use std::io::Cursor;

use base64::Engine;
use image::{ImageDecoder, ImageReader};

use crate::backend::{ChatRequest, ImageInput};

pub const MAX_IMAGE_BYTES: usize = 8 * 1024 * 1024; // one encoded picture
pub const MAX_IMAGES: usize = 4; // per request
pub const MAX_IMAGE_BYTES_TOTAL: usize = 16 * 1024 * 1024;
pub const MAX_IMAGE_SIDE: u32 = 12000; // refused when the header says larger
pub const MAX_IMAGE_PIXELS: u64 = 50_000_000; // refused before any pixel memory is allocated
pub const ENGINE_IMAGE_SIDE: u32 = 1568; // larger pictures are scaled down (area average) to this
pub const MAX_GENERATED_PNG: usize = 4 * 1024 * 1024;

/// What the bytes claim to be; anything else (JPEG XL, ICO, SVG, TIFF…) is not offered to an engine.
fn supported(data: &[u8]) -> bool {
    data.starts_with(b"\x89PNG\r\n\x1a\n")
        || data.starts_with(b"\xff\xd8\xff")
        || data.starts_with(b"GIF8")
        || data.starts_with(b"BM")
        || (data.len() >= 12 && data.starts_with(b"RIFF") && &data[8..12] == b"WEBP")
}

pub fn decode_image_input(data: &[u8]) -> Result<ImageInput, String> {
    if data.is_empty() {
        return Err("the picture is empty".into());
    }
    if data.len() > MAX_IMAGE_BYTES {
        return Err("the picture is larger than 8 MiB".into());
    }
    if !supported(data) {
        return Err("unsupported picture format (use PNG, JPEG, GIF, BMP or WebP)".into());
    }
    let reader = ImageReader::new(Cursor::new(data))
        .with_guessed_format()
        .map_err(|e| format!("the picture could not be decoded: {e}"))?;
    let decoder = reader
        .into_decoder()
        .map_err(|e| format!("the picture could not be decoded: {e}"))?;
    let (width, height) = decoder.dimensions();
    if width == 0 || height == 0 {
        return Err("the picture is empty".into());
    }
    if width > MAX_IMAGE_SIDE
        || height > MAX_IMAGE_SIDE
        || u64::from(width) * u64::from(height) > MAX_IMAGE_PIXELS
    {
        return Err("the picture is too large".into());
    }
    let picture = image::DynamicImage::from_decoder(decoder)
        .map_err(|e| format!("the picture could not be decoded: {e}"))?;
    let rgba = picture.to_rgba8();
    let mut rgb = Vec::with_capacity(width as usize * height as usize * 3);
    for pixel in rgba.pixels() {
        let alpha = u32::from(pixel[3]);
        for c in 0..3 {
            // Straight alpha over white.
            rgb.push(((u32::from(pixel[c]) * alpha + 255 * (255 - alpha) + 127) / 255) as u8);
        }
    }
    Ok(scale_down(
        &ImageInput { width, height, rgb },
        ENGINE_IMAGE_SIDE,
    ))
}

/// Scales an RGB picture so neither side exceeds `max_side`, averaging source pixels.
pub fn scale_down(picture: &ImageInput, max_side: u32) -> ImageInput {
    if picture.width == 0
        || picture.height == 0
        || (picture.width <= max_side && picture.height <= max_side)
    {
        return picture.clone();
    }
    let factor = f64::from(max_side) / f64::from(picture.width.max(picture.height));
    let width = ((f64::from(picture.width) * factor) as u32).max(1);
    let height = ((f64::from(picture.height) * factor) as u32).max(1);
    let mut rgb = vec![0u8; width as usize * height as usize * 3];
    for y in 0..height {
        let y0 = (u64::from(y) * u64::from(picture.height) / u64::from(height)) as u32;
        let y1 =
            (y0 + 1).max((u64::from(y + 1) * u64::from(picture.height) / u64::from(height)) as u32);
        for x in 0..width {
            let x0 = (u64::from(x) * u64::from(picture.width) / u64::from(width)) as u32;
            let x1 = (x0 + 1)
                .max((u64::from(x + 1) * u64::from(picture.width) / u64::from(width)) as u32);
            let mut sum = [0u64; 3];
            for sy in y0..y1.min(picture.height) {
                for sx in x0..x1.min(picture.width) {
                    let at = (sy as usize * picture.width as usize + sx as usize) * 3;
                    for (c, channel) in sum.iter_mut().enumerate() {
                        *channel += u64::from(picture.rgb[at + c]);
                    }
                }
            }
            let count = u64::from(y1 - y0) * u64::from(x1 - x0);
            let target = (y as usize * width as usize + x as usize) * 3;
            for c in 0..3 {
                rgb[target + c] = ((sum[c] + count / 2) / count) as u8;
            }
        }
    }
    ImageInput { width, height, rgb }
}

/// Base64 PNG of 8-bit RGB pixels; empty when the input is out of bounds.
pub fn encode_png(rgb: &[u8], width: u32, height: u32) -> String {
    if width == 0
        || height == 0
        || width > 768
        || height > 768
        || rgb.len() != width as usize * height as usize * 3
    {
        return String::new();
    }
    let mut bytes = Vec::new();
    let encoder = image::codecs::png::PngEncoder::new(&mut bytes);
    use image::ImageEncoder;
    if encoder
        .write_image(rgb, width, height, image::ExtendedColorType::Rgb8)
        .is_err()
        || bytes.len() > MAX_GENERATED_PNG
    {
        return String::new();
    }
    base64::engine::general_purpose::STANDARD.encode(bytes)
}

pub fn validate_image_request(request: &ChatRequest) -> Result<(), String> {
    let prompt = &request.image_prompt;
    if prompt.is_empty() || prompt.len() > 4000 {
        return Err("image prompt must be UTF-8 text up to 4000 bytes".into());
    }
    if prompt.contains('\0') {
        return Err("image prompt contains an invalid character".into());
    }
    let side_ok = |v: u32| (256..=768).contains(&v) && v.is_multiple_of(64);
    if !side_ok(request.image_width) || !side_ok(request.image_height) {
        return Err("image dimensions must be multiples of 64 between 256 and 768".into());
    }
    if request.image_steps < 1 || request.image_steps > 8 {
        return Err("image steps must be between 1 and 8".into());
    }
    if request.image_seed > 2_147_483_647 {
        return Err("image seed must be between 0 and 2147483647".into());
    }
    let model = &request.model;
    if !model.is_empty()
        && (model.len() > 200
            || !model.ends_with(".gguf")
            || model.starts_with('.')
            || !model
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || c == b'.' || c == b'-' || c == b'_'))
    {
        return Err("image model must be an installed GGUF file name".into());
    }
    Ok(())
}

pub fn decode_base64(text: &str) -> Option<Vec<u8>> {
    base64::engine::general_purpose::STANDARD.decode(text).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn png_round_trips_through_the_decoder() {
        let pixels = [10u8, 20, 30].repeat(256 * 256);
        let encoded = encode_png(&pixels, 256, 256);
        let bytes = decode_base64(&encoded).unwrap();
        let picture = decode_image_input(&bytes).unwrap();
        assert_eq!((picture.width, picture.height), (256, 256));
        assert_eq!(&picture.rgb[..3], &[10, 20, 30]);
    }

    #[test]
    fn refuses_unknown_formats_and_scales_large_pictures() {
        assert!(decode_image_input(b"not a picture").is_err());
        assert!(decode_image_input(&[]).is_err());
        let big = ImageInput {
            width: 3136,
            height: 1568,
            rgb: vec![7; 3136 * 1568 * 3],
        };
        let small = scale_down(&big, 1568);
        assert_eq!((small.width, small.height), (1568, 784));
        assert_eq!(small.rgb[0], 7);
    }
}
