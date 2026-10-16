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

/// Whether an API key may be sent to `base`. Over https always; over plain http only to this computer or a local network address,
/// so a key is never put on the open internet in clear text (a typo of `http://` for a cloud service would do that).
pub fn key_allowed(base: &str) -> bool {
    let base = base.trim();
    base.starts_with("https://") || (base.starts_with("http://") && helix_config::inference_routing::is_local_network_url(base))
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
    if api_key.map(str::trim).is_some_and(|k| !k.is_empty()) && !key_allowed(base) {
        return Err("An API key is only sent over https, or to this computer or your local network.".into());
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
    if api_key.map(str::trim).is_some_and(|k| !k.is_empty()) && !key_allowed(base) {
        return Err("An API key is only sent over https, or to this computer or your local network.".into());
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
    fn a_key_never_goes_to_the_internet_in_clear_text() {
        for ok in [
            "https://api.openai.com/v1", "https://openrouter.ai/api/v1", "http://localhost:11434/v1", "http://127.0.0.1:8080/v1",
            "http://192.168.1.20:1234/v1", "http://10.0.0.5/v1", "http://172.16.4.1/v1", "http://172.31.255.1/v1", "http://[::1]:8080/v1",
            "http://studio.local:11434/v1", "http://169.254.1.1/v1",
        ] {
            assert!(key_allowed(ok), "{ok}");
        }
        for bad in [
            "http://api.openai.com/v1", "http://8.8.8.8/v1", "http://172.32.0.1/v1", "http://172.15.0.1/v1", "http://192.169.1.1/v1",
            "http://localhost@evil.example/v1", "http://example.com:80/v1", "ftp://x", "file:///etc/passwd", "", "api.openai.com",
            "http://10.0.0.5.evil.example/v1", "http://127.0.0.1.evil.example/v1",
        ] {
            assert!(!key_allowed(bad), "{bad}");
        }
    }

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
