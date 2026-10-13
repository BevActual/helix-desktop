use std::cmp::{max, min};
use std::collections::BTreeSet;
use std::sync::Arc;

use smithay::utils::{Logical, Physical, Point, Rectangle, Scale};
use smithay::wayland::compositor::{RectangleKind, RegionAttributes};

/// Helper for fractionally transforming an i32 region while preserving adjacent rects.
///
/// Naively applying floating point transforms may cause adjacent rects to go misaligned due to
/// rounding differences. This struct helps apply the transforms in such a way as to preserve
/// alignment.
#[derive(Debug, Clone)]
pub struct TransformedRegion {
    /// Non-overlapping rects (usually in surface-local coordinates).
    pub rects: Arc<Vec<Rectangle<i32, Logical>>>,
    /// Scale to apply to each rect.
    pub scale: Scale<f64>,
    /// Translation to apply to each rect after scaling.
    pub offset: Point<f64, Logical>,
}

impl TransformedRegion {
    /// Returns an iterator over the top-left and bottom-right corners of transformed rects.
    pub fn iter(&self) -> impl Iterator<Item = (Point<f64, Logical>, Point<f64, Logical>)> + '_ {
        self.rects.iter().map(|r| {
            // Here we start in a happy i32 world where everything lines up, and rectangle loc +
            // size is exactly equal to the adjacent rectangle's loc.
            //
            // Unfortunately, we're about to descend to the floating point hell. And we *really*
            // want adjacent rects to remain adjacent no matter what. So we'll convert our rects to
            // their extremities (rather than loc and size), and operate on those. Coordinates from
            // adjacent rects will undergo exactly the same floating point operations, so when
            // they're ultimately rounded to physical pixels, they will remain adjacent.
            let r = r.to_f64();

            let mut a = r.loc;
            // f64 is enough to represent this i32 addition exactly.
            let mut b = r.loc + r.size.to_point();

            a = a.upscale(self.scale);
            b = b.upscale(self.scale);

            a += self.offset;
            b += self.offset;

            (a, b)
        })
    }

    /// Intersects damage with this subregion.
    pub fn filter_damage(
        &self,
        // Same coordinate space as self.iter().
        crop: Rectangle<f64, Logical>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        filtered: &mut Vec<Rectangle<i32, Physical>>,
    ) {
        let scale = dst.size.to_f64() / crop.size;

        let cs = crop.size.to_point();

        for (mut a, mut b) in self.iter() {
            // Convert to dst-relative.
            a -= crop.loc;
            b -= crop.loc;

            // Intersect with crop.
            let ia = Point::new(f64::max(a.x, 0.), f64::max(a.y, 0.));
            let ib = Point::new(f64::min(b.x, cs.x), f64::min(b.y, cs.y));
            if ib.x <= ia.x || ib.y <= ia.y {
                // No intersection.
                continue;
            }

            // Round extremities to physical pixels, ensuring that adjacent rectangles stay adjacent
            // at fractional scales.
            let ia = ia.to_physical_precise_round(scale);
            let ib = ib.to_physical_precise_round(scale);

            let r = Rectangle::from_extremities(ia, ib);

            // Intersect with each damage rect.
            for d in damage {
                if let Some(intersection) = r.intersection(*d) {
                    filtered.push(intersection);
                }
            }
        }
    }
}

pub fn region_to_non_overlapping_rects(
    region: &RegionAttributes,
    output: &mut Vec<Rectangle<i32, Logical>>,
) {
    let _span = tracy_client::span!("region_to_non_overlapping_rects");

    output.clear();

    // Collect all unique Y coordinates.
    let ys = BTreeSet::from_iter(
        region
            .rects
            .iter()
            .flat_map(|(_, r)| [r.loc.y, r.loc.y.saturating_add(r.size.h)]),
    );

    let mut ys = ys.into_iter();
    let Some(mut lo) = ys.next() else {
        // The region was empty.
        return;
    };

    // Sorted list of non-overlapping [start, end) tuples.
    let mut spans = Vec::<(i32, i32)>::new();

    // Iterate over Y bands.
    for hi in ys {
        spans.clear();

        'region: for (kind, r) in &region.rects {
            // Skip rects that don't overlap with the Y band.
            if hi <= r.loc.y || r.loc.y.saturating_add(r.size.h) <= lo {
                continue;
            }

            let mut x1 = r.loc.x;
            let mut x2 = r.loc.x.saturating_add(r.size.w);
            if x1 == x2 {
                // Empty rect.
                continue;
            }

            match *kind {
                RectangleKind::Add => {
                    // Iterate over existing spans backwards.
                    for i in (0..spans.len()).rev() {
                        let (start, end) = spans[i];

                        // New span is to the right.
                        if end < x1 {
                            spans.insert(i + 1, (x1, x2));
                            continue 'region;
                        }

                        // New span is to the left.
                        if x2 < start {
                            continue;
                        }

                        // New span overlaps this span; merge them.
                        spans.remove(i);
                        x1 = min(x1, start);
                        x2 = max(x2, end);
                    }

                    spans.insert(0, (x1, x2));
                }
                RectangleKind::Subtract => {
                    // Iterate over existing spans backwards.
                    for i in (0..spans.len()).rev() {
                        let (start, end) = spans[i];

                        // Subtract span is to the right.
                        if end <= x1 {
                            continue 'region;
                        }

                        // Subtract span is to the left.
                        if x2 <= start {
                            continue;
                        }

                        // Subtract span overlaps this span.
                        spans.remove(i);
                        if x2 < end {
                            spans.insert(i, (x2, end));
                        }
                        if start < x1 {
                            spans.insert(i, (start, x1));
                        }
                    }
                }
            }
        }

        for (x1, x2) in spans.drain(..) {
            output.push(Rectangle::from_extremities((x1, lo), (x2, hi)));
        }

        lo = hi;
    }
}

#[cfg(test)]
mod tests {
    use std::fmt::Write as _;

    use insta::assert_snapshot;
    use proptest::prelude::*;
    use smithay::utils::{Logical, Point, Rectangle, Size};
    use smithay::wayland::compositor::{RectangleKind, RegionAttributes};

    use super::region_to_non_overlapping_rects;

    #[allow(clippy::type_complexity)]
    fn check(rects: &[(RectangleKind, (i32, i32, i32, i32))]) -> String {
        let region = RegionAttributes {
            rects: rects
                .iter()
                .map(|(kind, (x1, y1, x2, y2))| {
                    (*kind, Rectangle::from_extremities((*x1, *y1), (*x2, *y2)))
                })
                .collect(),
        };
        let mut output = Vec::new();
        region_to_non_overlapping_rects(&region, &mut output);
        let mut s = String::new();
        for r in &output {
            let x1 = r.loc.x;
            let y1 = r.loc.y;
            let x2 = x1 + r.size.w;
            let y2 = y1 + r.size.h;
            writeln!(s, "{x1:2} {y1:2} - {x2:2} {y2:2}").unwrap();
        }
        s
    }

    #[test]
    fn test_region_to_non_overlapping_rects() {
        use RectangleKind::*;

        // empty_region
        assert_snapshot!(check(&[]), @"");

        // single_rectangle
        assert_snapshot!(check(&[(Add, (0, 0, 10, 10))]), @" 0  0 - 10 10");

        // empty_rectangle
        assert_snapshot!(check(&[(Add, (0, 0, 0, 1))]), @"");
        assert_snapshot!(check(&[(Add, (0, 0, 1, 0))]), @"");

        // two_non_overlapping
        assert_snapshot!(
            check(&[(Add, (0, 0, 5, 10)), (Add, (7, 0, 12, 10))]),
            @"
        0  0 -  5 10
        7  0 - 12 10
        "
        );

        // two_overlapping
        assert_snapshot!(
            check(&[(Add, (0, 0, 10, 10)), (Add, (5, 5, 15, 15))]),
            @"
        0  0 - 10  5
        0  5 - 15 10
        5 10 - 15 15
        "
        );

        // subtraction
        assert_snapshot!(
            check(&[(Add, (0, 0, 20, 20)), (Subtract, (5, 5, 15, 15))]),
            @"
         0  0 - 20  5
         0  5 -  5 15
        15  5 - 20 15
         0 15 - 20 20
        "
        );

        // adjacent_rectangles
        assert_snapshot!(
            check(&[(Add, (0, 0, 10, 10)), (Add, (10, 0, 20, 10))]),
            @" 0  0 - 20 10"
        );
    }

    // A client can send a region like wl_region.add(0, i32::MAX, 1, 1), and smithay stores the
    // coordinates as they came in. Computing the rect extremities must not overflow here, since
    // that panics with overflow-checks on.
    #[test]
    fn test_extremity_overflow() {
        let region = RegionAttributes {
            rects: vec![
                (
                    RectangleKind::Add,
                    Rectangle::new(Point::new(0, i32::MAX), Size::new(1, 1)),
                ),
                (
                    RectangleKind::Add,
                    Rectangle::new(Point::new(i32::MAX, 0), Size::new(1, 1)),
                ),
            ],
        };

        // Both rects are degenerate after clamping, so nothing comes out.
        let mut output = Vec::new();
        region_to_non_overlapping_rects(&region, &mut output);
        assert!(output.is_empty());
    }

    proptest! {
        #[test]
        fn non_overlapping_output(
            rects in proptest::collection::vec(
                (
                    prop_oneof![Just(RectangleKind::Add), Just(RectangleKind::Subtract)],
                    (0..20i32, 0..20i32, 0..20i32, 0..20i32),
                ),
                1..10,
            )
        ) {
            let region = RegionAttributes {
                rects: rects
                    .into_iter()
                    .map(|(kind, (x, y, w, h))| {
                        (kind, Rectangle::new(Point::new(x, y), Size::new(w, h)))
                    })
                    .collect(),
            };

            let mut output: Vec<Rectangle<i32, Logical>> = Vec::new();
            region_to_non_overlapping_rects(&region, &mut output);

            for i in 0..output.len() {
                prop_assert!(!output[i].is_empty());

                // Verify no pair of output rectangles overlaps.
                for j in (i + 1)..output.len() {
                    prop_assert!(
                        !output[i].overlaps(output[j]),
                        "rectangles overlap: {:?} and {:?}",
                        output[i],
                        output[j],
                    );
                }
            }
        }
    }
}

/// A glass shape rebuilt from blur-region strips: its bounding box and corner radii, in the
/// same coordinate space as [`TransformedRegion::iter`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GlassShape {
    pub rect: Rectangle<f64, Logical>,
    /// `[top_left, top_right, bottom_right, bottom_left]`.
    pub radii: [f64; 4],
}

impl TransformedRegion {
    /// Rebuilds rounded shapes from a strip tessellation.
    ///
    /// Helix Shell describes each glass shape to the compositor as horizontal strips: a top
    /// band of `ceil(r)` rows whose x-extent narrows toward the top, one full-width body, and
    /// a bottom band. Strips that touch belong to one shape; the shape is the union's bounding
    /// box and its top/bottom radii are the heights of the narrow bands. A square side (an
    /// attached panel meeting the bar) yields a band of zero rows and so a radius of zero.
    pub fn glass_shapes(&self) -> Vec<GlassShape> {
        let rects: Vec<Rectangle<i32, Logical>> = self.rects.iter().copied().collect();
        let n = rects.len();
        if n == 0 {
            return Vec::new();
        }

        // Union-find over touching rects.
        let mut parent: Vec<usize> = (0..n).collect();
        fn find(parent: &mut [usize], mut i: usize) -> usize {
            while parent[i] != i {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            i
        }
        let touches = |a: &Rectangle<i32, Logical>, b: &Rectangle<i32, Logical>| {
            a.loc.x <= b.loc.x + b.size.w
                && b.loc.x <= a.loc.x + a.size.w
                && a.loc.y <= b.loc.y + b.size.h
                && b.loc.y <= a.loc.y + a.size.h
        };
        for i in 0..n {
            for j in (i + 1)..n {
                if touches(&rects[i], &rects[j]) {
                    let (ri, rj) = (find(&mut parent, i), find(&mut parent, j));
                    if ri != rj {
                        parent[ri] = rj;
                    }
                }
            }
        }

        let mut groups: Vec<(usize, Vec<Rectangle<i32, Logical>>)> = Vec::new();
        for i in 0..n {
            let root = find(&mut parent, i);
            match groups.iter_mut().find(|(r, _)| *r == root) {
                Some((_, v)) => v.push(rects[i]),
                None => groups.push((root, vec![rects[i]])),
            }
        }

        groups
            .into_iter()
            .map(|(_, mut strips)| {
                let x0 = strips.iter().map(|r| r.loc.x).min().unwrap();
                let y0 = strips.iter().map(|r| r.loc.y).min().unwrap();
                let x1 = strips.iter().map(|r| r.loc.x + r.size.w).max().unwrap();
                let y1 = strips.iter().map(|r| r.loc.y + r.size.h).max().unwrap();
                let full_w = x1 - x0;

                // Rows from the top until a strip spans the full width.
                strips.sort_by_key(|r| r.loc.y);
                let mut top = 0;
                for r in &strips {
                    if r.size.w >= full_w {
                        break;
                    }
                    top = (r.loc.y + r.size.h - y0).max(top);
                }
                let mut bottom = 0;
                for r in strips.iter().rev() {
                    if r.size.w >= full_w {
                        break;
                    }
                    bottom = (y1 - r.loc.y).max(bottom);
                }
                let max_r = (full_w.min(y1 - y0)) as f64 / 2.0;
                let top = (top as f64).min(max_r);
                let bottom = (bottom as f64).min(max_r);

                let a = Point::<f64, Logical>::new(x0 as f64, y0 as f64).upscale(self.scale)
                    + self.offset;
                let b = Point::<f64, Logical>::new(x1 as f64, y1 as f64).upscale(self.scale)
                    + self.offset;
                let s = self.scale.x.min(self.scale.y);
                GlassShape {
                    rect: Rectangle::new(a, (b - a).to_size()),
                    radii: [top * s, top * s, bottom * s, bottom * s],
                }
            })
            .collect()
    }
}
