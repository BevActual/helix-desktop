//! The picture: a blurred, dimmed wallpaper, the clock, and a card with the account's avatar and the password field, in the same grammar as the
//! login screen. Drawn with Cairo and Pango into any cairo context, so a test can draw into a plain image and look at it.

use std::path::Path;

use cairo::{Context, Extend, Filter, Format, ImageSurface};
use helix_wallpaper_model::{cover_crop, Source};

/// What the screen says right now.
pub struct Frame<'a> {
    pub time: &'a str,
    pub date: &'a str,
    pub name: &'a str,
    pub initials: &'a str,
    pub dots: usize,
    pub message: &'a str,
    /// The message is a complaint (drawn in red).
    pub complaint: bool,
    /// Sideways wobble of the field after a wrong password, in logical pixels.
    pub shake: f64,
    pub accent: [u8; 3],
    pub checking: bool,
    /// The battery, on a computer that has one.
    pub battery: Option<crate::facts::Battery>,
}

const FONT: &str = "Inter";

fn rgb(c: &Context, [r, g, b]: [u8; 3], alpha: f64) {
    c.set_source_rgba(
        f64::from(r) / 255.0,
        f64::from(g) / 255.0,
        f64::from(b) / 255.0,
        alpha,
    );
}

/// Draws `text` centred on `x`, its top at `y`, and returns its height.
fn centred(c: &Context, text: &str, x: f64, y: f64, size: f64, weight: &str) -> f64 {
    let layout = pangocairo::functions::create_layout(c);
    let mut description = pango::FontDescription::from_string(&format!("{FONT} {weight}"));
    description.set_absolute_size(size * f64::from(pango::SCALE));
    layout.set_font_description(Some(&description));
    layout.set_text(text);
    let (width, height) = layout.pixel_size();
    c.move_to(x - f64::from(width) / 2.0, y);
    pangocairo::functions::show_layout(c, &layout);
    f64::from(height)
}

fn rounded(c: &Context, x: f64, y: f64, w: f64, h: f64, r: f64) {
    let r = r.min(w / 2.0).min(h / 2.0);
    c.new_sub_path();
    c.arc(x + w - r, y + r, r, -std::f64::consts::FRAC_PI_2, 0.0);
    c.arc(x + w - r, y + h - r, r, 0.0, std::f64::consts::FRAC_PI_2);
    c.arc(
        x + r,
        y + h - r,
        r,
        std::f64::consts::FRAC_PI_2,
        std::f64::consts::PI,
    );
    c.arc(
        x + r,
        y + r,
        r,
        std::f64::consts::PI,
        3.0 * std::f64::consts::FRAC_PI_2,
    );
    c.close_path();
}

/// Everything on the screen except the background, in logical pixels on a `w` x `h` screen.
pub fn draw(c: &Context, w: f64, h: f64, frame: &Frame) {
    let cx = w / 2.0;
    // The clock sits in the upper part, the card in the middle: the same order as the login screen.
    c.set_source_rgba(1.0, 1.0, 1.0, 0.96);
    let time_h = centred(
        c,
        frame.time,
        cx,
        h * 0.15,
        (h * 0.13).clamp(56.0, 120.0),
        "Light",
    );
    c.set_source_rgba(1.0, 1.0, 1.0, 0.62);
    centred(c, frame.date, cx, h * 0.15 + time_h + 4.0, 20.0, "Regular");

    let card_w = 340.0;
    let card_top = (h * 0.40).max(h * 0.15 + time_h + 70.0);
    // The avatar.
    let r = 38.0;
    let ay = card_top + r;
    rgb(c, frame.accent, 1.0);
    c.arc(cx, ay, r, 0.0, std::f64::consts::TAU);
    let _ = c.fill();
    c.set_source_rgba(1.0, 1.0, 1.0, 1.0);
    let initials_size = 28.0;
    centred(
        c,
        frame.initials,
        cx,
        ay - initials_size * 0.62,
        initials_size,
        "Bold",
    );
    // The name.
    c.set_source_rgba(1.0, 1.0, 1.0, 0.96);
    let name_h = centred(c, frame.name, cx, ay + r + 14.0, 21.0, "Bold");
    // The field.
    let fx = cx - card_w / 2.0 + frame.shake;
    let fy = ay + r + 14.0 + name_h + 18.0;
    let (fw, fh) = (card_w, 46.0);
    rounded(c, fx, fy, fw, fh, fh / 2.0);
    c.set_source_rgba(1.0, 1.0, 1.0, 0.12);
    let _ = c.fill_preserve();
    if frame.complaint {
        c.set_source_rgba(1.0, 0.27, 0.23, 0.9);
    } else {
        rgb(c, frame.accent, 0.85);
    }
    c.set_line_width(1.5);
    let _ = c.stroke();
    if frame.dots == 0 && !frame.checking {
        c.set_source_rgba(1.0, 1.0, 1.0, 0.45);
        centred(
            c,
            "Password",
            cx + frame.shake,
            fy + fh / 2.0 - 11.0,
            16.0,
            "Regular",
        );
    } else {
        let gap = 18.0;
        let shown = frame.dots.min(((fw - 48.0) / gap) as usize);
        let total = shown as f64 * gap;
        c.set_source_rgba(1.0, 1.0, 1.0, if frame.checking { 0.5 } else { 0.95 });
        for i in 0..shown {
            c.arc(
                cx + frame.shake - total / 2.0 + gap * (i as f64 + 0.5),
                fy + fh / 2.0,
                5.0,
                0.0,
                std::f64::consts::TAU,
            );
            let _ = c.fill();
        }
    }
    // The line under it.
    if !frame.message.is_empty() {
        if frame.complaint {
            c.set_source_rgba(1.0, 0.45, 0.4, 1.0);
        } else {
            c.set_source_rgba(1.0, 1.0, 1.0, 0.7);
        }
        centred(c, frame.message, cx, fy + fh + 16.0, 14.0, "Regular");
    }
    if let Some(battery) = frame.battery {
        // A small battery in the bottom right corner, with the number.
        let text = if battery.charging {
            format!("{}% · Charging", battery.percent)
        } else {
            format!("{}%", battery.percent)
        };
        let low = battery.percent <= 15 && !battery.charging;
        let layout = pangocairo::functions::create_layout(c);
        let mut description = pango::FontDescription::from_string(&format!("{FONT} Regular"));
        description.set_absolute_size(13.0 * f64::from(pango::SCALE));
        layout.set_font_description(Some(&description));
        layout.set_text(&text);
        let (text_w, _) = layout.pixel_size();
        // Flush right: the number ends 28 px from the edge and the gauge sits before it.
        let (bw, bh) = (22.0, 11.0);
        let bx = w - 28.0 - f64::from(text_w) - 10.0 - bw - 3.5;
        let by = h - 46.0;
        c.set_line_width(1.2);
        c.set_source_rgba(1.0, 1.0, 1.0, 0.55);
        rounded(c, bx, by, bw, bh, 2.5);
        let _ = c.stroke();
        c.rectangle(bx + bw + 1.5, by + 3.5, 2.0, bh - 7.0);
        let _ = c.fill();
        if low {
            c.set_source_rgba(1.0, 0.27, 0.23, 0.95);
        } else if battery.charging {
            c.set_source_rgba(0.19, 0.82, 0.35, 0.95);
        } else {
            c.set_source_rgba(1.0, 1.0, 1.0, 0.85);
        }
        let fill = (bw - 3.0) * f64::from(battery.percent) / 100.0;
        rounded(c, bx + 1.5, by + 1.5, fill.max(1.5), bh - 3.0, 1.5);
        let _ = c.fill();
        c.set_source_rgba(1.0, 1.0, 1.0, 0.6);
        c.move_to(bx + bw + 10.0, by - 2.5);
        pangocairo::functions::show_layout(c, &layout);
    }
    // A quiet note at the bottom, so the screen says what it is.
    c.set_source_rgba(1.0, 1.0, 1.0, 0.38);
    centred(c, "Locked", cx, h - 44.0, 13.0, "Regular");
}

/// Decodes a wallpaper small enough to blur cheaply: at most `max` pixels on a side.
pub fn load_picture(path: &Path, max: u32) -> Option<ImageSurface> {
    let image = image::open(path).ok()?.thumbnail(max, max).to_rgba8();
    let (iw, ih) = image.dimensions();
    let mut surface = ImageSurface::create(Format::ARgb32, iw as i32, ih as i32).ok()?;
    let stride = surface.stride() as usize;
    {
        let mut data = surface.data().ok()?;
        for (y, row) in image.rows().enumerate() {
            for (x, pixel) in row.enumerate() {
                let [r, g, b, a] = pixel.0;
                let premultiply = |v: u8| ((u16::from(v) * u16::from(a) + 127) / 255) as u8;
                let at = y * stride + x * 4;
                // Cairo's ARgb32 is native-endian: bytes are B, G, R, A on a little-endian machine.
                data[at] = premultiply(b);
                data[at + 1] = premultiply(g);
                data[at + 2] = premultiply(r);
                data[at + 3] = a;
            }
        }
    }
    Some(surface)
}

/// The background for a `w` x `h` screen: the picture (or the colour) filling it, blurred by drawing it small and scaling it up, then dimmed.
pub fn background(
    source: &Source,
    picture: Option<&ImageSurface>,
    w: i32,
    h: i32,
) -> Option<ImageSurface> {
    let out = ImageSurface::create(Format::ARgb32, w, h).ok()?;
    let c = Context::new(&out).ok()?;
    match (source, picture) {
        (Source::Image(_), Some(image)) => {
            let (iw, ih) = (image.width() as u32, image.height() as u32);
            let (cx, cy, cw, ch) = cover_crop(iw, ih, w as u32, h as u32);
            // The blur: the cropped picture onto something about a twentieth the size, and that scaled back up with smooth filtering.
            let (sw, sh) = ((w / 24).max(8), (h / 24).max(8));
            let small = ImageSurface::create(Format::ARgb32, sw, sh).ok()?;
            {
                let s = Context::new(&small).ok()?;
                s.scale(f64::from(sw) / f64::from(cw), f64::from(sh) / f64::from(ch));
                s.set_source_surface(image, -f64::from(cx), -f64::from(cy))
                    .ok()?;
                s.source().set_filter(Filter::Good);
                s.source().set_extend(Extend::Pad);
                s.paint().ok()?;
            }
            // Back up in two steps, each smoothed, so the blur has no visible blocks.
            let (mw, mh) = ((w / 6).max(sw), (h / 6).max(sh));
            let middle = ImageSurface::create(Format::ARgb32, mw, mh).ok()?;
            {
                let m = Context::new(&middle).ok()?;
                m.scale(f64::from(mw) / f64::from(sw), f64::from(mh) / f64::from(sh));
                m.set_source_surface(&small, 0.0, 0.0).ok()?;
                m.source().set_filter(Filter::Bilinear);
                m.source().set_extend(Extend::Pad);
                m.paint().ok()?;
            }
            c.scale(f64::from(w) / f64::from(mw), f64::from(h) / f64::from(mh));
            c.set_source_surface(&middle, 0.0, 0.0).ok()?;
            c.source().set_filter(Filter::Bilinear);
            c.source().set_extend(Extend::Pad);
            c.paint().ok()?;
            c.identity_matrix();
            c.set_source_rgba(0.0, 0.0, 0.0, 0.42);
            c.paint().ok()?;
        }
        (Source::Color([r, g, b]), _) => {
            rgb(&c, [*r, *g, *b], 1.0);
            c.paint().ok()?;
            c.set_source_rgba(0.0, 0.0, 0.0, 0.25);
            c.paint().ok()?;
        }
        _ => {
            rgb(&c, [0x0e, 0x10, 0x28], 1.0);
            c.paint().ok()?;
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(dots: usize, complaint: bool) -> Frame<'static> {
        Frame {
            time: "14:05",
            date: "Wednesday, October 21",
            name: "Ada Lovelace",
            initials: "AL",
            dots,
            message: if complaint {
                "That is not right. Try again."
            } else {
                ""
            },
            complaint,
            shake: 0.0,
            accent: [0x0a, 0x84, 0xff],
            checking: false,
            battery: Some(crate::facts::Battery {
                percent: 64,
                charging: false,
            }),
        }
    }

    fn render(frame: &Frame) -> ImageSurface {
        let out = background(&Source::Color([0x1c, 0x1c, 0x22]), None, 800, 600).unwrap();
        let c = Context::new(&out).unwrap();
        draw(&c, 800.0, 600.0, frame);
        drop(c);
        out.flush();
        out
    }

    fn lit(surface: &mut ImageSurface, x0: usize, y0: usize, x1: usize, y1: usize) -> usize {
        let stride = surface.stride() as usize;
        let data = surface.data().unwrap();
        let mut count = 0;
        for y in y0..y1 {
            for x in x0..x1 {
                let at = y * stride + x * 4;
                if data[at] > 120 || data[at + 1] > 120 || data[at + 2] > 120 {
                    count += 1;
                }
            }
        }
        count
    }

    #[test]
    fn the_clock_and_the_dots_appear_where_they_belong() {
        let mut empty = render(&frame(0, false));
        let mut typed = render(&frame(6, false));
        // The clock is drawn in the upper part, never in the lower.
        assert!(
            lit(&mut empty, 200, 60, 600, 200) > 400,
            "the clock is on the screen"
        );
        assert_eq!(
            lit(&mut empty, 0, 560, 100, 600),
            0,
            "nothing in the corner"
        );
        // Dots in the field make it brighter than the "Password" hint alone does not: compare the field's middle band.
        let band = |s: &mut ImageSurface| lit(s, 330, 350, 470, 420);
        assert_ne!(
            band(&mut empty),
            band(&mut typed),
            "typing changes the field"
        );
    }

    #[test]
    fn a_complaint_is_red() {
        let mut bad = render(&frame(0, true));
        let stride = bad.stride() as usize;
        let data = bad.data().unwrap();
        let reddish = (0..stride * 600)
            .step_by(4)
            .filter(|&i| data[i + 2] > 200 && data[i + 1] < 140 && data[i] < 140)
            .count();
        assert!(reddish > 20, "the border or the message is red ({reddish})");
    }
}
