//! OpenAI-compatible chat for LAN/cloud talk from Mobius.
//! Remote replies are plain text only — never Agent Surface tools (see inference-fabric.md).

use serde_json::{json, Value};

/// POST `{base}/chat/completions`. `base` is e.g. `http://host:11434/v1` (no trailing slash required).
pub async fn chat_completions(
    base_url: &str,
    model: &str,
    messages: &[(&str, String)],
) -> Result<String, String> {
    let base = base_url.trim().trim_end_matches('/');
    if base.is_empty() {
        return Err("LAN backend has no URL.".into());
    }
    if !(base.starts_with("http://") || base.starts_with("https://")) {
        return Err("LAN URL must start with http:// or https://.".into());
    }
    let url = format!("{base}/chat/completions");
    let body = json!({
        "model": if model.is_empty() { "default" } else { model },
        "messages": messages.iter().map(|(role, content)| json!({
            "role": *role,
            "content": content,
        })).collect::<Vec<_>>(),
        "temperature": 0.6,
        // Never request tools from remote backends.
        "stream": false,
    });
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(120))
        .build()
        .map_err(|e| e.to_string())?;
    let response = client
        .post(&url)
        .header("Content-Type", "application/json")
        .json(&body)
        .send()
        .await
        .map_err(|e| format!("LAN request failed: {e}"))?;
    if !response.status().is_success() {
        let status = response.status();
        let detail = response.text().await.unwrap_or_default();
        let short: String = detail.chars().take(200).collect();
        return Err(format!("LAN backend returned {status}: {short}"));
    }
    let value: Value = response
        .json()
        .await
        .map_err(|e| format!("LAN reply was not JSON: {e}"))?;
    extract_text(&value)
}

fn extract_text(value: &Value) -> Result<String, String> {
    let choices = value
        .get("choices")
        .and_then(Value::as_array)
        .ok_or_else(|| "LAN reply missing choices.".to_string())?;
    let message = choices
        .first()
        .and_then(|c| c.get("message"))
        .ok_or_else(|| "LAN reply had no message.".to_string())?;
    // Ignore any tool_calls field — remote brains never get Agent Surface.
    if message.get("tool_calls").is_some() {
        eprintln!("MOBIUS ignored tool_calls from LAN reply (Act-slot isolation)");
    }
    let content = message
        .get("content")
        .and_then(Value::as_str)
        .unwrap_or("")
        .trim();
    if content.is_empty() {
        return Err("LAN model returned nothing.".into());
    }
    Ok(content.to_string())
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
}
