import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

declare function icon(name: string): SVGElement;
function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}

type Source = "local" | "lan" | "openrouter";
type Section = "overview" | "local" | "lan" | "cloud" | "generation" | "advanced";
type Task = "chat" | "act" | "code" | "summarize" | "vision";

const SOURCES: { id: Source; label: string; short: string }[] = [
  { id: "local", label: "My computer", short: "This PC" },
  { id: "lan", label: "LAN", short: "LAN" },
  { id: "openrouter", label: "Cloud", short: "Cloud" },
];
const TASKS: { id: Task; kicker: string; title: string; locked: boolean }[] = [
  { id: "chat", kicker: "Talk", title: "Chat", locked: false },
  { id: "act", kicker: "Desktop", title: "Act", locked: true },
  { id: "code", kicker: "Dev", title: "Code", locked: false },
  { id: "summarize", kicker: "Quick", title: "Summarize", locked: false },
  { id: "vision", kicker: "Sight", title: "Vision", locked: false },
];
const SECTIONS: { id: Section; label: string }[] = [
  { id: "overview", label: "Overview" },
  { id: "local", label: "My computer" },
  { id: "lan", label: "LAN" },
  { id: "cloud", label: "Cloud" },
  { id: "generation", label: "Generation" },
  { id: "advanced", label: "Advanced" },
];
const OPENROUTER = "https://openrouter.ai/api/v1";

interface Profile { backend: string; model: string; }
interface Backend { id: string; kind: string; base_url: string; label: string; api_key_ref: string; }
interface Routing {
  backends?: Backend[];
  profiles?: Record<string, Profile>;
  allow_remote_on_battery_saver?: boolean;
  talk?: { temperature?: number; max_tokens?: number; remote_timeout_secs?: number };
  [key: string]: unknown;
}
interface PageInfo {
  config_path: string;
  local_model_count: number;
  inference_ready: boolean;
  has_cloud_key: boolean;
  lan_configured: boolean;
  cloud_configured: boolean;
}
interface Draft {
  profiles: Record<Task, Profile>;
  lanUrl: string;
  cloudUrl: string;
  battery: boolean;
  temperature: number;
  maxTokens: number;
  timeoutSecs: number;
}
interface Probe { text: string; ok: boolean; }

interface Progress { phase: "downloading" | "verifying" | "failed"; done: number; total: number; error: string; }
interface Card {
  id: string; file: string; title: string; maker: string; blurb: string; tasks: string[]; vision: boolean; license: string;
  bytes: number; memory_gb: number; installed: boolean; loaded: boolean; in_catalog: boolean; progress: Progress | null;
}

function size(bytes: number): string {
  if (bytes >= 1e9) return `${(bytes / 1e9).toFixed(1)} GB`;
  return `${Math.max(1, Math.round(bytes / 1e6))} MB`;
}

const TASK_WORD: Record<string, string> = { chat: "Chat", code: "Code", vision: "Pictures", summarize: "Summaries", act: "Act", speech: "Speech", image: "Image generation" };

/** The library: models installed on this computer (what each is used for, which is loaded) and ones that can be downloaded. */
function Library({ cards, draft, needle, onAssign, report }: {
  cards: Card[]; draft: Draft; needle: string; onAssign: (task: Task, source: Source, model: string) => void; report: (e: unknown) => void;
}) {
  const match = (card: Card) => !needle || `${card.title} ${card.maker} ${card.blurb} ${card.tasks.join(" ")}`.toLowerCase().includes(needle);
  const usedFor = (card: Card) => TASKS.filter((t) => backendOf(draft.profiles[t.id], t) === "local" && draft.profiles[t.id].model === card.file);
  const installed = cards.filter((c) => c.installed && match(c));
  const available = cards.filter((c) => !c.installed && match(c));
  const act = (command: string, id: string) => invoke(command, { id }).catch(report);
  const usesAuto = TASKS.filter((t) => backendOf(draft.profiles[t.id], t) === "local" && !draft.profiles[t.id].model);
  return (
    <>
      <h4 className="models-h4">Installed</h4>
      <div className="lib-grid" data-test="library-installed">
        {installed.map((card) => {
          const uses = usedFor(card);
          const speech = card.tasks.includes("speech");
          const image = card.tasks.includes("image");
          const assignable = speech || image ? [] : TASKS.filter((t) => !(t.id === "vision" && !card.vision) && t.id !== "summarize");
          return (
            <article key={card.id} className={`lib-card${uses.length ? " in-use" : ""}`} data-test={`lib-card-${card.id}`}>
              <header>
                <div>
                  <div className="lib-title">{card.title}{card.loaded ? <span className="lib-loaded" title="In memory now"> ● ready</span> : null}</div>
                  <div className="lib-sub">{[card.maker, size(card.bytes), card.memory_gb ? `≈ ${card.memory_gb} GB memory` : ""].filter(Boolean).join(" · ")}</div>
                </div>
                <div className="lib-tags">
                  {card.tasks.map((t) => <span key={t} className="lib-tag">{TASK_WORD[t] ?? t}</span>)}
                  {card.vision ? <span className="lib-tag vision">Sees pictures</span> : null}
                </div>
              </header>
              <p className="lib-blurb">{card.blurb}</p>
              <div className="lib-uses">
                {speech ? <span className="lib-sub">Used by the microphone button in the composer.</span> : null}
                {image ? <span className="lib-sub">Used by Create image in the Mobius menu. {card.license}</span> : null}
                {assignable.map((t) => {
                  const active = uses.some((u) => u.id === t.id);
                  return (
                    <button key={t.id} type="button" className={`route-use-btn${active ? " active" : ""}`} data-test={`lib-use-${card.id}-${t.id}`}
                      onClick={() => onAssign(t.id, "local", card.file)}>
                      {active ? `Used for ${t.title}` : `Use for ${t.title}`}
                    </button>
                  );
                })}
                {card.in_catalog ? (
                  <button type="button" className="lib-remove" data-test={`lib-remove-${card.id}`}
                    onClick={() => { if (window.confirm(`Remove ${card.title} (${size(card.bytes)}) from this computer? You can download it again later.`)) act("library_remove", card.id); }}>
                    Remove
                  </button>
                ) : null}
              </div>
            </article>
          );
        })}
        {!installed.length ? <p className="route-model-empty" data-test="library-installed-empty">{needle ? "Nothing installed matches." : "No models installed yet. Pick one below to download."}</p> : null}
      </div>
      {usesAuto.length && installed.length ? (
        <p className="models-lead">{usesAuto.map((t) => t.title).join(", ")} use{usesAuto.length === 1 ? "s" : ""} Auto: the service picks the first model in the folder.</p>
      ) : null}

      <h4 className="models-h4">Available to download</h4>
      <div className="lib-grid" data-test="library-available">
        {available.map((card) => {
          const p = card.progress;
          const percent = p && p.total ? Math.min(100, Math.round((p.done / p.total) * 100)) : 0;
          return (
            <article key={card.id} className="lib-card" data-test={`lib-card-${card.id}`}>
              <header>
                <div>
                  <div className="lib-title">{card.title}</div>
                  <div className="lib-sub">{[card.maker, size(card.bytes), `≈ ${card.memory_gb} GB memory`, card.license].filter(Boolean).join(" · ")}</div>
                </div>
                <div className="lib-tags">
                  {card.tasks.map((t) => <span key={t} className="lib-tag">{TASK_WORD[t] ?? t}</span>)}
                  {card.vision ? <span className="lib-tag vision">Sees pictures</span> : null}
                </div>
              </header>
              <p className="lib-blurb">{card.blurb}</p>
              {p && p.phase !== "failed" ? (
                <div className="lib-progress" data-test={`lib-progress-${card.id}`}>
                  <div className="lib-bar"><div style={{ width: `${p.phase === "verifying" ? 100 : percent}%` }} className={p.phase === "verifying" ? "checking" : ""} /></div>
                  <span>{p.phase === "verifying" ? "Checking the download…" : `${size(p.done)} of ${size(p.total)} · ${percent}%`}</span>
                  <button type="button" onClick={() => act("library_cancel", card.id)} data-test={`lib-cancel-${card.id}`}>Cancel</button>
                </div>
              ) : (
                <div className="lib-uses">
                  {p?.phase === "failed" ? <span className="lib-error" data-test={`lib-error-${card.id}`}>{p.error}</span> : null}
                  <button type="button" className="primary" data-test={`lib-get-${card.id}`} onClick={() => act("library_download", card.id)}>
                    {p?.phase === "failed" ? "Try again" : `Get · ${size(card.bytes)}`}
                  </button>
                </div>
              )}
            </article>
          );
        })}
        {!available.length ? <p className="route-model-empty">{needle ? "Nothing available matches." : "Everything in the library is installed."}</p> : null}
      </div>
    </>
  );
}

export interface ModelsHooks {
  /** The page appeared or went away (the host element and the page's open class are the app's). */
  visible: (shown: boolean) => void;
  /** Saved: the chat route now is this. */
  saved: (backend: string, model: string) => void;
  localModels: (ids: string[]) => void;
  report: (message: unknown) => void;
}
export interface ModelsHandle { open: () => void; }

const sourceLabel = (id: string) => SOURCES.find((s) => s.id === id)?.short ?? (id || "This PC");
const backendOf = (profile: Profile, task: (typeof TASKS)[number]) => (task.locked ? "local" : profile.backend);

function cleanProfiles(routing: Routing): Record<Task, Profile> {
  const out = {} as Record<Task, Profile>;
  TASKS.forEach((task) => {
    const p = routing.profiles?.[task.id] ?? { backend: "local", model: "" };
    out[task.id] = {
      backend: task.locked ? "local" : (["local", "lan", "openrouter"].includes(p.backend) ? p.backend : "local"),
      model: p.model || "",
    };
  });
  return out;
}

function ModelGrid({ source, draft, models, loading, needle, ids, onAssign }: {
  source: Source;
  draft: Draft;
  models: string[];
  loading: boolean;
  needle: string;
  ids: { grid: string; empty: string; test: { grid: string; empty?: string } };
  onAssign: (task: Task, source: Source, model: string) => void;
}) {
  const shown = needle ? models.filter((id) => id.toLowerCase().includes(needle)) : models;
  const badgesFor = (id: string) =>
    TASKS.filter((task) => {
      const profile = draft.profiles[task.id];
      return backendOf(profile, task) === source && profile.model === id;
    }).map((task) => task.title);
  const autoSelected = TASKS.some((task) => {
    const profile = draft.profiles[task.id];
    return backendOf(profile, task) === source && !profile.model;
  });
  const meta = source === "local" ? "Local GGUF" : source === "lan" ? "LAN · OpenAI-compatible" : "OpenRouter";
  const emptyText = loading && !shown.length
    ? "Loading models…"
    : source === "local"
      ? "No local GGUFs yet. Inference lists them when available."
      : source === "lan"
        ? "Set a LAN URL, then Refresh / Test connection."
        : "Add an API key, then Test connection (suggestions appear if catalog is offline).";
  const showEmpty = loading ? !shown.length : shown.length === 0;
  const uses = (model: string, badges: string[]) =>
    TASKS.filter((task) => !(task.locked && source !== "local")).map((task) => {
      const profile = draft.profiles[task.id];
      const active = model === "" ? backendOf(profile, task) === source && !profile.model : badges.includes(task.title);
      return (
        <button key={task.id} type="button" className={`route-use-btn${active ? " active" : ""}`} onClick={(event) => { event.stopPropagation(); onAssign(task.id, source, model); }}>
          Use for {task.title}
        </button>
      );
    });
  return (
    <>
      <div id={ids.grid} className="route-model-grid" data-test={ids.test.grid}>
        {loading && !shown.length ? null : (
          <>
            <button type="button" className={`route-model-card${autoSelected ? " selected" : ""}`} onClick={() => onAssign("chat", source, "")}>
              <div>
                <div className="route-model-name">Auto (default)</div>
                <div className="route-model-meta">{source === "local" ? "Inference service default" : "Provider default"}</div>
              </div>
              <div className="route-model-actions">{uses("", [])}</div>
            </button>
            {shown.map((id) => {
              const badges = badgesFor(id);
              return (
                <button key={id} type="button" className={`route-model-card${badges.length ? " selected" : ""}`} onClick={() => onAssign("chat", source, id)}>
                  <div>
                    <div className="route-model-name">{id}</div>
                    <div className="route-model-meta">{meta}</div>
                  </div>
                  {badges.length ? <div className="route-model-badges">{badges.map((b) => <span key={b} className="route-badge">{b}</span>)}</div> : null}
                  <div className="route-model-actions">{uses(id, badges)}</div>
                </button>
              );
            })}
          </>
        )}
      </div>
      <p id={ids.empty} className="route-model-empty" data-test={ids.test.empty} hidden={!showEmpty}>{showEmpty ? emptyText : ""}</p>
    </>
  );
}

/** The Models & routing page. Rust owns the routing file and the secret store; this edits a draft and sends the whole routing back on Save.
 * An API key is sent once to the secret store and never kept in the page after that. */
export function ModelsPage({ register, hooks }: { register: (handle: ModelsHandle) => void; hooks: ModelsHooks }) {
  const [open, setOpen] = useState(false);
  const [draft, setDraft] = useState<Draft | null>(null);
  const [info, setInfo] = useState<PageInfo | null>(null);
  const [section, setSection] = useState<Section>("overview");
  const [dirty, setDirty] = useState(false);
  const [cache, setCache] = useState<Record<Source, string[]>>({ local: [], lan: [], openrouter: [] });
  const [loading, setLoading] = useState(false);
  const [library, setLibrary] = useState<Card[]>([]);
  const [needle, setNeedle] = useState("");
  const [apiKey, setApiKey] = useState("");
  const [hasKey, setHasKey] = useState<boolean | null>(null);
  const [experiments, setExperiments] = useState<Record<string, boolean> | null>(null);
  const [lanProbe, setLanProbe] = useState<Probe | null>(null);
  const [cloudProbe, setCloudProbe] = useState<Probe | null>(null);
  const sourceRef = useRef<Source>("local");
  const hooksRef = useRef(hooks);
  hooksRef.current = hooks;
  const report = (error: unknown) => hooksRef.current.report(error);

  const loadModels = useCallback(async (source: Source) => {
    setLoading(true);
    try {
      if (source === "local") {
        const models = await invoke<string[]>("routing_list_models");
        const list = Array.isArray(models) ? models : [];
        setCache((old) => ({ ...old, local: list }));
        hooksRef.current.localModels(list);
      } else {
        if (source === "openrouter") await invoke("routing_ensure_openrouter").catch(() => undefined);
        const models = await invoke<string[]>("routing_list_remote_models", { backendId: source });
        setCache((old) => ({ ...old, [source]: Array.isArray(models) ? models : [] }));
      }
    } catch {
      /* the grid shows its empty text */
    } finally {
      setLoading(false);
    }
  }, []);

  const refreshLibrary = useCallback(() => {
    invoke<Card[]>("library_list").then((cards) => setLibrary(Array.isArray(cards) ? cards : []), () => undefined);
  }, []);

  useEffect(() => {
    if (!open) return undefined;
    // Downloads report their own progress; a finished one changes what is installed, so the list is fetched again then.
    const tauri = (window as unknown as { __TAURI__?: { event?: { listen: (name: string, cb: (e: { payload: Card[] }) => void) => Promise<() => void> } } }).__TAURI__;
    let stop: (() => void) | undefined;
    tauri?.event?.listen("library-changed", (event) => {
      setLibrary(event.payload);
      if (event.payload.every((c) => !c.progress)) loadModels("local");
    }).then((off) => { stop = off; });
    return () => stop?.();
  }, [open, loadModels]);

  const refreshKeyStatus = useCallback(async () => {
    const status = await invoke<{ has_key: boolean }>("routing_secret_status", { apiKeyRef: "openrouter" });
    setHasKey(!!status?.has_key);
  }, []);

  const show = useCallback((next: Section) => {
    setSection(next);
    const source: Source | null = next === "local" ? "local" : next === "lan" ? "lan" : next === "cloud" ? "openrouter" : null;
    if (source) {
      sourceRef.current = source;
      loadModels(source);
      if (source === "local") refreshLibrary();
    }
  }, [loadModels, refreshLibrary]);

  useEffect(() => {
    register({
      open: async () => {
        try {
          await invoke("routing_ensure_openrouter").catch(() => undefined);
          const routing = await invoke<Routing>("routing_get");
          const lan = routing.backends?.find((b) => b.id === "lan");
          const cloud = routing.backends?.find((b) => b.id === "openrouter");
          const talk = routing.talk ?? {};
          const next: Draft = {
            profiles: cleanProfiles(routing),
            lanUrl: lan?.base_url ?? "",
            cloudUrl: cloud?.base_url || OPENROUTER,
            battery: !!routing.allow_remote_on_battery_saver,
            temperature: talk.temperature ?? 0.6,
            maxTokens: talk.max_tokens ?? 1024,
            timeoutSecs: talk.remote_timeout_secs ?? 120,
          };
          setDraft(next);
          setApiKey("");
          setNeedle("");
          setLanProbe(null);
          setCloudProbe(null);
          setSection("overview");
          sourceRef.current = next.profiles.chat.backend as Source;
          await refreshKeyStatus().catch(() => setHasKey(null));
          setInfo(await invoke<PageInfo>("routing_page_info").catch(() => null));
          invoke<{ experiments: Record<string, boolean> }>("lab_get").then((view) => setExperiments(view.experiments), () => setExperiments(null));
          setDirty(false);
          setOpen(true);
          hooksRef.current.visible(true);
          loadModels("local");
          refreshLibrary();
        } catch (error) {
          report(error);
        }
      },
    });
  }, [register, loadModels, refreshKeyStatus, refreshLibrary]);

  const close = () => {
    setOpen(false);
    setDirty(false);
    hooksRef.current.visible(false);
  };
  const leave = () => {
    if (dirty && !window.confirm("Discard unsaved model changes?")) return;
    close();
  };
  const change = (patch: Partial<Draft>) => {
    setDraft((old) => (old ? { ...old, ...patch } : old));
    setDirty(true);
  };
  const assign = (task: Task, source: Source, model: string) => {
    if (task === "act" && source !== "local") {
      report("Act must stay on this computer.");
      return;
    }
    setDraft((old) => (old ? { ...old, profiles: { ...old.profiles, [task]: { backend: task === "act" ? "local" : source, model } } } : old));
    setDirty(true);
  };
  const pickBackend = (task: Task, source: Source) => {
    if (!draft) return;
    setDraft({ ...draft, profiles: { ...draft.profiles, [task]: { ...draft.profiles[task], backend: source } } });
    setDirty(true);
    if (source === "lan" && !draft.lanUrl) show("lan");
    if (source === "openrouter") show("cloud");
  };

  const save = async () => {
    if (!draft) return;
    const lanUrl = draft.lanUrl.trim();
    const cloudUrl = draft.cloudUrl.trim() || OPENROUTER;
    for (const task of ["chat", "code"] as Task[]) {
      if (draft.profiles[task].backend === "lan" && !lanUrl) {
        report(`Set a LAN URL before routing ${task === "chat" ? "Chat" : "Code"} there.`);
        show("lan");
        return;
      }
    }
    try {
      if (apiKey.trim()) await invoke("routing_set_secret", { apiKeyRef: "openrouter", secret: apiKey.trim() });
      setApiKey("");
      const routing = await invoke<Routing>("routing_get");
      routing.profiles = routing.profiles ?? {};
      routing.backends = routing.backends ?? [];
      TASKS.forEach((task) => {
        const existing = routing.profiles![task.id] ?? { backend: "local", model: "" };
        routing.profiles![task.id] = { ...existing, backend: draft.profiles[task.id].backend || "local", model: draft.profiles[task.id].model || "" };
      });
      routing.profiles.act.backend = "local";
      routing.allow_remote_on_battery_saver = draft.battery;
      routing.talk = { temperature: draft.temperature, max_tokens: draft.maxTokens, remote_timeout_secs: draft.timeoutSecs };
      const lan = routing.backends.find((b) => b.id === "lan");
      if (lanUrl) {
        if (!lan) routing.backends.push({ id: "lan", kind: "openai", base_url: lanUrl, label: "LAN", api_key_ref: "" });
        else Object.assign(lan, { kind: "openai", base_url: lanUrl, label: lan.label || "LAN", api_key_ref: lan.api_key_ref ?? "" });
      }
      const cloud = routing.backends.find((b) => b.id === "openrouter");
      if (!cloud) routing.backends.push({ id: "openrouter", kind: "openai", base_url: cloudUrl, label: "OpenRouter", api_key_ref: "openrouter" });
      else Object.assign(cloud, { kind: "openai", base_url: cloudUrl, label: cloud.label || "OpenRouter", api_key_ref: cloud.api_key_ref || "openrouter" });
      if (!routing.backends.some((b) => b.id === "local")) {
        routing.backends.unshift({ id: "local", kind: "local", base_url: "", label: "On this computer", api_key_ref: "" });
      }
      await invoke("routing_save", { routing });
      hooksRef.current.saved(draft.profiles.chat.backend || "local", draft.profiles.chat.model || "");
      close();
    } catch (error) {
      report(error);
    }
  };

  const probe = async (which: "lan" | "cloud") => {
    if (!draft) return;
    const set = which === "lan" ? setLanProbe : setCloudProbe;
    try {
      const routing = await (async () => {
        if (which === "cloud" && apiKey.trim()) await invoke("routing_set_secret", { apiKeyRef: "openrouter", secret: apiKey.trim() });
        if (which === "cloud") await invoke("routing_ensure_openrouter");
        return invoke<Routing>("routing_get");
      })();
      routing.backends = routing.backends ?? [];
      const id = which === "lan" ? "lan" : "openrouter";
      const url = which === "lan" ? draft.lanUrl.trim() : draft.cloudUrl.trim() || OPENROUTER;
      if (which === "lan" && !url) {
        report("Enter a LAN URL first.");
        return;
      }
      const backend = routing.backends.find((b) => b.id === id);
      if (!backend) routing.backends.push({ id, kind: "openai", base_url: url, label: which === "lan" ? "LAN" : "OpenRouter", api_key_ref: which === "lan" ? "" : "openrouter" });
      else Object.assign(backend, { kind: "openai", base_url: url });
      await invoke("routing_save", { routing });
      const message = await invoke<string>("routing_probe", { backendId: id });
      set({ text: message, ok: true });
      if (which === "cloud") await refreshKeyStatus();
      await loadModels(id);
    } catch (error) {
      set({ text: String(error), ok: false });
    }
  };

  if (!open || !draft) return null;
  const chat = draft.profiles.chat;
  const cards = [
    { kicker: "Inference", value: info?.inference_ready ? "Ready" : "Offline", detail: `${info?.local_model_count ?? 0} local GGUF(s)`, ok: !!info?.inference_ready },
    { kicker: "Chat route", value: sourceLabel(chat.backend), detail: library.find((c) => c.file === chat.model)?.title || chat.model || "Auto model", ok: true, always: true },
    { kicker: "LAN", value: info?.lan_configured ? "Configured" : "Not set", detail: draft.lanUrl || "No endpoint", ok: !!info?.lan_configured },
    { kicker: "Cloud", value: info?.has_cloud_key ? "Key saved" : "No key", detail: draft.cloudUrl || "OpenRouter", ok: !!info?.has_cloud_key },
  ];

  return (
    <>
      <header className="models-page-head" data-tauri-drag-region>
        <button id="models-back" className="icon-btn" data-test="models-back" type="button" title="Back to chat" aria-label="Back to chat" onClick={leave}><Icon name="chevron-left" /></button>
        <div className="models-page-titles">
          <h2>Models &amp; routing</h2>
          <p>Where Talk, Act, and Code run — and how replies are generated.</p>
        </div>
        <div className="models-page-actions">
          <button id="models-discard" type="button" data-test="models-discard" onClick={leave}>Discard</button>
          <button id="models-save" className="primary" type="button" data-test="models-save" onClick={save}>Save</button>
        </div>
      </header>
      <div className="models-page-body">
        <nav className="models-nav" aria-label="Models sections">
          {SECTIONS.map((entry) => (
            <button key={entry.id} type="button" className={`models-nav-btn${entry.id === section ? " active" : ""}`} data-models-section={entry.id} data-test={`models-nav-${entry.id}`} onClick={() => show(entry.id)}>
              {entry.label}
            </button>
          ))}
        </nav>
        <div className="models-panels">
          <section className={`models-panel${section === "overview" ? " active" : ""}`} data-models-panel="overview" data-test="models-panel-overview" hidden={section !== "overview"}>
            <div id="models-info-cards" className="models-info-cards" data-test="models-info-cards">
              {cards.map((card) => (
                <div key={card.kicker} className={`models-info-card${card.ok ? " ok" : ""}${!card.ok && !card.always ? " warn" : ""}`}>
                  <div className="kicker">{card.kicker}</div>
                  <div className="value">{card.value}</div>
                  <div className="detail">{card.detail}</div>
                </div>
              ))}
            </div>
            <h3 className="models-h3">Task map</h3>
            <p className="models-lead">Each Helix task can use a different brain. Act is locked local so desktop tools never leave this machine.</p>
            <div id="route-tasks" className="route-tasks" aria-label="Task destinations">
              {TASKS.map((task) => {
                const profile = draft.profiles[task.id];
                return (
                  <article key={task.id} className={`route-task${task.locked ? " locked" : ""}`} data-task={task.id}>
                    <div className="route-task-kicker">{task.kicker}</div>
                    <div className="route-task-title">{task.title}</div>
                    <div className="route-task-dest">{sourceLabel(task.locked ? "local" : profile.backend)}</div>
                    <div className="route-task-model">{library.find((c) => c.file === profile.model)?.title || profile.model || "Auto (default)"}</div>
                    {task.locked ? (
                      <p className="route-task-lock">Local only — tools stay on-device</p>
                    ) : (
                      <div className="route-task-picks">
                        {SOURCES.map((source) => (
                          <button key={source.id} type="button" className={`route-dest-btn${profile.backend === source.id ? " active" : ""}`} onClick={() => pickBackend(task.id, source.id)}>
                            {source.short}
                          </button>
                        ))}
                      </div>
                    )}
                  </article>
                );
              })}
            </div>
          </section>

          <section className={`models-panel${section === "local" ? " active" : ""}`} data-models-panel="local" data-test="models-panel-local" hidden={section !== "local"}>
            <h3 className="models-h3">Models on this computer</h3>
            <p className="models-lead">Everything runs here. Choose what each task uses; download more when you want them. Only one model sits in memory at a time, so switching takes a few seconds.</p>
            <div className="route-source-toolbar">
              <label className="route-search" htmlFor="route-model-search-local">
                <Icon name="search" />
                <input id="route-model-search-local" data-test="route-model-search" type="search" placeholder="Search models" autoComplete="off" value={needle} onChange={(event) => setNeedle(event.target.value.trim().toLowerCase())} />
              </label>
              <button id="route-refresh-models" type="button" data-test="route-refresh-models" onClick={() => { loadModels("local"); refreshLibrary(); }}>Refresh</button>
            </div>
            <Library cards={library} draft={draft} needle={needle} onAssign={assign} report={report} />
          </section>

          <section className={`models-panel${section === "lan" ? " active" : ""}`} data-models-panel="lan" data-test="models-panel-lan" hidden={section !== "lan"}>
            <h3 className="models-h3">LAN / OpenAI-compatible server</h3>
            <p className="models-lead">Point Mobius at Ollama, LM Studio, vLLM, or any <code>/v1</code> chat endpoint on your network.</p>
            <div className="models-form-card">
              <label className="sheet-field">Base URL
                <input id="route-lan-url" data-test="route-lan-url" type="url" placeholder="http://host:11434/v1" value={draft.lanUrl} onChange={(event) => change({ lanUrl: event.target.value })} />
              </label>
              <div className="models-form-row">
                <button id="route-probe-lan" type="button" data-test="route-probe-lan" onClick={() => probe("lan")}>Test connection</button>
                <span id="route-probe-lan-status" className={`models-inline-status${lanProbe ? (lanProbe.ok ? " ok" : " bad") : ""}`} data-test="route-probe-lan-status">{lanProbe?.text ?? ""}</span>
              </div>
            </div>
            <h4 className="models-h4">Models from server</h4>
            <ModelGrid source="lan" draft={draft} models={cache.lan} loading={loading && section === "lan"} needle="" onAssign={assign}
              ids={{ grid: "route-lan-model-grid", empty: "route-lan-model-empty", test: { grid: "route-lan-model-grid" } }} />
          </section>

          <section className={`models-panel${section === "cloud" ? " active" : ""}`} data-models-panel="cloud" data-test="models-panel-cloud" hidden={section !== "cloud"}>
            <h3 className="models-h3">Cloud (OpenRouter)</h3>
            <p className="models-lead">OpenAI-compatible cloud. API keys live in the Secret Service — never in transcripts or routing TOML. Remotes never get Act tools.</p>
            <div className="models-form-card">
              <label className="sheet-field">API URL
                <input id="route-cloud-url" data-test="route-cloud-url" type="url" placeholder={OPENROUTER} value={draft.cloudUrl} onChange={(event) => change({ cloudUrl: event.target.value })} />
              </label>
              <label className="sheet-field">API key
                <input id="route-cloud-key" data-test="route-cloud-key" type="password" autoComplete="off" placeholder="Paste to update; leave blank to keep" value={apiKey} onChange={(event) => { setApiKey(event.target.value); setDirty(true); }} />
              </label>
              <div className="models-form-row">
                <p id="route-cloud-key-status" className="sheet-note" data-test="route-cloud-key-status">{hasKey === null ? "" : hasKey ? "API key is saved securely." : "No API key saved yet."}</p>
                <button id="route-clear-key" type="button" data-test="route-clear-key" onClick={() => {
                  invoke("routing_clear_secret", { apiKeyRef: "openrouter" })
                    .then(() => { setApiKey(""); return refreshKeyStatus(); })
                    .then(() => invoke<PageInfo>("routing_page_info").then(setInfo))
                    .catch(report);
                }}>Clear key</button>
                <button id="route-probe-cloud" type="button" data-test="route-probe-cloud" onClick={() => probe("cloud")}>Test connection</button>
              </div>
              <span id="route-probe-cloud-status" className={`models-inline-status${cloudProbe ? (cloudProbe.ok ? " ok" : " bad") : ""}`} data-test="route-probe-cloud-status">{cloudProbe?.text ?? ""}</span>
            </div>
            <h4 className="models-h4">Catalog</h4>
            <ModelGrid source="openrouter" draft={draft} models={cache.openrouter} loading={loading && section === "cloud"} needle="" onAssign={assign}
              ids={{ grid: "route-cloud-model-grid", empty: "route-cloud-model-empty", test: { grid: "route-cloud-model-grid" } }} />
          </section>

          <section className={`models-panel${section === "generation" ? " active" : ""}`} data-models-panel="generation" data-test="models-panel-generation" hidden={section !== "generation"}>
            <h3 className="models-h3">Generation defaults</h3>
            <p className="models-lead">Applied to Talk and Code turns (local Inference and remote). Act still uses the local tool slot.</p>
            <div className="models-form-card">
              <label className="sheet-field">Temperature
                <div className="models-slider-row">
                  <input id="route-temperature" data-test="route-temperature" type="range" min={0} max={2} step={0.05} value={draft.temperature} onChange={(event) => change({ temperature: Number(event.target.value) })} />
                  <output id="route-temperature-value" htmlFor="route-temperature">{draft.temperature.toFixed(2)}</output>
                </div>
              </label>
              <label className="sheet-field">Max tokens
                <input id="route-max-tokens" data-test="route-max-tokens" type="number" min={64} max={8192} step={64} value={draft.maxTokens} onChange={(event) => change({ maxTokens: Number(event.target.value) })} />
              </label>
              <label className="sheet-field">Remote timeout (seconds)
                <input id="route-timeout" data-test="route-timeout" type="number" min={15} max={600} step={15} value={draft.timeoutSecs} onChange={(event) => change({ timeoutSecs: Number(event.target.value) })} />
              </label>
            </div>
          </section>

          <section className={`models-panel${section === "advanced" ? " active" : ""}`} data-models-panel="advanced" data-test="models-panel-advanced" hidden={section !== "advanced"}>
            <h3 className="models-h3">Experiments</h3>
            <p className="models-lead">Things being tried in Mobius. Each can be switched off to compare with how it worked before.</p>
            <div className="models-form-card" data-test="experiments">
              {([
                ["persona_chip", "Persona chip in the composer", "Switch persona from the composer; replaces the rail under the title."],
                ["style_overlays", "Style overlays", "Concise, Plain language, Teacher… per chat, from the composer or /style."],
                ["project_instructions", "Project instructions", "Instructions attached to a project and applied to its chats."],
                ["persona_starters", "Persona starter prompts", "An empty chat shows the persona's own example prompts."],
              ] as const).map(([key, title, note]) => (
                <label key={key} className="sheet-check experiment">
                  <input type="checkbox" data-test={`experiment-${key}`} checked={!!experiments?.[key]} disabled={!experiments}
                    onChange={(event) => {
                      const on = event.target.checked;
                      setExperiments((old) => (old ? { ...old, [key]: on } : old));
                      invoke("lab_set_experiment", { key, on }).catch(report);
                    }} />
                  <span><strong>{title}</strong><br /><span className="models-lead" style={{ margin: 0 }}>{note}</span></span>
                </label>
              ))}
            </div>
            <h3 className="models-h3" style={{ marginTop: 22 }}>Advanced</h3>
            <div className="models-form-card">
              <label className="sheet-check">
                <input id="route-battery" data-test="route-battery" type="checkbox" checked={draft.battery} onChange={(event) => change({ battery: event.target.checked })} />
                <span>Allow remote models when battery saver is on</span>
              </label>
              <div className="models-kv"><span>Config file</span><code id="route-config-path" data-test="route-config-path">{info?.config_path ?? ""}</code></div>
              <p className="models-lead">Profiles: chat, act, summarize, vision, code. Only Act is forced local.</p>
              <button id="route-reset-defaults" type="button" data-test="route-reset-defaults" onClick={() => change({ temperature: 0.6, maxTokens: 1024, timeoutSecs: 120 })}>Reset generation defaults</button>
            </div>
          </section>
        </div>
      </div>
    </>
  );
}
