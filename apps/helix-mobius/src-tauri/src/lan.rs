//! OpenAI-compatible chat for LAN/cloud talk from Mobius.
//! Remote replies are plain text only — never Agent Surface tools (see inference-fabric.md).

use serde_json::{json, Value};

#[derive(Clone, Copy, Debug)]
pub struct RemoteOptions {
    pub temperature: f64,
    pub max_tokens: u32,
    pub timeout_secs: u64,
}

impl Default for RemoteOptions {
    fn default() -> Self {
        Self {
            temperature: 0.6,
            max_tokens: 1024,
            timeout_secs: 120,
        }
    }
}

/// POST `{base}/chat/completions`. `base` is e.g. `http://host:11434/v1` (no trailing slash required).
/// `api_key` becomes `Authorization: Bearer …` when present (OpenRouter, OpenAI, etc.).
pub async fn chat_completions(
    base_url: &str,
    model: &str,
    messages: &[(&str, String)],
    api_key: Option<&str>,
    options: RemoteOptions,
) -> Result<String, String> {
    let base = base_url.trim().trim_end_matches('/');
    if base.is_empty() {
        return Err("Remote backend has no URL.".into());
    }
    if !(base.starts_with("http://") || base.starts_with("https://")) {
        return Err("Remote URL must start with http:// or https://.".into());
    }
    let url = format!("{base}/chat/completions");
    let temperature = if options.temperature.is_finite() {
        options.temperature.clamp(0.0, 2.0)
    } else {
        0.6
    };
    let max_tokens = options.max_tokens.clamp(64, 8192);
    let body = json!({
        "model": if model.is_empty() { "default" } else { model },
        "messages": messages.iter().map(|(role, content)| json!({
            "role": *role,
            "content": content,
        })).collect::<Vec<_>>(),
        "temperature": temperature,
        "max_tokens": max_tokens,
        // Never request tools from remote backends.
        "stream": false,
    });
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(options.timeout_secs.clamp(15, 600)))
        .build()
        .map_err(|e| e.to_string())?;
    let mut request = client
        .post(&url)
        .header("Content-Type", "application/json")
        .json(&body);
    if let Some(key) = api_key.map(str::trim).filter(|k| !k.is_empty()) {
        request = request.header("Authorization", format!("Bearer {key}"));
    }
    // OpenRouter ranks apps that identify themselves; harmless elsewhere.
    if base.contains("openrouter.ai") {
        request = request
            .header("HTTP-Referer", "https://helix.os")
            .header("X-Title", "Helix Mobius");
    }
    let response = request
        .send()
        .await
        .map_err(|e| format!("Remote request failed: {e}"))?;
    if !response.status().is_success() {
        let status = response.status();
        let detail = response.text().await.unwrap_or_default();
        let short: String = detail.chars().take(200).collect();
        return Err(format!("Remote backend returned {status}: {short}"));
    }
    let value: Value = response
        .json()
        .await
        .map_err(|e| format!("Remote reply was not JSON: {e}"))?;
    extract_text(&value)
}

/// GET `{base}/models` (OpenAI-compatible). Caps at 80 ids for the picker.
pub async fn list_models(base_url: &str, api_key: Option<&str>) -> Result<Vec<String>, String> {
    let base = base_url.trim().trim_end_matches('/');
    if base.is_empty() {
        return Err("Remote backend has no URL.".into());
    }
    let url = format!("{base}/models");
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(30))
        .build()
        .map_err(|e| e.to_string())?;
    let mut request = client.get(&url).header("Content-Type", "application/json");
    if let Some(key) = api_key.map(str::trim).filter(|k| !k.is_empty()) {
        request = request.header("Authorization", format!("Bearer {key}"));
    }
    if base.contains("openrouter.ai") {
        request = request
            .header("HTTP-Referer", "https://helix.os")
            .header("X-Title", "Helix Mobius");
    }
    let response = request
        .send()
        .await
        .map_err(|e| format!("Model list failed: {e}"))?;
    if !response.status().is_success() {
        let status = response.status();
        return Err(format!("Model list returned {status}"));
    }
    let value: Value = response
        .json()
        .await
        .map_err(|e| format!("Model list was not JSON: {e}"))?;
    extract_model_ids(&value)
}

/// Lightweight reachability check for the Models page.
pub async fn probe(base_url: &str, api_key: Option<&str>) -> Result<String, String> {
    let models = list_models(base_url, api_key).await?;
    Ok(format!("{} model(s) reachable", models.len()))
}

fn extract_text(value: &Value) -> Result<String, String> {
    let choices = value
        .get("choices")
        .and_then(Value::as_array)
        .ok_or_else(|| "Remote reply missing choices.".to_string())?;
    let message = choices
        .first()
        .and_then(|c| c.get("message"))
        .ok_or_else(|| "Remote reply had no message.".to_string())?;
    // Ignore any tool_calls field — remote brains never get Agent Surface.
    if message.get("tool_calls").is_some() {
        eprintln!("MOBIUS ignored tool_calls from remote reply (Act-slot isolation)");
    }
    let content = message
        .get("content")
        .and_then(Value::as_str)
        .unwrap_or("")
        .trim();
    if content.is_empty() {
        return Err("Remote model returned nothing.".into());
    }
    Ok(content.to_string())
}

fn extract_model_ids(value: &Value) -> Result<Vec<String>, String> {
    let data = value
        .get("data")
        .and_then(Value::as_array)
        .ok_or_else(|| "Model list missing data.".to_string())?;
    let mut ids = Vec::new();
    for row in data.iter().take(80) {
        if let Some(id) = row.get("id").and_then(Value::as_str) {
            let id = id.trim();
            if !id.is_empty() {
                ids.push(id.to_string());
            }
        }
    }
    ids.sort_unstable();
    ids.dedup();
    Ok(ids)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn extracts_content() {
        let v = json!({"choices":[{"message":{"role":"assistant","content":"hello"}}]});
        assert_eq!(extract_text(&v).unwrap(), "hello");
    }

    #[test]
    fn ignores_tool_calls_shape() {
        let v = json!({"choices":[{"message":{
            "role":"assistant",
            "content":"use the bar",
            "tool_calls":[{"id":"1","type":"function","function":{"name":"x","arguments":"{}"}}]
        }}]});
        assert_eq!(extract_text(&v).unwrap(), "use the bar");
    }

    #[test]
    fn extracts_model_ids() {
        let v = json!({"data":[{"id":"b"},{"id":"a"},{"id":"a"}]});
        assert_eq!(extract_model_ids(&v).unwrap(), vec!["a".to_string(), "b".to_string()]);
    }
}
