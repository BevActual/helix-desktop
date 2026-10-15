//! Custom (non-M3) schemes: vibrant, faithful, soft, dysfunctional, muted (C++ `custom_schemes.cpp`). These do not use
//! TonalPalette. Pipeline: downsample, k-means in Lab space (deterministic init by sort-by-L), scheme-specific scoring to pick
//! primary/secondary/tertiary seeds, then a token map built in HSL space with the shift/adjust/ensure-contrast helpers.

use std::collections::HashMap;

use crate::color::{adjust_surface, hue_distance, shift_hue, Color};
use crate::contrast::ensure_contrast;
use crate::custom::GeneratedPalette;
use crate::hct::Hct;
use crate::palette::{synthesize_terminal_palette_tokens, TokenMap};
use crate::wallpaper::Scheme;

// ---- Lab (D65) ---------------------------------------------------------------------------------------------------------

const WHITE_X: f64 = 95.047;
const WHITE_Y: f64 = 100.0;
const WHITE_Z: f64 = 108.883;

#[derive(Clone, Copy)]
struct Lab {
    l: f64,
    a: f64,
    b: f64,
}

fn linearize(c: i32) -> f64 {
    let n = f64::from(c) / 255.0;
    if n <= 0.04045 {
        n / 12.92
    } else {
        ((n + 0.055) / 1.055).powf(2.4)
    }
}

fn delinearize(v: f64) -> i32 {
    let n = if v <= 0.0031308 { v * 12.92 } else { 1.055 * v.powf(1.0 / 2.4) - 0.055 };
    ((n * 255.0).round() as i64).clamp(0, 255) as i32
}

fn lab_f(t: f64) -> f64 {
    if t > 0.008856 {
        t.cbrt()
    } else {
        (903.3 * t + 16.0) / 116.0
    }
}

fn lab_f_inv(t: f64) -> f64 {
    if t > 0.206893 {
        t * t * t
    } else {
        (116.0 * t - 16.0) / 903.3
    }
}

fn rgb_to_lab(c: Color) -> Lab {
    let (lr, lg, lb) = (linearize(c.r), linearize(c.g), linearize(c.b));
    let x = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) * 100.0;
    let y = (0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb) * 100.0;
    let z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) * 100.0;
    let (fx, fy, fz) = (lab_f(x / WHITE_X), lab_f(y / WHITE_Y), lab_f(z / WHITE_Z));
    Lab { l: 116.0 * fy - 16.0, a: 500.0 * (fx - fy), b: 200.0 * (fy - fz) }
}

fn lab_to_rgb(lab: Lab) -> Color {
    let fy = (lab.l + 16.0) / 116.0;
    let fx = lab.a / 500.0 + fy;
    let fz = fy - lab.b / 200.0;
    let x = WHITE_X * lab_f_inv(fx) / 100.0;
    let y = WHITE_Y * lab_f_inv(fy) / 100.0;
    let z = WHITE_Z * lab_f_inv(fz) / 100.0;
    let lr = (3.2404542 * x - 1.5371385 * y - 0.4985314 * z).clamp(0.0, 1.0);
    let lg = (-0.9692660 * x + 1.8760108 * y + 0.0415560 * z).clamp(0.0, 1.0);
    let lb = (0.0556434 * x - 0.2040259 * y + 1.0572252 * z).clamp(0.0, 1.0);
    Color::new(delinearize(lr), delinearize(lg), delinearize(lb))
}

fn lab_distance(a: Lab, b: Lab) -> f64 {
    let (dl, da, db) = (a.l - b.l, a.a - b.a, a.b - b.b);
    (dl * dl + da * da + db * db).sqrt()
}

struct HctValues {
    hue: f64,
    chroma: f64,
    tone: f64,
}

fn color_to_hct(c: Color) -> HctValues {
    let h = Hct::from_argb(c.to_argb());
    HctValues { hue: h.hue(), chroma: h.chroma(), tone: h.tone() }
}

// ---- downsample + k-means ------------------------------------------------------------------------------------------------

fn downsample_pixels(rgb: &[u8], factor: usize) -> Vec<Color> {
    let step = factor * factor;
    let npix = rgb.len() / 3;
    (0..npix)
        .step_by(step)
        .map(|i| Color::new(i32::from(rgb[i * 3]), i32::from(rgb[i * 3 + 1]), i32::from(rgb[i * 3 + 2])))
        .collect()
}

struct Cluster {
    centroid: Color,
    representative: Color,
    count: i32,
}

/// Lab-space k-means with deterministic init (indices sorted by L, even stride). Returns clusters sorted by size, descending.
fn kmeans_cluster(colors: &[Color], k: usize, iterations: usize) -> Vec<Cluster> {
    let n = colors.len();
    if n < k {
        // C++ iterated an unordered_map here; libstdc++ walks it newest-first.
        let mut order: Vec<u32> = Vec::new();
        let mut counts: HashMap<u32, i32> = HashMap::new();
        for c in colors {
            let argb = c.to_argb();
            if !counts.contains_key(&argb) {
                order.push(argb);
            }
            *counts.entry(argb).or_insert(0) += 1;
        }
        let mut out = Vec::new();
        for argb in order.into_iter().rev() {
            let c = Color::from_argb(argb);
            out.push(Cluster { centroid: c, representative: c, count: counts[&argb] });
            if out.len() >= k {
                break;
            }
        }
        return out;
    }

    let labs: Vec<Lab> = colors.iter().map(|c| rgb_to_lab(*c)).collect();
    let mut sorted: Vec<usize> = (0..n).collect();
    sorted.sort_by(|a, b| labs[*a].l.partial_cmp(&labs[*b].l).unwrap_or(std::cmp::Ordering::Equal));

    let step = n / k;
    let mut centroids: Vec<Lab> = (0..k).map(|i| labs[sorted[i * step]]).collect();
    let mut assignments = vec![0usize; n];
    for _ in 0..iterations {
        for idx in 0..n {
            let mut min_dist = f64::INFINITY;
            let mut min_cluster = 0;
            for (ci, centroid) in centroids.iter().enumerate() {
                let d = lab_distance(labs[idx], *centroid);
                if d < min_dist {
                    min_dist = d;
                    min_cluster = ci;
                }
            }
            assignments[idx] = min_cluster;
        }
        let mut acc = vec![Lab { l: 0.0, a: 0.0, b: 0.0 }; k];
        let mut cnt = vec![0i32; k];
        for idx in 0..n {
            let ci = assignments[idx];
            acc[ci].l += labs[idx].l;
            acc[ci].a += labs[idx].a;
            acc[ci].b += labs[idx].b;
            cnt[ci] += 1;
        }
        for ci in 0..k {
            if cnt[ci] > 0 {
                let nd = f64::from(cnt[ci]);
                centroids[ci] = Lab { l: acc[ci].l / nd, a: acc[ci].a / nd, b: acc[ci].b / nd };
            }
        }
    }

    let mut cluster_counts = vec![0i32; k];
    let mut reps: Vec<(Color, f64)> = vec![(Color::new(0, 0, 0), f64::INFINITY); k];
    for idx in 0..n {
        let ci = assignments[idx];
        cluster_counts[ci] += 1;
        let d = lab_distance(labs[idx], centroids[ci]);
        if d < reps[ci].1 {
            reps[ci] = (colors[idx], d);
        }
    }
    let mut results: Vec<Cluster> = (0..k)
        .filter(|ci| cluster_counts[*ci] > 0)
        .map(|ci| Cluster {
            centroid: lab_to_rgb(centroids[ci]),
            representative: reps[ci].0,
            count: cluster_counts[ci],
        })
        .collect();
    results.sort_by_key(|r| std::cmp::Reverse(r.count)); // stable
    results
}

// ---- hue families ------------------------------------------------------------------------------------------------------

fn hue_to_family(hue: f64) -> i32 {
    if hue >= 330.0 || hue < 30.0 {
        0
    } else if hue < 60.0 {
        1
    } else if hue < 105.0 {
        2
    } else if hue < 190.0 {
        3
    } else if hue < 270.0 {
        4
    } else {
        5
    }
}

fn family_center_hue(family: i32) -> f64 {
    [0.0, 45.0, 82.5, 147.5, 230.0, 300.0][family as usize]
}

fn circular_hue_diff(h1: f64, h2: f64) -> f64 {
    let diff = (h1 - h2).abs();
    diff.min(360.0 - diff)
}

struct Scored {
    color: Color,
    score: f64,
}

fn sort_scored(out: &mut [Scored]) {
    out.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
}

struct Entry {
    color: Color,
    chroma: f64,
    count: i32,
}

/// Families in the order libstdc++'s `unordered_map<int, ...>` iterates them (newest first), each with its entries in
/// insertion order.
fn group_by_family(input: &[(Color, i32)], min_chroma: f64) -> Vec<(i32, Vec<Entry>)> {
    let mut families: Vec<(i32, Vec<Entry>)> = Vec::new();
    for (color, count) in input {
        let hct = color_to_hct(*color);
        if hct.chroma >= min_chroma {
            let family = hue_to_family(hct.hue);
            let entry = Entry { color: *color, chroma: hct.chroma, count: *count };
            match families.iter_mut().find(|(f, _)| *f == family) {
                Some((_, list)) => list.push(entry),
                None => families.push((family, vec![entry])),
            }
        }
    }
    families.reverse();
    families
}

fn count_only(input: &[(Color, i32)]) -> Vec<Scored> {
    let mut result: Vec<Scored> =
        input.iter().map(|(color, count)| Scored { color: *color, score: f64::from(*count) }).collect();
    sort_scored(&mut result);
    result
}

fn score_chroma(input: &[(Color, i32)]) -> Vec<Scored> {
    let mut out: Vec<Scored> = input
        .iter()
        .map(|(color, count)| {
            let hct = color_to_hct(*color);
            let tone_penalty = if hct.tone < 20.0 {
                (20.0 - hct.tone) * 2.0
            } else if hct.tone > 80.0 {
                (hct.tone - 80.0) * 1.5
            } else if hct.tone < 40.0 {
                (40.0 - hct.tone) * 0.5
            } else if hct.tone > 60.0 {
                (hct.tone - 60.0) * 0.3
            } else {
                0.0
            };
            let hue_penalty = if hct.hue > 80.0 && hct.hue < 110.0 { 5.0 } else { 0.0 };
            let score = (hct.chroma - tone_penalty - hue_penalty) * f64::from(*count).powf(0.3);
            Scored { color: *color, score }
        })
        .collect();
    sort_scored(&mut out);
    out
}

fn score_count(input: &[(Color, i32)]) -> Vec<Scored> {
    let mut families = group_by_family(input, 10.0);
    if families.is_empty() {
        return count_only(input);
    }
    let mut totals: Vec<(usize, i32)> = families
        .iter()
        .enumerate()
        .map(|(i, (_, entries))| (i, entries.iter().map(|e| e.count).sum()))
        .collect();
    totals.sort_by_key(|t| std::cmp::Reverse(t.1)); // stable
    let family_count = totals.len();
    let mut result = Vec::new();
    for (rank, (index, _)) in totals.iter().enumerate() {
        let entries = &mut families[*index].1;
        entries.sort_by(|a, b| {
            if a.count != b.count {
                b.count.cmp(&a.count)
            } else {
                b.chroma.partial_cmp(&a.chroma).unwrap_or(std::cmp::Ordering::Equal)
            }
        });
        for e in entries.iter() {
            let score = (family_count - rank) as f64 * 1_000_000.0 + f64::from(e.count) * 1000.0 + e.chroma;
            result.push(Scored { color: e.color, score });
        }
    }
    sort_scored(&mut result);
    result
}

fn score_dysfunctional(input: &[(Color, i32)]) -> Vec<Scored> {
    const MIN_HUE_DISTANCE: f64 = 45.0;
    const MIN_COUNT_RATIO: f64 = 0.02;
    let mut families = group_by_family(input, 10.0);
    if families.is_empty() {
        return count_only(input);
    }
    let mut totals: Vec<(usize, i32)> = families
        .iter()
        .enumerate()
        .map(|(i, (_, entries))| (i, entries.iter().map(|e| e.count).sum()))
        .collect();
    totals.sort_by_key(|t| std::cmp::Reverse(t.1)); // stable

    let dominant_center = family_center_hue(families[totals[0].0].0);
    let total_colorful: i32 = totals.iter().map(|t| t.1).sum();
    let min_count = f64::from(total_colorful) * MIN_COUNT_RATIO;

    struct Distant {
        index: usize,
        hue_diff: f64,
        max_chroma: f64,
    }
    let mut distant: Vec<Distant> = Vec::new();
    let mut close: Vec<usize> = vec![totals[0].0];
    for (index, count) in totals.iter().skip(1) {
        let center = family_center_hue(families[*index].0);
        let hue_diff = circular_hue_diff(dominant_center, center);
        if hue_diff >= MIN_HUE_DISTANCE && f64::from(*count) >= min_count {
            let max_chroma = families[*index].1.iter().fold(0.0f64, |m, e| m.max(e.chroma));
            distant.push(Distant { index: *index, hue_diff, max_chroma });
        } else {
            close.push(*index);
        }
    }
    distant.sort_by(|a, b| {
        (b.hue_diff * b.max_chroma)
            .partial_cmp(&(a.hue_diff * a.max_chroma))
            .unwrap_or(std::cmp::Ordering::Equal)
    });

    let mut result = Vec::new();
    for (rank, d) in distant.iter().enumerate() {
        let entries = &mut families[d.index].1;
        entries.sort_by(|a, b| {
            if a.chroma != b.chroma {
                b.chroma.partial_cmp(&a.chroma).unwrap_or(std::cmp::Ordering::Equal)
            } else {
                b.count.cmp(&a.count)
            }
        });
        for e in entries.iter() {
            let score = (distant.len() - rank) as f64 * 1_000_000.0 + e.chroma * 1000.0 + f64::from(e.count);
            result.push(Scored { color: e.color, score });
        }
    }
    for index in close {
        let entries = &mut families[index].1;
        entries.sort_by(|a, b| {
            if a.count != b.count {
                b.count.cmp(&a.count)
            } else {
                b.chroma.partial_cmp(&a.chroma).unwrap_or(std::cmp::Ordering::Equal)
            }
        });
        for e in entries.iter() {
            result.push(Scored { color: e.color, score: f64::from(e.count) * 1000.0 + e.chroma });
        }
    }
    sort_scored(&mut result);
    result
}

fn extract_palette(rgb112: &[u8], scheme: Scheme, k: usize) -> Vec<Color> {
    let sampled = downsample_pixels(rgb112, 4);
    let mut cluster_count = 20;
    let mut filtered = sampled.clone();
    match scheme {
        Scheme::Vibrant => {
            cluster_count = 20;
            let kept: Vec<Color> = sampled.iter().copied().filter(|c| color_to_hct(*c).chroma >= 5.0).collect();
            filtered = if kept.len() >= cluster_count * 2 { kept } else { sampled.clone() };
        }
        Scheme::Faithful | Scheme::Soft | Scheme::Dysfunctional => cluster_count = 48,
        Scheme::Muted => cluster_count = 24,
        _ => {}
    }
    let clusters = kmeans_cluster(&filtered, cluster_count, 10);
    // Vibrant is scored from averaged centroids; the other schemes from representative pixels (actual image colours).
    let input: Vec<(Color, i32)> = clusters
        .iter()
        .map(|c| (if scheme == Scheme::Vibrant { c.centroid } else { c.representative }, c.count))
        .collect();
    let scored = match scheme {
        Scheme::Vibrant => score_chroma(&input),
        Scheme::Faithful | Scheme::Soft => score_count(&input),
        Scheme::Dysfunctional => score_dysfunctional(&input),
        _ => count_only(&input),
    };
    let mut colors: Vec<Color> = scored.into_iter().map(|s| s.color).collect();
    // Too few colours: synthesise the rest by rotating the primary in 60 degree steps.
    while colors.len() < k {
        match colors.first().copied() {
            None => colors.push(Color::from_hex("#6750A4").expect("literal")),
            Some(base) => {
                let offset = colors.len() as f64 * 60.0;
                colors.push(shift_hue(base, offset));
            }
        }
    }
    colors.truncate(k);
    colors
}

fn find_error_color(palette: &[Color]) -> Color {
    for c in palette {
        let (h, s, l) = c.to_hsl();
        if (h <= 30.0 || h >= 330.0) && s > 0.4 && l > 0.3 && l < 0.7 {
            return *c;
        }
    }
    Color::from_hex("#FD4663").expect("literal")
}

// ---- token maps --------------------------------------------------------------------------------------------------------

fn hsl(h: f64, s: f64, l: f64) -> Color {
    Color::from_hsl(h, s, l)
}

fn clamp(v: f64, lo: f64, hi: f64) -> f64 {
    v.max(lo).min(hi)
}

/// Every colour a custom scheme decides; [`Roles::into_tokens`] names them.
struct Roles {
    primary: Color,
    on_primary: Color,
    primary_container: Color,
    on_primary_container: Color,
    primary_fixed: Color,
    primary_fixed_dim: Color,
    on_primary_fixed: Color,
    on_primary_fixed_variant: Color,
    secondary: Color,
    on_secondary: Color,
    secondary_container: Color,
    on_secondary_container: Color,
    secondary_fixed: Color,
    secondary_fixed_dim: Color,
    on_secondary_fixed: Color,
    on_secondary_fixed_variant: Color,
    tertiary: Color,
    on_tertiary: Color,
    tertiary_container: Color,
    on_tertiary_container: Color,
    tertiary_fixed: Color,
    tertiary_fixed_dim: Color,
    on_tertiary_fixed: Color,
    on_tertiary_fixed_variant: Color,
    error: Color,
    on_error: Color,
    error_container: Color,
    on_error_container: Color,
    surface: Color,
    on_surface: Color,
    surface_variant: Color,
    on_surface_variant: Color,
    surface_dim: Color,
    surface_bright: Color,
    surface_container_lowest: Color,
    surface_container_low: Color,
    surface_container: Color,
    surface_container_high: Color,
    surface_container_highest: Color,
    outline: Color,
    outline_variant: Color,
    shadow: Color,
    scrim: Color,
    inverse_surface: Color,
    inverse_on_surface: Color,
    inverse_primary: Color,
    background: Color,
    on_background: Color,
}

impl Roles {
    fn into_tokens(self) -> TokenMap {
        let mut m = TokenMap::new();
        let mut set = |key: &str, c: Color| {
            m.insert(key.to_string(), c.to_argb());
        };
        set("primary", self.primary);
        set("on_primary", self.on_primary);
        set("primary_container", self.primary_container);
        set("on_primary_container", self.on_primary_container);
        set("primary_fixed", self.primary_fixed);
        set("primary_fixed_dim", self.primary_fixed_dim);
        set("on_primary_fixed", self.on_primary_fixed);
        set("on_primary_fixed_variant", self.on_primary_fixed_variant);
        set("surface_tint", self.primary);
        set("secondary", self.secondary);
        set("on_secondary", self.on_secondary);
        set("secondary_container", self.secondary_container);
        set("on_secondary_container", self.on_secondary_container);
        set("secondary_fixed", self.secondary_fixed);
        set("secondary_fixed_dim", self.secondary_fixed_dim);
        set("on_secondary_fixed", self.on_secondary_fixed);
        set("on_secondary_fixed_variant", self.on_secondary_fixed_variant);
        set("tertiary", self.tertiary);
        set("on_tertiary", self.on_tertiary);
        set("tertiary_container", self.tertiary_container);
        set("on_tertiary_container", self.on_tertiary_container);
        set("tertiary_fixed", self.tertiary_fixed);
        set("tertiary_fixed_dim", self.tertiary_fixed_dim);
        set("on_tertiary_fixed", self.on_tertiary_fixed);
        set("on_tertiary_fixed_variant", self.on_tertiary_fixed_variant);
        set("error", self.error);
        set("on_error", self.on_error);
        set("error_container", self.error_container);
        set("on_error_container", self.on_error_container);
        set("surface", self.surface);
        set("on_surface", self.on_surface);
        set("surface_variant", self.surface_variant);
        set("on_surface_variant", self.on_surface_variant);
        set("surface_dim", self.surface_dim);
        set("surface_bright", self.surface_bright);
        set("surface_container_lowest", self.surface_container_lowest);
        set("surface_container_low", self.surface_container_low);
        set("surface_container", self.surface_container);
        set("surface_container_high", self.surface_container_high);
        set("surface_container_highest", self.surface_container_highest);
        set("outline", self.outline);
        set("outline_variant", self.outline_variant);
        set("shadow", self.shadow);
        set("scrim", self.scrim);
        set("inverse_surface", self.inverse_surface);
        set("inverse_on_surface", self.inverse_on_surface);
        set("inverse_primary", self.inverse_primary);
        set("background", self.background);
        set("on_background", self.on_background);
        m
    }
}

/// The three accents and the error colour every scheme starts from.
struct Seeds {
    primary: Color,
    secondary: Color,
    tertiary: Color,
    error: Color,
}

/// Normal and soft schemes pick the second and third palette colours when they are far enough in hue from the first.
fn pick_seeds(palette: &[Color], default_primary: Color) -> Seeds {
    const MIN_HUE_DISTANCE: f64 = 30.0;
    let primary = palette.first().copied().unwrap_or(default_primary);
    let (primary_h, _, _) = primary.to_hsl();
    let mut secondary = shift_hue(primary, 30.0);
    if palette.len() > 1 {
        let (sh, _, _) = palette[1].to_hsl();
        if hue_distance(primary_h, sh) > MIN_HUE_DISTANCE {
            secondary = palette[1];
        }
    }
    let mut tertiary = shift_hue(primary, 60.0);
    if palette.len() > 2 {
        let (th, _, _) = palette[2].to_hsl();
        let (sh, _, _) = secondary.to_hsl();
        if hue_distance(primary_h, th) > MIN_HUE_DISTANCE && hue_distance(sh, th) > MIN_HUE_DISTANCE {
            tertiary = palette[2];
        }
    }
    Seeds { primary, secondary, tertiary, error: find_error_color(palette) }
}

fn hs(c: Color) -> (f64, f64) {
    let (h, s, _) = c.to_hsl();
    (h, s)
}

fn normal_dark(palette: &[Color]) -> TokenMap {
    let Seeds { primary, secondary, tertiary, error } = pick_seeds(palette, Color::new(255, 245, 155));
    let (primary_h, primary_s) = hs(primary);
    let (sec_h, sec_s) = hs(secondary);
    let (ter_h, ter_s) = hs(tertiary);
    let (err_h, err_s) = hs(error);
    let p0 = palette[0];
    let (p0_h, p0_s) = hs(p0);

    let (ph, ps, pl) = primary.to_hsl();
    let primary_adjusted = hsl(ph, ps.max(0.7), pl.max(0.65));
    let (sh, ss, sl) = secondary.to_hsl();
    let secondary_adjusted = hsl(sh, ss.max(0.6), sl.max(0.60));
    let (th, ts, tl) = tertiary.to_hsl();
    let tertiary_adjusted = hsl(th, ts.max(0.5), tl.max(0.60));

    // Scale the saturation step by the remaining headroom instead of adding a constant: a flat +0.15 clamps at S = 1.0
    // for already-saturated accents.
    let container = |base: Color| {
        let (h, s, l) = base.to_hsl();
        hsl(h, s + 0.15 * (1.0 - s), (l - 0.35).max(0.15))
    };
    let primary_container = container(primary_adjusted);
    let secondary_container = container(secondary_adjusted);
    let tertiary_container = container(tertiary_adjusted);
    let error_container = container(error);

    // Surface heuristic: shift cyan toward blue, cap saturation by hue band.
    let mut surface_hue = p0_h;
    if (160.0..=200.0).contains(&surface_hue) {
        surface_hue = (surface_hue + 10.0) % 360.0;
    }
    let surface_sat_cap = if surface_hue < 60.0 || surface_hue > 300.0 {
        0.35
    } else if surface_hue < 120.0 {
        0.50
    } else {
        0.90
    };
    let base_surface = hsl(surface_hue, p0_s.min(surface_sat_cap), 0.5);
    let surface = adjust_surface(base_surface, surface_sat_cap, 0.12);
    let surface_variant = adjust_surface(base_surface, 0.80f64.min(surface_sat_cap), 0.16);

    let on_surface = ensure_contrast(hsl(p0_h, 0.05, 0.95), surface, 4.5, 0);
    let on_surface_variant = ensure_contrast(hsl(p0_h, 0.05, 0.70), surface_variant, 4.5, 0);
    let outline = ensure_contrast(adjust_surface(p0, 0.10, 0.30), surface, 3.0, 0);
    let outline_variant = ensure_contrast(adjust_surface(p0, 0.10, 0.40), surface, 3.0, 0);

    let dark_fg = hsl(p0_h, 0.20, 0.12);
    let fixed = |base: Color| {
        let (h, s, _) = base.to_hsl();
        (hsl(h, s.max(0.70), 0.85), hsl(h, s.max(0.65), 0.75))
    };
    let (primary_fixed, primary_fixed_dim) = fixed(primary_adjusted);
    let (secondary_fixed, secondary_fixed_dim) = fixed(secondary_adjusted);
    let (tertiary_fixed, tertiary_fixed_dim) = fixed(tertiary_adjusted);
    let surface_base = surface;

    Roles {
        primary: primary_adjusted,
        on_primary: ensure_contrast(dark_fg, primary_adjusted, 4.5, 0),
        primary_container,
        on_primary_container: ensure_contrast(hsl(primary_h, primary_s, 0.90), primary_container, 4.5, 1),
        primary_fixed,
        primary_fixed_dim,
        on_primary_fixed: ensure_contrast(hsl(primary_h, 0.15, 0.15), primary_fixed, 4.5, 0),
        on_primary_fixed_variant: ensure_contrast(hsl(primary_h, 0.15, 0.20), primary_fixed_dim, 4.5, 0),
        secondary: secondary_adjusted,
        on_secondary: ensure_contrast(dark_fg, secondary_adjusted, 4.5, 0),
        secondary_container,
        on_secondary_container: ensure_contrast(hsl(sec_h, sec_s, 0.90), secondary_container, 4.5, 1),
        secondary_fixed,
        secondary_fixed_dim,
        on_secondary_fixed: ensure_contrast(hsl(sec_h, 0.15, 0.15), secondary_fixed, 4.5, 0),
        on_secondary_fixed_variant: ensure_contrast(hsl(sec_h, 0.15, 0.20), secondary_fixed_dim, 4.5, 0),
        tertiary: tertiary_adjusted,
        on_tertiary: ensure_contrast(dark_fg, tertiary_adjusted, 4.5, 0),
        tertiary_container,
        on_tertiary_container: ensure_contrast(hsl(ter_h, ter_s, 0.90), tertiary_container, 4.5, 1),
        tertiary_fixed,
        tertiary_fixed_dim,
        on_tertiary_fixed: ensure_contrast(hsl(ter_h, 0.15, 0.15), tertiary_fixed, 4.5, 0),
        on_tertiary_fixed_variant: ensure_contrast(hsl(ter_h, 0.15, 0.20), tertiary_fixed_dim, 4.5, 0),
        error,
        on_error: ensure_contrast(dark_fg, error, 4.5, 0),
        error_container,
        on_error_container: ensure_contrast(hsl(err_h, err_s, 0.90), error_container, 4.5, 1),
        surface,
        on_surface,
        surface_variant,
        on_surface_variant,
        surface_dim: adjust_surface(base_surface, 0.85, 0.08),
        surface_bright: adjust_surface(base_surface, 0.75, 0.24),
        surface_container_lowest: adjust_surface(base_surface, 0.85, 0.06),
        surface_container_low: adjust_surface(base_surface, 0.85, 0.10),
        surface_container: adjust_surface(base_surface, 0.70, 0.20),
        surface_container_high: adjust_surface(base_surface, 0.75, 0.18),
        surface_container_highest: adjust_surface(base_surface, 0.70, 0.22),
        outline,
        outline_variant,
        shadow: surface_base,
        scrim: Color::new(0, 0, 0),
        inverse_surface: hsl(p0_h, 0.08, 0.90),
        inverse_on_surface: hsl(p0_h, 0.05, 0.15),
        inverse_primary: hsl(primary_h, (primary_s * 0.8).max(0.5), 0.40),
        background: surface,
        on_background: on_surface,
    }
    .into_tokens()
}

fn normal_light(palette: &[Color]) -> TokenMap {
    let Seeds { primary, secondary, tertiary, error } = pick_seeds(palette, Color::new(93, 101, 245));
    let (primary_h, primary_s) = hs(primary);
    let (sec_h, sec_s) = hs(secondary);
    let (ter_h, ter_s) = hs(tertiary);
    let (err_h, err_s) = hs(error);
    let p0 = palette[0];
    let (text_h, surface_s) = hs(p0);

    let (ph, ps, pl) = primary.to_hsl();
    let primary_adjusted = hsl(ph, ps.max(0.7), clamp(pl, 0.25, 0.45));
    let (sh, ss, sl) = secondary.to_hsl();
    let secondary_adjusted = hsl(sh, ss.max(0.6), clamp(sl, 0.22, 0.40));
    let (th, ts, tl) = tertiary.to_hsl();
    let tertiary_adjusted = hsl(th, ts.max(0.5), clamp(tl, 0.20, 0.35));

    let container = |base: Color| {
        let (h, s, l) = base.to_hsl();
        hsl(h, (s - 0.20).max(0.30), (l + 0.35).min(0.85))
    };
    let primary_container = container(primary_adjusted);
    let secondary_container = container(secondary_adjusted);
    let tertiary_container = container(tertiary_adjusted);
    let error_container = container(error);

    let surface = adjust_surface(p0, 0.90, 0.90);
    let surface_variant = adjust_surface(p0, 0.80, 0.78);
    let on_surface = ensure_contrast(hsl(text_h, 0.05, 0.10), surface, 4.5, 0);
    let on_surface_variant = ensure_contrast(hsl(text_h, 0.05, 0.35), surface_variant, 4.5, 0);

    let light_fg = hsl(text_h, 0.1, 0.98);
    let fixed = |base: Color| {
        let (h, s, _) = base.to_hsl();
        (hsl(h, s.max(0.70), 0.40), hsl(h, s.max(0.65), 0.30))
    };
    let (primary_fixed, primary_fixed_dim) = fixed(primary_adjusted);
    let (secondary_fixed, secondary_fixed_dim) = fixed(secondary_adjusted);
    let (tertiary_fixed, tertiary_fixed_dim) = fixed(tertiary_adjusted);

    Roles {
        primary: primary_adjusted,
        on_primary: ensure_contrast(light_fg, primary_adjusted, 4.5, 0),
        primary_container,
        on_primary_container: ensure_contrast(hsl(primary_h, primary_s, 0.15), primary_container, 4.5, -1),
        primary_fixed,
        primary_fixed_dim,
        on_primary_fixed: ensure_contrast(hsl(primary_h, 0.15, 0.90), primary_fixed, 4.5, 0),
        on_primary_fixed_variant: ensure_contrast(hsl(primary_h, 0.15, 0.85), primary_fixed_dim, 4.5, 0),
        secondary: secondary_adjusted,
        on_secondary: ensure_contrast(light_fg, secondary_adjusted, 4.5, 0),
        secondary_container,
        on_secondary_container: ensure_contrast(hsl(sec_h, sec_s, 0.15), secondary_container, 4.5, -1),
        secondary_fixed,
        secondary_fixed_dim,
        on_secondary_fixed: ensure_contrast(hsl(sec_h, 0.15, 0.90), secondary_fixed, 4.5, 0),
        on_secondary_fixed_variant: ensure_contrast(hsl(sec_h, 0.15, 0.85), secondary_fixed_dim, 4.5, 0),
        tertiary: tertiary_adjusted,
        on_tertiary: ensure_contrast(light_fg, tertiary_adjusted, 4.5, 0),
        tertiary_container,
        on_tertiary_container: ensure_contrast(hsl(ter_h, ter_s, 0.15), tertiary_container, 4.5, -1),
        tertiary_fixed,
        tertiary_fixed_dim,
        on_tertiary_fixed: ensure_contrast(hsl(ter_h, 0.15, 0.90), tertiary_fixed, 4.5, 0),
        on_tertiary_fixed_variant: ensure_contrast(hsl(ter_h, 0.15, 0.85), tertiary_fixed_dim, 4.5, 0),
        error,
        on_error: ensure_contrast(light_fg, error, 4.5, 0),
        error_container,
        on_error_container: ensure_contrast(hsl(err_h, err_s, 0.15), error_container, 4.5, -1),
        surface,
        on_surface,
        surface_variant,
        on_surface_variant,
        surface_dim: adjust_surface(p0, 0.85, 0.82),
        surface_bright: adjust_surface(p0, 0.90, 0.95),
        surface_container_lowest: adjust_surface(p0, 0.85, 0.96),
        surface_container_low: adjust_surface(p0, 0.85, 0.92),
        surface_container: adjust_surface(p0, 0.80, 0.86),
        surface_container_high: adjust_surface(p0, 0.75, 0.84),
        surface_container_highest: adjust_surface(p0, 0.70, 0.80),
        outline: ensure_contrast(hsl(text_h, (surface_s * 0.4).max(0.25), 0.65), surface, 3.0, 0),
        outline_variant: ensure_contrast(hsl(text_h, (surface_s * 0.3).max(0.20), 0.75), surface, 3.0, 0),
        shadow: hsl(text_h, (surface_s * 0.3).max(0.15), 0.80),
        scrim: Color::new(0, 0, 0),
        inverse_surface: hsl(text_h, 0.08, 0.15),
        inverse_on_surface: hsl(text_h, 0.05, 0.90),
        inverse_primary: hsl(primary_h, (primary_s * 0.8).max(0.5), 0.70),
        background: surface,
        on_background: on_surface,
    }
    .into_tokens()
}

fn soft_dark(palette: &[Color]) -> TokenMap {
    let Seeds { primary, secondary, tertiary, error } = pick_seeds(palette, Color::new(255, 245, 155));
    let (primary_h, primary_s) = hs(primary);
    let (sec_h, sec_s) = hs(secondary);
    let (ter_h, ter_s) = hs(tertiary);
    let (err_h, err_s) = hs(error);

    let (ph, ps, pl) = primary.to_hsl();
    let primary_adjusted = hsl(ph, clamp(ps, 0.30, 0.55), pl.max(0.65));
    let (sh, ss, sl) = secondary.to_hsl();
    let secondary_adjusted = hsl(sh, clamp(ss, 0.24, 0.45), sl.max(0.60));
    let (th, ts, tl) = tertiary.to_hsl();
    let tertiary_adjusted = hsl(th, clamp(ts, 0.20, 0.40), tl.max(0.60));

    let container = |base: Color| {
        let (h, s, l) = base.to_hsl();
        hsl(h, (s + 0.08).min(0.60), (l - 0.35).max(0.15))
    };
    let primary_container = container(primary_adjusted);
    let secondary_container = container(secondary_adjusted);
    let tertiary_container = container(tertiary_adjusted);
    let error_container = container(error);

    let base_surface = hsl(primary_h, primary_s.min(0.22), 0.5);
    let surface = adjust_surface(base_surface, 0.16, 0.12);
    let surface_variant = adjust_surface(base_surface, 0.18, 0.16);
    let on_surface = ensure_contrast(hsl(primary_h, 0.04, 0.95), surface, 4.5, 0);
    let on_surface_variant = ensure_contrast(hsl(primary_h, 0.05, 0.70), surface_variant, 4.5, 0);
    let dark_fg = hsl(primary_h, 0.12, 0.12);

    let fixed = |base: Color| {
        let (h, s, _) = base.to_hsl();
        (hsl(h, clamp(s, 0.30, 0.55), 0.85), hsl(h, clamp(s, 0.26, 0.48), 0.75))
    };
    let (primary_fixed, primary_fixed_dim) = fixed(primary_adjusted);
    let (secondary_fixed, secondary_fixed_dim) = fixed(secondary_adjusted);
    let (tertiary_fixed, tertiary_fixed_dim) = fixed(tertiary_adjusted);

    Roles {
        primary: primary_adjusted,
        on_primary: ensure_contrast(dark_fg, primary_adjusted, 4.5, 0),
        primary_container,
        on_primary_container: ensure_contrast(hsl(primary_h, 0.20, 0.90), primary_container, 4.5, 1),
        primary_fixed,
        primary_fixed_dim,
        on_primary_fixed: ensure_contrast(hsl(primary_h, 0.10, 0.15), primary_fixed, 4.5, 0),
        on_primary_fixed_variant: ensure_contrast(hsl(primary_h, 0.10, 0.20), primary_fixed_dim, 4.5, 0),
        secondary: secondary_adjusted,
        on_secondary: ensure_contrast(dark_fg, secondary_adjusted, 4.5, 0),
        secondary_container,
        on_secondary_container: ensure_contrast(hsl(sec_h, sec_s.min(0.20), 0.90), secondary_container, 4.5, 1),
        secondary_fixed,
        secondary_fixed_dim,
        on_secondary_fixed: ensure_contrast(hsl(sec_h, 0.10, 0.15), secondary_fixed, 4.5, 0),
        on_secondary_fixed_variant: ensure_contrast(hsl(sec_h, 0.10, 0.20), secondary_fixed_dim, 4.5, 0),
        tertiary: tertiary_adjusted,
        on_tertiary: ensure_contrast(dark_fg, tertiary_adjusted, 4.5, 0),
        tertiary_container,
        on_tertiary_container: ensure_contrast(hsl(ter_h, ter_s.min(0.20), 0.90), tertiary_container, 4.5, 1),
        tertiary_fixed,
        tertiary_fixed_dim,
        on_tertiary_fixed: ensure_contrast(hsl(ter_h, 0.10, 0.15), tertiary_fixed, 4.5, 0),
        on_tertiary_fixed_variant: ensure_contrast(hsl(ter_h, 0.10, 0.20), tertiary_fixed_dim, 4.5, 0),
        error,
        on_error: ensure_contrast(dark_fg, error, 4.5, 0),
        error_container,
        on_error_container: ensure_contrast(hsl(err_h, err_s.min(0.25), 0.90), error_container, 4.5, 1),
        surface,
        on_surface,
        surface_variant,
        on_surface_variant,
        surface_dim: adjust_surface(base_surface, 0.14, 0.08),
        surface_bright: adjust_surface(base_surface, 0.20, 0.24),
        surface_container_lowest: adjust_surface(base_surface, 0.12, 0.06),
        surface_container_low: adjust_surface(base_surface, 0.14, 0.10),
        surface_container: adjust_surface(base_surface, 0.16, 0.20),
        surface_container_high: adjust_surface(base_surface, 0.18, 0.18),
        surface_container_highest: adjust_surface(base_surface, 0.20, 0.22),
        outline: ensure_contrast(hsl(primary_h, 0.10, 0.30), surface, 3.0, 0),
        outline_variant: ensure_contrast(hsl(primary_h, 0.12, 0.40), surface, 3.0, 0),
        shadow: surface,
        scrim: Color::new(0, 0, 0),
        inverse_surface: hsl(primary_h, 0.05, 0.90),
        inverse_on_surface: hsl(primary_h, 0.04, 0.15),
        inverse_primary: hsl(primary_h, clamp(primary_s * 0.65, 0.25, 0.45), 0.40),
        background: surface,
        on_background: on_surface,
    }
    .into_tokens()
}

fn soft_light(palette: &[Color]) -> TokenMap {
    let Seeds { primary, secondary, tertiary, error } = pick_seeds(palette, Color::new(93, 101, 245));
    let (primary_h, primary_s) = hs(primary);
    let (sec_h, sec_s) = hs(secondary);
    let (ter_h, ter_s) = hs(tertiary);
    let (err_h, err_s) = hs(error);

    let (ph, ps, pl) = primary.to_hsl();
    let primary_adjusted = hsl(ph, clamp(ps, 0.30, 0.55), clamp(pl, 0.25, 0.45));
    let (sh, ss, sl) = secondary.to_hsl();
    let secondary_adjusted = hsl(sh, clamp(ss, 0.24, 0.45), clamp(sl, 0.22, 0.40));
    let (th, ts, tl) = tertiary.to_hsl();
    let tertiary_adjusted = hsl(th, clamp(ts, 0.20, 0.40), clamp(tl, 0.20, 0.35));

    let container = |base: Color| {
        let (h, s, l) = base.to_hsl();
        hsl(h, (s - 0.10).max(0.18), (l + 0.35).min(0.85))
    };
    let primary_container = container(primary_adjusted);
    let secondary_container = container(secondary_adjusted);
    let tertiary_container = container(tertiary_adjusted);
    let error_container = container(error);

    let base_surface = hsl(primary_h, primary_s.min(0.22), 0.5);
    let surface = adjust_surface(base_surface, 0.16, 0.90);
    let surface_variant = adjust_surface(base_surface, 0.18, 0.78);
    let on_surface = ensure_contrast(hsl(primary_h, 0.04, 0.10), surface, 4.5, 0);
    let on_surface_variant = ensure_contrast(hsl(primary_h, 0.05, 0.35), surface_variant, 4.5, 0);
    let light_fg = hsl(primary_h, 0.08, 0.98);

    let fixed = |base: Color| {
        let (h, s, _) = base.to_hsl();
        (hsl(h, clamp(s, 0.30, 0.55), 0.40), hsl(h, clamp(s, 0.26, 0.48), 0.30))
    };
    let (primary_fixed, primary_fixed_dim) = fixed(primary_adjusted);
    let (secondary_fixed, secondary_fixed_dim) = fixed(secondary_adjusted);
    let (tertiary_fixed, tertiary_fixed_dim) = fixed(tertiary_adjusted);

    Roles {
        primary: primary_adjusted,
        on_primary: ensure_contrast(light_fg, primary_adjusted, 4.5, 0),
        primary_container,
        on_primary_container: ensure_contrast(hsl(primary_h, 0.20, 0.15), primary_container, 4.5, -1),
        primary_fixed,
        primary_fixed_dim,
        on_primary_fixed: ensure_contrast(hsl(primary_h, 0.10, 0.90), primary_fixed, 4.5, 0),
        on_primary_fixed_variant: ensure_contrast(hsl(primary_h, 0.10, 0.85), primary_fixed_dim, 4.5, 0),
        secondary: secondary_adjusted,
        on_secondary: ensure_contrast(light_fg, secondary_adjusted, 4.5, 0),
        secondary_container,
        on_secondary_container: ensure_contrast(hsl(sec_h, sec_s.min(0.20), 0.15), secondary_container, 4.5, -1),
        secondary_fixed,
        secondary_fixed_dim,
        on_secondary_fixed: ensure_contrast(hsl(sec_h, 0.10, 0.90), secondary_fixed, 4.5, 0),
        on_secondary_fixed_variant: ensure_contrast(hsl(sec_h, 0.10, 0.85), secondary_fixed_dim, 4.5, 0),
        tertiary: tertiary_adjusted,
        on_tertiary: ensure_contrast(light_fg, tertiary_adjusted, 4.5, 0),
        tertiary_container,
        on_tertiary_container: ensure_contrast(hsl(ter_h, ter_s.min(0.20), 0.15), tertiary_container, 4.5, -1),
        tertiary_fixed,
        tertiary_fixed_dim,
        on_tertiary_fixed: ensure_contrast(hsl(ter_h, 0.10, 0.90), tertiary_fixed, 4.5, 0),
        on_tertiary_fixed_variant: ensure_contrast(hsl(ter_h, 0.10, 0.85), tertiary_fixed_dim, 4.5, 0),
        error,
        on_error: ensure_contrast(light_fg, error, 4.5, 0),
        error_container,
        on_error_container: ensure_contrast(hsl(err_h, err_s.min(0.25), 0.15), error_container, 4.5, -1),
        surface,
        on_surface,
        surface_variant,
        on_surface_variant,
        surface_dim: adjust_surface(base_surface, 0.14, 0.82),
        surface_bright: adjust_surface(base_surface, 0.20, 0.95),
        surface_container_lowest: adjust_surface(base_surface, 0.12, 0.96),
        surface_container_low: adjust_surface(base_surface, 0.14, 0.92),
        surface_container: adjust_surface(base_surface, 0.16, 0.86),
        surface_container_high: adjust_surface(base_surface, 0.18, 0.84),
        surface_container_highest: adjust_surface(base_surface, 0.20, 0.80),
        outline: ensure_contrast(hsl(primary_h, 0.12, 0.65), surface, 3.0, 0),
        outline_variant: ensure_contrast(hsl(primary_h, 0.10, 0.75), surface, 3.0, 0),
        shadow: hsl(primary_h, 0.10, 0.80),
        scrim: Color::new(0, 0, 0),
        inverse_surface: hsl(primary_h, 0.05, 0.15),
        inverse_on_surface: hsl(primary_h, 0.04, 0.90),
        inverse_primary: hsl(primary_h, clamp(primary_s * 0.65, 0.25, 0.45), 0.70),
        background: surface,
        on_background: on_surface,
    }
    .into_tokens()
}

const MUTED_SAT_PRIMARY: f64 = 0.15;
const MUTED_SAT_SECONDARY: f64 = 0.12;
const MUTED_SAT_TERTIARY: f64 = 0.10;
const MUTED_SAT_SURFACE: f64 = 0.08;

fn muted_dark(palette: &[Color]) -> TokenMap {
    let primary = palette.first().copied().unwrap_or(Color::new(128, 128, 128));
    let secondary = shift_hue(primary, 15.0);
    let tertiary = shift_hue(primary, 30.0);
    let error = find_error_color(palette);
    let (primary_h, primary_s) = hs(primary);
    let (sec_h, _) = hs(secondary);
    let (ter_h, _) = hs(tertiary);
    let (err_h, _) = hs(error);

    let (ph, ps, pl) = primary.to_hsl();
    let primary_adjusted = hsl(ph, ps.min(MUTED_SAT_PRIMARY), pl.max(0.65));
    let (sh, ss, sl) = secondary.to_hsl();
    let secondary_adjusted = hsl(sh, ss.min(MUTED_SAT_SECONDARY), sl.max(0.60));
    let (th, ts, tl) = tertiary.to_hsl();
    let tertiary_adjusted = hsl(th, ts.min(MUTED_SAT_TERTIARY), tl.max(0.60));

    let container = |base: Color| {
        let (h, s, l) = base.to_hsl();
        hsl(h, (s + 0.05).min(MUTED_SAT_PRIMARY), (l - 0.35).max(0.15))
    };
    let primary_container = container(primary_adjusted);
    let secondary_container = container(secondary_adjusted);
    let tertiary_container = container(tertiary_adjusted);
    let error_container = container(error);

    let base_surface = hsl(primary_h, MUTED_SAT_SURFACE, 0.5);
    let surface = adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.12);
    let surface_variant = adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.16);
    let on_surface = ensure_contrast(hsl(primary_h, 0.03, 0.95), surface, 4.5, 0);
    let on_surface_variant = ensure_contrast(hsl(primary_h, 0.03, 0.70), surface_variant, 4.5, 0);
    let dark_fg = hsl(primary_h, 0.10, 0.12);

    let fixed = |base: Color| {
        let (h, s, _) = base.to_hsl();
        (hsl(h, s.min(MUTED_SAT_PRIMARY), 0.85), hsl(h, s.min(MUTED_SAT_PRIMARY), 0.75))
    };
    let (primary_fixed, primary_fixed_dim) = fixed(primary_adjusted);
    let (secondary_fixed, secondary_fixed_dim) = fixed(secondary_adjusted);
    let (tertiary_fixed, tertiary_fixed_dim) = fixed(tertiary_adjusted);

    Roles {
        primary: primary_adjusted,
        on_primary: ensure_contrast(dark_fg, primary_adjusted, 4.5, 0),
        primary_container,
        on_primary_container: ensure_contrast(hsl(primary_h, 0.05, 0.90), primary_container, 4.5, 1),
        primary_fixed,
        primary_fixed_dim,
        on_primary_fixed: ensure_contrast(hsl(primary_h, 0.05, 0.15), primary_fixed, 4.5, 0),
        on_primary_fixed_variant: ensure_contrast(hsl(primary_h, 0.05, 0.20), primary_fixed_dim, 4.5, 0),
        secondary: secondary_adjusted,
        on_secondary: ensure_contrast(dark_fg, secondary_adjusted, 4.5, 0),
        secondary_container,
        on_secondary_container: ensure_contrast(hsl(sec_h, 0.05, 0.90), secondary_container, 4.5, 1),
        secondary_fixed,
        secondary_fixed_dim,
        on_secondary_fixed: ensure_contrast(hsl(sec_h, 0.05, 0.15), secondary_fixed, 4.5, 0),
        on_secondary_fixed_variant: ensure_contrast(hsl(sec_h, 0.05, 0.20), secondary_fixed_dim, 4.5, 0),
        tertiary: tertiary_adjusted,
        on_tertiary: ensure_contrast(dark_fg, tertiary_adjusted, 4.5, 0),
        tertiary_container,
        on_tertiary_container: ensure_contrast(hsl(ter_h, 0.05, 0.90), tertiary_container, 4.5, 1),
        tertiary_fixed,
        tertiary_fixed_dim,
        on_tertiary_fixed: ensure_contrast(hsl(ter_h, 0.05, 0.15), tertiary_fixed, 4.5, 0),
        on_tertiary_fixed_variant: ensure_contrast(hsl(ter_h, 0.05, 0.20), tertiary_fixed_dim, 4.5, 0),
        error,
        on_error: ensure_contrast(dark_fg, error, 4.5, 0),
        error_container,
        on_error_container: ensure_contrast(hsl(err_h, 0.05, 0.90), error_container, 4.5, 1),
        surface,
        on_surface,
        surface_variant,
        on_surface_variant,
        surface_dim: adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.08),
        surface_bright: adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.24),
        surface_container_lowest: adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.06),
        surface_container_low: adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.10),
        surface_container: adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.20),
        surface_container_high: adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.18),
        surface_container_highest: adjust_surface(base_surface, MUTED_SAT_SURFACE, 0.22),
        outline: ensure_contrast(hsl(primary_h, 0.05, 0.30), surface, 3.0, 0),
        outline_variant: ensure_contrast(hsl(primary_h, 0.05, 0.40), surface, 3.0, 0),
        shadow: surface,
        scrim: Color::new(0, 0, 0),
        inverse_surface: hsl(primary_h, 0.05, 0.90),
        inverse_on_surface: hsl(primary_h, 0.03, 0.15),
        inverse_primary: hsl(primary_h, (primary_s * 0.5).min(MUTED_SAT_PRIMARY), 0.40),
        background: surface,
        on_background: on_surface,
    }
    .into_tokens()
}

fn muted_light(palette: &[Color]) -> TokenMap {
    let primary = palette.first().copied().unwrap_or(Color::new(128, 128, 128));
    let secondary = shift_hue(primary, 15.0);
    let tertiary = shift_hue(primary, 30.0);
    let error = find_error_color(palette);
    let (primary_h, primary_s) = hs(primary);
    let (sec_h, _) = hs(secondary);
    let (ter_h, _) = hs(tertiary);
    let (err_h, _) = hs(error);

    let (ph, ps, pl) = primary.to_hsl();
    let primary_adjusted = hsl(ph, ps.min(MUTED_SAT_PRIMARY), pl.min(0.45));
    let (sh, ss, sl) = secondary.to_hsl();
    let secondary_adjusted = hsl(sh, ss.min(MUTED_SAT_SECONDARY), sl.min(0.40));
    let (th, ts, tl) = tertiary.to_hsl();
    let tertiary_adjusted = hsl(th, ts.min(MUTED_SAT_TERTIARY), tl.min(0.35));

    let container = |base: Color| {
        let (h, s, l) = base.to_hsl();
        hsl(h, (s - 0.05).max(0.05), (l + 0.35).min(0.85))
    };
    let primary_container = container(primary_adjusted);
    let secondary_container = container(secondary_adjusted);
    let tertiary_container = container(tertiary_adjusted);
    let error_container = container(error);

    let surface = adjust_surface(primary, MUTED_SAT_SURFACE, 0.90);
    let surface_variant = adjust_surface(primary, MUTED_SAT_SURFACE, 0.78);
    let on_surface = ensure_contrast(hsl(primary_h, 0.03, 0.10), surface, 4.5, 0);
    let on_surface_variant = ensure_contrast(hsl(primary_h, 0.03, 0.35), surface_variant, 4.5, 0);
    let light_fg = hsl(primary_h, 0.05, 0.98);

    let fixed = |base: Color| {
        let (h, s, _) = base.to_hsl();
        (hsl(h, s.min(MUTED_SAT_PRIMARY), 0.40), hsl(h, s.min(MUTED_SAT_PRIMARY), 0.30))
    };
    let (primary_fixed, primary_fixed_dim) = fixed(primary_adjusted);
    let (secondary_fixed, secondary_fixed_dim) = fixed(secondary_adjusted);
    let (tertiary_fixed, tertiary_fixed_dim) = fixed(tertiary_adjusted);

    Roles {
        primary: primary_adjusted,
        on_primary: ensure_contrast(light_fg, primary_adjusted, 4.5, 0),
        primary_container,
        on_primary_container: ensure_contrast(hsl(primary_h, 0.05, 0.15), primary_container, 4.5, -1),
        primary_fixed,
        primary_fixed_dim,
        on_primary_fixed: ensure_contrast(hsl(primary_h, 0.05, 0.90), primary_fixed, 4.5, 0),
        on_primary_fixed_variant: ensure_contrast(hsl(primary_h, 0.05, 0.85), primary_fixed_dim, 4.5, 0),
        secondary: secondary_adjusted,
        on_secondary: ensure_contrast(light_fg, secondary_adjusted, 4.5, 0),
        secondary_container,
        on_secondary_container: ensure_contrast(hsl(sec_h, 0.05, 0.15), secondary_container, 4.5, -1),
        secondary_fixed,
        secondary_fixed_dim,
        on_secondary_fixed: ensure_contrast(hsl(sec_h, 0.05, 0.90), secondary_fixed, 4.5, 0),
        on_secondary_fixed_variant: ensure_contrast(hsl(sec_h, 0.05, 0.85), secondary_fixed_dim, 4.5, 0),
        tertiary: tertiary_adjusted,
        on_tertiary: ensure_contrast(light_fg, tertiary_adjusted, 4.5, 0),
        tertiary_container,
        on_tertiary_container: ensure_contrast(hsl(ter_h, 0.05, 0.15), tertiary_container, 4.5, -1),
        tertiary_fixed,
        tertiary_fixed_dim,
        on_tertiary_fixed: ensure_contrast(hsl(ter_h, 0.05, 0.90), tertiary_fixed, 4.5, 0),
        on_tertiary_fixed_variant: ensure_contrast(hsl(ter_h, 0.05, 0.85), tertiary_fixed_dim, 4.5, 0),
        error,
        on_error: ensure_contrast(light_fg, error, 4.5, 0),
        error_container,
        on_error_container: ensure_contrast(hsl(err_h, 0.05, 0.15), error_container, 4.5, -1),
        surface,
        on_surface,
        surface_variant,
        on_surface_variant,
        surface_dim: adjust_surface(primary, MUTED_SAT_SURFACE, 0.82),
        surface_bright: adjust_surface(primary, MUTED_SAT_SURFACE, 0.95),
        surface_container_lowest: adjust_surface(primary, MUTED_SAT_SURFACE, 0.96),
        surface_container_low: adjust_surface(primary, MUTED_SAT_SURFACE, 0.92),
        surface_container: adjust_surface(primary, MUTED_SAT_SURFACE, 0.86),
        surface_container_high: adjust_surface(primary, MUTED_SAT_SURFACE, 0.84),
        surface_container_highest: adjust_surface(primary, MUTED_SAT_SURFACE, 0.80),
        outline: ensure_contrast(hsl(primary_h, 0.05, 0.65), surface, 3.0, 0),
        outline_variant: ensure_contrast(hsl(primary_h, 0.05, 0.75), surface, 3.0, 0),
        shadow: hsl(primary_h, 0.05, 0.80),
        scrim: Color::new(0, 0, 0),
        inverse_surface: hsl(primary_h, 0.05, 0.15),
        inverse_on_surface: hsl(primary_h, 0.03, 0.90),
        inverse_primary: hsl(primary_h, (primary_s * 0.5).min(MUTED_SAT_PRIMARY), 0.70),
        background: surface,
        on_background: on_surface,
    }
    .into_tokens()
}

pub fn generate_custom(rgb112: &[u8], scheme: Scheme) -> GeneratedPalette {
    let palette = extract_palette(rgb112, scheme, 5);
    let (mut dark, mut light) = match scheme {
        Scheme::Soft => (soft_dark(&palette), soft_light(&palette)),
        Scheme::Muted => (muted_dark(&palette), muted_light(&palette)),
        _ => (normal_dark(&palette), normal_light(&palette)),
    };
    if let Some(first) = palette.first() {
        dark.insert("source_color".into(), first.to_argb());
        light.insert("source_color".into(), first.to_argb());
    }
    synthesize_terminal_palette_tokens(&mut dark);
    synthesize_terminal_palette_tokens(&mut light);
    GeneratedPalette { dark, light }
}
