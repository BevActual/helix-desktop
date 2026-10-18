import { useCallback, useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { invoke, listen } from "@helix/ui";

declare function icon(name: string): SVGElement;
function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}

interface Persona { id: string; name: string; system_prompt: string; task: string; backend: string; model: string; icon: string; description: string; starters: string[]; }
export interface LabView {
  experiments: { persona_chip: boolean; style_overlays: boolean; project_instructions: boolean; persona_starters: boolean };
  styles: { id: string; label: string; builtin: boolean }[];
  style: string;
  project: string;
  project_instructions: string;
}
/** The symbols a persona may pick from (must match `ICONS` in helix-chat). */
const ICONS = ["sparkle", "music", "palette", "fork", "clipboard", "eye", "shield", "sun", "moon", "bolt", "pin", "image", "folder", "lock"];
interface PersonaStore { active: string; personas: Persona[]; }
interface Route { backend?: string; model?: string; label?: string; own?: boolean; }

export interface DialogHooks {
  report: (error: unknown) => void;
  /** The experiments, styles and the chat's style changed (the composer chips and the persona rail follow). */
  labChanged: (lab: LabView) => void;
  /** A message was edited and sent: the chat should follow the new reply. */
  edited: (index: number, text: string) => void;
  /** The chat route was changed from the picker. */
  routeChanged: (backend: string, model: string) => void;
  /** Close the hub menu when a dialog opens from it. */
  closeMenu: () => void;
}
/** What the mode menu needs to know: the current mode and whether it can be changed now. */
export interface ModeState { mode: "ask" | "tools" | "act"; folder: string; busy: boolean }
export interface DialogHandle {
  openRename: (title: string) => void;
  openProject: (current: string, suggestions: string[]) => void;
  openModel: () => void;
  openPersona: (create?: boolean) => void;
  openEdit: (index: number, text: string) => void;
  /** A small menu above an anchor: the persona list or the style list. */
  openMenu: (kind: "persona" | "style" | "model" | "mode", anchor: { left: number; top: number; width: number }, mode?: ModeState) => void;
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
  // A menu taller than the room above its button scrolls, opened at its bottom where the actions are.
  const menuRef = useCallback((el: HTMLDivElement | null) => { if (el) el.scrollTop = el.scrollHeight; }, []);

  // personas
  const [store, setStore] = useState<PersonaStore>({ active: "helix", personas: [] });
  useEffect(() => {
    const stop = listen<PersonaStore>("personas-view", setStore);
    invoke<PersonaStore>("personas_get").then(setStore, () => undefined);
    return () => { stop.then((fn) => fn()); };
  }, []);

  // the experiments and the current chat's style, re-read when the chat changes
  const [lab, setLab] = useState<LabView | null>(null);
  const readLab = useCallback(() => {
    invoke<LabView>("lab_get").then((value) => { setLab(value); hooksRef.current.labChanged(value); }, () => undefined);
  }, []);
  useEffect(() => {
    const stops = [listen<LabView>("lab-view", (value) => { setLab(value); hooksRef.current.labChanged(value); }), listen<unknown>("chat-view", readLab)];
    readLab();
    return () => { stops.forEach((stop) => stop.then((fn) => fn())); };
  }, [readLab]);
  const [menu, setMenu] = useState<{ kind: "persona" | "style" | "model" | "mode"; left: number; top: number; width: number } | null>(null);
  const [modeState, setModeState] = useState<ModeState>({ mode: "ask", folder: "", busy: false });
  // What the model menu lists: the installed models with their friendly names, and where the chat is routed now.
  const [picker, setPicker] = useState<{ models: string[]; cards: Record<string, { title: string; bytes: number; vision: boolean; loaded: boolean; tasks: string[]; fit: string; speed: number | null }>; route: Route | null; cloud: { has_key: boolean; models: string[] } } | null>(null);
  useEffect(() => {
    if (!menu) return;
    const close = (event: Event) => {
      if (event instanceof KeyboardEvent && event.key !== "Escape") return;
      if (event instanceof MouseEvent && (event.target as HTMLElement | null)?.closest(".hx-menu")) return;
      setMenu(null);
    };
    window.addEventListener("mousedown", close);
    window.addEventListener("keydown", close);
    return () => { window.removeEventListener("mousedown", close); window.removeEventListener("keydown", close); };
  }, [menu]);

  // which sheet is open
  const [sheet, setSheet] = useState<"" | "rename" | "project" | "persona" | "model" | "edit">("");
  const [editIndex, setEditIndex] = useState(0);
  const [editText, setEditText] = useState("");
  const closeSheet = useCallback(() => setSheet(""), []);

  // rename
  const [title, setTitle] = useState("");
  // project
  const [project, setProject] = useState("");
  const [projectText, setProjectText] = useState("");
  const [scanned, setScanned] = useState<string[]>([]);
  const [suggestions, setSuggestions] = useState<string[]>([]);
  // persona
  const [draft, setDraft] = useState<Persona>({ id: "helix", name: "", system_prompt: "", task: "chat", backend: "", model: "", icon: "", description: "", starters: [] });
  useEffect(() => {
    if (sheet !== "persona") return;
    const timer = setTimeout(() => {
      invoke<string[]>("lab_scan", { text: draft.system_prompt }).then(setScanned, () => setScanned([]));
    }, 250);
    return () => clearTimeout(timer);
  }, [sheet, draft.system_prompt]);
  // model
  const [backend, setBackend] = useState("local");
  const [backends, setBackends] = useState<{ value: string; label: string }[]>([
    { value: "local", label: "On this computer" },
    { value: "lan", label: "LAN" },
    { value: "openrouter", label: "OpenRouter" },
  ]);
  const [models, setModels] = useState<string[]>([]);
  const [shelf, setShelf] = useState<Record<string, { title: string; bytes: number; vision: boolean; loaded: boolean; tasks: string[] }>>({});
  const [custom, setCustom] = useState("");
  const [current, setCurrent] = useState("");

  const loadModels = useCallback(async (which: string) => {
    try {
      if (which === "openrouter") await invoke("routing_ensure_openrouter").catch(() => undefined);
      const list = which === "local"
        ? await invoke<string[]>("routing_list_models")
        : await invoke<string[]>("routing_list_remote_models", { backendId: which });
      setModels(Array.isArray(list) ? list : []);
      if (which === "local") {
        const cards = await invoke<{ file: string; title: string; bytes: number; vision: boolean; loaded: boolean; installed: boolean; tasks: string[]; fit: string; speed: number | null }[]>("library_list").catch(() => []);
        const next: typeof shelf = {};
        cards.filter((c) => c.installed).forEach((c) => { next[c.file] = c; });
        setShelf(next);
      }
    } catch (error) {
      report(error);
    }
  }, []);

  const edit = (persona: Persona) => setDraft({ ...persona });
  const openPersona = (create?: boolean) => {
    hooksRef.current.closeMenu();
    if (create) edit({ id: newPersonaId(), name: "New persona", system_prompt: "", task: "chat", backend: "", model: "", icon: "sparkle", description: "", starters: [] });
    else edit(store.personas.find((p) => p.id === store.active) ?? store.personas[0] ?? draft);
    setSheet("persona");
  };

  const openModelSheet = async () => {
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
  };

  useEffect(() => {
    register({
      openRename: (value) => { setTitle(value || "New chat"); setSheet("rename"); },
      openProject: (value, names) => {
        setProject(value || "");
        setSuggestions(names);
        invoke<LabView>("lab_get").then((view) => setProjectText(view.project_instructions || ""), () => setProjectText(""));
        setSheet("project");
      },
      openMenu: (kind, anchor, mode) => {
        if (kind === "mode" && mode) setModeState(mode);
        if (kind !== "model") { setMenu({ kind, ...anchor }); return; }
        Promise.all([
          invoke<string[]>("routing_list_models").catch(() => [] as string[]),
          invoke<{ file: string; title: string; bytes: number; vision: boolean; loaded: boolean; installed: boolean; tasks: string[]; fit: string; speed: number | null }[]>("library_list").catch(() => []),
          invoke<Route>("chat_current_route").catch(() => null),
          invoke<{ has_key: boolean; models: string[] }>("routing_cloud_menu").catch(() => ({ has_key: false, models: [] as string[] })),
        ]).then(([models, cards, route, cloud]) => {
          const byFile: Record<string, { title: string; bytes: number; vision: boolean; loaded: boolean; tasks: string[]; fit: string; speed: number | null }> = {};
          cards.filter((card) => card.installed).forEach((card) => { byFile[card.file] = card; });
          setPicker({ models: Array.isArray(models) ? models : [], cards: byFile, route, cloud });
          setMenu({ kind, ...anchor });
        });
      },
      openPersona,
      openEdit: (index, text) => { setEditIndex(index); setEditText(text); setSheet("edit"); },
      openModel: () => openModelSheet(),
    });
  });

  /** Switching in the menu applies at once, on this computer: the next message goes to that model. */
  const useLocalModel = async (model: string) => {
    setMenu(null);
    try {
      await invoke("chat_set_route", { backend: "local", model });
      hooksRef.current.routeChanged("local", model);
    } catch (error) {
      report(error);
    }
  };
  const useCloudModel = async (model: string) => {
    setMenu(null);
    try {
      await invoke("chat_set_route", { backend: "openrouter", model });
      hooksRef.current.routeChanged("openrouter", model);
    } catch (error) {
      report(error);
    }
  };
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
                  <span className="persona-dot-face">{p.icon ? <Icon name={p.icon} /> : initials(p.name)}</span>
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

      {menu
        ? createPortal(
            <div ref={menuRef} className={`hx-menu${menu.kind === "mode" ? " hx-menu-wide" : ""}`} data-test={`menu-${menu.kind}`} style={{ left: Math.max(8, Math.min(menu.left, window.innerWidth - (menu.kind === "mode" ? 320 : Math.max(280, menu.width)) - 8)), bottom: window.innerHeight - menu.top + 6, minWidth: Math.max(220, menu.width), maxHeight: Math.max(160, Math.min(window.innerHeight * 0.6, menu.top - 18)) }} role="menu">
              {menu.kind === "mode" ? (
                <>
                  <div className="hx-menu-label">Mode <span className="hx-menu-hint">Shift+Tab</span></div>
                  {([
                    ["ask", "Ask", "Chat only. No tools."],
                    ["tools", "Tools", "Search the web, read pages and run commands in a project folder. Each step asks first."],
                    ["act", "Act", "Do it on the desktop. The next message goes to the desktop assistant, which asks before changing anything."],
                  ] as const).map(([id, title, sub]) => (
                    <button key={id} className={`hx-menu-item${modeState.mode === id ? " current" : ""}`} role="menuitemradio" aria-checked={modeState.mode === id} data-test={`menu-mode-${id}`} disabled={modeState.busy && id !== "act" && modeState.mode !== "act"}
                      onClick={() => { setMenu(null); window.dispatchEvent(new CustomEvent("mobius-set-mode", { detail: id })); }}>
                      <span className="mi-text"><span className="mi-title">{title}</span><span className="mi-sub">{sub}</span></span>
                      {modeState.mode === id ? <span className="mi-check">✓</span> : null}
                    </button>
                  ))}
                  <div className="hx-menu-sep" />
                  <button className="hx-menu-item" data-test="menu-mode-folder" onClick={() => { setMenu(null); window.dispatchEvent(new CustomEvent("mobius-set-mode", { detail: "folder" })); }}>
                    <span className="mi-text"><span className="mi-title">Project folder…</span><span className="mi-sub">{modeState.folder || "None: web only"}</span></span>
                  </button>
                </>
              ) : menu.kind === "model" ? (
                <>
                  <div className="hx-menu-label">Model</div>
                  {picker?.route && picker.route.backend !== "local" ? (
                    <button className="hx-menu-item current" role="menuitemradio" aria-checked data-test="menu-model-remote" onClick={() => setMenu(null)}>
                      <span className="mi-text"><span className="mi-title">{picker.route.model || "Auto"}</span><span className="mi-sub">{picker.route.label || picker.route.backend} · not on this computer</span></span>
                      <span className="mi-check">✓</span>
                    </button>
                  ) : null}
                  <button className={`hx-menu-item${picker?.route?.backend === "local" && !picker.route.model ? " current" : ""}`} role="menuitemradio" aria-checked={picker?.route?.backend === "local" && !picker.route.model} data-test="menu-model-auto" onClick={() => useLocalModel("")}>
                    <span className="mi-text"><span className="mi-title">Auto</span><span className="mi-sub">The service picks, and keeps the model already loaded</span></span>
                    {picker?.route?.backend === "local" && !picker.route.model ? <span className="mi-check">✓</span> : null}
                  </button>
                  {(picker?.models ?? []).map((id) => {
                    const info = picker?.cards[id];
                    const on = picker?.route?.backend === "local" && picker.route.model === id;
                    return (
                      <button key={id} className={`hx-menu-item${on ? " current" : ""}`} role="menuitemradio" aria-checked={on} data-test={`menu-model-${id}`} onClick={() => useLocalModel(id)}>
                        <span className="mi-text">
                          <span className="mi-title">{info?.title || id}</span>
                          <span className="mi-sub">{info ? `${(info.bytes / 1e9).toFixed(1)} GB${info.vision ? " · sees pictures" : ""}${info.tasks.includes("code") ? " · code" : ""} · ` : ""}{info?.loaded ? "ready now" : "loads on first use"}{info?.speed ? ` · ${Math.round(info.speed)} tok/s here` : ""}{info?.fit === "cpu" ? " · CPU, slower" : info?.fit === "no" ? " · too big" : ""}</span>
                        </span>
                        {on ? <span className="mi-check">✓</span> : null}
                      </button>
                    );
                  })}
                  {picker?.cloud ? (
                    <>
                      <div className="hx-menu-sep" />
                      <div className="hx-menu-label">OpenRouter <span className="hx-menu-hint">leaves this computer</span></div>
                      {picker.cloud.has_key ? (
                        <>
                          {picker.cloud.models.slice(0, 8).map((id) => {
                            const on = picker.route?.backend === "openrouter" && picker.route.model === id;
                            return (
                              <button key={id} className={`hx-menu-item${on ? " current" : ""}`} role="menuitemradio" aria-checked={on} data-test={`menu-cloud-${id}`} onClick={() => useCloudModel(id)}>
                                <span className="mi-text"><span className="mi-title">{id.split("/").pop()}</span><span className="mi-sub">{id.split("/")[0]} · cloud</span></span>
                                {on ? <span className="mi-check">✓</span> : null}
                              </button>
                            );
                          })}
                          <button className="hx-menu-item" data-test="menu-cloud-more" onClick={() => { setMenu(null); window.dispatchEvent(new CustomEvent("mobius-open-models", { detail: { section: "cloud" } })); }}>
                            <span className="mi-text"><span className="mi-title">More cloud models…</span><span className="mi-sub">Search the list and choose what appears here</span></span>
                          </button>
                        </>
                      ) : (
                        <button className="hx-menu-item" data-test="menu-cloud-setup" onClick={() => { setMenu(null); window.dispatchEvent(new CustomEvent("mobius-open-models")); }}>
                          <span className="mi-text"><span className="mi-title">Set up OpenRouter…</span><span className="mi-sub">Add an API key to use cloud models, one click each</span></span>
                        </button>
                      )}
                    </>
                  ) : null}
                  {picker?.route?.own ? (
                    <button className="hx-menu-item" data-test="menu-model-default" onClick={() => { setMenu(null); invoke("chat_use_default_route").then(() => window.dispatchEvent(new CustomEvent("mobius-route-changed")), report); }}>
                      <span className="mi-text"><span className="mi-title">Use the default model</span><span className="mi-sub">This chat follows your Settings again</span></span>
                    </button>
                  ) : null}
                  <div className="hx-menu-sep" />
                  <button className="hx-menu-item" data-test="menu-model-more" onClick={() => { setMenu(null); openModelSheet(); }}><span className="mi-text"><span className="mi-title">Other backends and custom ids…</span></span></button>
                  <button className="hx-menu-item" data-test="menu-model-routing" onClick={() => { setMenu(null); window.dispatchEvent(new CustomEvent("mobius-open-models")); }}><span className="mi-text"><span className="mi-title">Settings…</span></span></button>
                </>
              ) : menu.kind === "persona" ? (
                <>
                  <div className="hx-menu-label">Persona</div>
                  {store.personas.map((p, index) => (
                    <button key={p.id} className={`hx-menu-item${p.id === store.active ? " current" : ""}`} role="menuitemradio" aria-checked={p.id === store.active} data-test={`menu-persona-${index}`}
                      onClick={() => { setMenu(null); if (p.id !== store.active) invoke("personas_set_active", { id: p.id }).catch(report); }}>
                      <span className="mi-face" style={{ ["--dot-hue" as string]: String(hue(p.id)) }}>{p.icon ? <Icon name={p.icon} /> : initials(p.name)}</span>
                      <span className="mi-text"><span className="mi-title">{p.name}</span>{p.description ? <span className="mi-sub">{p.description}</span> : null}</span>
                      {p.id === store.active ? <span className="mi-check">✓</span> : null}
                    </button>
                  ))}
                  <div className="hx-menu-sep" />
                  <button className="hx-menu-item" data-test="menu-persona-manage" onClick={() => { setMenu(null); openPersona(false); }}><span className="mi-text"><span className="mi-title">Edit “{(store.personas.find((p) => p.id === store.active) ?? store.personas[0])?.name}”…</span></span></button>
                  <button className="hx-menu-item" data-test="menu-persona-new" onClick={() => { setMenu(null); openPersona(true); }}><span className="mi-text"><span className="mi-title">New persona…</span></span></button>
                </>
              ) : (
                <>
                  <div className="hx-menu-label">Style for this chat</div>
                  {[{ id: "", label: "None", builtin: true }, ...(lab?.styles ?? [])].map((style) => (
                    <button key={style.id || "none"} className={`hx-menu-item${(lab?.style ?? "") === style.id ? " current" : ""}`} role="menuitemradio" aria-checked={(lab?.style ?? "") === style.id} data-test={`menu-style-${style.id || "none"}`}
                      onClick={() => { setMenu(null); invoke("chat_set_style", { style: style.id }).catch(report); }}>
                      <span className="mi-text"><span className="mi-title">{style.label}</span></span>
                      {(lab?.style ?? "") === style.id ? <span className="mi-check">✓</span> : null}
                    </button>
                  ))}
                </>
              )}
            </div>,
            document.body,
          )
        : null}

      <Sheet id="edit-dialog" test="edit-dialog" open={sheet === "edit"} onClose={closeSheet}>
        <form
          className="sheet-body"
          onSubmit={(event) => {
            event.preventDefault();
            const value = editText.trim();
            if (!value) { report("Write something to send."); return; }
            closeSheet();
            hooksRef.current.edited(editIndex, value);
          }}
        >
          <header className="sheet-head">
            <h3>Edit message</h3>
            <p className="sheet-note">Later replies in this chat will be removed and a new answer will be generated.</p>
          </header>
          <div className="sheet-scroll">
            <label className="sheet-field">Message
              <textarea id="edit-text" data-test="edit-text" rows={8} maxLength={4000} required value={editText} onChange={(event) => setEditText(event.target.value)} />
            </label>
          </div>
          <div className="sheet-actions">
            <button type="button" onClick={closeSheet}>Cancel</button>
            <button type="submit" className="primary" data-test="edit-save">Send</button>
          </div>
        </form>
      </Sheet>

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
            const name = project.trim();
            invoke("chat_set_project", { project: name })
              .then(() => (name && lab?.experiments.project_instructions ? invoke("lab_set_project_instructions", { project: name, text: projectText }) : undefined))
              .then(closeSheet)
              .catch(report);
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
            {lab?.experiments.project_instructions ? (
              <label className="sheet-field">Instructions for chats in this project
                <textarea id="project-instructions" data-test="project-instructions" rows={5} maxLength={2000} placeholder="Optional. Applied to every chat in this project, after the persona." value={projectText} onChange={(event) => setProjectText(event.target.value)} />
              </label>
            ) : null}
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
            <div className="sheet-field">Icon
              <div className="icon-grid" data-test="persona-icons">
                {ICONS.map((name) => (
                  <button key={name} type="button" className={`icon-choice${draft.icon === name ? " current" : ""}`} data-test={`persona-icon-${name}`} aria-label={name} onClick={() => setDraft({ ...draft, icon: name })}>
                    <Icon name={name} />
                  </button>
                ))}
              </div>
            </div>
            <label className="sheet-field">Description
              <input id="persona-edit-description" data-test="persona-edit-description" type="text" maxLength={120} placeholder="What this persona is for" value={draft.description} onChange={(event) => setDraft({ ...draft, description: event.target.value })} />
            </label>
            <label className="sheet-field">System prompt
              <textarea id="persona-edit-prompt" data-test="persona-edit-prompt" rows={6} maxLength={4000} placeholder="Optional. Empty uses the default Mobius prompt." value={draft.system_prompt} onChange={(event) => setDraft({ ...draft, system_prompt: event.target.value })} />
            </label>
            {scanned.length ? (
              <div className="sheet-warning" data-test="persona-scan" role="status">
                {scanned.map((warning) => <div key={warning}>⚠ {warning}</div>)}
              </div>
            ) : null}
            <label className="sheet-field">Starter prompts (one per line, up to 4)
              <textarea id="persona-edit-starters" data-test="persona-edit-starters" rows={3} value={draft.starters.join("\n")} onChange={(event) => setDraft({ ...draft, starters: event.target.value.split("\n") })} />
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
            <button id="persona-new" type="button" data-test="persona-new" onClick={() => edit({ id: newPersonaId(), name: "New persona", system_prompt: "", task: "chat", backend: "", model: "", icon: "sparkle", description: "", starters: [] })}>New</button>
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
              {models.map((id) => {
                const info = backend === "local" ? shelf[id] : undefined;
                return (
                  <button key={id} type="button" className={`model-option${custom === id ? " active" : ""}`} data-test={`model-option-${id}`} onClick={() => setCustom(id)}>
                    {info ? (
                      <span className="model-option-rich">
                        <span className="model-option-title">{info.title}{info.loaded ? <span className="lib-loaded"> ● ready</span> : null}</span>
                        <span className="model-option-sub">{(info.bytes / 1e9).toFixed(1)} GB{info.vision ? " · sees pictures" : ""}{info.tasks.includes("code") ? " · code" : ""}</span>
                      </span>
                    ) : id}
                  </button>
                );
              })}
            </div>
            <p className="sheet-note">
              Want more models? <a href="#" data-test="model-browse" onClick={(event) => { event.preventDefault(); closeSheet(); window.dispatchEvent(new CustomEvent("mobius-open-models")); }}>Browse the library</a>
            </p>
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
