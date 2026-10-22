import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** The Agents home: every task an agent has worked on, in one page. What needs you comes first; a task shows its answer, the changes it made (to
 * apply or discard) and each step it took, with where each step ran. Tasks start here, or from Tools in a chat. */
interface Step { n: number; tool: string; input: string; outcome: string; ok: boolean; place: string; at: number; would_ask?: string }
interface Policy { name: string; folder: string; cloud: string; display: boolean; mode?: string; network?: boolean }
type TaskState = { state: "queued" | "running" | "stopped" } | { state: "waiting"; why: string } | { state: "done"; summary: string } | { state: "failed"; error: string };
interface AgentTask { id: string; goal: string; result_chat?: string; workdir?: string; dirty?: boolean; review?: string; policy: Policy; state: TaskState; steps: Step[]; created: number; updated: number }
export interface AgentTasksHandle { open: () => void; close: () => void }

const ago = (secs: number) => {
  const d = Math.max(0, Date.now() / 1000 - secs);
  return d < 90 ? "just now" : d < 5400 ? `${Math.round(d / 60)} min ago` : d < 129600 ? `${Math.round(d / 3600)} h ago` : `${Math.round(d / 86400)} days ago`;
};
const label = (t: AgentTask) => t.state.state === "running" ? "Working" : t.state.state === "waiting" ? "Needs you" : t.state.state === "done" ? "Done" : t.state.state === "failed" ? "Failed" : t.state.state === "stopped" ? "Stopped" : "Starting";
const TOOLS: Record<string, string> = { web_search: "Searched the web", web_read: "Read a page", terminal: "Ran a command", edit_file: "Edited a file", display_launch: "Opened an app", display_look: "Looked at the screen", display_click: "Clicked", display_type: "Typed", display_key: "Pressed a key", display_scroll: "Scrolled", display_help: "Asked for help" };
const group = (t: AgentTask) => t.state.state === "waiting" ? 0 : t.state.state === "running" || t.state.state === "queued" ? 1 : t.review === "pending" ? 2 : 3;
const GROUPS = ["Needs you", "Working", "Ready to review", "Earlier"];

/** A unified diff, line by line. */
function DiffView({ text }: { text: string }) {
  const lines = text.replace(/\n$/, "").split("\n");
  const kind = (line: string) => line.startsWith("@@") || line.startsWith("diff --git") ? "diff-head" : line.startsWith("+") && !line.startsWith("+++") ? "diff-add" : line.startsWith("-") && !line.startsWith("---") ? "diff-del" : "";
  return <pre className="diff" data-test="agent-task-diff">{lines.map((line, i) => <span key={i} className={kind(line)}>{line || " "}</span>)}</pre>;
}

export function AgentTasks({ register, visible, onOpenChat }: { register: (handle: AgentTasksHandle) => void; visible: (shown: boolean) => void; onOpenChat: (id: string) => void }) {
  const [shown, setShown] = useState(false);
  const [tasks, setTasks] = useState<AgentTask[]>([]);
  const [selected, setSelected] = useState("");
  const [error, setError] = useState("");
  const [goal, setGoal] = useState("");
  const [folder, setFolder] = useState("");
  const [policy, setPolicy] = useState("");
  const [practice, setPractice] = useState(false);
  const [network, setNetwork] = useState(false);
  const [onCopy, setOnCopy] = useState(true);
  const [busy, setBusy] = useState(false);
  const goalBox = useRef<HTMLTextAreaElement>(null);
  const [diffText, setDiffText] = useState("");
  const [diffOf, setDiffOf] = useState("");
  const load = useCallback(() => invoke<AgentTask[]>("agent_tasks").then(setTasks, () => setTasks([])), []);
  useEffect(() => { register({ open: () => { setError(""); setShown(true); visible(true); load(); }, close: () => { setShown(false); visible(false); } }); }, [register, visible, load]);
  useEffect(() => { if (!shown) return; const timer = setInterval(load, 2500); return () => clearInterval(timer); }, [shown, load]);
  // Nothing chosen: show what needs you, else the newest, else the empty page with the form.
  useEffect(() => {
    if (!shown || selected) return;
    const first = [...tasks].sort((a, b) => group(a) - group(b) || b.updated - a.updated)[0];
    setSelected(first ? first.id : "new");
  }, [shown, tasks, selected]);
  const task = tasks.find((t) => t.id === selected);
  // Choosing New task puts the cursor in the goal, whatever had the focus.
  useEffect(() => { if (shown && (selected === "new" || !task)) goalBox.current?.focus(); }, [shown, selected, !task]);
  // A task with changes waiting shows them straight away.
  useEffect(() => {
    if (!task || task.review !== "pending" || task.state.state === "running" || task.state.state === "waiting") { setDiffOf(""); return; }
    if (diffOf === task.id) return;
    invoke<string>("agent_task_diff", { id: task.id }).then((text) => { setDiffText(text); setDiffOf(task.id); }, (reason) => { setError(String(reason)); setDiffOf(task.id); setDiffText(""); });
  }, [task?.id, task?.review, task?.state.state, diffOf]);
  const call = async (command: string, args: Record<string, unknown>) => { setError(""); try { const result = await invoke<unknown>(command, args); if (Array.isArray(result)) setTasks(result as AgentTask[]); else await load(); return result; } catch (reason) { setError(String(reason)); return undefined; } };
  const choose = async () => { try { const path = await invoke<string | null>("tools_pick_folder"); if (path) setFolder(path); } catch (reason) { setError(String(reason)); } };
  const start = async () => {
    setBusy(true);
    const id = await call("agent_task_start", { goal, folder, policy, practice, network, copy: onCopy, from: null });
    setBusy(false);
    if (typeof id === "string") { setGoal(""); setPractice(false); setSelected(id); }
  };
  const settle = async (command: string, id: string) => { await call(command, { id }); setDiffOf(""); };
  const sorted = [...tasks].sort((a, b) => group(a) - group(b) || b.updated - a.updated);
  const waiting = tasks.filter((t) => t.state.state === "waiting").length;

  const composer = <form className="agents-compose" onSubmit={(e) => { e.preventDefault(); start(); }}>
    <h3>New task</h3>
    <textarea data-test="agent-task-goal" ref={goalBox} rows={3} maxLength={2000} placeholder="What should it do? For example: make the failing tests in this project pass." value={goal} onChange={(e) => setGoal(e.target.value)} />
    <div className="tools-row tools-folder">
      <span className="tools-row-text"><strong>Project folder</strong><code data-test="agent-task-folder">{folder || "None: web and the agent display only"}</code></span>
      <span className="tools-row-actions"><button type="button" data-test="agent-task-choose" onClick={choose}>Choose…</button><button type="button" disabled={!folder} onClick={() => { setFolder(""); setPolicy(""); }}>Clear</button></span>
    </div>
    <div className="tools-row" role="radiogroup" aria-label="How much to ask">
      <span className="tools-row-text"><strong>How much to ask</strong><span>{policy === "folder" ? "Checks and edits in the folder run without asking." : "Every command and edit waits for you."}</span></span>
      <span className="route-seg">
        <button type="button" role="radio" aria-checked={policy !== "folder"} className={policy !== "folder" ? "on" : ""} onClick={() => setPolicy("")}>Ask first</button>
        <button type="button" role="radio" aria-checked={policy === "folder"} className={policy === "folder" ? "on" : ""} data-test="agent-task-policy-folder" disabled={!folder} onClick={() => setPolicy("folder")}>Work in the folder</button>
      </span>
    </div>
    <div className="opt-chips" role="group" aria-label="Options">
      <button type="button" role="switch" aria-checked={onCopy} className={`opt-chip${onCopy ? " on" : ""}`} data-test="agent-task-copy" title="If the folder is a git project, the task works on its own copy of the last commit and you review its changes before anything reaches your project." onClick={() => setOnCopy(!onCopy)}>Work on a copy</button>
      <button type="button" role="switch" aria-checked={practice} className={`opt-chip${practice ? " on" : ""}`} data-test="agent-task-practice" title="Goes ahead without asking, inside the sandbox, and lists what it would have asked. Then run it for real with just that allowed." onClick={() => setPractice(!practice)}>Practice run</button>
      <button type="button" role="switch" aria-checked={network && !!folder} className={`opt-chip${network && folder ? " on" : ""}`} data-test="agent-task-network" disabled={!folder} title="Commands can reach the internet, for example to install packages. Needs a project folder." onClick={() => setNetwork(!network)}>Allow network</button>
    </div>
    <div className="agents-compose-actions">{tasks.length > 0 && <button type="button" data-test="agent-task-cancel" onClick={() => setSelected("")}>Cancel</button>}<button type="submit" className="primary" data-test="agent-task-start" disabled={!goal.trim() || busy}>Start task</button></div>
  </form>;

  const detail = task && <article className="agents-detail" data-test="agent-task-detail">
    <header>
      <h3>{task.goal}</h3>
      <p className="agents-meta"><span className={`agents-dot ${task.state.state}`} />{label(task)} · {task.steps.length} step{task.steps.length === 1 ? "" : "s"} · {ago(task.updated)} · {task.policy.name}{[...new Set(task.steps.filter((s) => s.place !== "local").map((s) => s.place))].map((p) => <span key={p} className="agents-badge remote" title="These steps ran on a remote model">{p}</span>)}{task.steps.every((s) => s.place === "local") && <span className="agents-badge" title="Every step ran on this computer">On this computer</span>}</p>
    </header>
    {task.state.state === "waiting" && <p className="agents-callout" data-test="agent-task-waiting">Waiting for you: {task.state.why}</p>}
    {task.state.state === "failed" && <p className="agents-callout bad">{task.state.error}</p>}
    {task.state.state === "done" && <p className="agents-answer">{task.state.summary}</p>}
    {task.review === "pending" && task.state.state !== "running" && task.state.state !== "waiting" && <section className="agents-changes" data-test="agent-task-changes">
      <div className="agents-changes-head"><strong>Changes to review</strong><span>{task.dirty ? "Your project had uncommitted changes that are not in the copy." : "Made on a copy of your project."}</span>
        <span className="agents-spacer" /><button type="button" className="mini" data-test={`agent-task-discard-${task.id}`} onClick={() => settle("agent_task_discard", task.id)}>Discard</button><button type="button" className="mini primary" data-test={`agent-task-apply-${task.id}`} disabled={diffOf !== task.id || !diffText.trim()} onClick={() => settle("agent_task_apply", task.id)}>Apply to project</button></div>
      {diffOf === task.id && (diffText.trim() ? <DiffView text={diffText} /> : <p className="sheet-note">The task changed nothing.</p>)}
    </section>}
    {task.review === "applied" && <p className="agents-note">Changes applied to your project.</p>}
    {task.review === "discarded" && <p className="agents-note">Changes discarded.</p>}
    <div className="agents-actions">
      {task.result_chat ? <button type="button" className="mini" data-test={`agent-task-open-${task.id}`} onClick={() => { onOpenChat(task.result_chat!); setShown(false); visible(false); }}>Open the answer chat</button> : null}
      {task.policy.mode === "practice" && task.steps.some((s) => s.would_ask) && task.state.state !== "running" && <button type="button" className="mini primary" data-test={`agent-task-again-${task.id}`} title="Start again, allowing what the practice run needed" onClick={async () => { const id = await call("agent_task_start", { goal: task.goal, folder: task.policy.folder, policy: "", practice: false, network: !!task.policy.network, copy: true, from: task.id }); if (typeof id === "string") setSelected(id); }}>Run for real</button>}
      {!["running", "waiting", "queued"].includes(task.state.state) && <button type="button" className="mini" data-test={`agent-task-delete-${task.id}`} onClick={() => { setSelected(""); call("agent_task_delete", { id: task.id }); }}>Delete</button>}
    </div>
    <h4>Steps</h4>
    {task.steps.length === 0 ? <p className="sheet-note">Nothing yet.</p> : <ol className="task-steps agents-steps" data-test="agent-task-steps">{task.steps.map((s) => <li key={s.n} className={s.ok ? "" : "bad"}>
      <strong>{TOOLS[s.tool] ?? s.tool}</strong>{s.place !== "local" && <span className="agents-badge remote">{s.place}</span>}
      {s.input ? <code>{s.input.slice(0, 200)}</code> : null}
      <span>{s.ok ? "" : "Failed: "}{s.outcome.slice(0, 300)}</span>
      {s.would_ask ? <span className="agents-callout" data-test="agent-task-would-ask">Would have asked: {s.would_ask}</span> : null}
    </li>)}</ol>}
  </article>;

  return <section className="agents-page" data-test="agent-tasks-panel" hidden={!shown} aria-label="Agents">
    <header className="agents-head" data-tauri-drag-region>
      <button type="button" className="icon-btn" data-test="agents-back" title="Back to chat" aria-label="Back to chat" onClick={() => { setShown(false); visible(false); }}>‹</button>
      <div><h2>Agents</h2><p>{waiting ? `${waiting} waiting for you` : "Tasks that run on their own and ask when they need you."}</p></div>
      <button type="button" className="primary" data-test="agent-task-new" onClick={() => { setSelected("new"); goalBox.current?.focus(); }}>New task</button>
    </header>
    {error && <p className="sheet-warning" role="alert" data-test="agent-task-error">{error}</p>}
    <div className="agents-body">
      <nav className="agents-list" aria-label="Tasks">
        {sorted.length === 0 && <p className="sheet-note">No tasks yet.</p>}
        {GROUPS.map((name, g) => { const rows = sorted.filter((t) => group(t) === g); return rows.length ? <div key={name} className="agents-group"><h5>{name}</h5>{rows.map((t) => <div key={t.id} className={`agents-row${selected === t.id ? " on" : ""}`}>
          <button type="button" className="agents-row-main" data-test={`agent-task-${t.id}`} onClick={() => setSelected(t.id)}><span className={`agents-dot ${t.state.state}`} /><strong>{t.goal.slice(0, 90)}</strong><span>{label(t)} · {t.steps.length} step{t.steps.length === 1 ? "" : "s"} · {ago(t.updated)}</span></button>
          {t.state.state === "waiting" && <span className="agents-row-actions"><button type="button" className="mini primary" data-test={`agent-task-approve-${t.id}`} onClick={() => call("agent_task_review", { id: t.id, approved: true })}>Allow</button><button type="button" className="mini" data-test={`agent-task-decline-${t.id}`} onClick={() => call("agent_task_review", { id: t.id, approved: false })}>Decline</button></span>}
          {(t.state.state === "running" || t.state.state === "waiting") && <span className="agents-row-actions"><button type="button" className="mini" data-test={`agent-task-stop-${t.id}`} onClick={() => call("agent_task_stop", { id: t.id })}>Stop</button></span>}
        </div>)}</div> : null; })}
      </nav>
      <div className="agents-main">{selected === "new" || !task ? composer : detail}</div>
    </div>
  </section>;
}
