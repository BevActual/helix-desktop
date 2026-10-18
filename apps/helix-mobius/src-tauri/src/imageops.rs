//! The picture operations the Image stage offers: a small fixed set, each a plain description (`{"op":"crop","x":..}`) that is checked and then
//! applied to a copy by deterministic code. The person's buttons and a model's suggestions are the same descriptions, so a suggestion can be read
//! before it is applied and nothing a model writes is ever run.

use image::{imageops, DynamicImage, GenericImageView, ImageFormat, Rgba, RgbaImage};
use serde::{Deserialize, Serialize};
use std::io::Cursor;

pub const MAX_SIDE: u32 = 8192;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum Op {
    Crop {
        x: u32,
        y: u32,
        w: u32,
        h: u32,
    },
    /// Either a percentage of the current size, or an exact width and height.
    Resize {
        percent: Option<f32>,
        w: Option<u32>,
        h: Option<u32>,
    },
    Rotate {
        degrees: u32,
    },
    Flip {
        axis: String,
    },
    /// -100 to 100.
    Brightness {
        amount: i32,
    },
    Contrast {
        amount: f32,
    },
    Grayscale,
    Invert,
    Blur {
        sigma: f32,
    },
    Sharpen {
        amount: f32,
    },
    /// A rectangle drawn on the picture: an outline, or filled.
    Rect {
        x: u32,
        y: u32,
        w: u32,
        h: u32,
        color: String,
        fill: Option<bool>,
        thickness: Option<u32>,
    },
}

impl Op {
    /// A short phrase for the history and for a suggestion card.
    pub fn label(&self) -> String {
        match self {
            Op::Crop { w, h, .. } => format!("Crop to {w}×{h}"),
            Op::Resize {
                percent: Some(p), ..
            } => format!("Resize to {}%", p.round()),
            Op::Resize { w, h, .. } => format!("Resize to {}×{}", w.unwrap_or(0), h.unwrap_or(0)),
            Op::Rotate { degrees } => format!("Rotate {degrees}°"),
            Op::Flip { axis } => format!("Flip {axis}"),
            Op::Brightness { amount } => format!("Brightness {amount:+}"),
            Op::Contrast { amount } => format!("Contrast {amount:+.0}"),
            Op::Grayscale => "Black and white".into(),
            Op::Invert => "Invert colours".into(),
            Op::Blur { sigma } => format!("Blur {sigma:.1}"),
            Op::Sharpen { amount } => format!("Sharpen {amount:.1}"),
            Op::Rect { .. } => "Rectangle".into(),
        }
    }
}

fn colour(text: &str) -> Result<Rgba<u8>, String> {
    let hex = text
        .strip_prefix('#')
        .ok_or("A colour is written like #ff0000.")?;
    if hex.len() != 6 || !hex.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err("A colour is written like #ff0000.".into());
    }
    let byte = |i: usize| u8::from_str_radix(&hex[i..i + 2], 16).unwrap_or(0);
    Ok(Rgba([byte(0), byte(2), byte(4), 255]))
}

pub fn decode(bytes: &[u8]) -> Result<DynamicImage, String> {
    let image = image::load_from_memory(bytes)
        .map_err(|_| "That picture could not be read.".to_string())?;
    let (w, h) = image.dimensions();
    if w == 0 || h == 0 || w > MAX_SIDE || h > MAX_SIDE {
        return Err(format!(
            "Pictures up to {MAX_SIDE} pixels a side can be edited."
        ));
    }
    Ok(image)
}

pub fn encode(image: &DynamicImage, format: ImageFormat) -> Result<Vec<u8>, String> {
    let mut out = Cursor::new(Vec::new());
    // JPEG has no transparency.
    let image = if format == ImageFormat::Jpeg {
        DynamicImage::ImageRgb8(image.to_rgb8())
    } else {
        image.clone()
    };
    image
        .write_to(&mut out, format)
        .map_err(|e| format!("Could not write the picture: {e}"))?;
    Ok(out.into_inner())
}

/// Applies one operation to a picture and returns the new one. Every number is checked against the picture's size first.
pub fn apply(image: &DynamicImage, op: &Op) -> Result<DynamicImage, String> {
    let (w, h) = image.dimensions();
    match op {
        Op::Crop { x, y, w: cw, h: ch } => {
            if *cw == 0
                || *ch == 0
                || x.checked_add(*cw).is_none_or(|e| e > w)
                || y.checked_add(*ch).is_none_or(|e| e > h)
            {
                return Err("The crop is outside the picture.".into());
            }
            Ok(image.crop_imm(*x, *y, *cw, *ch))
        }
        Op::Resize {
            percent,
            w: nw,
            h: nh,
        } => {
            let (tw, th) = match (percent, nw, nh) {
                (Some(p), _, _) if *p >= 1.0 && *p <= 800.0 => (
                    ((w as f32) * p / 100.0).round() as u32,
                    ((h as f32) * p / 100.0).round() as u32,
                ),
                (None, Some(a), Some(b)) => (*a, *b),
                (None, Some(a), None) => {
                    (*a, ((h as f64) * (*a as f64) / (w as f64)).round() as u32)
                }
                (None, None, Some(b)) => {
                    (((w as f64) * (*b as f64) / (h as f64)).round() as u32, *b)
                }
                _ => return Err("Give a percentage (1 to 800) or a width and height.".into()),
            };
            if tw == 0 || th == 0 || tw > MAX_SIDE || th > MAX_SIDE {
                return Err(format!(
                    "The result must be between 1 and {MAX_SIDE} pixels a side."
                ));
            }
            Ok(image.resize_exact(tw, th, imageops::FilterType::Lanczos3))
        }
        Op::Rotate { degrees } => match degrees % 360 {
            90 => Ok(image.rotate90()),
            180 => Ok(image.rotate180()),
            270 => Ok(image.rotate270()),
            _ => Err("Rotate by 90, 180 or 270 degrees.".into()),
        },
        Op::Flip { axis } => match axis.as_str() {
            "horizontal" => Ok(image.fliph()),
            "vertical" => Ok(image.flipv()),
            _ => Err("Flip horizontal or vertical.".into()),
        },
        Op::Brightness { amount } if (-100..=100).contains(amount) => {
            Ok(image.brighten(*amount * 255 / 100))
        }
        Op::Contrast { amount } if (-100.0..=100.0).contains(amount) => {
            Ok(image.adjust_contrast(*amount))
        }
        Op::Grayscale => Ok(image.grayscale()),
        Op::Invert => {
            let mut out = image.clone();
            out.invert();
            Ok(out)
        }
        Op::Blur { sigma } if *sigma > 0.0 && *sigma <= 25.0 => Ok(image.blur(*sigma)),
        Op::Sharpen { amount } if *amount > 0.0 && *amount <= 10.0 => {
            Ok(image.unsharpen(1.5, (*amount * 10.0) as i32))
        }
        Op::Rect {
            x,
            y,
            w: rw,
            h: rh,
            color,
            fill,
            thickness,
        } => {
            if *rw == 0
                || *rh == 0
                || x.checked_add(*rw).is_none_or(|e| e > w)
                || y.checked_add(*rh).is_none_or(|e| e > h)
            {
                return Err("The rectangle is outside the picture.".into());
            }
            let paint = colour(color)?;
            let line = thickness.unwrap_or(3).clamp(1, 64);
            let mut canvas: RgbaImage = image.to_rgba8();
            let filled = fill.unwrap_or(false);
            for py in *y..*y + *rh {
                for px in *x..*x + *rw {
                    let edge = px < x + line
                        || px >= x + rw - line.min(*rw)
                        || py < y + line
                        || py >= y + rh - line.min(*rh);
                    if filled || edge {
                        canvas.put_pixel(px, py, paint);
                    }
                }
            }
            Ok(DynamicImage::ImageRgba8(canvas))
        }
        Op::Brightness { .. } => Err("Brightness is -100 to 100.".into()),
        Op::Contrast { .. } => Err("Contrast is -100 to 100.".into()),
        Op::Blur { .. } => Err("Blur is above 0 and up to 25.".into()),
        Op::Sharpen { .. } => Err("Sharpen is above 0 and up to 10.".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn picture(w: u32, h: u32) -> DynamicImage {
        DynamicImage::ImageRgba8(RgbaImage::from_fn(w, h, |x, _| {
            if x < w / 2 {
                Rgba([200, 0, 0, 255])
            } else {
                Rgba([0, 0, 200, 255])
            }
        }))
    }

    #[test]
    fn crop_resize_rotate_and_flip_change_the_size_and_content() {
        let p = picture(40, 20);
        assert_eq!(
            apply(
                &p,
                &Op::Crop {
                    x: 10,
                    y: 5,
                    w: 20,
                    h: 10
                }
            )
            .unwrap()
            .dimensions(),
            (20, 10)
        );
        assert_eq!(
            apply(
                &p,
                &Op::Resize {
                    percent: Some(50.0),
                    w: None,
                    h: None
                }
            )
            .unwrap()
            .dimensions(),
            (20, 10)
        );
        assert_eq!(
            apply(
                &p,
                &Op::Resize {
                    percent: None,
                    w: Some(80),
                    h: None
                }
            )
            .unwrap()
            .dimensions(),
            (80, 40)
        );
        assert_eq!(
            apply(&p, &Op::Rotate { degrees: 90 }).unwrap().dimensions(),
            (20, 40)
        );
        let flipped = apply(
            &p,
            &Op::Flip {
                axis: "horizontal".into(),
            },
        )
        .unwrap();
        assert_eq!(flipped.get_pixel(0, 0), Rgba([0, 0, 200, 255]));
    }

    #[test]
    fn nothing_outside_the_picture_or_out_of_range_is_accepted() {
        let p = picture(40, 20);
        assert!(apply(
            &p,
            &Op::Crop {
                x: 30,
                y: 0,
                w: 20,
                h: 10
            }
        )
        .is_err());
        assert!(apply(
            &p,
            &Op::Crop {
                x: u32::MAX,
                y: 0,
                w: 2,
                h: 2
            }
        )
        .is_err());
        assert!(apply(
            &p,
            &Op::Crop {
                x: 0,
                y: 0,
                w: 0,
                h: 5
            }
        )
        .is_err());
        assert!(apply(
            &p,
            &Op::Resize {
                percent: Some(5000.0),
                w: None,
                h: None
            }
        )
        .is_err());
        assert!(apply(
            &p,
            &Op::Resize {
                percent: None,
                w: Some(100_000),
                h: Some(10)
            }
        )
        .is_err());
        assert!(apply(&p, &Op::Rotate { degrees: 45 }).is_err());
        assert!(apply(&p, &Op::Brightness { amount: 500 }).is_err());
        assert!(apply(&p, &Op::Blur { sigma: 0.0 }).is_err());
        assert!(apply(
            &p,
            &Op::Rect {
                x: 0,
                y: 0,
                w: 5,
                h: 5,
                color: "red".into(),
                fill: None,
                thickness: None
            }
        )
        .is_err());
        assert!(apply(
            &p,
            &Op::Rect {
                x: 38,
                y: 0,
                w: 5,
                h: 5,
                color: "#ff0000".into(),
                fill: None,
                thickness: None
            }
        )
        .is_err());
    }

    #[test]
    fn a_rectangle_is_drawn_and_the_original_is_left_alone() {
        let p = picture(40, 20);
        let out = apply(
            &p,
            &Op::Rect {
                x: 2,
                y: 2,
                w: 10,
                h: 10,
                color: "#00ff00".into(),
                fill: Some(false),
                thickness: Some(2),
            },
        )
        .unwrap();
        assert_eq!(out.get_pixel(2, 2), Rgba([0, 255, 0, 255]));
        assert_eq!(
            out.get_pixel(6, 6),
            Rgba([200, 0, 0, 255]),
            "the inside of an outline is untouched"
        );
        assert_eq!(p.get_pixel(2, 2), Rgba([200, 0, 0, 255]));
    }

    #[test]
    fn grayscale_invert_and_a_round_trip_through_png_and_jpeg() {
        let p = picture(16, 16);
        let g = apply(&p, &Op::Grayscale).unwrap();
        let px = g.get_pixel(0, 0);
        assert_eq!(px.0[0], px.0[1]);
        let inv = apply(&p, &Op::Invert).unwrap();
        assert_eq!(inv.get_pixel(0, 0), Rgba([55, 255, 255, 255]));
        let png = encode(&p, ImageFormat::Png).unwrap();
        assert_eq!(decode(&png).unwrap().dimensions(), (16, 16));
        let jpg = encode(&p, ImageFormat::Jpeg).unwrap();
        assert!(jpg.starts_with(&[0xFF, 0xD8]));
        assert!(decode(b"not a picture").is_err());
    }

    #[test]
    fn operations_read_from_json_and_unknown_fields_are_refused() {
        let op: Op =
            serde_json::from_str(r##"{"op":"rect","x":1,"y":2,"w":3,"h":4,"color":"#112233"}"##)
                .unwrap();
        assert!(matches!(op, Op::Rect { .. }));
        assert!(serde_json::from_str::<Op>(
            r#"{"op":"crop","x":1,"y":2,"w":3,"h":4,"shell":"rm -rf"}"#
        )
        .is_err());
        assert!(serde_json::from_str::<Op>(r#"{"op":"run","cmd":"x"}"#).is_err());
        assert_eq!(Op::Brightness { amount: 20 }.label(), "Brightness +20");
    }
}
