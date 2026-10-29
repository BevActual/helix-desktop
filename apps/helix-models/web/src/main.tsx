import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { createRoot } from "react-dom/client";
import { ActionMenu, AppFrame, Banner, Button, Chip, EmptyState, Group, HxRow, HxSwitch, Icon, IconTile, Popup, ProgressRing, Sheet, Sidebar, SidebarItem, Spinner, hueOf, invoke, useLayoutReport } from "@helix/ui";

type Page = "overview" | "backends" | "models" | "routing";
type Trust = "this" | "lan" | "cloud";

interface BackendView { id: string; label: string; kind: "local" | "openai"; base_url: string; trust: Trust; has_key: boolean }
interface SlotView { slot: string; backend: string; model: string; local_only: boolean; escalate_backend: string; escalate_model: string }
interface Device { name: string; description: string; gpu: boolean; memory_free: number; memory_total: number }
interface ModelView { id: string; bytes: number; loaded: boolean; vision: boolean }
interface Snapshot {
  backend?: string;
  loaded?: string;
  devices?: Device[];
  gpuLayers?: number;
  contextTokens?: number;
  queue?: { waiting: number; running: boolean; max: number };
  model_dir?: string;
}
interface View {
  gateway: boolean;
  error: string;
  snapshot: Snapshot;
  models: ModelView[];
  backends: BackendView[];
  slots: SlotView[];
  allow_remote_on_battery_saver: boolean;
}
interface Probe { ok: boolean; trust?: string; models?: string[]; error?: string }
interface HubFile { path: string; size: number }
interface DownloadView { id: number; repo: string; title: string; total: number; done: number; state: "running" | "done" | "failed" | "cancelled"; message: string }
interface Choice { path: string; label: string; size: number }
interface GetDraft { repo: string; fixed: boolean; files: HubFile[] | null; pick: string; projector: boolean; looking: boolean; error: string; entry?: Entry }

const PAGES: { id: Page; title: string; icon: string }[] = [
  { id: "overview", title: "Overview", icon: "sparkle" },
  { id: "backends", title: "Backends", icon: "wifi" },
  { id: "models", title: "Models", icon: "folder" },
  { id: "routing", title: "Routing", icon: "grid" },
];
const SLOTS: Record<string, { title: string; about: string; icon: string; hue: number }> = {
  act: { title: "Desktop", about: "Changes settings and apps when you ask. Only ever runs on this computer.", icon: "bolt", hue: 28 },
  chat: { title: "Talk", about: "Conversations in Mobius and the bar.", icon: "chat", hue: 212 },
  summarize: { title: "Summarise", about: "Short summaries and titles.", icon: "doc", hue: 268 },
  vision: { title: "Pictures", about: "Reading images and screenshots.", icon: "image", hue: 150 },
  code: { title: "Code", about: "Writing and explaining code.", icon: "code", hue: 238 },
};
const slotInfo = (slot: string) => SLOTS[slot] ?? { title: slot, about: "", icon: "sparkle", hue: hueOf(slot) };
const TRUST: Record<Trust, { label: string; tone?: "accent" | "ok" | "warn" }> = {
  this: { label: "This computer", tone: "ok" },
  lan: { label: "Your network", tone: "accent" },
  cloud: { label: "Internet", tone: "warn" },
};

function bytes(n: number): string {
  if (n >= 1e9) return `${(n / 1e9).toFixed(1)} GB`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(0)} MB`;
  return `${Math.round(n / 1e3)} KB`;
}
const shortName = (id: string) => id.replace(/\.gguf$/, "");
const QUANT = /[-_.]((?:I?Q\d[\w]*)|BF16|F16|F32)$/i;
const SHARD = /-(\d{5})-of-(\d{5})\.gguf$/;

/** `Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf` → "Qwen2.5 Coder 7B Instruct" and "Q4_K_M". */
function nameOf(id: string): { name: string; quant: string } {
  const plain = id.replace(SHARD, "").replace(/\.gguf$/, "");
  const quant = plain.match(QUANT)?.[1] ?? "";
  const name = (quant ? plain.slice(0, plain.length - quant.length - 1) : plain).replace(/[-_]/g, " ");
  return { name: name || plain, quant: quant.toUpperCase() };
}
/** One colour per model family ("Qwen3 4B" and "Qwen2.5 Coder" are both Qwen). */
const familyHue = (name: string) => hueOf(name.split(/[\s-]/)[0].toLowerCase().replace(/[\d.]+$/, ""));
const ModelTile = ({ name, large }: { name: string; large?: boolean }) => (
  <IconTile large={large} letter={Array.from(name)[0]?.toUpperCase() || "•"} hue={familyHue(name)} />
);
const homeless = (path: string) => path.replace(/^\/home\/[^/]+/, "~");

function errorText(error: unknown): string {
  return typeof error === "string" ? error : error instanceof Error ? error.message : String(error);
}

/** A row that is a button as a whole (opens something), with a chevron. */
function LinkRow({ onOpen, test, children }: { onOpen: () => void; test?: string; children: ReactNode }) {
  return (
    <div tabIndex={0} role="button" className="hx-row-button" data-test={test} onClick={onOpen}
      onKeyDown={(event) => { if ((event.key === "Enter" || event.key === " ") && event.target === event.currentTarget) { event.preventDefault(); onOpen(); } }}>
      {children}
    </div>
  );
}
const Chevron = () => <span className="hx-chevron"><Icon name="chevron-right" /></span>;

// --- Overview -------------------------------------------------------------------------------------------------------------------------

function Gauge({ percent }: { percent: number }) {
  const r = 20, c = 2 * Math.PI * r;
  return (
    <svg className="hx-gauge" width="48" height="48" viewBox="0 0 48 48" fill="none" strokeWidth="5" aria-hidden="true">
      <circle className="track" cx="24" cy="24" r={r} />
      <circle className="arc" cx="24" cy="24" r={r} strokeDasharray={c} strokeDashoffset={c * (1 - Math.max(0, Math.min(100, percent)) / 100)} transform="rotate(-90 24 24)" />
    </svg>
  );
}

function Widget({ icon, label, tint, children, line, onOpen, test }: { icon: string; label: string; tint: string; children: ReactNode; line?: ReactNode; onOpen?: () => void; test?: string }) {
  return (
    <div className={`hx-widget${onOpen ? " button" : ""}`} style={{ ["--tint" as string]: tint }} data-test={test} tabIndex={onOpen ? 0 : undefined} role={onOpen ? "button" : undefined}
      onClick={onOpen} onKeyDown={(e) => { if (onOpen && e.key === "Enter") onOpen(); }}>
      <div className="label"><Icon name={icon} />{label}</div>
      <div className="big">{children}</div>
      {line ? <div className="line">{line}</div> : null}
    </div>
  );
}

/** "4.7" and "GB", so the unit can be drawn smaller. */
function figure(n: number): [string, string] {
  if (n >= 1e9) return [(n / 1e9).toFixed(1), "GB"];
  if (n >= 1e6) return [(n / 1e6).toFixed(0), "MB"];
  return [String(Math.round(n / 1e3)), "KB"];
}

function Overview({ view, jobs, onUnload, goto }: { view: View; jobs: DownloadView[]; onUnload: () => void; goto: (page: Page) => void }) {
  const s = view.snapshot;
  const gpu = (s.devices ?? []).find((d) => d.gpu);
  const loaded = s.loaded ?? "";
  const waiting = s.queue?.waiting ?? 0;
  const busy = !!s.queue?.running || waiting > 0;
  const used = gpu ? gpu.memory_total - gpu.memory_free : 0;
  const usedPercent = gpu && gpu.memory_total ? (used / gpu.memory_total) * 100 : 0;
  const onDisk = view.models.reduce((sum, m) => sum + m.bytes, 0);
  const running = jobs.filter((j) => j.state === "running");
  const remote = view.backends.filter((b) => b.kind !== "local").length;
  const away = view.slots.filter((slot) => view.backends.find((b) => b.id === slot.backend)?.kind === "openai").length;
  const state = busy ? "busy" : loaded ? "ready" : "idle";
  const tint = state === "busy" ? "#0a84ff" : state === "ready" ? "#30d158" : "#8e8ea0";
  return (
    <div className="hx-stack" data-test="overview">
      <div className="hx-status-hero" data-state={state} style={{ ["--tint" as string]: tint }} data-test="engine-row">
        <div className="hx-orb"><Icon name="sparkle" /></div>
        <div className="text">
          <div className="state">{state === "busy" ? "Answering" : state === "ready" ? "Ready" : "Resting"}</div>
          <div className="line">{loaded
            ? `${nameOf(loaded).name} is loaded${waiting ? ` · ${waiting} waiting` : ""}${state === "ready" ? " · freed after a few idle minutes" : ""}`
            : view.models.length ? "A model loads the moment something asks." : "No models yet. Get one to answer on this computer."}</div>
        </div>
        {loaded ? <Button small data-test="unload" disabled={busy} onClick={onUnload}>Free Memory</Button>
          : view.models.length ? null : <Button kind="primary" small data-test="first-model" onClick={() => goto("models")}>Get a Model</Button>}
      </div>
      <div className="hx-widgets">
        <Widget icon="bolt" label={gpu ? "Graphics Memory" : "Processor"} tint={gpu ? "#30d158" : "#8e8ea0"} test="accelerator-row"
          line={gpu ? gpu.description : "No graphics card"}>
          {gpu ? <>{figure(used)[0]}<small>of {figure(gpu.memory_total).join(" ")}</small><Gauge percent={usedPercent} /></> : <>CPU</>}
        </Widget>
        <Widget icon="folder" label="Models" tint="#bf5af2" onOpen={() => goto("models")} test="widget-models"
          line={running.length ? `${running.length} downloading` : `${figure(onDisk).join(" ")} on disk`}>
          {view.models.length}{running.length ? <span className="corner"><ProgressRing percent={running[0].total ? (running[0].done / running[0].total) * 100 : null} label="Downloading" /></span> : null}
        </Widget>
        <Widget icon="wifi" label="Elsewhere" tint="#0a84ff" onOpen={() => goto("backends")} test="widget-backends"
          line={away ? `${away} of ${view.slots.length} tasks go elsewhere` : "All tasks stay here"}>
          {remote}<small>{remote === 1 ? "backend" : "backends"}</small>
        </Widget>
      </div>
      <Group title="Where Requests Go">
        {view.slots.map((slot) => {
          const backend = view.backends.find((b) => b.id === slot.backend);
          const trust = backend ? TRUST[backend.trust] : TRUST.this;
          const info = slotInfo(slot.slot);
          return (
            <LinkRow key={slot.slot} onOpen={() => goto("routing")}>
              <HxRow test={`slot-summary:${slot.slot}`} leading={<IconTile name={info.icon} hue={info.hue} />} title={info.title}
                sub={`${backend?.label ?? slot.backend}${slot.model ? ` · ${nameOf(slot.model).name}` : ""}`}
                trailing={<><Chip tone={trust.tone}>{trust.label}</Chip><Chevron /></>} />
            </LinkRow>
          );
        })}
      </Group>
    </div>
  );
}

// --- Backends -------------------------------------------------------------------------------------------------------------------------

interface Draft { id: string; label: string; base_url: string; key: string; editing: boolean; had_key: boolean }
const NEW_DRAFT: Draft = { id: "", label: "", base_url: "", key: "", editing: false, had_key: false };
const PRESETS: { title: string; draft: Partial<Draft> }[] = [
  { title: "OpenRouter", draft: { id: "openrouter", label: "OpenRouter", base_url: "https://openrouter.ai/api/v1" } },
  { title: "OpenAI", draft: { id: "openai", label: "OpenAI", base_url: "https://api.openai.com/v1" } },
  { title: "Another Computer", draft: { id: "", label: "", base_url: "http://" } },
];

function BackendTile({ b }: { b: BackendView }) {
  if (b.kind === "local") return <IconTile name="sparkle" hue={258} />;
  if (b.trust === "lan") return <IconTile name="wifi" hue={212} />;
  return <IconTile letter={Array.from(b.label)[0]?.toUpperCase() || "•"} hue={hueOf(b.label)} />;
}

/** A label on the left and a borderless field on the right, inside a group (macOS account sheets). */
function FieldRow({ label, children }: { label: string; children: ReactNode }) {
  return <label className="hx-field-row"><span className="hx-field-label">{label}</span>{children}</label>;
}

function BackendSheet({ draft, setDraft, onSave, onCancel, error }: { draft: Draft; setDraft: (d: Draft) => void; onSave: () => void; onCancel: () => void; error: string }) {
  const input = (name: keyof Draft, props: { placeholder?: string; type?: string; disabled?: boolean; autoFocus?: boolean } = {}) => (
    <input className="hx-field-input" data-test={`backend-${name}`} value={String(draft[name])} placeholder={props.placeholder} type={props.type ?? "text"} disabled={props.disabled}
      autoFocus={props.autoFocus} autoComplete="off" spellCheck={false} onChange={(e) => setDraft({ ...draft, [name]: e.target.value })}
      onKeyDown={(e) => { if (e.key === "Enter") onSave(); }} />
  );
  return (
    <Sheet title={draft.editing ? `Edit ${draft.label || "Backend"}` : "Add a Backend"} test="backend-sheet" onCancel={onCancel}
      actions={<><Button data-test="backend-cancel" onClick={onCancel}>Cancel</Button><Button kind="primary" data-test="backend-save" onClick={onSave}>{draft.editing ? "Save" : "Add"}</Button></>}>
      {!draft.editing ? (
        <div className="hx-presets">
          {PRESETS.map((p) => (
            <button key={p.title} type="button" className={`hx-preset${draft.base_url === p.draft.base_url && draft.id === p.draft.id ? " current" : ""}`} data-test={`preset:${p.title}`}
              onClick={() => setDraft({ ...draft, ...p.draft })}>{p.title}</button>
          ))}
        </div>
      ) : null}
      <div className="hx-group">
        <FieldRow label="Name">{input("label", { placeholder: "Studio desktop", autoFocus: true })}</FieldRow>
        <FieldRow label="Short ID">{input("id", { placeholder: "studio", disabled: draft.editing })}</FieldRow>
        <FieldRow label="Address">{input("base_url", { placeholder: "http://192.168.1.20:8080/v1" })}</FieldRow>
        <FieldRow label="API Key">{input("key", { type: "password", placeholder: draft.had_key ? "Saved · type to replace" : "Optional" })}</FieldRow>
      </div>
      <div className="setting-about">An address on the internet must use https; a computer on your own network may use http. The key is kept by the model service and never shown again.</div>
      {error ? <Banner tone="error" test="backend-error">{error}</Banner> : null}
    </Sheet>
  );
}

function Backends({ view, probes, probing, onProbe, onEdit, onAdd, onRemove, onClearKey }: {
  view: View; probes: Record<string, Probe>; probing: string; onProbe: (id: string) => void; onEdit: (b: BackendView) => void; onAdd: () => void; onRemove: (b: BackendView) => void; onClearKey: (b: BackendView) => void;
}) {
  return (
    <div className="hx-stack" data-test="backends">
      <Group>
        {view.backends.map((b) => {
          const result = probes[b.id];
          const where = b.kind === "local" ? "Runs models on this computer" : b.base_url;
          const key = b.kind !== "local" && b.has_key ? " · Key saved" : "";
          return (
            <HxRow key={b.id} test={`backend:${b.id}`} leading={<BackendTile b={b} />} title={b.label}
              sub={<>
                <span className="hx-truncate">{where}</span>
                {probing === b.id ? <div className="hx-status" data-state="busy">Testing…</div>
                  : result ? <div className="hx-status" data-state={result.ok ? "ok" : "bad"} data-test={`probe-result:${b.id}`}>
                      {result.ok ? `Reachable · ${result.models?.length ?? 0} models${key}` : result.error}</div>
                  : key ? <div className="hx-status">{key.slice(3)}</div> : null}
              </>}
              trailing={<>
                <Chip tone={TRUST[b.trust].tone}>{TRUST[b.trust].label}</Chip>
                {b.kind !== "local" ? (
                  <>
                    <Button small data-test={`probe:${b.id}`} disabled={probing === b.id} onClick={() => onProbe(b.id)}>Test</Button>
                    <ActionMenu label={`${b.label} actions`} test={`more:${b.id}`} items={[
                      { id: "edit", label: "Edit…", onSelect: () => onEdit(b) },
                      ...(b.has_key ? [{ id: "forget-key", label: "Forget Key…", onSelect: () => onClearKey(b) }] : []),
                      { id: "remove", label: "Remove…", destructive: true, onSelect: () => onRemove(b) },
                    ]} />
                  </>
                ) : null}
              </>} />
          );
        })}
        <LinkRow onOpen={onAdd} test="add-backend">
          <HxRow leading={<span className="hx-add-tile"><Icon name="plus" /></span>} title={<span className="hx-accent-text">Add a Backend…</span>} />
        </LinkRow>
      </Group>
      <div className="setting-about">Anything that speaks the OpenAI chat protocol works: another computer running llama-server, Ollama or LM Studio, or a hosted service. A key is only ever sent over https, or to a computer on your own network.</div>
    </div>
  );
}

// --- Models ---------------------------------------------------------------------------------------------------------------------------

interface Entry { repo: string; approx: number; title: string; maker: string; about: string; pictures?: boolean }
// Models people commonly start with, all GGUF from their makers or ggml-org. `approx` is the Q4_K_M download in GB (with the picture projector
// where there is one), for the fit hint before anything is looked up; the real files and sizes are looked up when asked for.
const CATALOGUE: Entry[] = [
  { repo: "Qwen/Qwen3-4B-GGUF", approx: 2.5, title: "Qwen3 4B", maker: "Qwen", about: "Small and quick. Talk, summaries and acting on the desktop." },
  { repo: "Qwen/Qwen3-8B-GGUF", approx: 5.0, title: "Qwen3 8B", maker: "Qwen", about: "A stronger all-rounder for 8 GB graphics cards." },
  { repo: "ggml-org/gemma-3-4b-it-GGUF", approx: 3.3, title: "Gemma 3 4B", maker: "Google", about: "Small, and reads pictures.", pictures: true },
  { repo: "ggml-org/gemma-3-12b-it-GGUF", approx: 8.0, title: "Gemma 3 12B", maker: "Google", about: "Reads pictures and writes well.", pictures: true },
  { repo: "Qwen/Qwen2.5-Coder-7B-Instruct-GGUF", approx: 4.7, title: "Qwen2.5 Coder 7B", maker: "Qwen", about: "Writing and explaining code." },
  { repo: "ggml-org/Qwen2.5-VL-7B-Instruct-GGUF", approx: 5.4, title: "Qwen2.5 VL 7B", maker: "Qwen", about: "Reads screenshots and documents.", pictures: true },
  { repo: "Qwen/Qwen2.5-1.5B-Instruct-GGUF", approx: 1.1, title: "Qwen2.5 1.5B", maker: "Qwen", about: "Tiny. For older computers and quick titles." },
];
/** Installed when a model file starts with the repository's name (less `-GGUF`). */
const installedFrom = (repo: string, models: ModelView[]) => {
  const stem = repo.split("/").pop()!.replace(/-GGUF$/i, "").toLowerCase() + "-";
  return models.some((m) => m.id.toLowerCase().startsWith(stem));
};

/** The model files a repository offers, a split model counted once with its parts' sizes added. */
function choicesOf(files: HubFile[]): Choice[] {
  const out = new Map<string, Choice>();
  for (const f of files) {
    if (f.path.startsWith("mmproj")) continue;
    const shard = f.path.match(SHARD);
    const key = shard ? f.path.replace(SHARD, "") : f.path;
    const quant = nameOf(f.path).quant;
    const label = `${quant || shortName(key)}${shard ? ` (${Number(shard[2])} parts)` : ""}`;
    const have = out.get(key);
    if (have) {
      have.size += f.size;
      if (shard && shard[1] === "00001") have.path = f.path;
    } else {
      out.set(key, { path: f.path, label, size: f.size });
    }
  }
  return [...out.values()].sort((a, b) => a.size - b.size);
}

/** Q4_K_M when there is one, a balance of size and quality, else the smallest of 4 to 6 bits, else the smallest. */
function preferred(choices: Choice[]): string {
  const q4 = choices.find((c) => /q4_k_m/i.test(c.path));
  if (q4) return q4.path;
  const middling = choices.find((c) => /q[4-6]_/i.test(c.path));
  return (middling ?? choices[0])?.path ?? "";
}

function fit(size: number, view: View, memory: number): { label: string; state: "ok" | "warn" | "bad" | "" } {
  const gpu = Math.max(0, ...(view.snapshot.devices ?? []).filter((d) => d.gpu).map((d) => d.memory_total));
  const need = size * 1.2; // the context and the engine's own working memory
  if (gpu && need <= gpu) return { label: "Fits the graphics card", state: "ok" };
  if (memory && need <= memory * 0.6) return { label: gpu ? "Partly on the graphics card · slower" : "Runs from memory · slower", state: "warn" };
  if (!memory && !gpu) return { label: "Unknown", state: "" };
  return { label: "Too large for this computer", state: "bad" };
}

/** Bytes per second for each running download, from the last few polls. */
function useRates(jobs: DownloadView[]): Record<number, number> {
  const samples = useRef(new Map<number, { t: number; done: number }[]>());
  const [rates, setRates] = useState<Record<number, number>>({});
  useEffect(() => {
    const now = performance.now();
    const next: Record<number, number> = {};
    for (const j of jobs) {
      if (j.state !== "running") { samples.current.delete(j.id); continue; }
      const list = [...(samples.current.get(j.id) ?? []), { t: now, done: j.done }].slice(-6);
      samples.current.set(j.id, list);
      const first = list[0];
      const seconds = (now - first.t) / 1000;
      if (seconds > 0.5) next[j.id] = (j.done - first.done) / seconds;
    }
    setRates(next);
  }, [jobs]);
  return rates;
}

function remaining(left: number, rate: number): string {
  if (!rate || rate < 1) return "";
  const s = left / rate;
  if (s < 60) return "less than a minute left";
  if (s < 3600) return `about ${Math.round(s / 60)} min left`;
  return `about ${(s / 3600).toFixed(1)} h left`;
}

function Downloads({ jobs, onCancel, onDismiss }: { jobs: DownloadView[]; onCancel: (id: number) => void; onDismiss: (id: number) => void }) {
  const rates = useRates(jobs);
  if (!jobs.length) return null;
  return (
    <Group title="Downloads">
      {jobs.map((j) => {
        const percent = j.total ? (j.done / j.total) * 100 : 0;
        const rate = rates[j.id] ?? 0;
        const { name, quant } = nameOf(j.title);
        const sub = j.state === "running" ? [`${bytes(j.done)} of ${bytes(j.total)}`, rate ? `${bytes(rate)}/s` : "", remaining(j.total - j.done, rate)].filter(Boolean).join(" · ")
          : j.state === "done" ? "Downloaded · ready to use"
          : j.state === "cancelled" ? "Stopped · getting it again continues from here"
          : j.message || "The download failed.";
        return (
          <HxRow key={j.id} test={`download:${j.title}`} leading={<ModelTile name={name} />}
            title={<>{name} {quant ? <span className="hx-quant">{quant}</span> : null}</>}
            sub={<span data-test={`download-state:${j.title}`} data-state={j.state} className={j.state === "failed" ? "hx-danger-text" : undefined}>{sub}</span>}
            trailing={j.state === "running"
              ? <ProgressRing percent={percent} label={`Stop downloading ${name} (${Math.round(percent)}%)`} test={`download-cancel:${j.title}`} onStop={() => onCancel(j.id)} />
              : <>
                  {j.state === "done" ? <span className="hx-done"><Icon name="check" /></span> : null}
                  <Button small data-test={`download-dismiss:${j.title}`} onClick={() => onDismiss(j.id)}>Clear</Button>
                </>} />
        );
      })}
    </Group>
  );
}

function CatalogueCard({ entry, installed, job, hint, onGet, onCancel }: { entry: Entry; installed: boolean; job?: DownloadView; hint: { label: string; state: string }; onGet: () => void; onCancel: (id: number) => void }) {
  return (
    <div className="hx-tile-card tinted" style={{ ["--tint" as string]: `hsl(${familyHue(entry.title)} 60% 55%)` }} tabIndex={0} role="button" data-test={`catalogue:${entry.repo}`} aria-label={entry.title}
      onClick={() => { if (!job) onGet(); }}
      onKeyDown={(e) => { if (e.key === "Enter" && e.target === e.currentTarget && !job) onGet(); }}>
      <ModelTile name={entry.title} large />
      <div>
        <div className="name">{entry.title}</div>
        <div className="maker">{entry.maker}{entry.pictures ? " · Reads pictures" : ""}</div>
      </div>
      <div className="sum">{entry.about}</div>
      <div className="foot">
        {job ? <ProgressRing percent={job.total ? (job.done / job.total) * 100 : null} label={`Stop downloading ${entry.title}`} onStop={() => onCancel(job.id)} />
          : installed ? <span className="hx-installed" data-test={`installed:${entry.repo}`}>Installed</span>
          : <button type="button" className="hx-get" data-test={`get:${entry.repo}`} onClick={(e) => { e.stopPropagation(); onGet(); }}>Get</button>}
        <span className="aside" title={hint.label}><span className="hx-fit-dot" data-state={hint.state} />{entry.approx} GB</span>
      </div>
    </div>
  );
}

/** The model this computer suits best and does not have yet: the largest that fits the graphics card, else one that runs from memory. */
function recommended(view: View, memory: number): Entry | null {
  const order = ["Qwen/Qwen3-8B-GGUF", "Qwen/Qwen3-4B-GGUF", "Qwen/Qwen2.5-1.5B-Instruct-GGUF"].map((r) => CATALOGUE.find((c) => c.repo === r)!);
  const pick = order.find((c) => fit(c.approx * 1e9, view, memory).state === "ok") ?? order.find((c) => fit(c.approx * 1e9, view, memory).state === "warn") ?? order[order.length - 1];
  return installedFrom(pick.repo, view.models) ? null : pick;
}

function Models({ view, jobs, memory, onDelete, onGet, onCancel, onDismiss }: { view: View; jobs: DownloadView[]; memory: number; onDelete: (m: ModelView) => void; onGet: (entry: Entry | null) => void; onCancel: (id: number) => void; onDismiss: (id: number) => void }) {
  if (!view.gateway) return <EmptyState icon="folder">The model service is not answering, so the installed models cannot be listed.</EmptyState>;
  const running = (repo: string) => jobs.find((j) => j.state === "running" && j.repo === repo);
  const featured = recommended(view, memory);
  return (
    <div className="hx-stack" data-test="models">
      {featured && !running(featured.repo) ? (
        <div className="hx-hero-card" style={{ ["--hero" as string]: `hsl(${familyHue(featured.title)} 45% 34%)` }} data-test="featured">
          <ModelTile name={featured.title} large />
          <div style={{ flex: 1, minWidth: 0 }}>
            <div className="over">Recommended for this computer</div>
            <h2>{featured.title}</h2>
            <p>{featured.about}</p>
            <div className="meta"><span>{featured.maker}</span><span>About {featured.approx} GB</span><span>{fit(featured.approx * 1e9, view, memory).label}</span></div>
            <button type="button" className="hx-get-hero" data-test="featured-get" onClick={() => onGet(featured)}>Get</button>
          </div>
        </div>
      ) : null}
      <Downloads jobs={jobs} onCancel={onCancel} onDismiss={onDismiss} />
      {view.models.length ? (
        <Group title="On This Computer">
          {view.models.map((m) => {
            const { name, quant } = nameOf(m.id);
            return (
              <HxRow key={m.id} test={`model:${m.id}`} leading={<ModelTile name={name} />}
                title={<>{name} {quant ? <span className="hx-quant">{quant}</span> : null}</>}
                sub={[bytes(m.bytes), m.vision ? "Reads pictures" : ""].filter(Boolean).join(" · ")}
                trailing={<>
                  {m.loaded ? <Chip tone="ok">In Use</Chip> : null}
                  <ActionMenu label={`${name} actions`} test={`more:${m.id}`} items={[{ id: "delete", label: "Delete…", destructive: true, onSelect: () => onDelete(m) }]} />
                </>} />
            );
          })}
        </Group>
      ) : null}
      <section className="hx-shelf">
        <div className="hx-shelf-head">
          <h3>Get Models</h3>
          <button type="button" className="hx-link" data-test="get-other" onClick={() => onGet(null)}>From Hugging Face…</button>
        </div>
        <div className="hx-card-grid">
          {CATALOGUE.map((c) => (
            <CatalogueCard key={c.repo} entry={c} installed={installedFrom(c.repo, view.models)} job={running(c.repo)} hint={fit(c.approx * 1e9, view, memory)} onGet={() => onGet(c)} onCancel={onCancel} />
          ))}
          <div className="hx-tile-card" tabIndex={0} role="button" data-test="catalogue-any" aria-label="Any model from Hugging Face" onClick={() => onGet(null)}
            onKeyDown={(e) => { if (e.key === "Enter" && e.target === e.currentTarget) onGet(null); }}>
            <IconTile large name="search" hue={212} />
            <div>
              <div className="name">Any Model</div>
              <div className="maker">Hugging Face</div>
            </div>
            <div className="sum">Any repository with GGUF files, by its name.</div>
            <div className="foot"><button type="button" className="hx-get" onClick={(e) => { e.stopPropagation(); onGet(null); }}>Choose</button></div>
          </div>
        </div>
      </section>
      <div className="setting-about">Models are kept in {homeless(view.snapshot.model_dir ?? "the models folder")}. The model service downloads them from Hugging Face only when you ask, and checks each file against the repository's checksum.</div>
    </div>
  );
}

function GetSheet({ draft, setDraft, view, memory, onLookUp, onDownload, onCancel }: { draft: GetDraft; setDraft: (d: GetDraft) => void; view: View; memory: number; onLookUp: () => void; onDownload: () => void; onCancel: () => void }) {
  const choices = draft.files ? choicesOf(draft.files) : [];
  const chosen = choices.find((c) => c.path === draft.pick);
  const projectors = (draft.files ?? []).filter((f) => f.path.startsWith("mmproj"));
  const projector = projectors.length ? Math.min(...projectors.map((f) => f.size)) : 0;
  const total = (chosen?.size ?? 0) + (draft.projector && projectors.length ? projector : 0);
  const hint = chosen ? fit(chosen.size, view, memory) : null;
  const entry = draft.entry;
  const repoName = draft.repo.split("/").pop() ?? draft.repo;
  return (
    <Sheet title={entry ? entry.title : draft.fixed ? repoName : "From Hugging Face"} test="get-sheet" onCancel={onCancel}
      actions={<>
        <Button data-test="get-cancel" onClick={onCancel}>Cancel</Button>
        {draft.files ? <Button kind="primary" data-test="get-download" disabled={!chosen} onClick={onDownload}>Download</Button>
          : <Button kind="primary" data-test="get-lookup" disabled={!draft.repo.includes("/") || draft.looking} onClick={onLookUp}>Look Up</Button>}
      </>}>
      {draft.fixed ? (
        <div className="hx-sheet-head">
          <ModelTile name={entry?.title ?? repoName} large />
          <div>
            <div className="hx-sheet-sub">{entry ? `${entry.maker} · ${draft.repo}` : draft.repo}</div>
            {entry ? <div>{entry.about}</div> : null}
          </div>
        </div>
      ) : (
        <>
          <div className="hx-group">
            <FieldRow label="Repository">
              <input className="hx-field-input" data-test="get-repo" autoFocus value={draft.repo} placeholder="owner/name-GGUF" autoComplete="off" spellCheck={false}
                onChange={(e) => setDraft({ ...draft, repo: e.target.value, files: null, error: "" })}
                onKeyDown={(e) => { if (e.key === "Enter" && draft.repo.includes("/") && !draft.files) onLookUp(); }} />
            </FieldRow>
          </div>
          {!draft.files ? <div className="setting-about">The repository's name on huggingface.co, such as Qwen/Qwen3-4B-GGUF.</div> : null}
        </>
      )}
      {draft.looking ? <div className="hx-looking" data-test="get-looking"><Spinner label="Looking up" /> Looking up the files…</div> : null}
      {draft.error ? <Banner tone="error" test="get-error">{draft.error}</Banner> : null}
      {draft.files && !choices.length ? <Banner tone="warn" test="get-none">This repository has no GGUF model files.</Banner> : null}
      {choices.length ? (
        <>
          <div className="hx-group">
            <HxRow title="Version"
              trailing={<Popup label="Version" test="get-version" value={draft.pick} onChange={(pick) => setDraft({ ...draft, pick })}
                options={choices.map((c) => ({ id: c.path, label: `${c.label} · ${bytes(c.size)}` }))} />} />
            {projectors.length ? (
              <HxRow title="Picture support" sub={`Adds ${bytes(projector)} so the model can read images`}
                trailing={<HxSwitch test="get-projector" checked={draft.projector} onChange={(projector) => setDraft({ ...draft, projector })} label="Picture support" />} />
            ) : null}
            <HxRow title="Download size" trailing={<span className="hx-value" data-test="get-size">{bytes(total)}</span>} />
            {hint ? <HxRow title="On this computer" trailing={<span className="hx-status" data-state={hint.state} data-test="get-fit">{hint.label}</span>} /> : null}
          </div>
          <div className="setting-about">Smaller versions are quicker and lighter but a little less capable. Q4_K_M is a good balance.</div>
        </>
      ) : null}
    </Sheet>
  );
}

// --- Routing --------------------------------------------------------------------------------------------------------------------------

const OTHER = "\u0000other";

function Routing({ view, probes, onSlot, onEscalate, onCustom, onBattery }: { view: View; probes: Record<string, Probe>; onSlot: (slot: string, backend: string, model: string) => void; onEscalate: (slot: string, backend: string, model: string) => void; onCustom: (slot: string, backend: string, model: string, escalate: boolean) => void; onBattery: (allow: boolean) => void }) {
  return (
    <div className="hx-stack" data-test="routing">
      {view.slots.map((slot) => {
        const backend = view.backends.find((b) => b.id === slot.backend);
        const local = !backend || backend.kind === "local";
        const choices = probes[slot.backend]?.models ?? [];
        const info = slotInfo(slot.slot);
        const strong = probes[slot.escalate_backend]?.models ?? [];
        return (
          <Group key={slot.slot} test={`slot:${slot.slot}`}>
            <HxRow leading={<IconTile name={info.icon} hue={info.hue} />} title={info.title} sub={info.about} />
            <HxRow title="Runs on"
              trailing={<Popup label={`${info.title}: where it runs`} test={`slot-backend:${slot.slot}`} value={slot.backend}
                options={view.backends.map((b) => ({ id: b.id, label: b.label, disabled: slot.local_only && b.kind !== "local", title: slot.local_only && b.kind !== "local" ? "Acting for the desktop only runs on this computer" : undefined }))}
                onChange={(id) => onSlot(slot.slot, id, "")} />} />
            <HxRow title="Model"
              trailing={<Popup label={`${info.title}: model`} test={`slot-model:${slot.slot}`} value={slot.model} placeholder={local ? "First installed" : "Choose a model"}
                options={[
                  ...(local ? [{ id: "", label: "First installed" }] : []),
                  ...(local ? view.models.map((m) => ({ id: m.id, label: shortName(m.id) })) : choices.map((m) => ({ id: m, label: m }))),
                  ...(slot.model && !(local ? view.models.map((m) => m.id) : choices).includes(slot.model) ? [{ id: slot.model, label: `${shortName(slot.model)}${local ? " (not installed)" : ""}` }] : []),
                  ...(local ? [] : [{ id: OTHER, label: "Other…" }]),
                ]}
                onChange={(id) => (id === OTHER ? onCustom(slot.slot, slot.backend, slot.model, false) : onSlot(slot.slot, slot.backend, id))} />} />
            {slot.local_only ? null : (
              <div data-test={`escalate:${slot.slot}`}>
                <HxRow title="When it is not enough"
                  trailing={<Popup label={`${info.title}: stronger model backend`} test={`escalate-backend:${slot.slot}`} value={slot.escalate_backend}
                    options={[{ id: "", label: "Nothing" }, ...view.backends.filter((b) => b.kind !== "local").map((b) => ({ id: b.id, label: b.label }))]}
                    onChange={(id) => onEscalate(slot.slot, id, "")} />} />
                {slot.escalate_backend ? (
                  <HxRow title="Stronger model"
                    trailing={<Popup label={`${info.title}: stronger model`} test={`escalate-model:${slot.slot}`} value={slot.escalate_model} placeholder="Choose a model"
                      options={[
                        ...strong.map((m) => ({ id: m, label: m })),
                        ...(slot.escalate_model && !strong.includes(slot.escalate_model) ? [{ id: slot.escalate_model, label: slot.escalate_model }] : []),
                        { id: OTHER, label: "Other…" },
                      ]}
                      onChange={(id) => (id === OTHER ? onCustom(slot.slot, slot.escalate_backend, slot.escalate_model, true) : onEscalate(slot.slot, slot.escalate_backend, id))} />} />
                ) : null}
              </div>
            )}
          </Group>
        );
      })}
      <Group title="Safety">
        <HxRow leading={<IconTile name="battery" hue={120} />} title="Use other computers on battery saver" sub="While the computer is saving power, only models on this computer are used unless this is on."
          trailing={<HxSwitch label="Use other computers and services on battery saver" checked={view.allow_remote_on_battery_saver} test="battery-remote" onChange={onBattery} />} />
        <HxRow leading={<IconTile name="lock" hue={212} />} title="Local by default" sub="Nothing leaves this computer unless you route a task to a backend here. If that backend cannot be reached, the request fails; it never quietly goes somewhere else." />
      </Group>
    </div>
  );
}

// --- the app --------------------------------------------------------------------------------------------------------------------------

function Models_() {
  useLayoutReport("models");
  const startPage = new URLSearchParams(location.search).get("page") as Page | null;
  const [page, setPage] = useState<Page>(PAGES.some((p) => p.id === startPage) ? (startPage as Page) : "overview");
  const [view, setView] = useState<View | null>(null);
  const [error, setError] = useState("");
  const [probes, setProbes] = useState<Record<string, Probe>>({});
  const [probing, setProbing] = useState("");
  const [draft, setDraft] = useState<Draft | null>(null);
  const [draftError, setDraftError] = useState("");
  const [custom, setCustom] = useState<{ slot: string; backend: string; value: string; escalate: boolean } | null>(null);
  const [jobs, setJobs] = useState<DownloadView[]>([]);
  const [dismissed, setDismissed] = useState<number[]>([]);
  const [memory, setMemory] = useState(0);
  const [get, setGet] = useState<GetDraft | null>(null);
  const [confirm, setConfirm] = useState<{ title: string; body: string; action: string; run: () => Promise<unknown>; test: string } | null>(null);

  const refresh = useCallback(() => {
    invoke<View>("models_view").then(setView, (e) => setError(errorText(e)));
  }, []);
  useEffect(() => {
    refresh();
    const timer = setInterval(refresh, 3000);
    return () => clearInterval(timer);
  }, [refresh]);

  // Downloads are watched more often than the rest, while there are any or the Models page is open.
  const anyRunning = jobs.some((j) => j.state === "running");
  useEffect(() => {
    if (page !== "models" && page !== "overview" && !anyRunning) return;
    let last = -1;
    const poll = () => invoke<DownloadView[]>("downloads").then((list) => {
      setJobs(list);
      const finished = list.filter((j) => j.state === "done").length;
      if (finished !== last) { if (last >= 0) refresh(); last = finished; }
    }, () => setJobs([]));
    poll();
    const timer = setInterval(poll, 1000);
    return () => clearInterval(timer);
  }, [page, anyRunning, refresh]);
  // Each page starts at its top, as a fresh page should.
  useEffect(() => { document.querySelector(".hx-scroll")?.scrollTo({ top: 0 }); }, [page]);
  useEffect(() => { invoke<number>("machine_memory").then(setMemory, () => {}); }, []);

  const lookUp = (draft: GetDraft) => {
    setGet({ ...draft, looking: true, error: "" });
    invoke<HubFile[]>("hub_files", { repo: draft.repo.trim() }).then(
      (files) => setGet((g) => g && g.repo === draft.repo ? { ...g, looking: false, files, pick: preferred(choicesOf(files)), projector: files.some((f) => f.path.startsWith("mmproj")) } : g),
      (e) => setGet((g) => g && g.repo === draft.repo ? { ...g, looking: false, error: errorText(e) } : g),
    );
  };
  const openGet = (entry: Entry | null) => {
    const draft: GetDraft = { repo: entry?.repo ?? "", fixed: !!entry, files: null, pick: "", projector: false, looking: false, error: "", entry: entry ?? undefined };
    if (entry) lookUp(draft); else setGet(draft);
  };
  const startDownload = () => {
    if (!get) return;
    const g = get;
    invoke<number>("model_download", { repo: g.repo.trim(), file: g.pick, projector: g.projector }).then(
      () => { setGet(null); setPage("models"); invoke<DownloadView[]>("downloads").then(setJobs, () => {}); },
      (e) => setGet({ ...g, error: errorText(e) }),
    );
  };

  const run = (promise: Promise<unknown>) => promise.then(() => { setError(""); refresh(); }, (e) => setError(errorText(e)));
  const probe = (id: string) => {
    setProbing(id);
    invoke<Probe>("backend_probe", { id }).then(
      (result) => { setProbes((p) => ({ ...p, [id]: result })); setProbing(""); },
      (e) => { setProbes((p) => ({ ...p, [id]: { ok: false, error: errorText(e) } })); setProbing(""); },
    );
  };
  const saveDraft = () => {
    if (!draft) return;
    invoke("backend_save", { id: draft.id.trim(), label: draft.label, baseUrl: draft.base_url, key: draft.key }).then(
      () => { const id = draft.id.trim(); setDraft(null); setDraftError(""); refresh(); probe(id); },
      (e) => setDraftError(errorText(e)),
    );
  };

  const title = PAGES.find((p) => p.id === page)?.title ?? "";
  const downloading = jobs.filter((j) => j.state === "running").length;
  let body: ReactNode = null;
  if (view) {
    if (page === "overview") body = <Overview view={view} jobs={jobs} goto={setPage} onUnload={() => run(invoke("engine_unload"))} />;
    if (page === "backends") {
      body = (
        <Backends view={view} probes={probes} probing={probing} onProbe={probe}
          onAdd={() => { setDraftError(""); setDraft({ ...NEW_DRAFT }); }}
          onEdit={(b) => { setDraftError(""); setDraft({ id: b.id, label: b.label, base_url: b.base_url, key: "", editing: true, had_key: b.has_key }); }}
          onClearKey={(b) => setConfirm({ title: `Forget the key for ${b.label}?`, body: "The saved key is deleted from this computer. You can enter it again later.", action: "Forget Key", test: "confirm-clear-key", run: () => invoke("key_clear", { id: b.id }) })}
          onRemove={(b) => setConfirm({ title: `Remove ${b.label}?`, body: "The backend and its saved key are removed. Tasks that use it must be moved first.", action: "Remove", test: "confirm-remove", run: () => invoke("backend_remove", { id: b.id }) })} />
      );
    }
    if (page === "models") {
      body = <Models view={view} memory={memory} jobs={jobs.filter((j) => !dismissed.includes(j.id))} onGet={openGet}
        onCancel={(id) => run(invoke("download_cancel", { id }))} onDismiss={(id) => setDismissed((d) => [...d, id])}
        onDelete={(m) => setConfirm({ title: `Delete ${nameOf(m.id).name}?`, body: `${bytes(m.bytes)} will be removed from this computer. You can download it again later.`, action: "Delete", test: "confirm-delete", run: () => invoke("model_delete", { id: m.id }) })} />;
    }
    if (page === "routing") {
      body = <Routing view={view} probes={probes} onSlot={(slot, backend, model) => run(invoke("slot_set", { slot, backend, model }))} onEscalate={(slot, backend, model) => run(invoke("slot_escalate", { slot, backend, model }))} onCustom={(slot, backend, model, escalate) => setCustom({ slot, backend, value: model, escalate })} onBattery={(allow) => run(invoke("battery_remote", { allow }))} />;
      // The models a backend offers are fetched once, so the model menus can offer them.
      view.slots.forEach((s) => { for (const id of [s.backend, s.escalate_backend]) { if (id && id !== "local" && !probes[id] && probing !== id) setTimeout(() => probe(id), 0); } });
    }
  }

  return (
    <>
      <AppFrame
        title={title}
        largeTitle
        toolbar={page === "backends" ? <button type="button" className="hx-tool" data-test="toolbar-add" title="Add a backend" aria-label="Add a backend" onClick={() => { setDraftError(""); setDraft({ ...NEW_DRAFT }); }}><Icon name="plus" /></button>
          : page === "models" ? <button type="button" className="hx-tool" data-test="toolbar-hub" title="Find a model on Hugging Face" aria-label="Find a model on Hugging Face" onClick={() => openGet(null)}><Icon name="search" /></button>
          : undefined}
        sidebar={<Sidebar>{PAGES.map((p) => <SidebarItem key={p.id} icon={p.icon} label={p.title} test={`page-${p.id}`} current={page === p.id} count={p.id === "models" && downloading ? downloading : undefined} onClick={() => setPage(p.id)} />)}</Sidebar>}
      >
        {view && !view.gateway ? <Banner tone="warn" test="gateway-down">{view.error || "The model service is not answering."}</Banner> : null}
        {error ? <Banner tone="error" test="error">{error}</Banner> : null}
        {body}
      </AppFrame>
      {draft ? <BackendSheet draft={draft} setDraft={setDraft} onSave={saveDraft} onCancel={() => setDraft(null)} error={draftError} /> : null}
      {get && view ? <GetSheet draft={get} setDraft={setGet} view={view} memory={memory} onLookUp={() => lookUp(get)} onDownload={startDownload} onCancel={() => setGet(null)} /> : null}
      {custom ? (
        <Sheet title="Model Name" test="custom-sheet" onCancel={() => setCustom(null)}
          actions={<><Button data-test="custom-cancel" onClick={() => setCustom(null)}>Cancel</Button><Button kind="primary" data-test="custom-save" onClick={() => { const c = custom; setCustom(null); run(invoke(c.escalate ? "slot_escalate" : "slot_set", { slot: c.slot, backend: c.backend, model: c.value })); }}>Use</Button></>}>
          <div className="hx-group">
            <FieldRow label="Model">
              <input className="hx-field-input" data-test="custom-model" autoFocus value={custom.value} placeholder="vendor/model-name" autoComplete="off" spellCheck={false}
                onChange={(e) => setCustom({ ...custom, value: e.target.value })} />
            </FieldRow>
          </div>
          <div className="setting-about">The name the backend knows the model by, such as big-model or vendor/model-name.</div>
        </Sheet>
      ) : null}
      {confirm ? (
        <Sheet alert title={confirm.title} test="confirm" onCancel={() => setConfirm(null)}
          actions={<><Button data-test="confirm-cancel" onClick={() => setConfirm(null)}>Cancel</Button><Button kind="destructive" data-test={confirm.test} onClick={() => { const c = confirm; setConfirm(null); run(c.run()); }}>{confirm.action}</Button></>}>
          <p>{confirm.body}</p>
        </Sheet>
      ) : null}
    </>
  );
}

function mount(host: HTMLElement) {
  document.body.classList.remove("settings");
  document.body.classList.add("hx", "hx-modern");
  createRoot(host).render(<Models_ />);
}

(window as unknown as { HelixModelsReact: unknown }).HelixModelsReact = { mount };
