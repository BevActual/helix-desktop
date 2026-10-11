import { useEffect, useState } from "react";
import { invoke, listen, type ClientView, type GrantsView } from "@helix/ui";
import type { PageHandle } from "./RowsPage";

type AgentsView = GrantsView & { status: string };

function Client({ client, index }: { client: ClientView; index: number }) {
  const stats = `${client.requests} requests${client.denied ? ` · ${client.denied} refused` : ""}${client.last ? ` · last ${client.last}` : ""}`;
  return (
    <div className="agent-card" data-test={`agent-${index}`}>
      <div className="agent-head">
        <div className="agent-who">
          <div className="agent-name">{client.name}</div>
          <div className="agent-identity">{client.identity}</div>
          <div className="agent-stats">{stats}</div>
        </div>
        <button className="danger" data-test={`revoke-${index}`} onClick={() => invoke("agents_revoke", { client: client.id })}>Revoke</button>
      </div>
      {client.tiers.map((tier) => (
        <label className="tier-row" key={tier.name}>
          <input
            type="checkbox"
            className="switch"
            checked={tier.on}
            data-test={`tier-${index}-${tier.name}`}
            onChange={(event) => invoke("agents_set_tier", { client: client.id, tier: tier.name, on: event.target.checked })}
          />
          <div className="tier-text">
            <div className="tier-name">{tier.name}</div>
            <div className="tier-about">{tier.about}</div>
          </div>
        </label>
      ))}
    </div>
  );
}

export function AgentsPage({ register }: { register: (handle: PageHandle) => void }) {
  const [view, setView] = useState<AgentsView | null>(null);
  const [allow, setAllow] = useState<boolean | null>(null);
  const [note, setNote] = useState("");

  useEffect(() => {
    const load = () => {
      invoke<AgentsView>("agents_view").then(setView);
      invoke<boolean>("approvals_view").then(setAllow);
    };
    register({ setRows: () => undefined, refresh: load });
    const unlisten = listen<AgentsView>("agents-view", setView);
    load();
    return () => { unlisten.then((stop) => stop()); };
  }, [register]);

  const status =
    !view || view.status === "connected" ? ""
    : view.status === "denied" ? "This copy of Settings is not the installed one, so it cannot manage agent access."
    : "The desktop service is not running.";

  return (
    <div data-test="react-page">
      {status || note ? <div id="agents-note">{note || status}</div> : null}
      <h4>Models &amp; routing</h4>
      <div className="rows-card" id="ai-routing-card">
        <p id="ai-routing-note">Personas, local model slots, and LAN endpoints are managed in Mobius. Agent grants stay here.</p>
        <button
          data-test="open-mobius"
          type="button"
          onClick={() => invoke("open_mobius").then(() => setNote(""), (error) => setNote(String(error)))}
        >
          Open Mobius
        </button>
      </div>
      <h4>Approvals</h4>
      <div className="rows-card">
        {allow !== null ? (
          <div className="setting-row">
            <div className="setting-text">
              <div className="setting-title">Approve on a desktop that cannot protect the dialog</div>
              <div className="setting-about">
                {allow
                  ? "On. Other programs on this desktop might be able to click Allow for you or read the dialog. Actions that need confirmation are shown anyway."
                  : "Off. Where the desktop cannot protect the approval dialog from other programs, actions that need confirmation are refused instead of shown."}
              </div>
            </div>
            <input
              type="checkbox"
              className="switch"
              checked={allow}
              data-test="approvals-unprotected"
              onChange={(event) => invoke<boolean>("approvals_set", { allow: event.target.checked }).then(setAllow)}
            />
          </div>
        ) : null}
      </div>
      <h4>Applications with access</h4>
      <div>
        {view?.clients.map((client, index) => <Client key={client.id} client={client} index={index} />)}
        {view && view.status === "connected" && view.clients.length === 0 ? <div className="empty">No application has been given access.</div> : null}
      </div>
      <h4>Recent activity</h4>
      <div>
        {view?.history.map((row, index) => (
          <div className="hist-row" key={index}>
            <div className="hist-main">
              <div className="hist-what">{row.tool + (row.args ? ` ${row.args}` : "")}</div>
              <div className="hist-sub">{`${row.who} · ${row.time} · ${row.tier} · ${row.decision}${row.outcome ? ` · ${row.outcome}` : ""}`}</div>
            </div>
            {row.tainted ? <span className="hist-flag">untrusted input</span> : null}
          </div>
        ))}
        {view && view.history.length === 0 ? <div className="empty">Nothing yet.</div> : null}
      </div>
    </div>
  );
}
