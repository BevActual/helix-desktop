//! Parity with `day_night_schedule::evaluate` (fake clock). Regenerate via `tools/regen_golden.sh`.

use helix_theme::schedule::{evaluate, LocalTime, LocationConfig};

fn place(name: &str) -> (f64, f64, i64) {
    match name {
        "london" => (51.5, -0.12, 0),
        "sydney" => (-33.9, 151.2, 39600),
        "quito" => (-0.2, -78.5, -18000),
        "tromso" => (69.6, 18.9, 3600),
        "mcmurdo" => (-77.8, 166.7, 43200),
        other => panic!("unknown place {other}"),
    }
}

#[test]
fn matches_cpp_schedule() {
    let golden = include_str!("golden/schedule.tsv");
    let mut rows = 0;
    for line in golden.lines() {
        let f: Vec<&str> = line.split('\t').collect();
        let (config, now, night, until, since);
        match f[0] {
            "coords" => {
                let (lat, lon, offset) = place(f[1]);
                config = LocationConfig {
                    latitude: Some(lat),
                    longitude: Some(lon),
                    ..Default::default()
                };
                let minute: i32 = f[3].parse().unwrap();
                now = LocalTime {
                    minute_of_day: minute,
                    second: f[4].parse().unwrap(),
                    day_of_year: f[2].parse().unwrap(),
                    gmt_offset_secs: offset,
                };
                night = f[5];
                until = f[6];
                since = f[7];
            }
            "manual" => {
                config = LocationConfig {
                    custom_schedule: f[3] == "1",
                    sunset: f[1].into(),
                    sunrise: f[2].into(),
                    ..Default::default()
                };
                now = LocalTime {
                    minute_of_day: f[4].parse().unwrap(),
                    second: 11,
                    day_of_year: 1,
                    gmt_offset_secs: 0,
                };
                night = f[5];
                until = f[6];
                since = f[7];
            }
            other => panic!("unknown row kind {other}"),
        }
        let e = evaluate(&config, now);
        let label = line.replace('\t', " ");
        assert_eq!(i32::from(e.night).to_string(), night, "{label} night");
        assert_eq!(e.until_boundary_ms.to_string(), until, "{label} until");
        assert_eq!(e.since_boundary_ms.to_string(), since, "{label} since");
        rows += 1;
    }
    assert_eq!(rows, 505);
}
