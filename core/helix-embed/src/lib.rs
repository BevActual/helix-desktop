//! Sentence embeddings on the CPU, in Rust, and a decider built on them.
//!
//! [`Embedder`] runs a small BERT-family sentence model (a folder with `config.json`, `tokenizer.json` and `model.safetensors`, such as
//! bge-small-en-v1.5 or all-MiniLM-L6-v2) with candle: no C++ engine, no GPU, no model swap. [`Decider`] turns vectors into a choice among
//! a few named routes, each described by example sentences, and says how sure it is.

mod decider;
mod embedder;

pub use decider::{Choice, Decider, Route};
pub use embedder::{limit_threads, Embedder, Pooling};
