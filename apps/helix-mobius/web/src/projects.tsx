import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** The Projects place: a project is a name with instructions every chat in it follows, a folder its chats work in, its chats, and the
 * documents they made. Everything about a project in one page; the chat list's project filter and the move-to-project action still work. */
interface Chat { id: string; title: string; updated: number }
interface Doc { title: string; chat_id: string; chat_title: string }
interface Project { name: string; instructions: string; folder: string; chats: Chat[]; documents: Doc[] }
export interface ProjectsHandle { open: () => void }
export interface ProjectsHooks { openChat: (id: string, doc?: string) => void; newChatIn: (project: string) => void; report: (error: unknown) => void }

export function Projects({ register, hooks }: { register: (handle: ProjectsHandle) => void; hooks: ProjectsHooks }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [projects, setProjects] = useState<Project[]>([]);
  const [selected, setSelected] = useState("");
  const [name, setName] = useState("");
  const [text, setText] = useState("");
  const [error, setError] = useState("");
  const [saved, setSaved] = useState(true);
  const current = projects.find((p) => p.name === selected);
  const load = (keep?: string) => invoke<Project[]>("projects_overview").then((list) => {
    setProjects(list);
    const pick = list.find((p) => p.name === (keep ?? selected)) ?? list[0];
    setSelected(pick?.name ?? ""); setText(pick?.instructions ?? ""); setSaved(true);
  }, () => setProjects([]));
  useEffect(() => { register({ open: () => { setError(""); setName(""); load(); dialog.current?.showModal(); } }); }, [register]);
  const choose = (p: Project) => { setSelected(p.name); setText(p.instructions); setSaved(true); setError(""); };
  const run = async (call: Promise<unknown>, keep?: string) => {
    setError("");
    try { const result = await call; if (Array.isArray(result)) { setProjects(result as Project[]); const pick = (result as Project[]).find((p) => p.name === (keep ?? selected)); if (pick) { setSelected(pick.name); if (keep) setText(pick.instructions); } } }
    catch (reason) { setError(String(reason)); }
  };
  const create = async () => { const n = name.trim(); if (!n) return; await run(invoke("project_create", { name: n }), n.slice(0, 40)); setName(""); setText(""); };
  const saveInstructions = async () => { if (!current) return; setError(""); try { await invoke("lab_set_project_instructions", { project: current.name, text }); setSaved(true); load(current.name); } catch (reason) { setError(String(reason)); } };
  const pickFolder = async () => { if (!current) return; try { const folder = await invoke<string | null>("tools_pick_folder"); if (folder) await run(invoke("project_set_folder", { project: current.name, folder })); } catch (reason) { setError(String(reason)); } };
  const go = (fn: () => void) => { dialog.current?.close(); fn(); };
  return <dialog ref={dialog} className="sheet projects-sheet" data-test="projects-panel" aria-labelledby="projects-title"><div className="sheet-body">
    <header className="sheet-head"><h3 id="projects-title">Projects</h3><p className="sheet-note">A project keeps what its chats share: instructions every chat in it follows, the folder they work in, and what they made.</p></header>
    <div className="projects-body">
      <nav className="projects-list" aria-label="Projects">
        {projects.map((p) => <button key={p.name} type="button" className={p.name === selected ? "active" : ""} data-test={`project-${p.name}`} onClick={() => choose(p)}><strong>{p.name}</strong><span>{p.chats.length} chat{p.chats.length === 1 ? "" : "s"}{p.folder ? " · folder" : ""}</span></button>)}
        <form onSubmit={(e) => { e.preventDefault(); create(); }}><input data-test="project-new-name" placeholder="New project" value={name} maxLength={40} onChange={(e) => setName(e.target.value)} /><button type="submit" className="mini" data-test="project-new" disabled={!name.trim()}>Add</button></form>
      </nav>
      <section className="projects-detail">
        {!current ? <p className="sheet-note" data-test="projects-empty">No projects yet. Add one, or move a chat into a project from its header.</p> : <>
          <div className="projects-head"><h4>{current.name}</h4><button type="button" className="primary" data-test="project-chat-new" onClick={() => go(() => hooks.newChatIn(current.name))}>New chat in this project</button></div>
          <label className="projects-label">Folder <span>(new chats in the project start there, for Tools and the Code stage)</span></label>
          <div className="projects-row"><code data-test="project-folder">{current.folder || "None"}</code><button type="button" className="mini" data-test="project-choose-folder" onClick={pickFolder}>Choose…</button>{current.folder && <button type="button" className="mini" onClick={() => run(invoke("project_set_folder", { project: current.name, folder: "" }))}>Clear</button>}</div>
          <label className="projects-label">Instructions <span>(every chat in the project follows them)</span></label>
          <textarea data-test="project-instructions" rows={4} value={text} maxLength={4000} placeholder="For example: This is my homelab. Prefer systemd units and Fedora commands." onChange={(e) => { setText(e.target.value); setSaved(false); }} />
          <div className="projects-row"><button type="button" className="mini" data-test="project-save" disabled={saved} onClick={saveInstructions}>{saved ? "Saved" : "Save instructions"}</button></div>
          <label className="projects-label">Chats</label>
          {current.chats.length === 0 ? <p className="sheet-note">None yet.</p> : <ul className="projects-items">{current.chats.map((c) => <li key={c.id}><button type="button" className="link-row" data-test={`project-chat-${c.id}`} onClick={() => go(() => hooks.openChat(c.id))}>{c.title}</button></li>)}</ul>}
          {current.documents.length > 0 && <><label className="projects-label">Documents</label><ul className="projects-items">{current.documents.map((d, i) => <li key={i}><button type="button" className="link-row" onClick={() => go(() => hooks.openChat(d.chat_id, d.title))}>{d.title}<span> · {d.chat_title}</span></button></li>)}</ul></>}
        </>}
      </section>
    </div>
    {error && <p className="sheet-warning" role="alert" data-test="projects-error">{error}</p>}
    <footer className="sheet-actions"><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="projects-done" onClick={() => dialog.current?.close()}>Done</button></footer>
  </div></dialog>;
}
