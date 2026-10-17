//! Measures the decider on the labelled messages in `tools/bench/decision/routing_data.json`:
//!   cargo run --release -p helix-embed --example decide -- <model dir> <cls|mean> [prefix] [extra messages to classify ...]
use std::time::Instant;

use helix_embed::{Decider, Embedder, Pooling, Route};

fn main() {
    helix_embed::limit_threads();
    let mut args = std::env::args().skip(1);
    let dir = args.next().expect("model dir");
    let pooling = if args.next().as_deref() == Some("mean") {
        Pooling::Mean
    } else {
        Pooling::Cls
    };
    let prefix = args.next().unwrap_or_default();
    let data: serde_json::Value = serde_json::from_str(
        &std::fs::read_to_string("tools/bench/decision/routing_data.json").expect("data"),
    )
    .expect("json");
    let started = Instant::now();
    let embedder = Embedder::load(std::path::Path::new(&dir), pooling).expect("load");
    println!(
        "loaded in {:.0} ms",
        started.elapsed().as_secs_f64() * 1000.0
    );
    let mut routes = Vec::new();
    let mut vectors = Vec::new();
    for (name, list) in data["train"].as_object().unwrap() {
        let examples: Vec<String> = list
            .as_array()
            .unwrap()
            .iter()
            .map(|v| format!("{prefix}{}", v.as_str().unwrap()))
            .collect();
        let refs: Vec<&str> = examples.iter().map(String::as_str).collect();
        vectors.push(embedder.embed(&refs).expect("embed"));
        routes.push(Route {
            name: name.clone(),
            examples,
        });
    }
    let decider = Decider::new(&routes, &vectors);
    for text in args {
        let v = embedder.embed(&[text.as_str()]).expect("embed").remove(0);
        let c = decider.decide(&v).unwrap();
        println!(
            "{text:?} -> {} margin {:.3} {:?}",
            c.route, c.margin, c.scores
        );
    }
    let (mut right, mut total, mut ms) = (0, 0, 0.0);
    for pair in data["test"].as_array().unwrap() {
        let text = format!("{prefix}{}", pair[0].as_str().unwrap());
        let want = pair[1].as_str().unwrap();
        let t = Instant::now();
        let v = embedder.embed(&[text.as_str()]).expect("embed").remove(0);
        let choice = decider.decide(&v).unwrap();
        ms += t.elapsed().as_secs_f64() * 1000.0;
        total += 1;
        if choice.route == want {
            right += 1;
        } else {
            println!(
                "  miss: {:?} want {want} got {} margin {:.3}",
                &text[..text.len().min(50)],
                choice.route,
                choice.margin
            );
        }
    }
    println!(
        "{right}/{total} right, {:.1} ms per message (one at a time)",
        ms / f64::from(total)
    );
}
