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
    base.starts_with("https://")
        || (base.starts_with("http://")
            && helix_config::inference_routing::is_local_network_url(base))
}

/// How OpenRouter should choose among its providers and models for one request. Only sent to OpenRouter.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct OpenRouterRoute {
    /// Models to try, in order, when the first fails (an error, a rate limit, or a conversation too long for it).
    pub fallbacks: Vec<String>,
    /// `price` or `speed`: which provider of a model to prefer. Empty lets OpenRouter balance them.
    pub prefer: String,
    /// Only providers that promise not to keep or train on the prompt.
    pub private_only: bool,
}

/// What came back: the text, the model that actually answered (it may be a fallback), what it cost and how long it wrote.
#[derive(Clone, Debug, PartialEq)]
pub struct Reply {
    pub text: String,
    pub model: String,
    /// Millionths of a dollar, when the provider said.
    pub cost_micro: Option<u64>,
    pub completion_tokens: Option<u32>,
}

/// The request body. With a route, OpenRouter gets its own fields: a `models` list in place of `model` when there are fallbacks, a
/// `provider` object for the price/speed preference and the privacy rule, and `usage` so the reply carries its cost.
pub fn request_body(
    model: &str,
    messages: &[(&str, String)],
    temperature: f64,
    max_tokens: u32,
    route: Option<&OpenRouterRoute>,
) -> Value {
    let mut body = json!({
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
    if let Some(route) = route {
        if !route.fallbacks.is_empty() && !model.is_empty() {
            let mut all = vec![model.to_string()];
            all.extend(route.fallbacks.iter().filter(|f| *f != model).cloned());
            body.as_object_mut().expect("object").remove("model");
            body["models"] = json!(all);
        }
        let mut provider = serde_json::Map::new();
        if route.private_only {
            provider.insert("data_collection".into(), json!("deny"));
        }
        match route.prefer.as_str() {
            "price" => {
                provider.insert("sort".into(), json!("price"));
            }
            "speed" => {
                provider.insert("sort".into(), json!("throughput"));
            }
            _ => {}
        }
        if !provider.is_empty() {
            body["provider"] = Value::Object(provider);
        }
        body["usage"] = json!({"include": true});
    }
    body
}

/// Reads the reply, with the model that answered and its cost.
pub fn parse_reply(value: &Value, asked: &str) -> Result<Reply, String> {
    let text = extract_text(value)?;
    let model = value
        .get("model")
        .and_then(Value::as_str)
        .filter(|m| !m.is_empty())
        .unwrap_or(asked)
        .to_string();
    let usage = value.get("usage");
    let cost_micro = usage
        .and_then(|u| u.get("cost"))
        .and_then(Value::as_f64)
        .filter(|c| c.is_finite() && *c >= 0.0)
        .map(|c| (c * 1_000_000.0).round() as u64);
    let completion_tokens = usage
        .and_then(|u| u.get("completion_tokens"))
        .and_then(Value::as_u64)
        .map(|t| t.min(u64::from(u32::MAX)) as u32);
    Ok(Reply {
        text,
        model,
        cost_micro,
        completion_tokens,
    })
}

/// What a refusal means to the person, when it is one they can act on.
fn explain_refusal(status: u16, detail: &str) -> Option<String> {
    let lower = detail.to_lowercase();
    if lower.contains("data policy")
        || lower.contains("data_collection")
        || (status == 404 && lower.contains("no endpoints found"))
    {
        return Some("No provider for this model promises not to keep your prompts. Allow other providers for this job in Settings → Models, or choose another model.".into());
    }
    if status == 402 {
        return Some("OpenRouter says the account is out of credit.".into());
    }
    if status == 401 {
        return Some(
            "OpenRouter did not accept the API key. Check it in Settings → Providers.".into(),
        );
    }
    None
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
    chat_completions_routed(base_url, model, messages, api_key, options, None)
        .await
        .map(|reply| reply.text)
}

/// Like [`chat_completions`], with OpenRouter's routing options, and the model that answered and its cost reported back.
pub async fn chat_completions_routed(
    base_url: &str,
    model: &str,
    messages: &[(&str, String)],
    api_key: Option<&str>,
    options: RemoteOptions,
    route: Option<&OpenRouterRoute>,
) -> Result<Reply, String> {
    let base = base_url.trim().trim_end_matches('/');
    if base.is_empty() {
        return Err("Remote backend has no URL.".into());
    }
    if !(base.starts_with("http://") || base.starts_with("https://")) {
        return Err("Remote URL must start with http:// or https://.".into());
    }
    if api_key.map(str::trim).is_some_and(|k| !k.is_empty()) && !key_allowed(base) {
        return Err(
            "An API key is only sent over https, or to this computer or your local network.".into(),
        );
    }
    let url = format!("{base}/chat/completions");
    let temperature = if options.temperature.is_finite() {
        options.temperature.clamp(0.0, 2.0)
    } else {
        0.6
    };
    let max_tokens = options.max_tokens.clamp(64, 8192);
    let body = request_body(model, messages, temperature, max_tokens, route);
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(
            options.timeout_secs.clamp(15, 600),
        ))
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
        if let Some(plain) = explain_refusal(status.as_u16(), &detail) {
            return Err(plain);
        }
        let short: String = detail.chars().take(200).collect();
        return Err(format!("Remote backend returned {status}: {short}"));
    }
    let value: Value = response
        .json()
        .await
        .map_err(|e| format!("Remote reply was not JSON: {e}"))?;
    parse_reply(&value, model)
}

/// GET `{base}/models` (OpenAI-compatible). Caps at 80 ids for the picker.
pub async fn list_models(base_url: &str, api_key: Option<&str>) -> Result<Vec<String>, String> {
    let base = base_url.trim().trim_end_matches('/');
    if base.is_empty() {
        return Err("Remote backend has no URL.".into());
    }
    if api_key.map(str::trim).is_some_and(|k| !k.is_empty()) && !key_allowed(base) {
        return Err(
            "An API key is only sent over https, or to this computer or your local network.".into(),
        );
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

    fn msgs() -> Vec<(&'static str, String)> {
        vec![("user", "hi".to_string())]
    }

    #[test]
    fn without_a_route_the_request_is_plain() {
        let body = request_body("vendor/m", &msgs(), 0.6, 512, None);
        assert_eq!(body["model"], "vendor/m");
        assert!(
            body.get("models").is_none()
                && body.get("provider").is_none()
                && body.get("usage").is_none()
        );
        assert_eq!(body["stream"], false);
    }

    #[test]
    fn a_route_adds_fallbacks_a_preference_the_privacy_rule_and_cost_reporting() {
        let route = OpenRouterRoute {
            fallbacks: vec!["b/m2".into(), "vendor/m".into(), "c/m3".into()],
            prefer: "price".into(),
            private_only: true,
        };
        let body = request_body("vendor/m", &msgs(), 0.6, 512, Some(&route));
        assert!(
            body.get("model").is_none(),
            "the models list replaces model"
        );
        assert_eq!(
            body["models"],
            serde_json::json!(["vendor/m", "b/m2", "c/m3"])
        );
        assert_eq!(
            body["provider"],
            serde_json::json!({"data_collection": "deny", "sort": "price"})
        );
        assert_eq!(body["usage"], serde_json::json!({"include": true}));
        let speed = OpenRouterRoute {
            prefer: "speed".into(),
            ..OpenRouterRoute::default()
        };
        let body = request_body("vendor/m", &msgs(), 0.6, 512, Some(&speed));
        assert_eq!(body["model"], "vendor/m");
        assert_eq!(body["provider"], serde_json::json!({"sort": "throughput"}));
        let none = OpenRouterRoute::default();
        assert!(request_body("vendor/m", &msgs(), 0.6, 512, Some(&none))
            .get("provider")
            .is_none());
    }

    #[test]
    fn the_reply_says_which_model_answered_and_what_it_cost() {
        let value = serde_json::json!({"model": "b/m2", "choices": [{"message": {"content": " Hello. "}}], "usage": {"completion_tokens": 7, "cost": 0.0012}});
        let reply = parse_reply(&value, "vendor/m").unwrap();
        assert_eq!(
            (
                reply.text.as_str(),
                reply.model.as_str(),
                reply.cost_micro,
                reply.completion_tokens
            ),
            ("Hello.", "b/m2", Some(1200), Some(7))
        );
        let bare = serde_json::json!({"choices": [{"message": {"content": "x"}}]});
        let reply = parse_reply(&bare, "asked/model").unwrap();
        assert_eq!(
            (
                reply.model.as_str(),
                reply.cost_micro,
                reply.completion_tokens
            ),
            ("asked/model", None, None)
        );
        let free =
            serde_json::json!({"choices": [{"message": {"content": "x"}}], "usage": {"cost": 0}});
        assert_eq!(parse_reply(&free, "m").unwrap().cost_micro, Some(0));
        let odd = serde_json::json!({"choices": [{"message": {"content": "x"}}], "usage": {"cost": -1.0}});
        assert_eq!(parse_reply(&odd, "m").unwrap().cost_micro, None);
    }

    #[test]
    fn refusals_a_person_can_act_on_are_said_plainly() {
        assert!(
            explain_refusal(404, "No endpoints found matching your data policy")
                .unwrap()
                .contains("keep your prompts")
        );
        assert!(explain_refusal(402, "").unwrap().contains("credit"));
        assert!(explain_refusal(401, "").unwrap().contains("API key"));
        assert!(explain_refusal(500, "boom").is_none());
    }

    use serde_json::json;

    #[test]
    fn a_key_never_goes_to_the_internet_in_clear_text() {
        for ok in [
            "https://api.openai.com/v1",
            "https://openrouter.ai/api/v1",
            "http://localhost:11434/v1",
            "http://127.0.0.1:8080/v1",
            "http://192.168.1.20:1234/v1",
            "http://10.0.0.5/v1",
            "http://172.16.4.1/v1",
            "http://172.31.255.1/v1",
            "http://[::1]:8080/v1",
            "http://studio.local:11434/v1",
            "http://169.254.1.1/v1",
        ] {
            assert!(key_allowed(ok), "{ok}");
        }
        for bad in [
            "http://api.openai.com/v1",
            "http://8.8.8.8/v1",
            "http://172.32.0.1/v1",
            "http://172.15.0.1/v1",
            "http://192.169.1.1/v1",
            "http://localhost@evil.example/v1",
            "http://example.com:80/v1",
            "ftp://x",
            "file:///etc/passwd",
            "",
            "api.openai.com",
            "http://10.0.0.5.evil.example/v1",
            "http://127.0.0.1.evil.example/v1",
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
        assert_eq!(
            extract_model_ids(&v).unwrap(),
            vec!["a".to_string(), "b".to_string()]
        );
    }
}
