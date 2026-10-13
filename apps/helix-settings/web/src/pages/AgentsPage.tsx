import { useCallback, useEffect, useState } from "react";
import { invoke, listen, type ClientView, type GrantsView } from "@helix/ui";
import type { PageHandle } from "./RowsPage";

type AgentsView = GrantsView & { status: string };

type CompositorSettingsView = {
  config: {
    niri_dir: string;
    main_config: string;
    setup_file: string;
    setup_included: boolean;
    snippet: { agent_desk_name: string };
    prefs: { agent_desk_placement: boolean };
    includes: { filename: string; label: string; about: string; included: boolean; exists: boolean }[];
  };
  caps: { liquid_glass: boolean; agent_desk: boolean };
  desk: {
    name: string;
    workspace_id: number | null;
    window_count: number;
    is_focused: boolean;
    placements_pending: number;
    available: boolean;
  };
};

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
  const [compositor, setCompositor] = useState<CompositorSettingsView | null>(null);
  const [deskName, setDeskName] = useState("");

  const refreshCompositor = useCallback(() => {
    invoke<CompositorSettingsView>("compositor_view").then((next) => {
      setCompositor(next);
      setDeskName(next.config.snippet.agent_desk_name);
    });
  }, []);

  useEffect(() => {
    const load = () => {
      invoke<AgentsView>("agents_view").then(setView);
      invoke<boolean>("approvals_view").then(setAllow);
      refreshCompositor();
    };
    register({ setRows: () => undefined, refresh: load });
    const unlisten = listen<AgentsView>("agents-view", setView);
    load();
    return () => { unlisten.then((stop) => stop()); };
  }, [register, refreshCompositor]);

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
      <h4>Agent desk &amp; compositor</h4>
      <div className="rows-card" data-test="compositor-card">
        <p className="setting-about note">
          Helix keeps compositor snippets in <code>helix-setup.kdl</code> (included from your niri <code>config.kdl</code>), separate from theme and layout includes Control Center writes. Agent-launched windows open on the named desk without switching your workspace.
        </p>
        {compositor && !compositor.caps.agent_desk ? (
          <p className="setting-about">This compositor build does not report agent-desk support yet. Install helix-niri to use a dedicated agent workspace.</p>
        ) : null}
        {compositor?.caps.agent_desk ? (
          <>
            <label className="sheet-field" htmlFor="agent-desk-name">Agent desk workspace name</label>
            <div className="setting-row">
              <input
                id="agent-desk-name"
                className="text-input"
                data-test="agent-desk-name"
                value={deskName}
                onChange={(event) => setDeskName(event.target.value)}
              />
              <button
                type="button"
                data-test="agent-desk-save"
                onClick={() =>
                  invoke("compositor_set_agent_desk_name", { name: deskName })
                    .then(() => refreshCompositor(), (error) => setNote(String(error)))
                }
              >
                Save to compositor
              </button>
            </div>
            {!compositor.config.setup_included ? (
              <p className="setting-about">Saving will add <code>include &quot;helix-setup.kdl&quot;</code> to your niri config.</p>
            ) : null}
            <div className="setting-row">
              <div className="setting-text">
                <div className="setting-title">Place agent app launches on the agent desk</div>
                <div className="setting-about">When off, Act and app-launch open on your current workspace (legacy behaviour).</div>
              </div>
              <input
                type="checkbox"
                className="switch"
                data-test="agent-desk-placement"
                checked={compositor.config.prefs.agent_desk_placement}
                onChange={(event) =>
                  invoke("compositor_set_placement", { on: event.target.checked }).then(() => refreshCompositor(), (error) => setNote(String(error)))
                }
              />
            </div>
            {compositor.desk.available ? (
              <p className="setting-about" data-test="agent-desk-status">
                {compositor.desk.workspace_id != null
                  ? `Desk “${compositor.desk.name}” · ${compositor.desk.window_count} window${compositor.desk.window_count === 1 ? "" : "s"}${compositor.desk.is_focused ? " · focused" : ""}.`
                  : `Desk “${compositor.desk.name}” will be created when needed.`}
              </p>
            ) : null}
            <div className="setting-row">
              <button type="button" data-test="agent-desk-focus" onClick={() => invoke("compositor_focus_agent_desk").catch((error) => setNote(String(error)))}>
                Show agent desk
              </button>
              <button type="button" data-test="open-compositor-setup" onClick={() => invoke("compositor_open_config", { which: "setup" }).catch((error) => setNote(String(error)))}>
                Open helix-setup.kdl
              </button>
              <button type="button" data-test="open-compositor-main" onClick={() => invoke("compositor_open_config", { which: "main" }).catch((error) => setNote(String(error)))}>
                Open niri config
              </button>
            </div>
          </>
        ) : null}
        {compositor?.config.includes.length ? (
          <>
            <h5 className="setting-subhead">Compositor includes</h5>
            <p className="setting-about note">
              Helix and Control Center write snippets next to <code>config.kdl</code>. Missing files appear after you change the related setting.
            </p>
            <div className="include-list" data-test="compositor-includes">
              {compositor.config.includes.map((row) => (
                <div className="include-row" key={row.filename} data-test={`include-${row.filename}`}>
                  <div className="include-text">
                    <div className="setting-title">{row.label} <code>{row.filename}</code></div>
                    <div className="setting-about">{row.about}</div>
                    <div className="setting-about include-status">
                      {row.included ? "Included from config.kdl" : "Not included"}
                      {row.exists ? (row.included ? "" : " · file on disk") : " · not created yet"}
                    </div>
                  </div>
                  <button
                    type="button"
                    className="mini"
                    disabled={!row.exists}
                    data-test={`open-include-${row.filename}`}
                    onClick={() =>
                      invoke("compositor_open_config", { which: row.filename }).catch((error) => setNote(String(error)))
                    }
                  >
                    Open
                  </button>
                </div>
              ))}
            </div>
          </>
        ) : null}
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
