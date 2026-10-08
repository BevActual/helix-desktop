//! Which parts of the shell surface the compositor should blur behind. The page reports the rectangles of the things that are drawn as
//! glass (the bar, the open panel, toasts); this turns them into the plain rectangles a blur region is made of, with the rounded corners
//! cut in so the blur does not poke out past them. Pure geometry: whatever the page sends is bounded and clamped to the surface here.

use serde::Deserialize;

/// More glass elements than this are never on screen at once (bar, one panel, a few toasts).
pub const MAX_ELEMENTS: usize = 16;
pub const MAX_RADIUS: f64 = 64.0;

/// One glass element as the page measured it (CSS pixels, which are surface pixels here).
#[derive(Clone, Copy, Debug, Deserialize, PartialEq)]
pub struct Glass {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub radius: f64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

/// Rows per strip inside a rounded corner: the cut is smooth to the eye at two pixels and keeps the rectangle count low.
const STRIP: i32 = 2;

/// The blur region for `elements` on a surface of `width` x `height`. Elements that are not finite, are empty, or lie wholly outside the
/// surface are dropped; the rest are clamped to the surface and their corners rounded.
pub fn blur_rects(elements: &[Glass], width: i32, height: i32) -> Vec<Rect> {
    let mut out = Vec::new();
    for glass in elements.iter().take(MAX_ELEMENTS) {
        if ![glass.x, glass.y, glass.w, glass.h, glass.radius].iter().all(|v| v.is_finite()) {
            continue;
        }
        let left = glass.x.round().max(0.0) as i32;
        let top = glass.y.round().max(0.0) as i32;
        let right = ((glass.x + glass.w).round() as i32).min(width);
        let bottom = ((glass.y + glass.h).round() as i32).min(height);
        let (w, h) = (right - left, bottom - top);
        if w < 1 || h < 1 {
            continue;
        }
        let radius = glass.radius.clamp(0.0, MAX_RADIUS).min(f64::from(w.min(h)) / 2.0).floor() as i32;
        if radius == 0 {
            out.push(Rect { x: left, y: top, w, h });
            continue;
        }
        // Corner rows, top and bottom mirrored: the inset of a row is how far the circle of the corner leaves the edge there.
        let mut row = 0;
        while row < radius {
            let rows = STRIP.min(radius - row);
            let centre = f64::from(radius) - f64::from(row) - f64::from(rows) / 2.0;
            let inset = (f64::from(radius) - (f64::from(radius).powi(2) - centre.powi(2)).max(0.0).sqrt()).ceil() as i32;
            let strip_w = w - 2 * inset;
            if strip_w > 0 {
                out.push(Rect { x: left + inset, y: top + row, w: strip_w, h: rows });
                out.push(Rect { x: left + inset, y: bottom - row - rows, w: strip_w, h: rows });
            }
            row += rows;
        }
        if h - 2 * radius > 0 {
            out.push(Rect { x: left, y: top + radius, w, h: h - 2 * radius });
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn glass(x: f64, y: f64, w: f64, h: f64, radius: f64) -> Glass {
        Glass { x, y, w, h, radius }
    }

    fn area(rects: &[Rect]) -> i64 {
        rects.iter().map(|r| i64::from(r.w) * i64::from(r.h)).sum()
    }

    fn covers(rects: &[Rect], x: i32, y: i32) -> bool {
        rects.iter().any(|r| x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
    }

    #[test]
    fn a_square_cornered_element_is_one_rectangle() {
        assert_eq!(blur_rects(&[glass(10.0, 20.0, 100.0, 50.0, 0.0)], 1000, 800), vec![Rect { x: 10, y: 20, w: 100, h: 50 }]);
    }

    #[test]
    fn rounded_corners_are_cut_out_but_the_body_is_kept() {
        let rects = blur_rects(&[glass(0.0, 0.0, 200.0, 100.0, 24.0)], 1000, 800);
        assert!(!covers(&rects, 0, 0), "the corner pixel is outside the rounded shape");
        assert!(!covers(&rects, 199, 99));
        assert!(covers(&rects, 100, 50) && covers(&rects, 100, 0) && covers(&rects, 0, 50), "edges and centre are inside");
        let full = 200 * 100;
        let lost = full - area(&rects);
        let expected = (24.0_f64 * 24.0 * (4.0 - std::f64::consts::PI)) as i64; // the four corners
        assert!((lost - expected).abs() < 120, "lost {lost}, a circle cut should lose about {expected}");
    }

    #[test]
    fn no_rectangle_overlaps_another() {
        let rects = blur_rects(&[glass(5.0, 5.0, 90.0, 60.0, 20.0)], 500, 500);
        for (i, a) in rects.iter().enumerate() {
            for b in &rects[i + 1..] {
                let apart = a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
                assert!(apart, "{a:?} overlaps {b:?}");
            }
        }
    }

    #[test]
    fn everything_stays_inside_the_surface() {
        let rects = blur_rects(&[glass(-50.0, -50.0, 300.0, 300.0, 24.0), glass(900.0, 700.0, 500.0, 500.0, 24.0)], 1000, 800);
        assert!(!rects.is_empty());
        for r in &rects {
            assert!(r.x >= 0 && r.y >= 0 && r.x + r.w <= 1000 && r.y + r.h <= 800, "{r:?}");
        }
    }

    #[test]
    fn nonsense_is_dropped_not_guessed() {
        let bad = [
            glass(f64::NAN, 0.0, 10.0, 10.0, 0.0),
            glass(0.0, 0.0, f64::INFINITY, 10.0, 0.0),
            glass(0.0, 0.0, 0.0, 10.0, 0.0),
            glass(0.0, 0.0, 10.0, -5.0, 0.0),
            glass(5000.0, 5000.0, 10.0, 10.0, 0.0),
            glass(0.0, 0.0, 10.0, 10.0, f64::NAN),
        ];
        assert!(blur_rects(&bad, 100, 100).is_empty());
    }

    #[test]
    fn a_radius_larger_than_the_element_becomes_a_pill() {
        let rects = blur_rects(&[glass(0.0, 0.0, 100.0, 20.0, 500.0)], 1000, 800);
        assert!(!covers(&rects, 0, 0));
        assert!(covers(&rects, 50, 10) && covers(&rects, 50, 0));
        assert!(rects.iter().all(|r| r.w <= 100 && r.y >= 0 && r.y + r.h <= 20), "{rects:?}");
    }

    #[test]
    fn only_a_bounded_number_of_elements_are_used() {
        let many: Vec<Glass> = (0..100).map(|i| glass(f64::from(i) * 5.0, 0.0, 4.0, 4.0, 0.0)).collect();
        assert_eq!(blur_rects(&many, 1000, 800).len(), MAX_ELEMENTS);
    }

    #[test]
    fn the_rectangle_count_stays_small_for_a_big_radius() {
        let rects = blur_rects(&[glass(0.0, 0.0, 400.0, 400.0, 300.0)], 1000, 800);
        assert!(rects.len() <= 70, "{}", rects.len());
    }

    #[test]
    fn an_odd_radius_does_not_spill_past_the_corner_or_overlap() {
        let rects = blur_rects(&[glass(0.0, 0.0, 60.0, 40.0, 7.0)], 1000, 800);
        for (i, a) in rects.iter().enumerate() {
            assert!(a.y >= 0 && a.y + a.h <= 40 && a.x >= 0 && a.x + a.w <= 60, "{a:?}");
            for b in &rects[i + 1..] {
                assert!(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y, "{a:?} overlaps {b:?}");
            }
        }
    }

    #[test]
    fn tiny_elements_never_produce_an_empty_rectangle() {
        for (w, h, r) in [(2.0, 2.0, 1.0), (3.0, 3.0, 5.0), (1.0, 1.0, 1.0), (2.0, 10.0, 1.0)] {
            for rect in blur_rects(&[glass(0.0, 0.0, w, h, r)], 100, 100) {
                assert!(rect.w > 0 && rect.h > 0, "{rect:?} from {w}x{h} r{r}");
            }
        }
    }

    #[test]
    fn corners_are_symmetric_top_and_bottom() {
        let rects = blur_rects(&[glass(0.0, 0.0, 120.0, 80.0, 20.0)], 1000, 800);
        for dy in 0..20 {
            let top = (0..120).filter(|&x| covers(&rects, x, dy)).count();
            let bottom = (0..120).filter(|&x| covers(&rects, x, 79 - dy)).count();
            assert_eq!(top, bottom, "row {dy}");
        }
    }
}
