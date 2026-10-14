//! Parity with the C++ reference. Regenerate with `tools/gen_golden.cpp`.

use helix_theme::contrast::ensure_contrast;
use helix_theme::Color;

const BACKGROUNDS: [Color; 5] = [
    Color::new(0, 0, 0),
    Color::new(255, 255, 255),
    Color::new(0x20, 0x20, 0x28),
    Color::new(0xe8, 0xe0, 0xd0),
    Color::new(0x80, 0x80, 0x80),
];

#[test]
fn matches_cpp_reference() {
    let golden = include_str!("golden/color_contrast.tsv");
    let mut rows = 0;
    for line in golden.lines().skip(1) {
        let f: Vec<&str> = line.split('\t').collect();
        assert_eq!(f.len(), 15, "{line}");
        let c = Color::from_hex(f[0]).unwrap();

        let (h, s, l) = c.to_hsl();
        for (got, want) in [(h, f[1]), (s, f[2]), (l, f[3])] {
            let want: f64 = want.parse().unwrap();
            assert!((got - want).abs() < 1e-9, "{} hsl {got} vs {want}", f[0]);
        }
        assert_eq!(Color::from_hsl(h, s, l).to_hex(), f[4], "{} from_hsl", f[0]);

        for (i, ratio) in [4.5, 3.0].into_iter().enumerate() {
            for (j, bg) in BACKGROUNDS.iter().enumerate() {
                let got = ensure_contrast(c, *bg, ratio, 0).to_hex();
                assert_eq!(got, f[5 + i * 5 + j], "{} ratio {ratio} bg {j}", f[0]);
            }
        }
        rows += 1;
    }
    assert_eq!(rows, 616);
}
