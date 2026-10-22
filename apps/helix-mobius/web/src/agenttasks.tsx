import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** Agent tasks: every Tools run is a task with a goal, a policy and a record. The record says where each step ran, so what left the computer is visible. */
interface Step { n: number; tool: string; input: string; outcome: string; ok: boolean; place: string; at: number; would_ask?: string }
interface Policy { name: string; folder: string; cloud: string; display: boolean; mode?: string; network?: boolean }
type TaskState = { state: "queued" | "running" | "stopped" } | { state: "waiting"; why: string } | { state: "done"; summary: string } | { state: "failed"; error: string };
interface AgentTask { id: string; goal: string; result_chat?: string; policy: Policy; state: TaskState; steps: Step[]; created: number; updated: number }
export interface AgentTasksHandle { open: () => void }

const ago = (secs: number) => {
  const d = Math.max(0, Date.now() / 1000 - secs);
  return d < 90 ? "just now" : d < 5400 ? `${Math.round(d / 60)} min ago` : d < 129600 ? `${Math.round(d / 3600)} h ago` : `${Math.round(d / 86400)} days ago`;
};
const label = (state: TaskState) => state.state === "running" ? "Working" : state.state === "waiting" ? "Needs you" : state.state === "done" ? "Done" : state.state === "failed" ? "Failed" : state.state === "stopped" ? "Stopped" : "Waiting to start";
const TOOLS: Record<string, string> = { web_search: "Searched the web", web_read: "Read a page", terminal: "Ran a command", edit_file: "Edited a file", display_launch: "Opened an app", display_look: "Looked at the screen", display_click: "Clicked", display_type: "Typed", display_key: "Pressed a key", display_scroll: "Scrolled", display_help: "Asked for help" };

export function AgentTasks({ register, onOpenChat }: { register: (handle: AgentTasksHandle) => void; onOpenChat: (id: string) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [tasks, setTasks] = useState<AgentTask[]>([]);
  const [open, setOpen] = useState("");
  const [error, setError] = useState("");
  const [goal, setGoal] = useState("");
  const [folder, setFolder] = useState("");
  const [policy, setPolicy] = useState("");
  const [busy, setBusy] = useState(false);
  const [practice, setPractice] = useState(false);
  const [network, setNetwork] = useState(false);
  const load = () => invoke<AgentTask[]>("agent_tasks").then(setTasks, () => setTasks([]));
  useEffect(() => { register({ open: () => { setError(""); load(); dialog.current?.showModal(); } }); }, [register]);
  useEffect(() => { const timer = setInterval(() => { if (dialog.current?.open) load(); }, 3000); return () => clearInterval(timer); }, []);
  const remove = async (id: string) => { setError(""); try { setTasks(await invoke<AgentTask[]>("agent_task_delete", { id })); } catch (reason) { setError(String(reason)); } };
  const call = async (command: string, args: Record<string, unknown>) => { setError(""); try { await invoke(command, args); await load(); } catch (reason) { setError(String(reason)); } };
  const choose = async () => { try { const path = await invoke<string | null>("tools_pick_folder"); if (path) setFolder(path); } catch (reason) { setError(String(reason)); } };
  const start = async () => { setBusy(true); await call("agent_task_start", { goal, folder, policy, practice, network, from: null }); setBusy(false); if (!error) setGoal(""); };
  return <dialog ref={dialog} className="sheet" data-test="agent-tasks-panel" aria-labelledby="agent-tasks-title"><div className="sheet-body">
    <header className="sheet-head"><h3 id="agent-tasks-title">Agent tasks</h3><p className="sheet-note">Start a task and carry on: it works by itself and tells you when it is done or needs you. Every Tools run in a chat is kept here too. A step marked with a model's name ran on that remote model; the rest ran on this computer.</p></header>
    <div className="sheet-scroll">
      <form className="sched-form" onSubmit={(e) => { e.preventDefault(); start(); }}>
        <h4>New task</h4>
        <textarea data-test="agent-task-goal" rows={3} maxLength={2000} placeholder="What should it do? For example: make the failing tests in this project pass." value={goal} onChange={(e) => setGoal(e.target.value)} />
        <div className="tools-row tools-folder">
          <span className="tools-row-text"><strong>Project folder</strong><code data-test="agent-task-folder">{folder || "None: web and the agent display only"}</code></span>
          <span className="tools-row-actions"><button type="button" data-test="agent-task-choose" onClick={choose}>Choose…</button><button type="button" disabled={!folder} onClick={() => { setFolder(""); setPolicy(""); }}>Clear</button></span>
        </div>
        <div className="tools-row" role="radiogroup" aria-label="How much to ask">
          <span className="tools-row-text"><strong>How much to ask</strong><span>{policy === "folder" ? "Checks and edits in the folder run without asking." : "Every command and edit waits for you."}</span></span>
          <span className="route-seg">
            <button type="button" role="radio" aria-checked={policy !== "folder"} className={policy !== "folder" ? "on" : ""} onClick={() => setPolicy("")}>Ask first</button>
            <button type="button" role="radio" aria-checked={policy === "folder"} className={policy === "folder" ? "on" : ""} disabled={!folder} onClick={() => setPolicy("folder")}>Work in the folder</button>
          </span>
        </div>
        <label className="tools-row"><span className="tools-row-text"><strong>Practice run</strong><span>It goes ahead without asking, still inside the sandbox, and lists everything it would have asked about. Then you can start again with just those things allowed.</span></span><input type="checkbox" className="switch" data-test="agent-task-practice" checked={practice} onChange={(e) => setPractice(e.target.checked)} /></label>
        <label className="tools-row"><span className="tools-row-text"><strong>Allow network for commands</strong><span>Commands can reach the internet, for example to install packages. Off by default; needs a project folder.</span></span><input type="checkbox" className="switch" data-test="agent-task-network" checked={network && !!folder} disabled={!folder} onChange={(e) => setNetwork(e.target.checked)} /></label>
        <div className="sheet-actions"><span className="sheet-actions-spacer" /><button type="submit" className="primary" data-test="agent-task-start" disabled={!goal.trim() || busy}>Start task</button></div>
      </form>
      {tasks.length === 0 ? <p className="sheet-note" data-test="agent-tasks-empty">No tasks yet. Turn on Tools in a chat and ask for something.</p> : <ul className="sched-list">{tasks.map((t) => {
        const remote = new Set(t.steps.filter((s) => s.place !== "local").map((s) => s.place));
        return <li key={t.id} data-test={`agent-task-${t.id}`}>
          <div className="sched-main">
            <strong>{t.goal.slice(0, 140) || "Task"}</strong>
            <span>{label(t.state)} · {t.steps.length} step{t.steps.length === 1 ? "" : "s"} · {ago(t.updated)} · {t.policy.name}{remote.size ? ` · used ${[...remote].join(", ")}` : " · all on this computer"}</span>
            {t.state.state === "waiting" && <span className="sched-error" data-test="agent-task-waiting">{t.state.why}</span>}
            {t.state.state === "failed" && <span className="sched-error">{t.state.error}</span>}
            {open === t.id && <ol className="task-steps" data-test="agent-task-steps">{t.steps.map((s) => <li key={s.n} className={s.ok ? "" : "sched-error"}><strong>{TOOLS[s.tool] ?? s.tool}</strong>{s.input ? <code>{s.input.slice(0, 160)}</code> : null}<span>{s.ok ? "" : "Failed: "}{s.outcome.slice(0, 200)}{s.place !== "local" ? ` · ${s.place}` : ""}</span>{s.would_ask ? <span className="sched-error" data-test="agent-task-would-ask">Would have asked: {s.would_ask}</span> : null}</li>)}</ol>}
          </div>
          <div className="sched-actions">
            {t.state.state === "waiting" && <><button type="button" className="mini primary" data-test={`agent-task-approve-${t.id}`} onClick={() => call("agent_task_review", { id: t.id, approved: true })}>Allow</button><button type="button" className="mini" data-test={`agent-task-decline-${t.id}`} onClick={() => call("agent_task_review", { id: t.id, approved: false })}>Decline</button></>}
            {(t.state.state === "running" || t.state.state === "waiting") && <button type="button" className="mini" data-test={`agent-task-stop-${t.id}`} onClick={() => call("agent_task_stop", { id: t.id })}>Stop</button>}
            {t.policy.mode === "practice" && t.steps.some((s) => s.would_ask) && t.state.state !== "running" && <button type="button" className="mini primary" data-test={`agent-task-again-${t.id}`} title="Start again, allowing what the practice run needed" onClick={() => call("agent_task_start", { goal: t.goal, folder: t.policy.folder, policy: "", practice: false, network: !!t.policy.network, from: t.id })}>Run for real</button>}
            {t.result_chat ? <button type="button" className="mini" data-test={`agent-task-open-${t.id}`} onClick={() => { dialog.current?.close(); onOpenChat(t.result_chat!); }}>Open answer</button> : null}
            <button type="button" className="mini" data-test={`agent-task-toggle-${t.id}`} onClick={() => setOpen(open === t.id ? "" : t.id)}>{open === t.id ? "Hide steps" : "Steps"}</button>
            <button type="button" className="mini" data-test={`agent-task-delete-${t.id}`} onClick={() => remove(t.id)}>Delete</button>
          </div></li>;
      })}</ul>}
      {error && <p className="sheet-warning" role="alert">{error}</p>}
    </div>
    <footer className="sheet-actions"><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="agent-tasks-done" onClick={() => dialog.current?.close()}>Done</button></footer>
  </div></dialog>;
}
