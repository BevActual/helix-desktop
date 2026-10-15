use std::collections::HashMap;
use std::sync::Arc;

use helix_shell_core::{
    evaluate, result_is_untrusted, shell_ipc, AuditRecord, Decision, RiskTier, Verdict,
};
use serde_json::json;
use tokio::sync::Mutex;
use zbus::fdo;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Value};
use zbus::{interface, Connection};

use crate::agent_activity::AgentActivity;
use crate::agent_support::{
    action_result_json, append_audit, audit_record, denial_json, emit_activity_pending_changed,
    grant_change_record, is_helix_settings, require_registered_desktop,
};
use crate::dbus_value::{dict_entry, json_to_owned, owned_bool, owned_str, owned_u64};
use crate::peer::resolve_peer;
use crate::state::{ClientSession, ConfirmKind, PendingConfirm, State};

pub struct Agent {
    pub state: Arc<Mutex<State>>,
}

impl Agent {
    pub async fn notify_confirmation_ended(connection: &Connection, operation: u64) {
        if let Ok(iface) = connection
            .object_server()
            .interface::<_, Agent>(crate::agent_support::OBJECT_PATH)
            .await
        {
            Self::confirmation_ended(iface.signal_emitter(), operation)
                .await
                .ok();
        }
    }

    pub async fn notify_completed(connection: &Connection, operation: u64, ok: bool, result: &str) {
        if let Ok(iface) = connection
            .object_server()
            .interface::<_, Agent>(crate::agent_support::OBJECT_PATH)
            .await
        {
            Self::completed(iface.signal_emitter(), operation, ok, result)
                .await
                .ok();
        }
    }
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
        Err(fdo::Error::AccessDenied(
            "Only the registered manager may do that".into(),
        ))
    }
}

fn require_confirmer(state: &State, unique: &str) -> fdo::Result<()> {
    if state.confirmer.as_deref() == Some(unique) {
        Ok(())
    } else {
        Err(fdo::Error::AccessDenied(
            "Only the registered confirmer may answer confirmations".into(),
        ))
    }
}

fn parse_tiers(names: &[String]) -> fdo::Result<Vec<RiskTier>> {
    parse_grant_tiers(names, false)
}

fn parse_grant_tiers(names: &[String], allow_blocked_name: bool) -> fdo::Result<Vec<RiskTier>> {
    let mut out = Vec::new();
    for name in names {
        let Some(tier) = RiskTier::parse(name) else {
            return Err(fdo::Error::InvalidArgs(
                "unknown or ungrantable tier".into(),
            ));
        };
        if tier == RiskTier::Blocked && !allow_blocked_name {
            return Err(fdo::Error::InvalidArgs(
                "unknown or ungrantable tier".into(),
            ));
        }
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

async fn audit_and_notify(state: &mut State, record: AuditRecord) {
    append_audit(state, record).await;
    if let Some(conn) = &state.bus {
        AgentActivity::notify_changed(conn).await;
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
        session.insert("confirmer".into(), owned_bool(state.confirmer.is_some()));
        Ok(session)
    }

    async fn get_snapshot(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
    ) -> fdo::Result<HashMap<String, OwnedValue>> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let state = self.state.lock().await;
        let tiers: Vec<String> = state
            .grants
            .tiers_for(&peer.client)
            .into_iter()
            .map(|tier| tier.name().to_string())
            .collect();
        let mut map = HashMap::new();
        map.insert("implementation".into(), owned_str("rust"));
        map.insert("available".into(), owned_bool(true));
        map.insert("client".into(), owned_str(&peer.client));
        map.insert(
            "tiers".into(),
            OwnedValue::try_from(Value::from(tiers)).expect("tiers"),
        );
        map.insert("pending".into(), owned_u64(state.pending.len() as u64));
        map.insert("confirmer".into(), owned_bool(state.confirmer.is_some()));
        Ok(map)
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
        let wanted = parse_grant_tiers(&tiers, true)?;
        if wanted.is_empty() {
            return Err(fdo::Error::InvalidArgs("no grantable tiers".into()));
        }
        let mut state = self.state.lock().await;
        if !state.agent_enabled {
            let operation = state.alloc_operation();
            let result = denial_json("agent access is turned off");
            Self::completed(&emitter, operation, false, &result)
                .await
                .ok();
            return Ok(operation);
        }
        let held = state.grants.tiers_for(&peer.client);
        if wanted.iter().all(|tier| held.contains(tier)) {
            let operation = state.alloc_operation();
            let granted: Vec<String> = wanted.iter().map(|tier| tier.name().to_string()).collect();
            let result = json!({"trust": "trusted", "granted": granted});
            Self::completed(&emitter, operation, true, &result.to_string())
                .await
                .ok();
            return Ok(operation);
        }
        if state.confirmer.is_none() {
            let operation = state.alloc_operation();
            let result = denial_json("access needs approval and no approval dialog is available");
            Self::completed(&emitter, operation, false, &result)
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
            sender: sender.clone(),
            client: peer.client.clone(),
            kind: ConfirmKind::Grant,
            tool: String::new(),
            argv: Vec::new(),
            literal: Vec::new(),
            reason: String::new(),
            tiers: wanted.iter().map(|tier| tier.name().to_string()).collect(),
            tainted: state.taint.tainted(&peer.client),
            label,
            deadline: State::confirmation_deadline(),
        };
        state.pending.insert(operation, pending.clone());
        let bus = state.bus.clone();
        let desktop = state.desktop.clone();
        drop(state);
        if let Some(conn) = &bus {
            emit_activity_pending_changed(conn, desktop.as_deref()).await;
        }
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
            let operation = state.alloc_operation();
            let result = denial_json("agent access is turned off");
            Self::completed(&emitter, operation, false, &result)
                .await
                .ok();
            return Ok(operation);
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
                let pending = PendingConfirm {
                    sender: sender.clone(),
                    client: peer.client.clone(),
                    kind: ConfirmKind::Action,
                    tool: action.to_string(),
                    argv: argv.clone(),
                    literal: literal.clone(),
                    reason: reason.clone(),
                    tiers: vec![tier.name().into()],
                    tainted,
                    label: state
                        .clients
                        .get(&peer.client)
                        .map(|session| session.label.clone())
                        .unwrap_or_default(),
                    deadline: State::confirmation_deadline(),
                };
                audit_and_notify(
                    &mut state,
                    audit_record(&pending, tier, "denied", &deny_reason),
                )
                .await;
                let result = denial_json(&deny_reason);
                Self::completed(&emitter, operation, false, &result)
                    .await
                    .ok();
                Ok(operation)
            }
            Verdict::Allow => {
                let client = peer.client.clone();
                let tool = action.to_string();
                let label = state
                    .clients
                    .get(&peer.client)
                    .map(|session| session.label.clone())
                    .unwrap_or_default();
                let pending = PendingConfirm {
                    sender: sender.clone(),
                    client: client.clone(),
                    kind: ConfirmKind::Action,
                    tool: tool.clone(),
                    argv: argv.clone(),
                    literal: literal.clone(),
                    reason: reason.clone(),
                    tiers: vec![tier.name().into()],
                    tainted,
                    label,
                    deadline: State::confirmation_deadline(),
                };
                let decision = if tier == RiskTier::Confirm {
                    "confirmed"
                } else {
                    "allowed"
                };
                audit_and_notify(&mut state, audit_record(&pending, tier, decision, "")).await;
                drop(state);
                self.run_shell_action(&emitter, operation, tier, &client, &tool, &argv, &literal)
                    .await?;
                Ok(operation)
            }
            Verdict::NeedsConfirmation => {
                if state.confirmer.is_none() {
                    let pending = PendingConfirm {
                        sender: sender.clone(),
                        client: peer.client.clone(),
                        kind: ConfirmKind::Action,
                        tool: action.to_string(),
                        argv: argv.clone(),
                        literal: literal.clone(),
                        reason: reason.clone(),
                        tiers: vec![tier.name().into()],
                        tainted,
                        label: state
                            .clients
                            .get(&peer.client)
                            .map(|session| session.label.clone())
                            .unwrap_or_default(),
                        deadline: State::confirmation_deadline(),
                    };
                    audit_and_notify(
                        &mut state,
                        audit_record(&pending, tier, "denied", "no confirmer"),
                    )
                    .await;
                    let result = denial_json(
                        "this action needs confirmation and no confirmation dialog is available",
                    );
                    Self::completed(&emitter, operation, false, &result)
                        .await
                        .ok();
                    return Ok(operation);
                }
                let label = state
                    .clients
                    .get(&peer.client)
                    .map(|session| session.label.clone())
                    .unwrap_or_default();
                let pending = PendingConfirm {
                    sender: sender.clone(),
                    client: peer.client,
                    kind: ConfirmKind::Action,
                    tool: action.to_string(),
                    argv,
                    literal,
                    reason,
                    tiers: vec![tier.name().into()],
                    tainted,
                    label,
                    deadline: State::confirmation_deadline(),
                };
                state.pending.insert(operation, pending.clone());
                let bus = state.bus.clone();
                let desktop = state.desktop.clone();
                drop(state);
                if let Some(conn) = &bus {
                    emit_activity_pending_changed(conn, desktop.as_deref()).await;
                }
                Self::emit_confirmation(&emitter, operation, &pending).await?;
                Ok(operation)
            }
        }
    }

    async fn cancel(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        operation: u64,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let peer = resolve_peer(connection, &sender).await?;
        let mut state = self.state.lock().await;
        let Some(pending) = state.pending.remove(&operation) else {
            return Err(fdo::Error::InvalidArgs("unknown operation".into()));
        };
        if pending.client != peer.client {
            return Err(fdo::Error::InvalidArgs(
                "There is no cancellable operation with that id".into(),
            ));
        }
        audit_and_notify(
            &mut state,
            audit_record(&pending, RiskTier::Confirm, "cancelled", "stopped"),
        )
        .await;
        drop(state);
        Self::confirmation_ended(&emitter, operation).await.ok();
        let result = denial_json("cancelled");
        Self::completed(&emitter, operation, false, &result)
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
        require_registered_desktop(&state, &sender)?;
        if let Some(existing) = &state.confirmer {
            if existing != &peer.unique_name {
                return Err(fdo::Error::Failed(
                    "A confirmer is already registered".into(),
                ));
            }
        }
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
            audit_and_notify(
                &mut state,
                audit_record(&pending, RiskTier::Confirm, "rejected", ""),
            )
            .await;
            drop(state);
            let result = denial_json("the user declined");
            Self::completed(&emitter, operation, false, &result)
                .await
                .ok();
            return Ok(());
        }
        match pending.kind {
            ConfirmKind::Grant => {
                let mut tiers = parse_grant_tiers(&pending.tiers, true)?;
                if let Some(value) = options.get("tiers") {
                    let names = string_list(value);
                    if !names.is_empty() {
                        tiers = parse_grant_tiers(&names, true)?;
                    }
                }
                let before = state.grants.tiers_for(&pending.client);
                let mut after = before;
                for tier in tiers {
                    after.insert(tier);
                }
                state.grants.set(&pending.client, after.iter().copied());
                if let Err(error) = state.grants.save() {
                    tracing::warn!(%error, "grants save failed");
                    let result = denial_json("the grant could not be saved");
                    Self::completed(&emitter, operation, false, &result)
                        .await
                        .ok();
                    return Ok(());
                }
                // Approving a grant is not a review of any later action: taint stays until an action is confirmed.
                let granted: Vec<String> =
                    after.iter().map(|tier| tier.name().to_string()).collect();
                audit_and_notify(
                    &mut state,
                    audit_record(&pending, RiskTier::Confirm, "granted", &granted.join(",")),
                )
                .await;
                let result = json!({"trust": "trusted", "granted": granted});
                drop(state);
                Self::completed(&emitter, operation, true, &result.to_string())
                    .await
                    .ok();
            }
            ConfirmKind::Action => {
                state.taint.clear(&pending.client);
                let tier = parse_tiers(&pending.tiers)?
                    .into_iter()
                    .next()
                    .unwrap_or(RiskTier::Confirm);
                audit_and_notify(&mut state, audit_record(&pending, tier, "confirmed", "")).await;
                let client = pending.client.clone();
                let tool = pending.tool.clone();
                let argv = pending.argv.clone();
                let literal = pending.literal.clone();
                drop(state);
                self.run_shell_action(&emitter, operation, tier, &client, &tool, &argv, &literal)
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
        if !is_helix_settings(&peer.exe) {
            return Err(fdo::Error::InvalidArgs(
                "Only Helix Settings may manage agent access".into(),
            ));
        }
        let mut state = self.state.lock().await;
        if let Some(existing) = &state.manager {
            if existing != &peer.client {
                return Err(fdo::Error::Failed("A manager is already registered".into()));
            }
        }
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
        let parsed = parse_grant_tiers(&tiers, false)?;
        let mut state = self.state.lock().await;
        require_manager(&state, &peer.client)?;
        let before = state.grants.tiers_for(client);
        state.grants.set(client, parsed.iter().copied());
        if let Err(error) = state.grants.save() {
            state.grants.set(client, before.iter().copied());
            return Err(fdo::Error::Failed(error));
        }
        let tier_names: Vec<String> = tiers.clone();
        append_audit(&mut state, grant_change_record(client, "set", &tier_names)).await;
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
        append_audit(&mut state, grant_change_record(client, "revoked", &[])).await;
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
            .read_recent(_since, limit)
            .iter()
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
    #[allow(clippy::too_many_arguments)]
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
                let result = action_result_json("trusted", false, &error);
                Self::completed(emitter, operation, false, &result)
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
                let result = action_result_json("trusted", false, &error);
                Self::completed(emitter, operation, false, &result)
                    .await
                    .ok();
                return Ok(());
            }
        };
        let body = response.trim_end_matches('\n').to_string();
        let ok = !body.starts_with("error:");
        if ok && result_is_untrusted(tier) {
            self.state.lock().await.taint.mark_untrusted(client);
        }
        let trust = if ok && result_is_untrusted(tier) {
            "untrusted"
        } else {
            "trusted"
        };
        let result = action_result_json(trust, ok, &body);
        Self::completed(emitter, operation, ok, &result).await.ok();
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
