use std::collections::HashMap;
use std::sync::Arc;

use helix_shell_core::{evaluate, result_is_untrusted, shell_ipc, Decision, RiskTier, Verdict};
use serde_json::json;
use tokio::sync::Mutex;
use zbus::fdo;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};
use zbus::{interface, Connection};

use crate::dbus_value::{dict_entry, json_to_owned, owned_bool, owned_str, owned_u64};
use crate::peer::resolve_peer;
use crate::state::{ClientSession, ConfirmKind, PendingConfirm, State};

pub struct Agent {
    pub state: Arc<Mutex<State>>,
}

fn sender_name(header: &Header<'_>) -> fdo::Result<String> {
    Ok(header
        .sender()
        .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
        .to_string())
}

fn require_manager(state: &State, client: &str) -> fdo::Result<()> {
    if state.manager.as_deref() == Some(client) {
        Ok(())
    } else {
        Err(fdo::Error::AccessDenied("not the Agent Surface manager".into()))
    }
}

fn require_confirmer(state: &State, unique: &str) -> fdo::Result<()> {
    if state.confirmer.as_deref() == Some(unique) {
        Ok(())
    } else {
        Err(fdo::Error::AccessDenied("not the Agent Surface confirmer".into()))
    }
}

fn parse_tiers(names: &[String]) -> fdo::Result<Vec<RiskTier>> {
    let mut out = Vec::new();
    for name in names {
        let Some(tier) = RiskTier::parse(name) else {
            return Err(fdo::Error::InvalidArgs(format!("unknown tier {name}")));
        };
        if tier != RiskTier::Blocked {
            out.push(tier);
        }
    }
    Ok(out)
}

fn string_list(value: &OwnedValue) -> Vec<String> {
    match &**value {
        Value::Array(array) => array
            .iter()
            .filter_map(|item| match item {
                Value::Str(text) => Some(text.to_string()),
                _ => None,
            })
            .collect(),
        _ => Vec::new(),
    }
}

fn string_field(value: &OwnedValue) -> String {
    match &**value {
        Value::Str(text) => text.to_string(),
        _ => String::new(),
    }
}

fn action_args(args: &HashMap<String, OwnedValue>) -> (Vec<String>, Vec<String>, String) {
    let argv = args.get("argv").map(string_list).unwrap_or_default();
    let literal = args.get("literal").map(string_list).unwrap_or_default();
    let reason = args.get("reason").map(string_field).unwrap_or_default();
    (argv, literal, reason)
}

fn combined_args(argv: &[String], literal: &[String]) -> Vec<String> {
    argv.iter().chain(literal.iter()).cloned().collect()
}

fn action_content(response: &str) -> serde_json::Value {
    if response.starts_with("error:") {
        json!(response.strip_prefix("error:").unwrap_or(response).trim())
    } else {
        serde_json::from_str(response).unwrap_or_else(|_| json!(response))
    }
}

#[interface(name = "org.helix.Desktop.AgentSurface1")]
impl Agent {
    async fn hello(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        label: &str,
        version: &str,
    ) -> fdo::Result<HashMap<String, OwnedValue>> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let mut state = self.state.lock().await;
        state.clients.insert(
            peer.client.clone(),
            ClientSession {
                label: label.chars().take(64).collect(),
                version: version.chars().take(32).collect(),
            },
        );
        let tiers: Vec<String> = state
            .grants
            .tiers_for(&peer.client)
            .into_iter()
            .map(|tier| tier.name().to_string())
            .collect();
        let mut session = HashMap::new();
        session.insert("client".into(), owned_str(&peer.client));
        session.insert(
            "tiers".into(),
            OwnedValue::try_from(Value::from(tiers)).expect("tiers"),
        );
        Ok(session)
    }

    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        let mut map = HashMap::new();
        map.insert("implementation".into(), owned_str("rust"));
        map.insert("pending".into(), owned_u64(state.pending.len() as u64));
        map.insert("confirmer".into(), owned_bool(state.confirmer.is_some()));
        map
    }

    async fn list_tools(&self) -> Vec<OwnedValue> {
        let state = self.state.lock().await;
        let supported = shell_ipc::shell_supported_tools();
        state
            .risk
            .commands()
            .into_iter()
            .filter(|name| supported.is_empty() || supported.contains(name))
            .map(|name| {
                let tier = state.risk.tier_for(&name, &[]);
                dict_entry(HashMap::from([
                    ("name", owned_str(&name)),
                    ("tier", owned_str(tier.name())),
                ]))
            })
            .collect()
    }

    async fn request_access(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        tiers: Vec<String>,
    ) -> fdo::Result<u64> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let wanted = parse_tiers(&tiers)?;
        if wanted.is_empty() {
            return Err(fdo::Error::InvalidArgs("no grantable tiers".into()));
        }
        let mut state = self.state.lock().await;
        if !state.agent_enabled {
            return Err(fdo::Error::Failed(
                "Agent access requests are disabled".into(),
            ));
        }
        let held = state.grants.tiers_for(&peer.client);
        if wanted.iter().all(|tier| held.contains(tier)) {
            let operation = state.alloc_operation();
            let result = json!({
                "trust": "trusted",
                "content": { "tiers": wanted.iter().map(|tier| tier.name()).collect::<Vec<_>>() }
            });
            Self::completed(&emitter, operation, true, &result.to_string())
                .await
                .ok();
            return Ok(operation);
        }
        let operation = state.alloc_operation();
        let label = state
            .clients
            .get(&peer.client)
            .map(|session| session.label.clone())
            .unwrap_or_default();
        let pending = PendingConfirm {
            client: peer.client.clone(),
            kind: ConfirmKind::Grant,
            tool: String::new(),
            argv: Vec::new(),
            literal: Vec::new(),
            reason: String::new(),
            tiers: wanted.iter().map(|tier| tier.name().to_string()).collect(),
            tainted: state.taint.tainted(&peer.client),
            label,
        };
        state.pending.insert(operation, pending.clone());
        drop(state);
        Self::emit_confirmation(&emitter, operation, &pending).await?;
        Ok(operation)
    }

    async fn action(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        action: &str,
        args: HashMap<String, OwnedValue>,
    ) -> fdo::Result<u64> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let (argv, literal, reason) = action_args(&args);
        let all_args = combined_args(&argv, &literal);

        let mut state = self.state.lock().await;
        if !state.agent_enabled {
            return Err(fdo::Error::Failed(
                "Agent actions are disabled".into(),
            ));
        }
        let Decision {
            verdict,
            tier,
            tainted,
            reason: deny_reason,
        } = evaluate(
            &state.risk,
            &state.grants,
            &state.taint,
            &peer.client,
            action,
            &all_args,
        );
        let operation = state.alloc_operation();
        match verdict {
            Verdict::Deny => {
                let result = json!({"trust": "trusted", "content": {"error": deny_reason}});
                Self::completed(&emitter, operation, false, &result.to_string())
                    .await
                    .ok();
                Ok(operation)
            }
            Verdict::Allow => {
                let client = peer.client.clone();
                let tool = action.to_string();
                drop(state);
                self.run_shell_action(
                    &emitter,
                    operation,
                    tier,
                    &client,
                    &tool,
                    &argv,
                    &literal,
                )
                .await?;
                Ok(operation)
            }
            Verdict::NeedsConfirmation => {
                let label = state
                    .clients
                    .get(&peer.client)
                    .map(|session| session.label.clone())
                    .unwrap_or_default();
                let pending = PendingConfirm {
                    client: peer.client,
                    kind: ConfirmKind::Action,
                    tool: action.to_string(),
                    argv,
                    literal,
                    reason,
                    tiers: vec![tier.name().into()],
                    tainted,
                    label,
                };
                state.pending.insert(operation, pending.clone());
                drop(state);
                Self::emit_confirmation(&emitter, operation, &pending).await?;
                Ok(operation)
            }
        }
    }

    async fn cancel(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        operation: u64,
    ) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        if state.pending.remove(&operation).is_none() {
            return Err(fdo::Error::InvalidArgs("unknown operation".into()));
        }
        drop(state);
        Self::confirmation_ended(&emitter, operation).await.ok();
        let result = json!({"trust": "trusted", "content": {"error": "cancelled"}});
        Self::completed(&emitter, operation, false, &result.to_string())
            .await
            .ok();
        Ok(())
    }

    async fn register_confirmer(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let mut state = self.state.lock().await;
        state.confirmer = Some(peer.unique_name);
        tracing::info!(client = %peer.client, "RegisterConfirmer");
        Ok(())
    }

    async fn confirm(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        operation: u64,
        approved: bool,
        options: HashMap<String, OwnedValue>,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let mut state = self.state.lock().await;
        require_confirmer(&state, &sender)?;
        let Some(pending) = state.pending.remove(&operation) else {
            return Err(fdo::Error::InvalidArgs("unknown operation".into()));
        };
        if !approved {
            drop(state);
            let result = json!({"trust": "trusted", "content": {"error": "denied"}});
            Self::completed(&emitter, operation, false, &result.to_string())
                .await
                .ok();
            return Ok(());
        }
        match pending.kind {
            ConfirmKind::Grant => {
                let mut tiers = parse_tiers(&pending.tiers)?;
                if let Some(value) = options.get("tiers") {
                    let names = string_list(value);
                    if !names.is_empty() {
                        tiers = parse_tiers(&names)?;
                    }
                }
                state.grants.set(&pending.client, tiers.iter().copied());
                if let Err(error) = state.grants.save() {
                    tracing::warn!(%error, "grants save failed");
                }
                state.taint.clear(&pending.client);
                let result = json!({
                    "trust": "trusted",
                    "content": { "tiers": tiers.iter().map(|tier| tier.name()).collect::<Vec<_>>() }
                });
                drop(state);
                Self::completed(&emitter, operation, true, &result.to_string())
                    .await
                    .ok();
            }
            ConfirmKind::Action => {
                state.taint.clear(&pending.client);
                let tier = parse_tiers(&pending.tiers)?.into_iter().next().unwrap_or(RiskTier::Confirm);
                let client = pending.client.clone();
                let tool = pending.tool.clone();
                let argv = pending.argv.clone();
                let literal = pending.literal.clone();
                drop(state);
                self.run_shell_action(
                    &emitter,
                    operation,
                    tier,
                    &client,
                    &tool,
                    &argv,
                    &literal,
                )
                .await?;
            }
        }
        Ok(())
    }

    async fn register_manager(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let mut state = self.state.lock().await;
        // Scaffold: any same-user caller may manage (C++ restricts to Settings exe).
        state.manager = Some(peer.client.clone());
        tracing::info!(client = %peer.client, "RegisterManager");
        Ok(())
    }

    async fn get_grants(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
    ) -> fdo::Result<Vec<OwnedValue>> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let state = self.state.lock().await;
        require_manager(&state, &peer.client)?;
        Ok(state
            .grants
            .as_rows()
            .iter()
            .filter_map(json_to_owned)
            .collect())
    }

    async fn set_grant(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        client: &str,
        tiers: Vec<String>,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let parsed = parse_tiers(&tiers)?;
        let mut state = self.state.lock().await;
        require_manager(&state, &peer.client)?;
        state.grants.set(client, parsed);
        state.grants.save().map_err(fdo::Error::Failed)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn revoke_client(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        client: &str,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let mut state = self.state.lock().await;
        require_manager(&state, &peer.client)?;
        state.grants.revoke(client);
        state.grants.save().map_err(fdo::Error::Failed)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn get_audit(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        _since: u64,
        limit: u32,
    ) -> fdo::Result<Vec<OwnedValue>> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let state = self.state.lock().await;
        require_manager(&state, &peer.client)?;
        Ok(state
            .audit
            .iter()
            .rev()
            .take(limit as usize)
            .filter_map(json_to_owned)
            .collect())
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn completed(
        emitter: &SignalEmitter<'_>,
        operation: u64,
        ok: bool,
        result: &str,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn confirmation_requested(
        emitter: &SignalEmitter<'_>,
        details: HashMap<String, OwnedValue>,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn confirmation_ended(emitter: &SignalEmitter<'_>, operation: u64) -> zbus::Result<()>;
}

impl Agent {
    async fn run_shell_action(
        &self,
        emitter: &SignalEmitter<'_>,
        operation: u64,
        tier: RiskTier,
        client: &str,
        tool: &str,
        argv: &[String],
        literal: &[String],
    ) -> fdo::Result<()> {
        let line = match shell_ipc::build_wire_command(tool, argv, literal) {
            Ok(line) => line,
            Err(error) => {
                let result = json!({"trust": "trusted", "content": {"error": error}});
                Self::completed(emitter, operation, false, &result.to_string())
                    .await
                    .ok();
                return Ok(());
            }
        };
        let response = tokio::task::spawn_blocking(move || shell_ipc::exchange(&line, None))
            .await
            .map_err(|error| fdo::Error::Failed(error.to_string()))?;
        let response = match response {
            Ok(body) => body,
            Err(error) => {
                let result = json!({"trust": "trusted", "content": {"error": error}});
                Self::completed(emitter, operation, false, &result.to_string())
                    .await
                    .ok();
                return Ok(());
            }
        };
        let ok = !response.starts_with("error:");
        if ok && result_is_untrusted(tier) {
            self.state.lock().await.taint.mark_untrusted(client);
        }
        let trust = if ok && result_is_untrusted(tier) {
            "untrusted"
        } else {
            "trusted"
        };
        let content = action_content(&response);
        let result = json!({"trust": trust, "content": content});
        Self::completed(emitter, operation, ok, &result.to_string())
            .await
            .ok();
        Ok(())
    }

    async fn emit_confirmation(
        emitter: &SignalEmitter<'_>,
        operation: u64,
        pending: &PendingConfirm,
    ) -> fdo::Result<()> {
        let kind = match pending.kind {
            ConfirmKind::Action => "action",
            ConfirmKind::Grant => "grant",
        };
        let mut details = HashMap::new();
        details.insert("operation".into(), owned_u64(operation));
        details.insert("kind".into(), owned_str(kind));
        details.insert("client".into(), owned_str(&pending.client));
        details.insert("label".into(), owned_str(&pending.label));
        details.insert("tool".into(), owned_str(&pending.tool));
        details.insert("reason".into(), owned_str(&pending.reason));
        details.insert("tainted".into(), owned_bool(pending.tainted));
        details.insert(
            "args".into(),
            OwnedValue::try_from(Value::from(combined_args(&pending.argv, &pending.literal)))
                .expect("args"),
        );
        details.insert(
            "tiers".into(),
            OwnedValue::try_from(Value::from(pending.tiers.clone())).expect("tiers"),
        );
        Self::confirmation_requested(emitter, details)
            .await
            .map_err(|error| fdo::Error::Failed(error.to_string()))
    }
}
