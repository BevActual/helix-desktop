import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** Scheduled prompts: asked by themselves at a time (a systemd user timer each), answered by the model on this computer, saved as a chat.
 * Chat only: no tools, no desktop actions, never a cloud model. */
interface Task { id: string; name: string; prompt: string; enabled: boolean; summary: string; last_run: number; last_chat: string; last_error: string }
export interface ScheduledHandle { open: () => void }

const DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
const ago = (secs: number) => {
  if (!secs) return "never run";
  const d = Math.max(0, Date.now() / 1000 - secs);
  return d < 90 ? "just now" : d < 5400 ? `${Math.round(d / 60)} min ago` : d < 129600 ? `${Math.round(d / 3600)} h ago` : `${Math.round(d / 86400)} days ago`;
};

export function Scheduled({ register, onOpenChat }: { register: (handle: ScheduledHandle) => void; onOpenChat: (id: string) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [tasks, setTasks] = useState<Task[]>([]);
  const [name, setName] = useState("");
  const [prompt, setPrompt] = useState("");
  const [kind, setKind] = useState<"daily" | "weekly" | "once">("daily");
  const [time, setTime] = useState("08:00");
  const [days, setDays] = useState<number[]>([0, 1, 2, 3, 4]);
  const [date, setDate] = useState("");
  const [error, setError] = useState("");
  const load = () => invoke<Task[]>("scheduled_list").then(setTasks, () => setTasks([]));
  useEffect(() => { register({ open: () => { setError(""); load(); dialog.current?.showModal(); } }); }, [register]);
  const run = async (call: Promise<unknown>) => { setError(""); try { const result = await call; if (Array.isArray(result)) setTasks(result as Task[]); } catch (reason) { setError(String(reason)); } };
  const when = () => kind === "daily" ? { kind, time } : kind === "weekly" ? { kind, time, days } : { kind, time, date };
  const add = async () => {
    await run(invoke("scheduled_save", { name, prompt, when: when() }));
    setName(""); setPrompt("");
  };
  return <dialog ref={dialog} className="sheet" data-test="scheduled-panel" aria-labelledby="scheduled-title"><div className="sheet-body">
    <header className="sheet-head"><h3 id="scheduled-title">Scheduled</h3><p className="sheet-note">Prompts that run by themselves at a time you choose, even when Mobius is closed. The model on this computer answers, the answer is saved as a chat, and you get a notification. They only chat: no tools, no desktop actions, never a cloud model.</p></header>
    <div className="sheet-scroll">
      {tasks.length === 0 ? <p className="sheet-note" data-test="scheduled-empty">Nothing scheduled yet.</p> : <ul className="sched-list">{tasks.map((t) => <li key={t.id} data-test={`sched-${t.id}`}>
        <div className="sched-main"><strong>{t.name}</strong><span>{t.summary}{t.enabled ? "" : " · off"}</span>
          <span className={t.last_error ? "sched-error" : ""}>{t.last_error ? `Last run failed: ${t.last_error}` : `Last run: ${ago(t.last_run)}`}{t.last_chat ? <> · <button type="button" className="link" data-test={`sched-open-${t.id}`} onClick={() => { dialog.current?.close(); onOpenChat(t.last_chat); }}>open the answer</button></> : null}</span></div>
        <div className="sched-actions">
          <input type="checkbox" className="switch" aria-label={`${t.name} is on`} data-test={`sched-toggle-${t.id}`} checked={t.enabled} onChange={(e) => run(invoke("scheduled_set_enabled", { id: t.id, enabled: e.target.checked }))} />
          <button type="button" className="mini" data-test={`sched-run-${t.id}`} onClick={() => run(invoke("scheduled_run_now", { id: t.id }).then(() => setTimeout(load, 4000)))}>Run now</button>
          <button type="button" className="mini" data-test={`sched-delete-${t.id}`} onClick={() => run(invoke("scheduled_delete", { id: t.id }))}>Delete</button>
        </div></li>)}</ul>}
      <form className="sched-form" onSubmit={(e) => { e.preventDefault(); add(); }}>
        <h4>New scheduled prompt</h4>
        <input data-test="sched-name" placeholder="Name, for example Morning brief" value={name} maxLength={60} onChange={(e) => setName(e.target.value)} />
        <textarea data-test="sched-prompt" rows={3} placeholder="What to ask, for example: Give me five things worth doing today, based on what you remember about me." value={prompt} maxLength={2000} onChange={(e) => setPrompt(e.target.value)} />
        <div className="sched-when">
          <select data-test="sched-kind" value={kind} onChange={(e) => setKind(e.target.value as typeof kind)}><option value="daily">Every day</option><option value="weekly">On certain days</option><option value="once">Once</option></select>
          {kind === "once" && <input type="date" data-test="sched-date" value={date} onChange={(e) => setDate(e.target.value)} />}
          <input type="time" data-test="sched-time" value={time} onChange={(e) => setTime(e.target.value)} />
        </div>
        {kind === "weekly" && <div className="sched-days">{DAYS.map((d, i) => <label key={d}><input type="checkbox" checked={days.includes(i)} onChange={(e) => setDays(e.target.checked ? [...days, i] : days.filter((x) => x !== i))} />{d}</label>)}</div>}
        {error && <p className="sheet-warning" role="alert" data-test="sched-error">{error}</p>}
        <div className="sheet-actions"><span className="sheet-actions-spacer" /><button type="submit" className="primary" data-test="sched-add" disabled={!name.trim() || !prompt.trim()}>Schedule it</button></div>
      </form>
    </div>
    <footer className="sheet-actions"><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="sched-done" onClick={() => dialog.current?.close()}>Done</button></footer>
  </div></dialog>;
}
