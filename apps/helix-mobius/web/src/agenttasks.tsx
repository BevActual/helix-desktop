import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** Agent tasks: every Tools run is a task with a goal, a policy and a record. The record says where each step ran, so what left the computer is visible. */
interface Step { n: number; tool: string; input: string; outcome: string; ok: boolean; place: string; at: number }
interface Policy { name: string; folder: string; cloud: string; display: boolean }
type TaskState = { state: "queued" | "running" | "stopped" } | { state: "waiting"; why: string } | { state: "done"; summary: string } | { state: "failed"; error: string };
interface AgentTask { id: string; goal: string; policy: Policy; state: TaskState; steps: Step[]; created: number; updated: number }
export interface AgentTasksHandle { open: () => void }

const ago = (secs: number) => {
  const d = Math.max(0, Date.now() / 1000 - secs);
  return d < 90 ? "just now" : d < 5400 ? `${Math.round(d / 60)} min ago` : d < 129600 ? `${Math.round(d / 3600)} h ago` : `${Math.round(d / 86400)} days ago`;
};
const label = (state: TaskState) => state.state === "running" ? "Working" : state.state === "waiting" ? "Needs you" : state.state === "done" ? "Done" : state.state === "failed" ? "Failed" : state.state === "stopped" ? "Stopped" : "Waiting to start";
const TOOLS: Record<string, string> = { web_search: "Searched the web", web_read: "Read a page", terminal: "Ran a command", edit_file: "Edited a file", display_launch: "Opened an app", display_look: "Looked at the screen", display_click: "Clicked", display_type: "Typed", display_key: "Pressed a key", display_scroll: "Scrolled", display_help: "Asked for help" };

export function AgentTasks({ register }: { register: (handle: AgentTasksHandle) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [tasks, setTasks] = useState<AgentTask[]>([]);
  const [open, setOpen] = useState("");
  const [error, setError] = useState("");
  const load = () => invoke<AgentTask[]>("agent_tasks").then(setTasks, () => setTasks([]));
  useEffect(() => { register({ open: () => { setError(""); load(); dialog.current?.showModal(); } }); }, [register]);
  useEffect(() => { const timer = setInterval(() => { if (dialog.current?.open) load(); }, 3000); return () => clearInterval(timer); }, []);
  const remove = async (id: string) => { setError(""); try { setTasks(await invoke<AgentTask[]>("agent_task_delete", { id })); } catch (reason) { setError(String(reason)); } };
  return <dialog ref={dialog} className="sheet" data-test="agent-tasks-panel" aria-labelledby="agent-tasks-title"><div className="sheet-body">
    <header className="sheet-head"><h3 id="agent-tasks-title">Agent tasks</h3><p className="sheet-note">Every Tools run is kept here with what it did. A step marked with a model's name ran on that remote model; the rest ran on this computer.</p></header>
    <div className="sheet-scroll">
      {tasks.length === 0 ? <p className="sheet-note" data-test="agent-tasks-empty">No tasks yet. Turn on Tools in a chat and ask for something.</p> : <ul className="sched-list">{tasks.map((t) => {
        const remote = new Set(t.steps.filter((s) => s.place !== "local").map((s) => s.place));
        return <li key={t.id} data-test={`agent-task-${t.id}`}>
          <div className="sched-main">
            <strong>{t.goal.slice(0, 140) || "Task"}</strong>
            <span>{label(t.state)} · {t.steps.length} step{t.steps.length === 1 ? "" : "s"} · {ago(t.updated)} · {t.policy.name}{remote.size ? ` · used ${[...remote].join(", ")}` : " · all on this computer"}</span>
            {t.state.state === "failed" && <span className="sched-error">{t.state.error}</span>}
            {open === t.id && <ol className="task-steps" data-test="agent-task-steps">{t.steps.map((s) => <li key={s.n} className={s.ok ? "" : "sched-error"}><strong>{TOOLS[s.tool] ?? s.tool}</strong>{s.input ? <code>{s.input.slice(0, 160)}</code> : null}<span>{s.ok ? "" : "Failed: "}{s.outcome.slice(0, 200)}{s.place !== "local" ? ` · ${s.place}` : ""}</span></li>)}</ol>}
          </div>
          <div className="sched-actions">
            <button type="button" className="mini" data-test={`agent-task-toggle-${t.id}`} onClick={() => setOpen(open === t.id ? "" : t.id)}>{open === t.id ? "Hide steps" : "Steps"}</button>
            <button type="button" className="mini" data-test={`agent-task-delete-${t.id}`} onClick={() => remove(t.id)}>Delete</button>
          </div></li>;
      })}</ul>}
      {error && <p className="sheet-warning" role="alert">{error}</p>}
    </div>
    <footer className="sheet-actions"><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="agent-tasks-done" onClick={() => dialog.current?.close()}>Done</button></footer>
  </div></dialog>;
}
