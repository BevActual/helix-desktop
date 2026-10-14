//! Parity with material_color_utilities. Regenerate with `tools/gen_golden_hct.cpp`.

use helix_theme::hct::{Hct, TonalPalette};

const TONES: [f64; 11] = [0.0, 5.0, 10.0, 25.5, 40.0, 50.0, 60.0, 80.0, 90.0, 99.99999, 100.0];
const CHROMAS: [f64; 5] = [0.0, 8.0, 36.0, 80.0, 200.0];
const PALETTE_TONES: [i32; 18] = [0, 5, 10, 15, 20, 25, 30, 35, 40, 50, 60, 70, 80, 90, 95, 98, 99, 100];

fn hex(v: u32) -> String {
    format!("{v:08x}")
}

#[test]
fn matches_material_color_utilities() {
    let golden = include_str!("golden/hct.tsv");
    let mut rows = 0;
    for line in golden.lines() {
        let f: Vec<&str> = line.split('\t').collect();
        let argb = u32::from_str_radix(f[0], 16).unwrap();
        let hct = Hct::from_argb(argb);
        for (got, want) in [(hct.hue(), f[1]), (hct.chroma(), f[2]), (hct.tone(), f[3])] {
            let want: f64 = want.parse().unwrap();
            assert!((got - want).abs() < 1e-6, "{} hct {got} vs {want}", f[0]);
        }
        let mut i = 4;
        for t in TONES {
            let mut h = Hct::from_argb(argb);
            h.set_tone(t);
            assert_eq!(hex(h.to_argb()), f[i], "{} set_tone {t}", f[0]);
            i += 1;
        }
        for c in CHROMAS {
            let mut h = Hct::from_argb(argb);
            h.set_chroma(c);
            assert_eq!(hex(h.to_argb()), f[i], "{} set_chroma {c}", f[0]);
            i += 1;
        }
        let mut h = Hct::from_argb(argb);
        h.set_hue(h.hue() + 90.0);
        assert_eq!(hex(h.to_argb()), f[i], "{} set_hue", f[0]);
        i += 1;
        let palette = TonalPalette::from_argb(argb);
        for t in PALETTE_TONES {
            assert_eq!(hex(palette.get(f64::from(t))), f[i], "{} palette tone {t}", f[0]);
            i += 1;
        }
        assert_eq!(i, f.len());
        rows += 1;
    }
    assert_eq!(rows, 516);
}
