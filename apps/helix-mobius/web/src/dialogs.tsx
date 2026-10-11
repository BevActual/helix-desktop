import { useCallback, useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { invoke, listen } from "@helix/ui";

declare function icon(name: string): SVGElement;
function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}

interface Persona { id: string; name: string; system_prompt: string; task: string; backend: string; model: string; }
interface PersonaStore { active: string; personas: Persona[]; }
interface Route { backend?: string; model?: string; label?: string; }

export interface DialogHooks {
  report: (error: unknown) => void;
  /** The chat route was changed from the picker. */
  routeChanged: (backend: string, model: string) => void;
  /** Close the hub menu when a dialog opens from it. */
  closeMenu: () => void;
}
export interface DialogHandle {
  openRename: (title: string) => void;
  openProject: (current: string, suggestions: string[]) => void;
  openModel: () => void;
  openPersona: (create?: boolean) => void;
}

function initials(name: string): string {
  const parts = String(name || "").trim().split(/\s+/).filter(Boolean);
  if (parts.length >= 2) return (parts[0][0] + parts[1][0]).toUpperCase();
  return (parts[0] || "?").slice(0, 2).toUpperCase();
}
function hue(id: string): number {
  let hash = 0;
  for (let i = 0; i < id.length; i++) hash = ((hash << 5) - hash + id.charCodeAt(i)) | 0;
  return Math.abs(hash) % 360;
}

/** A native `<dialog>` that is always mounted (so other code can still find its fields) and shown from state. */
function Sheet({ id, test, small, open, onClose, children }: { id: string; test: string; small?: boolean; open: boolean; onClose: () => void; children: React.ReactNode }) {
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const dialog = ref.current;
    if (!dialog) return;
    if (open && !dialog.open) dialog.showModal();
    if (!open && dialog.open) dialog.close();
  }, [open]);
  return (
    <dialog ref={ref} id={id} className={`sheet${small ? " sheet-sm" : ""}`} data-test={test} onClose={onClose}>
      {children}
    </dialog>
  );
}

function newPersonaId() {
  return "persona-" + Date.now().toString(36).slice(-6);
}

/** The persona rail and the four sheets (rename, project, persona, model). Rust owns the personas, the route and the chat; this sends back
 * the values typed and the ids it was given. */
export function Dialogs({ register, hooks, rail }: { register: (handle: DialogHandle) => void; hooks: DialogHooks; rail: HTMLElement | null }) {
  const hooksRef = useRef(hooks);
  hooksRef.current = hooks;
  const report = (error: unknown) => hooksRef.current.report(error);

  // personas
  const [store, setStore] = useState<PersonaStore>({ active: "helix", personas: [] });
  useEffect(() => {
    const stop = listen<PersonaStore>("personas-view", setStore);
    invoke<PersonaStore>("personas_get").then(setStore, () => undefined);
    return () => { stop.then((fn) => fn()); };
  }, []);

  // which sheet is open
  const [sheet, setSheet] = useState<"" | "rename" | "project" | "persona" | "model">("");
  const closeSheet = useCallback(() => setSheet(""), []);

  // rename
  const [title, setTitle] = useState("");
  // project
  const [project, setProject] = useState("");
  const [suggestions, setSuggestions] = useState<string[]>([]);
  // persona
  const [draft, setDraft] = useState<Persona>({ id: "helix", name: "", system_prompt: "", task: "chat", backend: "", model: "" });
  // model
  const [backend, setBackend] = useState("local");
  const [backends, setBackends] = useState<{ value: string; label: string }[]>([
    { value: "local", label: "On this computer" },
    { value: "lan", label: "LAN" },
    { value: "openrouter", label: "OpenRouter" },
  ]);
  const [models, setModels] = useState<string[]>([]);
  const [custom, setCustom] = useState("");
  const [current, setCurrent] = useState("");

  const loadModels = useCallback(async (which: string) => {
    try {
      if (which === "openrouter") await invoke("routing_ensure_openrouter").catch(() => undefined);
      const list = which === "local"
        ? await invoke<string[]>("routing_list_models")
        : await invoke<string[]>("routing_list_remote_models", { backendId: which });
      setModels(Array.isArray(list) ? list : []);
    } catch (error) {
      report(error);
    }
  }, []);

  const edit = (persona: Persona) => setDraft({ ...persona });
  const openPersona = (create?: boolean) => {
    hooksRef.current.closeMenu();
    if (create) edit({ id: newPersonaId(), name: "New persona", system_prompt: "", task: "chat", backend: "", model: "" });
    else edit(store.personas.find((p) => p.id === store.active) ?? store.personas[0] ?? draft);
    setSheet("persona");
  };

  useEffect(() => {
    register({
      openRename: (value) => { setTitle(value || "New chat"); setSheet("rename"); },
      openProject: (value, names) => { setProject(value || ""); setSuggestions(names); setSheet("project"); },
      openPersona,
      openModel: async () => {
        try {
          const route = await invoke<Route>("chat_current_route");
          const model = route?.model || "";
          const which = route?.backend || "local";
          setCurrent(model);
          setCustom(model);
          setBackend(which);
          setBackends((old) => (old.some((entry) => entry.value === which) ? old : [...old, { value: which, label: route?.label || which }]));
          await loadModels(which);
          setSheet("model");
        } catch (error) {
          report(error);
        }
      },
    });
  });

  const pickBackend = (which: string) => {
    setBackend(which);
    loadModels(which);
  };
  const applyRoute = async (model: string) => {
    try {
      if (backend === "openrouter") await invoke("routing_ensure_openrouter");
      await invoke("chat_set_route", { backend, model });
      hooksRef.current.routeChanged(backend, model);
      closeSheet();
    } catch (error) {
      report(error);
    }
  };

  return (
    <>
      {rail
        ? createPortal(
            <>
              {store.personas.map((p, index) => (
                <button
                  key={p.id}
                  type="button"
                  className={`persona-dot${p.id === store.active ? " active" : ""}`}
                  role="option"
                  aria-selected={p.id === store.active}
                  data-test={`persona-dot-${index}`}
                  title={`${p.name} — click to use, double-click to edit`}
                  style={{ ["--dot-hue" as string]: String(hue(p.id)) }}
                  onClick={() => {
                    if (p.id === store.active) { edit(p); hooksRef.current.closeMenu(); setSheet("persona"); }
                    else invoke("personas_set_active", { id: p.id }).catch(report);
                  }}
                  onDoubleClick={(event) => { event.preventDefault(); edit(p); hooksRef.current.closeMenu(); setSheet("persona"); }}
                >
                  <span className="persona-dot-face">{initials(p.name)}</span>
                  <span className="persona-dot-label">{p.name}</span>
                </button>
              ))}
              <button type="button" className="persona-dot persona-dot-add" data-test="persona-dot-add" title="New persona" aria-label="New persona" onClick={() => openPersona(true)}>
                <span className="persona-dot-face"><Icon name="plus" /></span>
                <span className="persona-dot-label">New</span>
              </button>
            </>,
            rail,
          )
        : null}

      <Sheet id="rename-dialog" test="rename-dialog" small open={sheet === "rename"} onClose={closeSheet}>
        <form
          className="sheet-body"
          onSubmit={(event) => {
            event.preventDefault();
            const value = title.trim();
            if (!value) { report("Give the chat a title."); return; }
            invoke("chat_rename", { title: value }).then(closeSheet).catch(report);
          }}
        >
          <header className="sheet-head"><h3>Rename chat</h3></header>
          <div className="sheet-scroll">
            <label className="sheet-field">Title
              <input id="rename-title" data-test="rename-title" type="text" maxLength={48} required autoFocus value={title} onChange={(event) => setTitle(event.target.value)} />
            </label>
          </div>
          <div className="sheet-actions">
            <button type="button" onClick={closeSheet}>Cancel</button>
            <button type="submit" className="primary" data-test="rename-save">Save</button>
          </div>
        </form>
      </Sheet>

      <Sheet id="project-dialog" test="project-dialog" small open={sheet === "project"} onClose={closeSheet}>
        <form
          className="sheet-body"
          onSubmit={(event) => {
            event.preventDefault();
            invoke("chat_set_project", { project: project.trim() }).then(closeSheet).catch(report);
          }}
        >
          <header className="sheet-head">
            <h3>Project</h3>
            <p className="sheet-note">Group related chats. Leave empty to remove from a project.</p>
          </header>
          <div className="sheet-scroll">
            <label className="sheet-field">Name
              <input id="project-name" data-test="project-name" type="text" maxLength={40} placeholder="e.g. Work, Homelab" list="project-suggestions" autoFocus value={project} onChange={(event) => setProject(event.target.value)} />
            </label>
            <datalist id="project-suggestions">{suggestions.map((name) => <option key={name} value={name} />)}</datalist>
          </div>
          <div className="sheet-actions">
            <button type="button" data-test="project-clear" onClick={() => { invoke("chat_set_project", { project: "" }).then(closeSheet).catch(report); }}>No project</button>
            <span className="sheet-actions-spacer" />
            <button type="button" onClick={closeSheet}>Cancel</button>
            <button type="submit" className="primary">Save</button>
          </div>
        </form>
      </Sheet>

      <Sheet id="persona-dialog" test="persona-dialog" open={sheet === "persona"} onClose={closeSheet}>
        <form
          className="sheet-body"
          onSubmit={(event) => {
            event.preventDefault();
            const persona = { ...draft, id: draft.id || newPersonaId(), name: draft.name.trim(), backend: draft.backend.trim(), model: draft.model.trim(), task: draft.task || "chat" };
            if (!persona.name) { report("Give the persona a name."); return; }
            invoke("personas_upsert", { persona })
              .then(() => invoke("personas_set_active", { id: persona.id }))
              .then(closeSheet)
              .catch(report);
          }}
        >
          <header className="sheet-head">
            <h3>Persona</h3>
            <p className="sheet-note">A hub mind for talk. Desktop change still goes through Act and the bar Assistant.</p>
          </header>
          <div className="sheet-scroll">
            <label className="sheet-field">Name
              <input id="persona-edit-name" data-test="persona-edit-name" type="text" maxLength={40} required value={draft.name} onChange={(event) => setDraft({ ...draft, name: event.target.value })} />
            </label>
            <label className="sheet-field">System prompt
              <textarea id="persona-edit-prompt" data-test="persona-edit-prompt" rows={6} maxLength={4000} placeholder="Optional. Empty uses the default Mobius prompt." value={draft.system_prompt} onChange={(event) => setDraft({ ...draft, system_prompt: event.target.value })} />
            </label>
            <label className="sheet-field">Task profile
              <select id="persona-edit-task" data-test="persona-edit-task" value={draft.task || "chat"} onChange={(event) => setDraft({ ...draft, task: event.target.value })}>
                <option value="chat">Chat</option>
                <option value="code">Code</option>
                <option value="summarize">Summarize</option>
                <option value="vision">Vision</option>
              </select>
            </label>
            <label className="sheet-field">Backend override
              <input id="persona-edit-backend" data-test="persona-edit-backend" type="text" placeholder="optional, e.g. local or lan" value={draft.backend} onChange={(event) => setDraft({ ...draft, backend: event.target.value })} />
            </label>
            <label className="sheet-field">Model override
              <input id="persona-edit-model" data-test="persona-edit-model" type="text" placeholder="optional GGUF id" value={draft.model} onChange={(event) => setDraft({ ...draft, model: event.target.value })} />
            </label>
            <label className="sheet-field">Switch to
              <select id="persona-pick" data-test="persona-pick" value={store.personas.some((p) => p.id === draft.id) ? draft.id : ""} onChange={(event) => { const next = store.personas.find((p) => p.id === event.target.value); if (next) edit(next); }}>
                {store.personas.some((p) => p.id === draft.id) ? null : <option value="">(new persona)</option>}
                {store.personas.map((p) => <option key={p.id} value={p.id}>{p.name}</option>)}
              </select>
            </label>
          </div>
          <div className="sheet-actions">
            <button id="persona-new" type="button" data-test="persona-new" onClick={() => edit({ id: newPersonaId(), name: "New persona", system_prompt: "", task: "chat", backend: "", model: "" })}>New</button>
            <span className="sheet-actions-spacer" />
            <button type="button" onClick={closeSheet}>Cancel</button>
            <button id="persona-save" type="submit" className="primary" data-test="persona-save">Save</button>
          </div>
        </form>
      </Sheet>

      <Sheet id="model-dialog" test="model-dialog" small open={sheet === "model"} onClose={closeSheet}>
        <form className="sheet-body" onSubmit={(event) => { event.preventDefault(); applyRoute(custom.trim()); }}>
          <header className="sheet-head">
            <h3>Chat model</h3>
            <p className="sheet-note">Local, LAN, or cloud for talk. Act stays on its own local model — remotes never run desktop tools.</p>
          </header>
          <div className="sheet-scroll">
            <label className="sheet-field">Backend
              <select id="model-backend" data-test="model-backend" value={backend} onChange={(event) => pickBackend(event.target.value)}>
                {backends.map((entry) => <option key={entry.value} value={entry.value}>{entry.label}</option>)}
              </select>
            </label>
            <div id="model-list" className="model-list" data-test="model-list">
              <button type="button" className={`model-option${!custom ? " active" : ""}`} onClick={() => setCustom("")}>Auto (default)</button>
              {models.map((id) => (
                <button key={id} type="button" className={`model-option${custom === id ? " active" : ""}`} onClick={() => setCustom(id)}>{id}</button>
              ))}
            </div>
            <label className="sheet-field">Custom id
              <input id="model-custom" data-test="model-custom" type="text" placeholder="GGUF id or provider/model" value={custom} onChange={(event) => setCustom(event.target.value)} />
            </label>
          </div>
          <div className="sheet-actions">
            <button type="button" data-test="model-auto" onClick={() => applyRoute("")}>Auto</button>
            <span className="sheet-actions-spacer" />
            <button type="button" onClick={closeSheet}>Cancel</button>
            <button type="submit" className="primary" data-current={current}>Use</button>
          </div>
        </form>
      </Sheet>
    </>
  );
}
