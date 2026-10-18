//! Does retrieval find the right page? Indexes a folder of Markdown and scores a question set three ways: words only, meaning only,
//! and both fused.
//!
//!   cargo run --release -p helix-knowledge --example measure -- <model dir> [docs dir] [questions.json] [query prefix]
//!
//! A question counts as found at k when one of its expected files is among the first k pages returned.
use std::path::{Path, PathBuf};
use std::time::Instant;

use helix_embed::{Embedder, Pooling};
use helix_knowledge::{Embed, NewPage, SearchOptions, Store};

struct Bge {
    inner: Embedder,
    query_prefix: String,
}

impl Embed for Bge {
    fn model_id(&self) -> &str {
        "bge-small-en-v1.5"
    }
    fn embed(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, String> {
        self.inner.embed(texts)
    }
    fn embed_query(&self, query: &str) -> Result<Vec<f32>, String> {
        let text = format!("{}{query}", self.query_prefix);
        self.inner
            .embed(&[text.as_str()])?
            .into_iter()
            .next()
            .ok_or_else(|| "no vector".to_string())
    }
}

fn files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    let mut entries: Vec<_> = entries.flatten().collect();
    entries.sort_by_key(|e| e.path());
    for entry in entries {
        let path = entry.path();
        if path.is_dir() {
            if path.file_name().is_some_and(|n| n == "archive") {
                continue;
            }
            files(&path, out);
        } else if path.extension().is_some_and(|e| e == "md" || e == "mdx") {
            out.push(path);
        }
    }
}

fn main() {
    helix_embed::limit_threads();
    let mut args = std::env::args().skip(1);
    let model = args.next().expect("model dir");
    let docs = PathBuf::from(args.next().unwrap_or_else(|| "docs".into()));
    let questions_path = args
        .next()
        .unwrap_or_else(|| "tools/bench/knowledge/questions.json".into());
    let query_prefix = args.next().unwrap_or_default();

    let started = Instant::now();
    let embedder = Bge {
        inner: Embedder::load(Path::new(&model), Pooling::Cls).expect("load model"),
        query_prefix,
    };
    println!(
        "model loaded in {:.0} ms",
        started.elapsed().as_secs_f64() * 1e3
    );

    // HELIX_KNOWLEDGE_DB keeps the index between runs, so a second run embeds only what changed
    let store = match std::env::var_os("HELIX_KNOWLEDGE_DB") {
        Some(path) => Store::open(Path::new(&path)).expect("store"),
        None => Store::in_memory().expect("store"),
    };
    let mut paths = Vec::new();
    files(&docs, &mut paths);
    let started = Instant::now();
    let mut embedded = 0;
    for path in &paths {
        let body = std::fs::read_to_string(path).unwrap_or_default();
        let rel = path
            .strip_prefix(&docs)
            .unwrap()
            .to_string_lossy()
            .to_string();
        let title = body
            .lines()
            .find_map(|l| l.strip_prefix("# "))
            .unwrap_or(&rel)
            .trim()
            .to_string();
        let put = store
            .put_page(&NewPage::note(&rel, &title, &body), Some(&embedder))
            .expect("put");
        embedded += put.embedded;
    }
    let stats = store.stats().unwrap();
    println!(
        "indexed {} pages, {} passages ({embedded} embedded) in {:.1} s",
        stats.pages,
        stats.chunks,
        started.elapsed().as_secs_f64()
    );

    let data: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(&questions_path).expect("questions"))
            .expect("json");
    let questions = data["questions"].as_array().expect("questions array");
    let options = SearchOptions {
        limit: 5,
        ..Default::default()
    };
    // (name, uses words, uses meaning)
    let modes = [
        ("words", true, false),
        ("meaning", false, true),
        ("both", true, true),
    ];
    let mut found = [[0usize; 3]; 3]; // mode, then k = 1, 3, 5
    let mut slowest_ms = 0.0f64;
    let mut misses: Vec<String> = Vec::new();
    for question in questions {
        let text = question["q"].as_str().unwrap();
        let expect: Vec<&str> = question["expect"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|v| v.as_str())
            .collect();
        let began = Instant::now();
        let vector = embedder.embed_query(text).expect("query vector");
        for (index, (name, words, meaning)) in modes.iter().enumerate() {
            let hits = store
                .search(
                    if *words { text } else { "" },
                    meaning.then_some(("bge-small-en-v1.5", vector.as_slice())),
                    &options,
                )
                .unwrap();
            let position = hits.iter().position(|h| expect.contains(&h.key.as_str()));
            for (slot, k) in [1usize, 3, 5].iter().enumerate() {
                if position.is_some_and(|p| p < *k) {
                    found[index][slot] += 1;
                }
            }
            if *name == "both" && position.is_none_or(|p| p >= 3) {
                let top: Vec<&str> = hits.iter().take(3).map(|h| h.key.as_str()).collect();
                misses.push(format!("  {text}\n    wanted {expect:?}, got {top:?}"));
            }
        }
        slowest_ms = slowest_ms.max(began.elapsed().as_secs_f64() * 1e3);
    }
    let total = questions.len();
    println!("\n{total} questions, pages found at k = 1 / 3 / 5");
    for (index, (name, _, _)) in modes.iter().enumerate() {
        println!(
            "  {name:8} {:>2} / {:>2} / {:>2}",
            found[index][0], found[index][1], found[index][2]
        );
    }
    println!("slowest question (embed + three searches): {slowest_ms:.0} ms");
    if !misses.is_empty() {
        println!("\n'both' missed the top 3:");
        for miss in misses {
            println!("{miss}");
        }
    }

    // How similar is the best passage to a question that its pages answer, and to one that they cannot? A cut-off between the two
    // decides when a chat may be given passages at all.
    let prefix = "Represent this sentence for searching relevant passages: ";
    let _ = prefix;
    let meaning_only = SearchOptions {
        limit: 1,
        ..Default::default()
    };
    let top_similarity = |text: &str| -> f32 {
        let vector = embedder.embed_query(text).expect("vector");
        store
            .search(
                "",
                Some(("bge-small-en-v1.5", vector.as_slice())),
                &meaning_only,
            )
            .unwrap()
            .first()
            .and_then(|h| h.similarity)
            .unwrap_or(0.0)
    };
    println!("\nbest similarity: questions the docs answer");
    let mut on: Vec<f32> = questions
        .iter()
        .map(|q| top_similarity(q["q"].as_str().unwrap()))
        .collect();
    on.sort_by(|a, b| a.total_cmp(b));
    println!(
        "  min {:.2}  median {:.2}  max {:.2}",
        on[0],
        on[on.len() / 2],
        on[on.len() - 1]
    );
    let off = [
        "what is the weather like in Paris tomorrow",
        "give me a recipe for banana pancakes",
        "who won the football world cup in 2010",
        "how do I train a puppy to sit",
        "translate good morning into Japanese",
        "write a poem about the sea",
        "what is the capital of Australia",
        "how many calories are in an apple",
    ];
    println!("best similarity: questions the docs cannot answer");
    let mut off_sims: Vec<f32> = off.iter().map(|q| top_similarity(q)).collect();
    for (q, v) in off.iter().zip(&off_sims) {
        println!("  {v:.2}  {q}");
    }
    off_sims.sort_by(|a, b| a.total_cmp(b));
    println!(
        "  min {:.2}  max {:.2}",
        off_sims[0],
        off_sims[off_sims.len() - 1]
    );
}
