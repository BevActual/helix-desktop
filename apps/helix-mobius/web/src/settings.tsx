import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";
import { eligible, listOrder, reason, recommend, specialist, type Job, type ModelCard } from "./recommend";
import { CloudModels, type CloudView } from "./cloudmodels";
import { Icon, Picker, type Option } from "./settingsui";
import { HubFind } from "./hubfind";
import { KnowledgePanel } from "./knowledge";

type Source = "local" | "lan" | "openrouter";
type Section = "models" | "library" | "cloud" | "knowledge" | "providers" | "generation" | "experiments";
type Task = "chat" | "act" | "code" | "summarize" | "vision";

const SECTIONS: { id: Section; label: string; icon: string }[] = [
  { id: "models", label: "Models", icon: "sparkle" },
  { id: "library", label: "Library", icon: "grid" },
  { id: "cloud", label: "Cloud models", icon: "bolt" },
  { id: "knowledge", label: "Knowledge", icon: "folder" },
  { id: "providers", label: "Providers", icon: "wifi" },
  { id: "generation", label: "Generation", icon: "settings" },
  { id: "experiments", label: "Experiments", icon: "palette" },
];
const OPENROUTER = "https://openrouter.ai/api/v1";

interface JobInfo { id: Job; title: string; note: string; task?: Task; local?: boolean; }
/** What a person wants done, in their words. Each is run by one model, chosen here. */
const JOBS: JobInfo[] = [
  { id: "chat", title: "Chat", note: "Everyday questions, writing and ideas", task: "chat" },
  { id: "code", title: "Code", note: "Writing, explaining and fixing code", task: "code" },
  { id: "vision", title: "Pictures", note: "Reading pictures and screenshots you attach", task: "vision" },
  { id: "summarize", title: "Summaries", note: "Short digests and chat titles. A small, quick model is best", task: "summarize" },
  { id: "act", title: "Act", note: "Controls your desktop. Always runs on this computer", task: "act", local: true },
  { id: "speech", title: "Dictation", note: "Turns your voice into text with the microphone button" },
  { id: "image", title: "Image creation", note: "Makes pictures from a description" },
  { id: "embedding", title: "Meaning search", note: "Reads what a message means: suggests the right mode now, and will search your notes and files" },
];

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
interface Card extends ModelCard {
  maker: string; blurb: string; license: string; loaded: boolean; progress: Progress | null;
}

export interface SettingsHooks {
  /** The page appeared or went away (the host element and the page's open class are the app's). */
  visible: (shown: boolean) => void;
  /** The chat route changed: it now is this. */
  saved: (backend: string, model: string) => void;
  localModels: (ids: string[]) => void;
  report: (message: unknown) => void;
}
export interface SettingsHandle { open: (section?: string) => void; }

const FIT_WORD: Record<string, string> = { gpu: "Fits your graphics card", tight: "Tight on your graphics card", cpu: "Runs on the processor (slower)", no: "Too big for this computer" };
const TASK_WORD: Record<string, string> = { chat: "Chat", code: "Code", vision: "Pictures", summarize: "Summaries", act: "Act", speech: "Speech", image: "Image generation", embedding: "Meaning search", reasoning: "Thinks first" };

function size(bytes: number): string {
  if (bytes >= 1e9) return `${(bytes / 1e9).toFixed(1)} GB`;
  return `${Math.max(1, Math.round(bytes / 1e6))} MB`;
}

function FitTag({ card }: { card: Card }) {
  // Speech and meaning models run on the processor whatever the graphics card is, so how they fit a graphics card means nothing.
  if (card.tasks.includes("speech") || card.tasks.includes("embedding")) return <span className="lib-tag" data-test={`lib-fit-${card.id}`}>Runs on the processor</span>;
  if (!card.fit) return null;
  return <span className={`lib-tag fit-${card.fit}`} data-test={`lib-fit-${card.id}`} title="Worked out from this computer's graphics and system memory">{FIT_WORD[card.fit]}{card.speed ? ` · ${Math.round(card.speed)} tok/s here` : ""}</span>;
}

function cleanProfiles(routing: Routing): Record<Task, Profile> {
  const out = {} as Record<Task, Profile>;
  JOBS.filter((j) => j.task).forEach((job) => {
    const task = job.task as Task;
    const p = routing.profiles?.[task] ?? { backend: "local", model: "" };
    out[task] = {
      backend: job.local ? "local" : (["local", "lan", "openrouter"].includes(p.backend) ? p.backend : "local"),
      model: p.model || "",
    };
  });
  return out;
}

/** The backends the page manages always exist in the routing file, whatever else is in it. */
function ensureBackends(routing: Routing, lanUrl: string, cloudUrl: string) {
  routing.backends = routing.backends ?? [];
  if (!routing.backends.some((b) => b.id === "local")) {
    routing.backends.unshift({ id: "local", kind: "local", base_url: "", label: "On this computer", api_key_ref: "" });
  }
  const lan = routing.backends.find((b) => b.id === "lan");
  if (lanUrl) {
    if (!lan) routing.backends.push({ id: "lan", kind: "openai", base_url: lanUrl, label: "LAN", api_key_ref: "" });
    else Object.assign(lan, { kind: "openai", base_url: lanUrl, label: lan.label || "LAN", api_key_ref: lan.api_key_ref ?? "" });
  }
  const cloud = routing.backends.find((b) => b.id === "openrouter");
  if (!cloud) routing.backends.push({ id: "openrouter", kind: "openai", base_url: cloudUrl, label: "OpenRouter", api_key_ref: "openrouter" });
  else Object.assign(cloud, { kind: "openai", base_url: cloudUrl, label: cloud.label || "OpenRouter", api_key_ref: cloud.api_key_ref || "openrouter" });
}

function DownloadBar({ card, act }: { card: Card; act: (command: string, id: string) => void }) {
  const p = card.progress;
  if (!p || p.phase === "failed") return null;
  const percent = p.total ? Math.min(100, Math.round((p.done / p.total) * 100)) : 0;
  return (
    <div className="lib-progress" data-test={`lib-progress-${card.id}`}>
      <div className="lib-bar"><div style={{ width: `${p.phase === "verifying" ? 100 : percent}%` }} className={p.phase === "verifying" ? "checking" : ""} /></div>
      <span>{p.phase === "verifying" ? "Checking the download…" : `${size(p.done)} of ${size(p.total)} · ${percent}%`}</span>
      <button type="button" onClick={() => act("library_cancel", card.id)} data-test={`lib-cancel-${card.id}`}>Cancel</button>
    </div>
  );
}

/** One job: what it is for, which model does it, and, when nothing suitable is installed or a better one exists, what to get. */
function JobRow({ job, draft, cards, lan, cloud, hasKey, onAssign, onBrowse, onMoreCloud, act }: {
  job: JobInfo; draft: Draft; cards: Card[]; lan: string[]; cloud: { id: string; name: string }[]; hasKey: boolean | null;
  onAssign: (task: Task, backend: string, model: string) => void; onBrowse: (job: Job) => void; onMoreCloud: () => void; act: (command: string, id: string) => void;
}) {
  const suggestion = recommend(job.id, cards) as Card | null;
  const installed = cards.filter((c) => c.installed && eligible(job.id, c)).sort(listOrder);
  const profile = job.task ? draft.profiles[job.task] : null;

  let current = "";
  let label = "";
  let where = "";
  let chosen: Card | undefined;
  if (profile) {
    current = `${profile.backend}|${profile.model}`;
    chosen = profile.backend === "local" ? cards.find((c) => c.file === profile.model) : undefined;
    if (profile.backend === "local") {
      label = profile.model ? (chosen?.title ?? profile.model) : "Auto";
      where = "On this computer";
    } else {
      label = (profile.backend === "openrouter" ? cloud.find((m) => m.id === profile.model)?.name : "") || profile.model || "Provider default";
      where = profile.backend === "lan" ? "On your network" : "Leaves this computer";
    }
  }
  const options: Option[] = [];
  if (job.task) {
    options.push({ key: "local|", group: "", title: "Auto", detail: "The first model on this computer", backend: "local", model: "", test: `job-opt-${job.id}-auto` });
    installed.forEach((c) => options.push({ key: `local|${c.file}`, group: "On this computer", title: c.title, detail: [size(c.bytes), c.fit ? FIT_WORD[c.fit] : ""].filter(Boolean).join(" · "), backend: "local", model: c.file, test: `job-opt-${job.id}-${c.id}` }));
    if (!job.local) {
      lan.slice(0, 12).forEach((m) => options.push({ key: `lan|${m}`, group: "On your network", title: m, detail: "", backend: "lan", model: m, test: `job-opt-${job.id}-lan-${m}` }));
      if (hasKey) {
        // The pinned models, plus the one this job already uses if it is not pinned, then a way to the rest.
        const listed = cloud.some((m) => m.id === profile?.model) || profile?.backend !== "openrouter" || !profile?.model ? cloud : [...cloud, { id: profile.model, name: profile.model }];
        listed.forEach((m) => options.push({ key: `openrouter|${m.id}`, group: "Cloud · leaves this computer", title: m.name, detail: m.name === m.id ? "" : m.id, backend: "openrouter", model: m.id, test: `job-opt-${job.id}-cloud-${m.id}` }));
        options.push({ key: "more", group: "Cloud · leaves this computer", title: "More cloud models…", detail: "Search the whole OpenRouter list", backend: "__more", model: "", test: `job-opt-${job.id}-more` });
      }
    }
  }

  // What to offer under the row.
  // Under Auto with several models installed, say which one suits this job best; with one there is nothing to choose.
  const useIt = suggestion && suggestion.installed && job.task && profile && installed.length >= 2 && !profile.model ? suggestion : null;
  const missing = !job.task ? suggestion : (!installed.length ? suggestion : null);
  const ready = !job.task ? cards.find((c) => c.installed && eligible(job.id, c)) : undefined;

  return (
    <div className="set-row" data-test={`job-${job.id}`}>
      <div className="set-row-main">
        <div className="set-row-text">
          <div className="set-row-title">{job.title}</div>
          <div className="set-row-note">{job.note}</div>
        </div>
        <div className="set-row-control">
          {job.task ? (
            <>
              <Picker id={job.id} label={label} options={options} current={current} onPick={(o) => (o.backend === "__more" ? onMoreCloud() : onAssign(job.task as Task, o.backend, o.model))} />
              <div className="set-row-where" data-test={`job-where-${job.id}`}>
                {where}
                {chosen?.fit ? ` · ${FIT_WORD[chosen.fit].toLowerCase()}` : ""}
                {chosen?.speed ? ` · ${Math.round(chosen.speed)} tok/s here` : ""}
              </div>
            </>
          ) : ready ? (
            <div className="set-row-ready" data-test={`job-ready-${job.id}`}><Icon name="check" /> {ready.title} is ready</div>
          ) : null}
        </div>
      </div>
      {missing && !missing.installed ? (
        <div className="set-offer" data-test={`job-offer-${job.id}`}>
          <div className="set-offer-text">
            <div><strong>{missing.title}</strong> · {size(missing.bytes)}</div>
            <span>{reason(job.id, missing)}</span>
          </div>
          {missing.progress && missing.progress.phase !== "failed" ? <DownloadBar card={missing} act={act} /> : (
            <button type="button" className="primary" data-test={`job-get-${job.id}`} onClick={() => act("library_download", missing.id)}>
              {missing.progress?.phase === "failed" ? "Try again" : `Get · ${size(missing.bytes)}`}
            </button>
          )}
          {missing.progress?.phase === "failed" ? <span className="lib-error">{missing.progress.error}</span> : null}
          <button type="button" className="set-link" data-test={`job-browse-${job.id}`} onClick={() => onBrowse(job.id)}>Other models</button>
        </div>
      ) : null}
      {useIt && !missing ? (
        <div className="set-offer" data-test={`job-suggest-${job.id}`}>
          <div className="set-offer-text"><div><strong>{useIt.title}</strong> is a better fit</div><span>{reason(job.id, useIt)}</span></div>
          <button type="button" data-test={`job-use-${job.id}`} onClick={() => onAssign(job.task as Task, "local", useIt.file)}>Use it</button>
        </div>
      ) : null}
    </div>
  );
}

type Filter = "all" | "installed" | "chat" | "code" | "vision" | "reasoning" | "speech" | "image" | "embedding" | "fits";
type Sort = "best" | "smallest" | "name";
const FILTERS: { id: Filter; label: string }[] = [
  { id: "all", label: "All" },
  { id: "installed", label: "Installed" },
  { id: "chat", label: "Chat" },
  { id: "code", label: "Code" },
  { id: "vision", label: "Pictures" },
  { id: "reasoning", label: "Thinks first" },
  { id: "speech", label: "Speech" },
  { id: "image", label: "Images" },
  { id: "embedding", label: "Meaning" },
  { id: "fits", label: "Fits this computer" },
];

/** Every model Mobius can use on this computer: search, narrow by what it does, and get or remove it. */
function Library({ cards, draft, filter, setFilter, onAssign, act, report }: {
  cards: Card[]; draft: Draft; filter: Filter; setFilter: (filter: Filter) => void; onAssign: (task: Task, backend: string, model: string) => void; act: (command: string, id: string) => void; report: (error: unknown) => void;
}) {
  const [needle, setNeedle] = useState("");
  const [adding, setAdding] = useState(false);
  const [added, setAdded] = useState("");
  const addFile = () => {
    setAdding(true);
    setAdded("");
    invoke<string | null>("library_import").then((name) => { if (name) setAdded(name); }, report).finally(() => setAdding(false));
  };
  const [sort, setSort] = useState<Sort>("best");
  const text = needle.trim().toLowerCase();
  const matches = (card: Card) => {
    if (text && !`${card.title} ${card.maker} ${card.blurb} ${card.tasks.join(" ")}`.toLowerCase().includes(text)) return false;
    switch (filter) {
      case "installed": return card.installed;
      case "fits": return card.fit === "gpu" || card.fit === "tight";
      case "vision": return card.vision;
      case "all": return true;
      default: return card.tasks.includes(filter);
    }
  };
  const shown = cards.filter(matches).sort(sort === "smallest" ? (a, b) => a.bytes - b.bytes : sort === "name" ? (a, b) => a.title.localeCompare(b.title) : listOrder);
  const usedFor = (card: Card) => JOBS.filter((j) => j.task && draft.profiles[j.task as Task].backend === "local" && draft.profiles[j.task as Task].model === card.file);
  return (
    <>
      <div className="lib-tools">
        <label className="route-search" htmlFor="route-model-search-local">
          <Icon name="search" />
          <input id="route-model-search-local" data-test="route-model-search" type="search" placeholder="Search models" autoComplete="off" value={needle} onChange={(event) => setNeedle(event.target.value)} />
        </label>
        <label className="lib-sort">Sort
          <select data-test="lib-sort" value={sort} onChange={(event) => setSort(event.target.value as Sort)}>
            <option value="best">Best for this computer</option>
            <option value="smallest">Smallest first</option>
            <option value="name">Name</option>
          </select>
        </label>
      </div>
      <div className="lib-add">
        <button type="button" data-test="lib-import" disabled={adding} onClick={addFile}>{adding ? "Adding…" : "Add a model file…"}</button>
        <button type="button" data-test="lib-folder" onClick={() => invoke("library_reveal").catch(report)}>Open the models folder</button>
        <span className="set-hint">A <code>.gguf</code> file you already have. Mobius checks that it is one and keeps your original.</span>
      </div>
      <HubFind report={report} />
      {added ? <p className="set-offer" data-test="lib-added"><span className="set-offer-text"><span><strong>{added}</strong> was added. It is not checked against a published list, so Mobius cannot say what it is or whether it is intact.</span></span></p> : null}
      <div className="lib-chips" role="group" aria-label="Filter models">
        {FILTERS.map((f) => (
          <button key={f.id} type="button" className={`lib-chip${filter === f.id ? " on" : ""}`} aria-pressed={filter === f.id} data-test={`lib-filter-${f.id}`} onClick={() => setFilter(f.id)}>{f.label}</button>
        ))}
      </div>
      <div className="lib-grid" data-test="library-list">
        {shown.map((card) => {
          const uses = usedFor(card);
          const fixed = specialist(card);
          const assignable = fixed ? [] : JOBS.filter((j) => j.task && !j.local && eligible(j.id, card));
          return (
            <article key={card.id} className={`lib-card${uses.length ? " in-use" : ""}${card.installed ? " is-installed" : ""}`} data-test={`lib-card-${card.id}`}>
              <header>
                <div>
                  <div className="lib-title">{card.title}{card.loaded ? <span className="lib-loaded" title="In memory now"> ● ready</span> : null}{card.installed ? <span className="lib-installed" data-test={`lib-installed-${card.id}`}>Installed</span> : null}</div>
                  <div className="lib-sub">{[card.maker, size(card.bytes), card.memory_gb ? `≈ ${card.memory_gb} GB memory` : "", card.license].filter(Boolean).join(" · ")}</div>
                </div>
                <div className="lib-tags">
                  {card.tasks.map((t) => <span key={t} className="lib-tag">{TASK_WORD[t] ?? t}</span>)}
                  {card.vision ? <span className="lib-tag vision">Sees pictures</span> : null}
                  {!card.in_catalog ? <span className="lib-tag" data-test={`lib-unchecked-${card.id}`}>Added by you</span> : null}
                  <FitTag card={card} />
                </div>
              </header>
              <p className="lib-blurb">{card.blurb}</p>
              {card.progress && card.progress.phase !== "failed" ? <DownloadBar card={card} act={act} /> : (
                <div className="lib-uses">
                  {card.progress?.phase === "failed" ? <span className="lib-error" data-test={`lib-error-${card.id}`}>{card.progress.error}</span> : null}
                  {card.installed && fixed ? <span className="lib-sub" data-test={`lib-role-${card.id}`}>{card.tasks.includes("speech") ? "Used by the microphone button in the composer." : card.tasks.includes("image") ? "Used by Create image in the Mobius menu." : "Used to read what a message means."}</span> : null}
                  {card.installed ? assignable.map((j) => {
                    const active = uses.some((u) => u.id === j.id);
                    return (
                      <button key={j.id} type="button" className={`route-use-btn${active ? " active" : ""}`} data-test={`lib-use-${card.id}-${j.id}`} onClick={() => onAssign(j.task as Task, "local", card.file)}>
                        {active ? `Used for ${j.title}` : `Use for ${j.title}`}
                      </button>
                    );
                  }) : (
                    <button type="button" className="primary" data-test={`lib-get-${card.id}`} onClick={() => act("library_download", card.id)}>
                      {card.progress?.phase === "failed" ? "Try again" : `Get · ${size(card.bytes)}`}
                    </button>
                  )}
                  {card.installed ? (
                    <button type="button" className="lib-remove" data-test={`lib-remove-${card.id}`}
                      onClick={() => { if (window.confirm(card.in_catalog ? `Remove ${card.title} (${size(card.bytes)}) from this computer? You can download it again later.` : `Remove ${card.title} (${size(card.bytes)}) from the models folder? Your original file, if you added it from somewhere else, is not touched.`)) act("library_remove", card.id); }}>
                      Remove
                    </button>
                  ) : null}
                </div>
              )}
            </article>
          );
        })}
        {!shown.length ? <p className="route-model-empty" data-test="library-empty">{text || filter !== "all" ? "Nothing matches. Try another word or filter." : "No models yet."}</p> : null}
      </div>
    </>
  );
}

/** Settings for Mobius. Rust owns the routing file and the secret store; changes here are written as they are made, except a
 * provider's address and key, which are saved with its Test button. An API key is sent once to the secret store and is not kept in the page. */
export function SettingsPage({ register, hooks }: { register: (handle: SettingsHandle) => void; hooks: SettingsHooks }) {
  const [open, setOpen] = useState(false);
  const [draft, setDraft] = useState<Draft | null>(null);
  const [info, setInfo] = useState<PageInfo | null>(null);
  const [section, setSection] = useState<Section>("models");
  const [cache, setCache] = useState<Record<Source, string[]>>({ local: [], lan: [], openrouter: [] });
  const [library, setLibrary] = useState<Card[]>([]);
  const [apiKey, setApiKey] = useState("");
  const [hasKey, setHasKey] = useState<boolean | null>(null);
  const [experiments, setExperiments] = useState<Record<string, boolean> | null>(null);
  const [lanProbe, setLanProbe] = useState<Probe | null>(null);
  const [cloudProbe, setCloudProbe] = useState<Probe | null>(null);
  const [saved, setSaved] = useState("");
  const [filter, setFilter] = useState<Filter>("all");
  const [cloudView, setCloudView] = useState<CloudView | null>(null);
  const [cloudBusy, setCloudBusy] = useState(false);
  const draftRef = useRef<Draft | null>(null);
  const timer = useRef<number | undefined>(undefined);
  const hooksRef = useRef(hooks);
  hooksRef.current = hooks;
  const report = (error: unknown) => hooksRef.current.report(error);
  draftRef.current = draft;

  const loadModels = useCallback(async (source: Source) => {
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
      /* the lists stay as they were */
    }
  }, []);

  const refreshLibrary = useCallback(() => {
    invoke<Card[]>("library_list").then((cards) => setLibrary(Array.isArray(cards) ? cards : []), () => undefined);
  }, []);

  useEffect(() => {
    if (!open) return undefined;
    // Downloads report their own progress; a finished one changes what is installed, so the list of models is fetched again then.
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

  /** Reads the routing file, lets `change` edit it, and writes it back: every setting on this page is saved this way. */
  const patchRouting = useCallback(async (change: (routing: Routing, now: Draft) => void) => {
    const now = draftRef.current;
    if (!now) return;
    const routing = await invoke<Routing>("routing_get");
    routing.profiles = routing.profiles ?? {};
    ensureBackends(routing, now.lanUrl.trim(), now.cloudUrl.trim() || OPENROUTER);
    change(routing, now);
    await invoke("routing_save", { routing });
    setSaved("Saved");
    window.setTimeout(() => setSaved((s) => (s === "Saved" ? "" : s)), 1600);
  }, []);

  const flush = useCallback(async () => {
    if (timer.current === undefined) return;
    window.clearTimeout(timer.current);
    timer.current = undefined;
    await patchRouting((routing, now) => {
      routing.talk = { temperature: now.temperature, max_tokens: now.maxTokens, remote_timeout_secs: now.timeoutSecs };
    }).catch(report);
  }, [patchRouting]);

  const show = useCallback((next: Section) => {
    setSection(next);
    if (next === "providers") {
      loadModels("lan");
      loadModels("openrouter");
    }
    if (next === "models" || next === "library") refreshLibrary();
  }, [loadModels, refreshLibrary]);
  const browse = (job: Job) => {
    setFilter(job === "speech" ? "speech" : job === "image" ? "image" : job === "vision" ? "vision" : job === "code" ? "code" : "chat");
    show("library");
  };

  useEffect(() => {
    register({
      open: async (wanted?: string) => {
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
          draftRef.current = next;
          setApiKey("");
          setLanProbe(null);
          setCloudProbe(null);
          setSaved("");
          const start = SECTIONS.find((s) => s.id === wanted)?.id ?? "models";
          setSection(start);
          setFilter("all");
          await refreshKeyStatus().catch(() => setHasKey(null));
          setInfo(await invoke<PageInfo>("routing_page_info").catch(() => null));
          invoke<{ experiments: Record<string, boolean> }>("lab_get").then((view) => setExperiments(view.experiments), () => setExperiments(null));
          setOpen(true);
          hooksRef.current.visible(true);
          loadModels("local");
          refreshLibrary();
          loadModels("lan");
          loadModels("openrouter");
          invoke<CloudView>("cloud_catalog", { refresh: false }).then(setCloudView, () => setCloudView({ models: [], fetched: 0, error: "", pins: [] }));
        } catch (error) {
          report(error);
        }
      },
    });
  }, [register, loadModels, refreshKeyStatus, refreshLibrary]);

  const close = () => {
    flush().finally(() => {
      setOpen(false);
      hooksRef.current.visible(false);
    });
  };
  const change = (patch: Partial<Draft>) => setDraft((old) => (old ? { ...old, ...patch } : old));
  const changeTalk = (patch: Partial<Draft>) => {
    change(patch);
    window.clearTimeout(timer.current);
    // The sliders move through many values; one write follows the last of them.
    timer.current = window.setTimeout(() => {
      draftRef.current = { ...(draftRef.current as Draft), ...patch };
      flush();
    }, 450);
    draftRef.current = { ...(draftRef.current as Draft), ...patch };
  };

  const assign = (task: Task, backend: string, model: string) => {
    if (task === "act" && backend !== "local") {
      report("Act must stay on this computer.");
      return;
    }
    const place = task === "act" ? "local" : backend;
    setDraft((old) => (old ? { ...old, profiles: { ...old.profiles, [task]: { backend: place, model } } } : old));
    patchRouting((routing) => {
      const existing = routing.profiles![task] ?? { backend: "local", model: "" };
      routing.profiles![task] = { ...existing, backend: place, model };
      // Act never leaves this computer, whatever else is saved.
      routing.profiles!.act = { ...routing.profiles!.act, model: routing.profiles!.act?.model ?? "", backend: "local" };
    })
      .then(() => { if (task === "chat") hooksRef.current.saved(place || "local", model || ""); })
      .catch(report);
  };

  const act = (command: string, id: string) => invoke(command, { id }).catch(report);
  const refreshCloud = () => {
    setCloudBusy(true);
    invoke<CloudView>("cloud_catalog", { refresh: true }).then(setCloudView, report).finally(() => setCloudBusy(false));
  };
  const pinCloud = (id: string, on: boolean) => {
    invoke<string[]>("cloud_pin_set", { id, on })
      .then((pins) => setCloudView((old) => (old ? { ...old, pins } : { models: [], fetched: 0, error: "", pins })))
      .catch(report);
  };

  const probe = async (which: "lan" | "cloud") => {
    if (!draft) return;
    const set = which === "lan" ? setLanProbe : setCloudProbe;
    try {
      if (which === "cloud" && apiKey.trim()) await invoke("routing_set_secret", { apiKeyRef: "openrouter", secret: apiKey.trim() });
      if (which === "cloud") await invoke("routing_ensure_openrouter");
      const lanUrl = draft.lanUrl.trim();
      if (which === "lan" && !lanUrl) {
        report("Enter a LAN URL first.");
        return;
      }
      await patchRouting(() => undefined);
      const id = which === "lan" ? "lan" : "openrouter";
      const message = await invoke<string>("routing_probe", { backendId: id });
      set({ text: message, ok: true });
      if (which === "cloud") {
        setApiKey("");
        await refreshKeyStatus();
        setInfo(await invoke<PageInfo>("routing_page_info").catch(() => null));
      } else {
        setInfo(await invoke<PageInfo>("routing_page_info").catch(() => null));
      }
      await loadModels(id);
    } catch (error) {
      set({ text: String(error), ok: false });
    }
  };

  if (!open || !draft) return null;
  const pins = cloudView?.pins ?? [];
  const named = (id: string) => ({ id, name: cloudView?.models.find((m) => m.id === id)?.name ?? id });
  // The jobs offer what the person pinned; until they pin any, a short list of well-known models.
  const clouds = (pins.length ? pins : cache.openrouter.slice(0, 8)).map(named);
  const lans = cache.lan;
  return (
    <>
      <header className="models-page-head set-head" data-tauri-drag-region>
        <button id="models-back" className="icon-btn" data-test="models-back" type="button" title="Back to chat" aria-label="Back to chat" onClick={close}><Icon name="chevron-left" /></button>
        <div className="models-page-titles">
          <h2>Settings</h2>
          <p>Choose the models Mobius uses, where they run, and how it behaves.</p>
        </div>
        <div className="set-saved" aria-live="polite" data-test="settings-saved">{saved}</div>
      </header>
      <div className="models-page-body">
        <nav className="models-nav set-nav" aria-label="Settings sections">
          {SECTIONS.map((entry) => (
            <button key={entry.id} type="button" className={`models-nav-btn${entry.id === section ? " active" : ""}`} data-models-section={entry.id} data-test={`models-nav-${entry.id}`} onClick={() => show(entry.id)}>
              <Icon name={entry.icon} /><span>{entry.label}</span>
            </button>
          ))}
        </nav>
        <div className="models-panels">
          <section className="models-panel active set-panel" data-test="models-panel-models" hidden={section !== "models"}>
            <h3 className="models-h3">What each job uses</h3>
            <p className="models-lead">Pick a model for each kind of work. Mobius suggests what runs well on this computer, and downloads only when you press Get. Only one model sits in memory at a time, so switching takes a few seconds.</p>
            <div className="set-list" data-test="jobs">
              {JOBS.map((job) => (
                <JobRow key={job.id} job={job} draft={draft} cards={library} lan={lans} cloud={clouds} hasKey={hasKey} onAssign={assign} onBrowse={browse} onMoreCloud={() => show("cloud")} act={act} />
              ))}
            </div>
            <button type="button" className="set-link set-gap-sm" data-test="models-see-library" onClick={() => { setFilter("all"); show("library"); }}>See every model in the Library</button>
          </section>

          <section className="models-panel active set-panel" data-test="models-panel-library" hidden={section !== "library"}>
            <h3 className="models-h3">Library</h3>
            <p className="models-lead">Everything Mobius can run on this computer. Nothing downloads until you press Get, and each download is checked against a published checksum before it is used.</p>
            <Library cards={library} draft={draft} filter={filter} setFilter={setFilter} onAssign={assign} act={act} report={report} />
          </section>

          <section className="models-panel active set-panel" data-test="models-panel-cloud" hidden={section !== "cloud"}>
            <h3 className="models-h3">Cloud models</h3>
            <p className="models-lead">Hundreds of hosted models through OpenRouter. Add the ones you want to <strong>my menu</strong> and they appear in the chat's model menu and in the choices for each job. The menu holds only what you add.</p>
            <CloudModels view={cloudView} loading={cloudBusy} hasKey={hasKey} profiles={draft.profiles} now={Math.floor(Date.now() / 1000)}
              onRefresh={refreshCloud} onPin={pinCloud} onAssign={assign} onProviders={() => show("providers")} />
          </section>

          <section className="models-panel active set-panel" data-test="models-panel-knowledge" hidden={section !== "knowledge"}>
            <KnowledgePanel report={report} onGetModel={() => { setFilter("embedding"); show("library"); }} />
          </section>

          <section className="models-panel active set-panel" data-test="models-panel-providers" hidden={section !== "providers"}>
            <h3 className="models-h3">Another computer on your network</h3>
            <p className="models-lead">Point Mobius at Ollama, LM Studio, vLLM, or any <code>/v1</code> chat endpoint on your network. Replies come from that computer, so it appears under each job's choices.</p>
            <div className="models-form-card">
              <label className="sheet-field">Base URL
                <input id="route-lan-url" data-test="route-lan-url" type="url" placeholder="http://host:11434/v1" value={draft.lanUrl} onChange={(event) => change({ lanUrl: event.target.value })} />
              </label>
              <div className="models-form-row">
                <button id="route-probe-lan" type="button" className="primary" data-test="route-probe-lan" onClick={() => probe("lan")}>Save &amp; test</button>
                <span id="route-probe-lan-status" className={`models-inline-status${lanProbe ? (lanProbe.ok ? " ok" : " bad") : ""}`} data-test="route-probe-lan-status">{lanProbe?.text ?? (info?.lan_configured ? `${lans.length} model${lans.length === 1 ? "" : "s"} found` : "")}</span>
              </div>
            </div>
            <h3 className="models-h3 set-gap">Cloud (OpenRouter)</h3>
            <p className="models-lead">Hundreds of hosted models through one key. The key lives in the Secret Service, never in transcripts or the routing file. Chats on a cloud model leave this computer, and Act never uses one.</p>
            <div className="models-form-card">
              <label className="sheet-field">API URL
                <input id="route-cloud-url" data-test="route-cloud-url" type="url" placeholder={OPENROUTER} value={draft.cloudUrl} onChange={(event) => change({ cloudUrl: event.target.value })} />
              </label>
              <label className="sheet-field">API key
                <input id="route-cloud-key" data-test="route-cloud-key" type="password" autoComplete="off" placeholder="Paste to update; leave blank to keep" value={apiKey} onChange={(event) => setApiKey(event.target.value)} />
              </label>
              <div className="models-form-row">
                <button id="route-probe-cloud" type="button" className="primary" data-test="route-probe-cloud" onClick={() => probe("cloud")}>Save &amp; test</button>
                <button id="route-clear-key" type="button" data-test="route-clear-key" onClick={() => {
                  invoke("routing_clear_secret", { apiKeyRef: "openrouter" })
                    .then(() => { setApiKey(""); return refreshKeyStatus(); })
                    .then(() => invoke<PageInfo>("routing_page_info").then(setInfo))
                    .catch(report);
                }}>Clear key</button>
                <p id="route-cloud-key-status" className="sheet-note" data-test="route-cloud-key-status">{hasKey === null ? "" : hasKey ? "API key is saved securely." : "No API key saved yet."}</p>
              </div>
              <span id="route-probe-cloud-status" className={`models-inline-status${cloudProbe ? (cloudProbe.ok ? " ok" : " bad") : ""}`} data-test="route-probe-cloud-status">{cloudProbe?.text ?? ""}</span>
            </div>
          </section>

          <section className="models-panel active set-panel" data-test="models-panel-generation" hidden={section !== "generation"}>
            <h3 className="models-h3">How replies are written</h3>
            <p className="models-lead">Applied to Chat and Code, on this computer and on other providers. Changes are saved as you make them.</p>
            <div className="models-form-card">
              <label className="sheet-field">Temperature
                <div className="models-slider-row">
                  <input id="route-temperature" data-test="route-temperature" type="range" min={0} max={2} step={0.05} value={draft.temperature} onChange={(event) => changeTalk({ temperature: Number(event.target.value) })} />
                  <output id="route-temperature-value" htmlFor="route-temperature">{draft.temperature.toFixed(2)}</output>
                </div>
                <span className="set-hint">Lower is steadier and more literal. Higher is looser and more inventive.</span>
              </label>
              <label className="sheet-field">Longest reply (tokens)
                <input id="route-max-tokens" data-test="route-max-tokens" type="number" min={64} max={8192} step={64} value={draft.maxTokens} onChange={(event) => changeTalk({ maxTokens: Number(event.target.value) })} />
              </label>
              <label className="sheet-field">Wait for another provider (seconds)
                <input id="route-timeout" data-test="route-timeout" type="number" min={15} max={600} step={15} value={draft.timeoutSecs} onChange={(event) => changeTalk({ timeoutSecs: Number(event.target.value) })} />
              </label>
              <button id="route-reset-defaults" type="button" data-test="route-reset-defaults" onClick={() => changeTalk({ temperature: 0.6, maxTokens: 1024, timeoutSecs: 120 })}>Reset to defaults</button>
            </div>
            <label className="sheet-check set-check">
              <input id="route-battery" data-test="route-battery" type="checkbox" checked={draft.battery}
                onChange={(event) => { const on = event.target.checked; change({ battery: on }); patchRouting((routing) => { routing.allow_remote_on_battery_saver = on; }).catch(report); }} />
              <span><strong>Use other providers when battery saver is on</strong><br /><span className="set-hint">Off by default: cloud and network replies keep the radios busy.</span></span>
            </label>
          </section>

          <section className="models-panel active set-panel" data-test="models-panel-experiments" hidden={section !== "experiments"}>
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
                  <span><strong>{title}</strong><br /><span className="set-hint">{note}</span></span>
                </label>
              ))}
            </div>
            <div className="models-kv set-gap"><span>Routing file</span><code id="route-config-path" data-test="route-config-path">{info?.config_path ?? ""}</code></div>
          </section>
        </div>
      </div>
    </>
  );
}
