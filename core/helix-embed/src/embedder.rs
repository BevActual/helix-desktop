use std::path::Path;

use candle_core::{DType, Device, Tensor};
use candle_nn::VarBuilder;
use candle_transformers::models::bert::{BertModel, Config};
use tokenizers::{PaddingParams, Tokenizer, TruncationParams};

/// How the token vectors of a sentence become one vector: the model card says which (bge uses its first token, MiniLM the mean).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pooling {
    Cls,
    Mean,
}

pub struct Embedder {
    model: BertModel,
    tokenizer: Tokenizer,
    pooling: Pooling,
    device: Device,
}

const MAX_TOKENS: usize = 256;
/// Candle splits each matrix product over as many threads as it is told about (every core by default). For a sentence that costs
/// several times what four threads take (8 ms against 36 ms for one message on 32 cores), so unless the process says otherwise
/// (`RAYON_NUM_THREADS`) it is told four, by [`limit_threads`]: call it at the start of `main`, before other threads exist (it changes the environment, and the value is read when the first model runs).
const THREADS: &str = "4";

impl Embedder {
    /// Loads the model in `dir`. Fails with a plain message when a file is missing or does not match.
    pub fn load(dir: &Path, pooling: Pooling) -> Result<Self, String> {
        let read = |name: &str| {
            std::fs::read(dir.join(name)).map_err(|e| format!("{}: {e}", dir.join(name).display()))
        };
        let config: Config = serde_json::from_slice(&read("config.json")?)
            .map_err(|e| format!("config.json: {e}"))?;
        let mut tokenizer = Tokenizer::from_bytes(read("tokenizer.json")?)
            .map_err(|e| format!("tokenizer.json: {e}"))?;
        tokenizer
            .with_truncation(Some(TruncationParams {
                max_length: MAX_TOKENS,
                ..Default::default()
            }))
            .map_err(|e| e.to_string())?;
        tokenizer.with_padding(Some(PaddingParams::default()));
        let device = Device::Cpu;
        let weights = dir.join("model.safetensors");
        // SAFETY: the file is mapped read-only and not modified while the model is alive (it is a model file we do not write).
        let vb = unsafe { VarBuilder::from_mmaped_safetensors(&[weights], DType::F32, &device) }
            .map_err(|e| e.to_string())?;
        let model = BertModel::load(vb, &config).map_err(|e| e.to_string())?;
        Ok(Self {
            model,
            tokenizer,
            pooling,
            device,
        })
    }

    /// One unit-length vector per text, in order.
    pub fn embed(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, String> {
        if texts.is_empty() {
            return Ok(Vec::new());
        }
        let encoded = self
            .tokenizer
            .encode_batch(texts.to_vec(), true)
            .map_err(|e| e.to_string())?;
        let rows = |f: &dyn Fn(&tokenizers::Encoding) -> Vec<u32>| -> Result<Tensor, String> {
            let data: Vec<Vec<u32>> = encoded.iter().map(f).collect();
            Tensor::new(data, &self.device).map_err(|e| e.to_string())
        };
        let ids = rows(&|e| e.get_ids().to_vec())?;
        let mask = rows(&|e| e.get_attention_mask().to_vec())?;
        let types = ids.zeros_like().map_err(|e| e.to_string())?;
        let out = self
            .model
            .forward(&ids, &types, Some(&mask))
            .map_err(|e| e.to_string())?; // batch x tokens x width
        let pooled = match self.pooling {
            Pooling::Cls => out.narrow(1, 0, 1).and_then(|t| t.squeeze(1)),
            Pooling::Mean => {
                let m = mask.to_dtype(DType::F32).and_then(|m| m.unsqueeze(2));
                m.and_then(|m| {
                    let sum = out.broadcast_mul(&m)?.sum(1)?;
                    let count = m.sum(1)?;
                    sum.broadcast_div(&count)
                })
            }
        }
        .map_err(|e| e.to_string())?;
        let unit = pooled
            .broadcast_div(
                &pooled
                    .sqr()
                    .and_then(|t| t.sum_keepdim(1))
                    .and_then(|t| t.sqrt())
                    .map_err(|e| e.to_string())?,
            )
            .map_err(|e| e.to_string())?;
        unit.to_vec2::<f32>().map_err(|e| e.to_string())
    }
}

/// See `THREADS`: sets `RAYON_NUM_THREADS` to four unless it is already set. Call it first thing in `main`.
pub fn limit_threads() {
    if std::env::var_os("RAYON_NUM_THREADS").is_none() {
        std::env::set_var("RAYON_NUM_THREADS", THREADS);
    }
}
