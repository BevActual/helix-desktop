//! OpenRouter's catalog of hosted models: what each costs, how much it can read, what it accepts, and what it can do.
//!
//! The list is public, so it is fetched without the person's key, only when they ask (Settings → Cloud models) or the first time after
//! OpenRouter is set up, and kept on disk so the page opens at once and works offline. The model menu shows only the models the person
//! has pinned, the way Cursor's switcher does, so a catalog of hundreds never has to fit in a menu.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct CloudModel {
    pub id: String,
    pub name: String,
    /// The first sentence or two of the publisher's description.
    pub blurb: String,
    /// How many tokens it can read at once.
    pub context: u64,
    /// US dollars per million tokens. `None` when the price depends on the request (OpenRouter's own routers).
    pub prompt_per_m: Option<f64>,
    pub completion_per_m: Option<f64>,
    pub free: bool,
    /// Reads pictures.
    pub vision: bool,
    /// Writes pictures.
    pub makes_images: bool,
    /// Can call tools.
    pub tools: bool,
    /// Can think before it answers.
    pub reasoning: bool,
    /// Unix seconds when OpenRouter listed it.
    pub created: i64,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Catalog {
    /// Unix seconds when this copy was fetched; 0 when there is none.
    pub fetched: u64,
    pub models: Vec<CloudModel>,
}

fn price(value: Option<&Value>) -> Option<f64> {
    let per_token: f64 = value?.as_str()?.parse().ok()?;
    // OpenRouter writes -1 for "depends on the request".
    (per_token >= 0.0).then_some(per_token * 1_000_000.0)
}

fn blurb(text: &str) -> String {
    let flat: String = text.split_whitespace().collect::<Vec<_>>().join(" ");
    let mut out = String::new();
    for sentence in flat.split_inclusive(". ") {
        if out.chars().count() + sentence.chars().count() > 200 && !out.is_empty() {
            break;
        }
        out.push_str(sentence);
        if out.chars().count() >= 90 {
            break;
        }
    }
    let out = out.trim().to_string();
    if out.chars().count() > 220 {
        let cut: String = out.chars().take(217).collect();
        format!("{}…", cut.trim_end())
    } else {
        out
    }
}

/// Reads the `/models` reply. An entry without an id is skipped; anything else missing is left unknown, never guessed.
pub fn parse(reply: &Value) -> Vec<CloudModel> {
    let Some(list) = reply.get("data").and_then(Value::as_array) else {
        return Vec::new();
    };
    let mut models: Vec<CloudModel> = list
        .iter()
        .filter_map(|entry| {
            let id = entry.get("id")?.as_str()?.trim().to_string();
            if id.is_empty() {
                return None;
            }
            let strings = |value: Option<&Value>| -> Vec<String> {
                value
                    .and_then(Value::as_array)
                    .map(|a| {
                        a.iter()
                            .filter_map(|v| v.as_str().map(str::to_string))
                            .collect()
                    })
                    .unwrap_or_default()
            };
            let architecture = entry.get("architecture");
            let inputs = strings(architecture.and_then(|a| a.get("input_modalities")));
            let outputs = strings(architecture.and_then(|a| a.get("output_modalities")));
            let params = strings(entry.get("supported_parameters"));
            let pricing = entry.get("pricing");
            let prompt = price(pricing.and_then(|p| p.get("prompt")));
            let completion = price(pricing.and_then(|p| p.get("completion")));
            let name = entry
                .get("name")
                .and_then(Value::as_str)
                .unwrap_or(&id)
                .to_string();
            Some(CloudModel {
                free: prompt == Some(0.0) && completion == Some(0.0),
                blurb: blurb(
                    entry
                        .get("description")
                        .and_then(Value::as_str)
                        .unwrap_or(""),
                ),
                context: entry
                    .get("context_length")
                    .and_then(Value::as_u64)
                    .unwrap_or(0),
                vision: inputs.iter().any(|m| m == "image"),
                makes_images: outputs.iter().any(|m| m == "image"),
                tools: params.iter().any(|p| p == "tools"),
                reasoning: params.iter().any(|p| p == "reasoning"),
                created: entry.get("created").and_then(Value::as_i64).unwrap_or(0),
                prompt_per_m: prompt,
                completion_per_m: completion,
                name,
                id,
            })
        })
        .collect();
    models.sort_by(|a, b| b.created.cmp(&a.created).then_with(|| a.id.cmp(&b.id)));
    models.dedup_by(|a, b| a.id == b.id);
    models
}

fn data_home() -> PathBuf {
    std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
        .unwrap_or_else(|| PathBuf::from("."))
        .join("helix/mobius")
}

fn catalog_path() -> PathBuf {
    data_home().join("openrouter-models.json")
}

fn pins_path() -> PathBuf {
    data_home().join("cloud-pins.json")
}

pub fn load_catalog() -> Catalog {
    std::fs::read_to_string(catalog_path())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn write_atomic(path: &PathBuf, text: &str) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temp = path.with_extension("tmp");
    std::fs::write(&temp, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temp, path).map_err(|e| e.to_string())
}

/// Fetches the catalog from `{base}/models` and keeps it. Only https hosts are asked, and no key is sent: the list is public.
pub async fn refresh(base_url: &str, now: u64) -> Result<Catalog, String> {
    let base = base_url.trim().trim_end_matches('/');
    if !base.starts_with("https://") {
        return Err("The model catalog is only fetched over https.".into());
    }
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(30))
        .build()
        .map_err(|e| e.to_string())?;
    let response = client
        .get(format!("{base}/models"))
        .header("HTTP-Referer", "https://helix.os")
        .header("X-Title", "Helix Mobius")
        .send()
        .await
        .map_err(|e| format!("Could not fetch the catalog: {e}"))?;
    if !response.status().is_success() {
        return Err(format!("The catalog returned {}", response.status()));
    }
    let value: Value = response
        .json()
        .await
        .map_err(|e| format!("The catalog was not JSON: {e}"))?;
    let models = parse(&value);
    if models.is_empty() {
        return Err("The catalog had no models.".into());
    }
    let catalog = Catalog {
        fetched: now,
        models,
    };
    let text = serde_json::to_string(&catalog).map_err(|e| e.to_string())?;
    write_atomic(&catalog_path(), &text)?;
    Ok(catalog)
}

/// The models the person put in the chat model menu, in the order they were added.
pub fn pins() -> Vec<String> {
    std::fs::read_to_string(pins_path())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

/// Adds or removes a pin. An id is a model id as OpenRouter writes it (`vendor/name`), checked so nothing else is stored.
pub fn set_pin(id: &str, on: bool) -> Result<Vec<String>, String> {
    let id = id.trim();
    if !valid_id(id) {
        return Err("That is not a model id.".into());
    }
    let mut list = pins();
    list.retain(|p| p != id);
    if on {
        list.push(id.to_string());
    }
    let text = serde_json::to_string(&list).map_err(|e| e.to_string())?;
    write_atomic(&pins_path(), &text)?;
    Ok(list)
}

/// Letters, digits and `/ . : _ -`, at most 120 of them, with a vendor before a slash.
pub fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 120
        && id.contains('/')
        && !id.starts_with('/')
        && !id.ends_with('/')
        && !id.contains("..")
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '/' | '.' | ':' | '_' | '-'))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn sample() -> Value {
        json!({"data": [
            {"id": "acme/small:free", "name": "Acme Small (free)", "created": 100, "context_length": 32768,
             "description": "A small model.  It is quick. It runs anywhere and costs nothing to try out on a busy afternoon at all.",
             "architecture": {"input_modalities": ["text"], "output_modalities": ["text"]},
             "pricing": {"prompt": "0", "completion": "0"}, "supported_parameters": ["temperature"]},
            {"id": "acme/vision-pro", "name": "Acme Vision Pro", "created": 300, "context_length": 1000000,
             "architecture": {"input_modalities": ["text", "image"], "output_modalities": ["text"]},
             "pricing": {"prompt": "0.0000025", "completion": "0.00001"}, "supported_parameters": ["tools", "reasoning"]},
            {"id": "router/auto", "name": "Auto", "created": 200, "context_length": 2000000,
             "architecture": {"input_modalities": ["text"], "output_modalities": ["text", "image"]},
             "pricing": {"prompt": "-1", "completion": "-1"}},
            {"name": "no id"}
        ]})
    }

    #[test]
    fn reads_prices_abilities_and_orders_newest_first() {
        let models = parse(&sample());
        assert_eq!(
            models.iter().map(|m| m.id.as_str()).collect::<Vec<_>>(),
            ["acme/vision-pro", "router/auto", "acme/small:free"]
        );
        let pro = &models[0];
        assert_eq!(pro.prompt_per_m, Some(2.5));
        assert_eq!(pro.completion_per_m, Some(10.0));
        assert!(pro.vision && pro.tools && pro.reasoning && !pro.free && !pro.makes_images);
        let free = &models[2];
        assert!(free.free && !free.vision && !free.tools);
        assert_eq!(free.context, 32768);
    }

    #[test]
    fn a_price_that_depends_on_the_request_is_unknown_not_free() {
        let auto = &parse(&sample())[1];
        assert_eq!((auto.prompt_per_m, auto.free), (None, false));
        assert!(auto.makes_images);
    }

    #[test]
    fn the_blurb_is_whole_sentences_until_it_is_long_enough_and_never_too_long() {
        let text = parse(&sample()).pop().unwrap().blurb;
        assert!(text.starts_with("A small model. It is quick. "), "{text}");
        assert!(text.ends_with("at all."), "{text}");
        assert!(blurb(&"word ".repeat(200)).chars().count() <= 220);
    }

    #[test]
    fn a_reply_without_a_list_is_empty() {
        assert!(parse(&json!({"error": "nope"})).is_empty());
    }

    #[test]
    fn only_vendor_slash_name_ids_are_stored() {
        for good in [
            "openai/gpt-4o",
            "meta-llama/llama-3.3-70b-instruct:free",
            "a/b.c_d-e",
        ] {
            assert!(valid_id(good), "{good}");
        }
        for bad in [
            "",
            "gpt-4o",
            "/x",
            "x/",
            "a b/c",
            "a/b\"; drop",
            "../../etc/passwd",
        ] {
            assert!(!valid_id(bad), "{bad}");
        }
    }
}
