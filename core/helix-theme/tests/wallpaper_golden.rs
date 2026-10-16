//! Wallpaper palettes against the C++ generator (`tests/golden/wallpaper.json`: six synthetic 112x112 images x ten schemes).

use helix_theme::wallpaper::{generate, Scheme};
use serde_json::Value;

fn image(kind: u32) -> Vec<u8> {
    // Mirrors the C++ golden generator's deterministic pictures, except noise (kind 2), which used std::mt19937: this is the
    // same generator, reproduced below.
    let mut rng = Mt19937::new(1234 + kind);
    let mut px = vec![0u8; 112 * 112 * 3];
    for y in 0..112u32 {
        for x in 0..112u32 {
            let (r, g, b): (u32, u32, u32) = match kind {
                0 => (30, 90, 200),
                1 => (x * 255 / 111, y * 255 / 111, 128),
                2 => (rng.next() % 256, rng.next() % 256, rng.next() % 256),
                3 => {
                    let a = (x / 14 + y / 14) % 2 == 1;
                    if a {
                        (220, 60, 40)
                    } else {
                        (40, 160, 90)
                    }
                }
                4 => {
                    let v = (x + y) * 255 / 222;
                    (v, v, v)
                }
                _ => {
                    let (mut r, mut g, mut b);
                    if y < 50 {
                        r = 90 + y;
                        g = 140 + y / 2;
                        b = 220;
                    } else if y < 80 {
                        r = 40 + (x % 30);
                        g = 110 + (x % 17);
                        b = 50;
                    } else {
                        r = 120 + (x % 40);
                        g = 90 + (y % 20);
                        b = 60;
                    }
                    let dx = x as i32 - 80;
                    let dy = y as i32 - 30;
                    if dx * dx + dy * dy < 120 {
                        r = 250;
                        g = 230;
                        b = 90;
                    }
                    (r, g, b)
                }
            };
            let at = ((y * 112 + x) * 3) as usize;
            px[at] = r as u8;
            px[at + 1] = g as u8;
            px[at + 2] = b as u8;
        }
    }
    px
}

/// `std::mt19937` (32-bit Mersenne Twister), so the noise picture is the C++ one.
struct Mt19937 {
    state: [u32; 624],
    index: usize,
}

impl Mt19937 {
    fn new(seed: u32) -> Self {
        let mut state = [0u32; 624];
        state[0] = seed;
        for i in 1..624 {
            state[i] = 1812433253u32
                .wrapping_mul(state[i - 1] ^ (state[i - 1] >> 30))
                .wrapping_add(i as u32);
        }
        Self { state, index: 624 }
    }

    fn next(&mut self) -> u32 {
        if self.index >= 624 {
            for i in 0..624 {
                let y = (self.state[i] & 0x8000_0000) | (self.state[(i + 1) % 624] & 0x7fff_ffff);
                let mut next = self.state[(i + 397) % 624] ^ (y >> 1);
                if y & 1 != 0 {
                    next ^= 0x9908_b0df;
                }
                self.state[i] = next;
            }
            self.index = 0;
        }
        let mut y = self.state[self.index];
        self.index += 1;
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c_5680;
        y ^= (y << 15) & 0xefc6_0000;
        y ^= y >> 18;
        y
    }
}

fn hex(map: &std::collections::HashMap<String, u32>, key: &str) -> Value {
    match map.get(key) {
        Some(v) => Value::String(format!("#{:06x}", v & 0x00ff_ffff)),
        None => Value::Null,
    }
}

fn check(only_material: bool) {
    let golden: Vec<Value> = serde_json::from_str(include_str!("golden/wallpaper.json")).unwrap();
    let mut compared = 0;
    let mut mismatches = 0;
    for entry in &golden {
        let scheme = Scheme::parse(entry["scheme"].as_str().unwrap()).unwrap();
        if only_material != scheme.is_material() {
            continue;
        }
        let kind = entry["image"].as_u64().unwrap() as u32;
        let palette = generate(&image(kind), scheme).unwrap();
        for (mode, tokens) in [("dark", &palette.dark), ("light", &palette.light)] {
            for (key, want) in entry["palette"][mode].as_object().unwrap() {
                if &hex(tokens, key) != want {
                    eprintln!(
                        "MISMATCH image {kind} {} {mode} {key}: got {} want {want}",
                        entry["scheme"],
                        hex(tokens, key)
                    );
                    mismatches += 1;
                }
            }
        }
        compared += 1;
    }
    assert!(compared > 0);
    assert_eq!(mismatches, 0, "{mismatches} tokens differ");
}

#[test]
fn material_schemes_match_cpp() {
    check(true);
}

#[test]
fn custom_schemes_match_cpp() {
    check(false);
}
