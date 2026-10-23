import { useCallback, useEffect, useState } from "react";
import { invoke } from "@helix/ui";

export interface AgentView { id: string; title: string; command_line: string; found: boolean; trusted: boolean; custom: boolean; }
interface TestResult { name: string; version: string; sign_in: string[]; capabilities: { loadSession?: boolean } & Record<string, unknown>; }

/** Chooses who works in this chat's project: Mobius itself, or an agent the person has allowed. Shown in the Code stage. */
export function AgentChooser({ report, enabled }: { report: (error: unknown) => void; enabled: boolean }) {
  const [agents, setAgents] = useState<AgentView[]>([]);
  const [selected, setSelected] = useState<string | null>(null);
  const refresh = useCallback(() => {
    invoke<AgentView[]>("acp_agents").then(setAgents, () => undefined);
    invoke<string | null>("acp_selected").then(setSelected, () => undefined);
  }, []);
  useEffect(() => {
    refresh();
    const tauri = (window as unknown as { __TAURI__?: { event?: { listen: (name: string, cb: (e: { payload: string | null }) => void) => Promise<() => void> } } }).__TAURI__;
    let stop: (() => void) | undefined;
    tauri?.event?.listen("acp-changed", (event) => setSelected(event.payload)).then((off) => { stop = off; });
    return () => stop?.();
  }, [refresh]);
  const usable = agents.filter((a) => a.found && a.trusted);
  if (!enabled) return null;
  if (!usable.length) {
    return <button type="button" className="mini" data-test="code-agent-setup" title="Let another coding agent work in this project" onClick={() => window.dispatchEvent(new CustomEvent("mobius-open-models", { detail: { section: "agents" } }))}>Use an agent…</button>;
  }
  const choose = (id: string | null) => invoke("acp_select", { id }).catch((error) => { report(error); refresh(); });
  return (
    <div className="agent-choose" role="radiogroup" aria-label="Who works in this project">
      <span>Work with</span>
      <div className="route-seg">
        <button type="button" role="radio" aria-checked={!selected} className={!selected ? "on" : ""} data-test="code-agent-mobius" onClick={() => choose(null)}>Mobius</button>
        {usable.map((a) => <button key={a.id} type="button" role="radio" aria-checked={selected === a.id} className={selected === a.id ? "on" : ""} data-test={`code-agent-${a.id}`} onClick={() => choose(a.id)}>{a.title}</button>)}
      </div>
    </div>
  );
}

/** Settings → Agents: which coding agents Mobius can drive, and the person's say-so for each. */
export function AgentsPanel({ report }: { report: (error: unknown) => void }) {
  const [agents, setAgents] = useState<AgentView[]>([]);
  const [tests, setTests] = useState<Record<string, TestResult | string | "running">>({});
  const [form, setForm] = useState({ title: "", command: "", args: "" });
  const refresh = useCallback(() => { invoke<AgentView[]>("acp_agents").then(setAgents, () => undefined); }, []);
  useEffect(refresh, [refresh]);

  const test = (id: string) => {
    setTests((old) => ({ ...old, [id]: "running" }));
    invoke<TestResult>("acp_test", { id }).then((r) => setTests((old) => ({ ...old, [id]: r })), (e) => setTests((old) => ({ ...old, [id]: String(e) })));
  };
  const add = () => {
    const args = form.args.trim() ? form.args.trim().split(/\s+/) : [];
    invoke("acp_add_agent", { title: form.title, command: form.command, args }).then(() => { setForm({ title: "", command: "", args: "" }); refresh(); }, report);
  };

  return (
    <>
      <h3 className="models-h3">Coding agents</h3>
      <p className="models-lead">In Code, Mobius can hand the project to another coding agent. That agent keeps its own sign-in, which Mobius never reads. Every file it wants to write is a change you apply or skip, and every command waits until you say so, then runs in the project sandbox.</p>
      <div className="set-offer" data-test="agents-warning">
        <div className="set-offer-text"><div><strong>An agent is a program running as you</strong></div><span>Mobius controls what the agent asks Mobius for, but cannot stop an agent that acts on its own. Turn on only agents you trust.</span></div>
      </div>
      <div className="set-list" data-test="agents">
        {agents.map((a) => {
          const result = tests[a.id];
          return (
            <div key={a.id} className="set-row" data-test={`agent-${a.id}`}>
              <div className="set-row-main">
                <div className="set-row-text">
                  <div className="set-row-title">{a.title} <span className={`lib-tag${a.found ? " fit-gpu" : ""}`}>{a.found ? "Installed" : "Not found"}</span></div>
                  <div className="set-row-note"><code>{a.command_line}</code></div>
                </div>
                <div className="kn-actions">
                  <button type="button" data-test={`agent-test-${a.id}`} disabled={!a.found || result === "running"} onClick={() => test(a.id)}>{result === "running" ? "Testing…" : "Test"}</button>
                  {a.custom ? <button type="button" className="lib-remove" data-test={`agent-remove-${a.id}`} onClick={() => { if (window.confirm(`Remove ${a.title} from the list?`)) invoke("acp_remove_agent", { id: a.id }).then(refresh, report); }}>Remove</button> : null}
                </div>
              </div>
              <label className="sheet-check set-check">
                <input type="checkbox" data-test={`agent-trust-${a.id}`} checked={a.trusted} disabled={!a.found}
                  onChange={(e) => { const on = e.target.checked; invoke("acp_trust", { id: a.id, on }).then(refresh, report); }} />
                <span><strong>{a.title} may run on this computer</strong><br /><span className="set-hint">It can then be chosen in a project's Code view.</span></span>
              </label>
              {result && result !== "running" ? (
                typeof result === "string"
                  ? <p className="lib-error" data-test={`agent-test-error-${a.id}`}>{result}</p>
                  : <p className="set-hint" data-test={`agent-test-ok-${a.id}`}>It answered: {result.name || a.title} {result.version}. {result.sign_in.length ? `Sign-in methods it lists: ${result.sign_in.join(", ")}.` : "It lists no sign-in method."} {result.capabilities?.loadSession ? "It can resume earlier sessions." : ""}</p>
              ) : null}
            </div>
          );
        })}
      </div>
      <h4 className="models-h4">Add one by its command</h4>
      <div className="agent-add">
        <input data-test="agent-add-title" type="text" placeholder="Name" value={form.title} onChange={(e) => setForm({ ...form, title: e.target.value })} />
        <input data-test="agent-add-command" type="text" spellCheck={false} placeholder="Command, e.g. my-agent" value={form.command} onChange={(e) => setForm({ ...form, command: e.target.value })} />
        <input data-test="agent-add-args" type="text" spellCheck={false} placeholder="Arguments, e.g. acp" value={form.args} onChange={(e) => setForm({ ...form, args: e.target.value })} />
        <button type="button" data-test="agent-add" disabled={!form.title.trim() || !form.command.trim()} onClick={add}>Add</button>
      </div>
      <p className="set-hint">The command starts the agent speaking the Agent Client Protocol on its standard input and output. Nothing is downloaded or installed for you.</p>
    </>
  );
}
