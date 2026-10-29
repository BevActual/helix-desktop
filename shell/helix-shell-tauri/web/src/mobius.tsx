import { useCallback, useEffect, useRef, useState } from "react";
import { invoke, listen } from "@helix/ui";
import { Icon } from "./icon";
import type { ActivityView, AssistantView, TalkView } from "./types";

export type MobiusMode = "talk" | "act" | "agents";

export interface MobiusHandle {
  /** The dropdown was opened (or its door changed): show that pane and focus its field. */
  setMode: (mode: string) => void;
}

const SUGGESTIONS = ["What can you help with?", "Dim the screen a bit", "Summarise what Agents are for", "How do I switch models?"];

const door = (mode: string): MobiusMode => (mode === "act" || mode === "agents" ? mode : "talk");

function Row({ what, sub, flag, children }: { what: string; sub?: string; flag?: string; children?: React.ReactNode }) {
  return (
    <div className="arow">
      <div className="main">
        <div className="what">{what}</div>
        {sub ? <div className="sub">{sub}</div> : null}
      </div>
      {flag ? <span className="flag">{flag}</span> : null}
      {children}
    </div>
  );
}

function Talk({ active }: { active: boolean }) {
  const [view, setView] = useState<TalkView | null>(null);
  const [error, setError] = useState("");
  const [text, setText] = useState("");
  const field = useRef<HTMLInputElement>(null);
  const thread = useRef<HTMLDivElement>(null);
  const read = useCallback(() => invoke<TalkView>("mobius_talk_view").then(setView, () => undefined), []);
  useEffect(() => {
    const stop = listen<TalkView>("mobius-talk-view", setView);
    read();
    return () => { stop.then((fn) => fn()); };
  }, [read]);
  useEffect(() => {
    if (active) field.current?.focus();
  }, [active]);
  useEffect(() => {
    if (thread.current) thread.current.scrollTop = thread.current.scrollHeight;
  }, [view]);

  const history = view?.history ?? [];
  const streaming = view?.phase === "streaming" && !!view.reply;
  const hasHistory = history.length > 0 || streaming;
  const busy = view?.phase === "thinking" || view?.phase === "streaming";
  const send = (message: string) => {
    setText("");
    setError("");
    invoke("mobius_talk", { text: message }).catch((e) => setError(String(e)));
  };
  const status = view?.phase === "thinking" ? "Thinking…" : view?.phase === "failed" ? view.error || "Something went wrong." : error;
  const fail = (e: unknown) => setError(String(e));

  return (
    <div id="mobius-talk" className="mobius-pane" data-test="mobius-talk-pane" hidden={!active}>
      <div id="talk-empty" className="talk-empty" data-test="talk-empty" hidden={hasHistory}>
        <div className="talk-empty-title">Ask Mobius</div>
        <div className="talk-empty-note">Quick answers here. Continue in the full window anytime.</div>
        <div id="talk-suggestions" className="talk-suggestions" data-test="talk-suggestions">
          {SUGGESTIONS.map((suggestion, index) => (
            <button key={suggestion} type="button" data-test={`talk-suggestion-${index}`} onClick={() => send(suggestion)}>
              {suggestion}
            </button>
          ))}
        </div>
      </div>
      <div id="talk-thread" data-test="talk-thread" ref={thread}>
        {history.map((line, index) => (
          <div key={index} className={`talk-line ${line.role === "user" ? "user" : "assistant"}`}>{line.text}</div>
        ))}
        {streaming ? <div className="talk-line assistant">{view?.reply}</div> : null}
      </div>
      <div id="talk-status" data-test="talk-status">{streaming ? "" : status}</div>
      <div className="talk-compose">
        <input
          id="talk-q"
          data-test="talk-q"
          ref={field}
          placeholder="Ask Mobius…"
          autoComplete="off"
          value={text}
          disabled={busy}
          onChange={(event) => setText(event.target.value)}
          onKeyDown={(event) => {
            if (event.key !== "Enter") return;
            event.preventDefault();
            send(text);
          }}
        />
        <button id="talk-send" className="primary" data-test="talk-send" type="button" title="Ask" aria-label="Ask" disabled={busy} onClick={() => send(text)}>Ask</button>
      </div>
      <div className="mobius-talk-actions">
        <button id="talk-stop" data-test="talk-stop" type="button" hidden={!busy} onClick={() => invoke("mobius_talk_cancel").then(read)}>Stop</button>
        <button id="talk-clear" data-test="talk-clear" type="button" hidden={busy || !hasHistory} onClick={() => invoke("mobius_talk_clear").then(read)}>Clear</button>
        <button id="talk-act" data-test="talk-act" type="button" title="Act on the last question" hidden={busy || !history.some((line) => line.role === "user")} onClick={() => invoke("mobius_act_from_talk", { text: null }).catch(fail)}>Act</button>
        <button id="talk-open-full" data-test="talk-open-full" type="button" onClick={() => invoke(hasHistory ? "mobius_continue_full" : "open_mobius").catch(fail)}>
          {hasHistory ? "Continue in Mobius" : "Open full Mobius"}
        </button>
      </div>
    </div>
  );
}

function Act({ active, resetKey }: { active: boolean; resetKey: number }) {
  const [view, setView] = useState<AssistantView | null>(null);
  const [text, setText] = useState("");
  const [undoError, setUndoError] = useState("");
  const field = useRef<HTMLInputElement>(null);
  useEffect(() => {
    const stop = listen<AssistantView>("assistant-view", (next) => {
      setUndoError("");
      setView(next);
    });
    return () => { stop.then((fn) => fn()); };
  }, []);
  useEffect(() => {
    if (resetKey) setText(""); // opening on Act starts a fresh request
  }, [resetKey]);
  const phase = view?.phase ?? "idle";
  useEffect(() => {
    if (active && (phase === "idle" || phase === "result")) field.current?.focus();
  }, [active, phase, resetKey]);

  let body = "";
  let reason = "";
  let note = "";
  if (view) {
    if (phase === "thinking") body = "Thinking…";
    else if (phase === "acting") body = "Doing it…";
    else if (phase === "confirm") {
      body = "About to: " + view.description;
      if (view.reason) reason = "The assistant says: “" + view.reason + "”";
    } else if (phase === "result") {
      body = view.text || "Done.";
      if (view.unverified) note = "This came from outside the assistant.";
    } else if (view.failed) body = view.text;
  }
  const canUndo = phase === "result" && !view?.failed && !!view?.undoable && (view?.task ?? 0) > 0;
  const undo = () => {
    if (!view) return;
    setUndoError("");
    invoke<AssistantView>("assistant_undo", { task: view.task }).then(setView).catch((error) => {
      setUndoError(String(error));
      setView((current) => (current ? { ...current, undoable: false } : current));
    });
  };
  return (
    <div id="mobius-act" className="mobius-pane" data-test="mobius-act-pane" hidden={!active}>
      <p className="ai-rail-note">Change the desktop. Confirm here when Mobius asks.</p>
      <input
        id="q"
        ref={field}
        placeholder="What should Mobius change?"
        autoComplete="off"
        value={text}
        onChange={(event) => setText(event.target.value)}
        onKeyDown={(event) => {
          if (event.key !== "Enter") return;
          event.preventDefault();
          const request = text;
          setText("");
          invoke<AssistantView>("assistant_submit", { request }).then(setView);
        }}
      />
      <div id="body">{body}</div>
      <div id="reason">{reason}</div>
      <div id="note">{undoError || note}</div>
      <div id="buttons" style={{ display: phase === "confirm" || canUndo ? "flex" : "none" }}>
        {phase === "confirm" ? <button id="cancel" onClick={() => invoke<AssistantView>("assistant_cancel").then(setView)}>Cancel</button> : null}
        {phase === "confirm" ? <button id="doit" className="primary" disabled={!view?.armed} onClick={() => invoke<AssistantView>("assistant_confirm").then(setView)}>Do it</button> : null}
        {canUndo ? <button id="undo" type="button" data-test="act-undo" onClick={undo}>Undo</button> : null}
      </div>
    </div>
  );
}

interface AgentSession { session: string; agent: string; title: string; profile: string; project: string; boxed: boolean; minutes: number; pid: number }

function Agents({ active, resetKey }: { active: boolean; resetKey: number }) {
  const [view, setView] = useState<ActivityView | null>(null);
  const [sessions, setSessions] = useState<AgentSession[]>([]);
  const readSessions = useCallback(() => invoke<AgentSession[]>("agent_sessions").then(setSessions, () => undefined), []);
  // Coding agents started with helix-agents change as they start and end: look while the pane is open.
  useEffect(() => {
    if (!active) return;
    readSessions();
    const timer = setInterval(readSessions, 3000);
    return () => clearInterval(timer);
  }, [active, resetKey, readSessions]);
  const read = useCallback(() => invoke<ActivityView>("activity_view").then(setView, () => undefined), []);
  useEffect(() => {
    const stop = listen<ActivityView>("activity-view", setView);
    read();
    return () => { stop.then((fn) => fn()); };
  }, [read]);
  useEffect(() => {
    if (active) read();
  }, [active, resetKey, read]);
  const v = view ?? { available: false, enabled: false, busy: false, pending: [], grants: [], history: [] };
  const state = !v.available
    ? "Agent activity is shown by the registered shell."
    : v.enabled ? "Agents may ask to act. Nothing runs without the checks in Settings." : "Agents are off. Every request is refused until you turn them on.";
  return (
    <div id="mobius-agents" className="mobius-pane" data-test="mobius-agents-pane" hidden={!active}>
      <p className="ai-rail-note">See and stop what Mobius can do on the desktop.</p>
      <div id="agent-toolbar">
        <button id="agent-stopall" data-test="agent-stopall" disabled={!v.available || !v.busy} onClick={() => invoke("activity_stop_all")}>Stop all</button>
        <button id="agent-toggle" data-test="agent-toggle" disabled={!v.available} onClick={() => invoke("activity_set_enabled", { enabled: !v.enabled })}>{v.enabled ? "Turn off" : "Turn on"}</button>
      </div>
      <div id="agent-state">{state}</div>
      <h4>Running now</h4>
      <div id="agent-sessions" data-test="agent-sessions">
        {sessions.map((row) => (
          <Row key={row.session} what={`${row.title} · ${row.profile}${row.boxed ? " · in the box" : " · no box"}`} sub={`${row.project} · ${row.minutes < 1 ? "just started" : `${row.minutes} min`}`}>
            <button data-test={`session-stop-${row.agent}`} onClick={() => invoke("agent_session_stop", { session: row.session }).then(readSessions, readSessions)}>Stop</button>
          </Row>
        ))}
        {sessions.length ? null : <Row what="No coding agent is running." sub="Start one with the agent key, or helix-claude, helix-opencode, helix-codex." />}
      </div>
      <h4>Waiting for you</h4>
      <div id="agent-pending">
        {v.pending.map((row, index) => (
          <Row key={row.operation} what={`${row.who} · ${row.grant ? "wants access" : row.tool}`} sub={row.reason} flag={row.tainted ? "untrusted input" : ""}>
            <button data-test={`stop-${index}`} onClick={() => invoke("activity_stop", { operation: row.operation })}>Stop</button>
          </Row>
        ))}
        {v.pending.length ? null : <Row what="Nothing is waiting." />}
      </div>
      <h4>Access</h4>
      <div id="agent-grants">
        {v.grants.map((row, index) => (
          <Row key={row.id} what={row.who} sub={row.tiers.length ? row.tiers.join(", ") : "no access"}>
            <button data-test={`revoke-${index}`} onClick={() => invoke("activity_revoke", { id: row.id })}>Revoke</button>
          </Row>
        ))}
        {v.grants.length ? null : <Row what="No agent has access." />}
      </div>
      <h4>Recent</h4>
      <div id="agent-history">
        {v.history.map((row, index) => (
          <Row
            key={index}
            what={row.tool + (row.args ? " " + row.args : "")}
            sub={`${row.who} · ${row.time} · ${row.tier} · ${row.decision}${row.outcome ? " · " + row.outcome : ""}`}
            flag={row.tainted ? "untrusted input" : ""}
          />
        ))}
        {v.history.length ? null : <Row what="Nothing yet." />}
      </div>
    </div>
  );
}

/** The Mobius dropdown: Talk, Act and Agents. Rust owns the conversation, the pending request and the audit; this draws views and sends
 * back the same commands (a typed request, the one confirm, the operation or grant id it was given). */
export function Mobius({ register }: { register: (handle: MobiusHandle) => void }) {
  const [mode, setMode] = useState<MobiusMode>("talk");
  const [opened, setOpened] = useState(0);
  useEffect(() => {
    register({
      setMode: (next) => {
        setMode(door(next));
        setOpened((n) => n + 1);
      },
    });
  }, [register]);
  const tabs: [MobiusMode, string, string][] = [
    ["talk", "Talk", "Talk"],
    ["act", "Act", "Act on the desktop"],
    ["agents", "Agents", "Agent activity"],
  ];
  return (
    <>
      <div id="panel-head">
        <Icon name="sparkle" />
        <h3>Mobius</h3>
        <nav className="ai-rail" aria-label="Mobius">
          {tabs.map(([value, label, title]) => (
            <button
              key={value}
              type="button"
              className={`ai-rail-btn${value === mode ? " active" : ""}`}
              data-test={`mobius-${value}`}
              data-mobius-mode={value}
              title={title}
              aria-current={value === mode ? "page" : undefined}
              onClick={() => invoke("mobius_set_mode", { mode: value })}
            >
              {label}
            </button>
          ))}
        </nav>
      </div>
      <Talk active={mode === "talk"} />
      <Act active={mode === "act"} resetKey={mode === "act" ? opened : 0} />
      <Agents active={mode === "agents"} resetKey={opened} />
    </>
  );
}
