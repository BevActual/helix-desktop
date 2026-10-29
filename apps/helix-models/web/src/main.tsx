import { useCallback, useEffect, useState, type ReactNode } from "react";
import { createRoot } from "react-dom/client";
import { AppFrame, Banner, Button, Chip, EmptyState, Group, HxRow, Meter, Popup, Sheet, Sidebar, SidebarItem, HxSwitch, invoke, useLayoutReport } from "@helix/ui";

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
interface HubFile { path: string; size: number }
interface DownloadView { id: number; repo: string; title: string; total: number; done: number; state: "running" | "done" | "failed" | "cancelled"; message: string }
interface Choice { path: string; label: string; size: number }
interface GetDraft { repo: string; fixed: boolean; files: HubFile[] | null; pick: string; projector: boolean; looking: boolean; error: string; about?: string }
interface Probe { ok: boolean; trust?: string; models?: string[]; error?: string }

const PAGES: { id: Page; title: string; icon: string }[] = [
  { id: "overview", title: "Overview", icon: "sparkle" },
  { id: "backends", title: "Backends", icon: "wifi" },
  { id: "models", title: "Models", icon: "folder" },
  { id: "routing", title: "Routing", icon: "grid" },
];
const SLOT_TITLES: Record<string, { title: string; about: string }> = {
  act: { title: "Act", about: "Changes the desktop for you. Only ever runs on this computer." },
  chat: { title: "Talk", about: "Conversations in Mobius and the bar." },
  summarize: { title: "Summarise", about: "Short summaries and titles." },
  vision: { title: "Pictures", about: "Reading images and screenshots." },
  code: { title: "Code", about: "Writing and explaining code." },
};
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

function errorText(error: unknown): string {
  return typeof error === "string" ? error : error instanceof Error ? error.message : String(error);
}

// --- Overview -------------------------------------------------------------------------------------------------------------------------

function Overview({ view, onUnload, goto }: { view: View; onUnload: () => void; goto: (page: Page) => void }) {
  const s = view.snapshot;
  const gpu = (s.devices ?? []).find((d) => d.gpu);
  const loaded = s.loaded ?? "";
  const busy = !!s.queue?.running || (s.queue?.waiting ?? 0) > 0;
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 24 }} data-test="overview">
      <Group title="Engine">
        <HxRow test="engine-row" title={loaded ? shortName(loaded) : "No model is loaded"}
          sub={loaded ? (busy ? "Answering now" : "Ready. Freed after a few idle minutes.") : "A model loads when something asks for it."}
          trailing={loaded ? <Button small data-test="unload" disabled={busy} onClick={onUnload}>Free memory</Button> : undefined} />
        <HxRow test="accelerator-row" title={gpu ? gpu.description : "Processor only"}
          sub={gpu ? `${bytes(gpu.memory_free)} free of ${bytes(gpu.memory_total)} video memory` : "No graphics card is being used. Answers are slower."}
          trailing={<Chip tone={gpu ? "ok" : undefined}>{gpu ? "Graphics card" : "CPU"}</Chip>} />
        {s.queue ? <HxRow test="queue-row" title="Waiting" sub={`${s.queue.waiting} of ${s.queue.max} places in line`} /> : null}
      </Group>
      <Group title="Where requests go">
        {view.slots.map((slot) => {
          const backend = view.backends.find((b) => b.id === slot.backend);
          const trust = backend ? TRUST[backend.trust] : TRUST.this;
          return (
            <HxRow key={slot.slot} test={`slot-summary:${slot.slot}`} title={SLOT_TITLES[slot.slot]?.title ?? slot.slot}
              sub={`${backend?.label ?? slot.backend}${slot.model ? ` · ${shortName(slot.model)}` : ""}`}
              trailing={<Chip tone={trust.tone}>{trust.label}</Chip>} />
          );
        })}
        <HxRow title="Change where they go" trailing={<Button small data-test="to-routing" onClick={() => goto("routing")}>Routing</Button>} />
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
  { title: "Another computer", draft: { id: "", label: "", base_url: "http://" } },
];

function BackendSheet({ draft, setDraft, onSave, onCancel, error }: { draft: Draft; setDraft: (d: Draft) => void; onSave: () => void; onCancel: () => void; error: string }) {
  const field = (name: keyof Draft, label: string, props: { placeholder?: string; type?: string; disabled?: boolean } = {}) => (
    <label style={{ display: "flex", flexDirection: "column", gap: 4 }}>
      <span className="setting-about">{label}</span>
      <input className="text-input" data-test={`backend-${name}`} value={String(draft[name])} placeholder={props.placeholder} type={props.type ?? "text"} disabled={props.disabled}
        autoComplete="off" onChange={(e) => setDraft({ ...draft, [name]: e.target.value })} />
    </label>
  );
  return (
    <Sheet title={draft.editing ? "Edit backend" : "Add a backend"} test="backend-sheet" onCancel={onCancel}
      actions={<><Button data-test="backend-cancel" onClick={onCancel}>Cancel</Button><Button kind="primary" data-test="backend-save" onClick={onSave}>Save</Button></>}>
      <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
        {!draft.editing ? (
          <div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
            {PRESETS.map((p) => <Button key={p.title} small data-test={`preset:${p.title}`} onClick={() => setDraft({ ...draft, ...p.draft })}>{p.title}</Button>)}
          </div>
        ) : null}
        {field("label", "Name", { placeholder: "Studio desktop" })}
        {field("id", "Short id", { placeholder: "studio", disabled: draft.editing })}
        {field("base_url", "Address", { placeholder: "http://192.168.1.20:8080/v1" })}
        {field("key", draft.had_key ? "API key (leave empty to keep the saved one)" : "API key (if it needs one)", { type: "password" })}
        <div className="setting-about">An address on the internet must be https. A computer on your own network may use http. The key goes to the model service, which keeps it; this page never shows it again.</div>
        {error ? <Banner tone="error" test="backend-error">{error}</Banner> : null}
      </div>
    </Sheet>
  );
}

function Backends({ view, probes, probing, onProbe, onEdit, onAdd, onRemove, onClearKey }: {
  view: View; probes: Record<string, Probe>; probing: string; onProbe: (id: string) => void; onEdit: (b: BackendView) => void; onAdd: () => void; onRemove: (b: BackendView) => void; onClearKey: (b: BackendView) => void;
}) {
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 24 }} data-test="backends">
      <Group>
        {view.backends.map((b) => {
          const result = probes[b.id];
          return (
            <HxRow key={b.id} test={`backend:${b.id}`} title={b.label}
              sub={<>{b.kind === "local" ? "The model engine on this computer" : b.base_url}
                {result ? <div className="setting-about" data-test={`probe-result:${b.id}`} style={{ color: result.ok ? undefined : "var(--danger, #d33)" }}>
                  {result.ok ? `Reachable · ${result.models?.length ?? 0} models offered` : result.error}</div> : null}</>}
              trailing={<>
                <Chip tone={TRUST[b.trust].tone}>{TRUST[b.trust].label}</Chip>
                {b.kind !== "local" ? <Chip tone={b.has_key ? "ok" : undefined}>{b.has_key ? "Key saved" : "No key"}</Chip> : null}
              </>}>
              {b.kind !== "local" ? (
                <div style={{ display: "flex", gap: 8, flexWrap: "wrap", marginTop: 8 }}>
                  <Button small data-test={`probe:${b.id}`} disabled={probing === b.id} onClick={() => onProbe(b.id)}>{probing === b.id ? "Testing…" : "Test"}</Button>
                  <Button small data-test={`edit:${b.id}`} onClick={() => onEdit(b)}>Edit</Button>
                  {b.has_key ? <Button small data-test={`clear-key:${b.id}`} onClick={() => onClearKey(b)}>Forget key</Button> : null}
                  <Button small kind="destructive" data-test={`remove:${b.id}`} onClick={() => onRemove(b)}>Remove</Button>
                </div>
              ) : null}
            </HxRow>
          );
        })}
      </Group>
      <div><Button kind="primary" data-test="add-backend" onClick={onAdd}>Add a backend…</Button></div>
      <div className="setting-about">Anything that speaks the OpenAI chat protocol works: another computer running llama-server, Ollama or LM Studio, or a hosted service. A key is only ever sent over https, or to a computer on your own network.</div>
    </div>
  );
}

// --- Models ---------------------------------------------------------------------------------------------------------------------------

// Models people commonly start with, all GGUF from their makers or ggml-org. Files and sizes are looked up when asked for.
const CATALOGUE: { repo: string; title: string; about: string; pictures?: boolean }[] = [
  { repo: "Qwen/Qwen3-4B-GGUF", title: "Qwen3 4B", about: "Small and quick. Good for talking, summaries and acting on the desktop." },
  { repo: "Qwen/Qwen3-8B-GGUF", title: "Qwen3 8B", about: "A stronger all-rounder for a graphics card with 8 GB or more." },
  { repo: "ggml-org/gemma-3-4b-it-GGUF", title: "Gemma 3 4B", about: "Small, and reads pictures.", pictures: true },
  { repo: "ggml-org/gemma-3-12b-it-GGUF", title: "Gemma 3 12B", about: "Reads pictures and writes well. Needs a large graphics card.", pictures: true },
  { repo: "Qwen/Qwen2.5-Coder-7B-Instruct-GGUF", title: "Qwen2.5 Coder 7B", about: "Writing and explaining code." },
  { repo: "ggml-org/Qwen2.5-VL-7B-Instruct-GGUF", title: "Qwen2.5 VL 7B", about: "Reads screenshots and documents.", pictures: true },
  { repo: "Qwen/Qwen2.5-1.5B-Instruct-GGUF", title: "Qwen2.5 1.5B", about: "Tiny. For older computers and quick titles." },
];

const SHARD = /-(\d{5})-of-(\d{5})\.gguf$/;

/** The model files a repository offers, a split model counted once with its parts' sizes added. */
function choicesOf(files: HubFile[]): Choice[] {
  const out = new Map<string, Choice>();
  for (const f of files) {
    if (f.path.startsWith("mmproj")) continue;
    const shard = f.path.match(SHARD);
    const key = shard ? f.path.replace(SHARD, "") : f.path;
    const label = shard ? `${key} (${Number(shard[2])} parts)` : shortName(f.path);
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

/** Q4_K_M when there is one, a balance of size and quality, else the smallest above 4 bits, else the smallest. */
function preferred(choices: Choice[]): string {
  const q4 = choices.find((c) => /q4_k_m/i.test(c.path));
  if (q4) return q4.path;
  const middling = choices.find((c) => /q[4-6]_/i.test(c.path));
  return (middling ?? choices[0])?.path ?? "";
}

function fit(size: number, view: View, memory: number): { label: string; tone?: "ok" | "warn" | "danger" } {
  const gpu = Math.max(0, ...(view.snapshot.devices ?? []).filter((d) => d.gpu).map((d) => d.memory_total));
  const need = size * 1.2; // the context and the engine's own working memory
  if (gpu && need <= gpu) return { label: "Fits the graphics card", tone: "ok" };
  if (memory && need <= memory * 0.6) return { label: gpu ? "Partly on the graphics card, slower" : "Runs from memory, slower", tone: "warn" };
  if (!memory && !gpu) return { label: bytes(size) };
  return { label: "Too large for this computer", tone: "danger" };
}

function Downloads({ jobs, onCancel, onDismiss }: { jobs: DownloadView[]; onCancel: (id: number) => void; onDismiss: (id: number) => void }) {
  if (!jobs.length) return null;
  return (
    <Group title="Downloads">
      {jobs.map((j) => {
        const percent = j.total ? (j.done / j.total) * 100 : 0;
        const sub = j.state === "running" ? `${bytes(j.done)} of ${bytes(j.total)}`
          : j.state === "done" ? "Ready to use"
          : j.state === "cancelled" ? "Stopped. Getting it again carries on where it stopped."
          : j.message || "The download failed.";
        return (
          <HxRow key={j.id} test={`download:${j.title}`} title={shortName(j.title)} sub={<span data-test={`download-state:${j.title}`} data-state={j.state}>{sub}</span>}
            trailing={j.state === "running"
              ? <Button small data-test={`download-cancel:${j.title}`} onClick={() => onCancel(j.id)}>Stop</Button>
              : <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
                  {j.state === "done" ? <Chip tone="ok">Done</Chip> : j.state === "failed" ? <Chip tone="danger">Failed</Chip> : null}
                  <Button small data-test={`download-dismiss:${j.title}`} onClick={() => onDismiss(j.id)}>Clear</Button>
                </div>}>
            {j.state === "running" ? <Meter percent={percent} live label={`${Math.round(percent)}%`} /> : null}
          </HxRow>
        );
      })}
    </Group>
  );
}

function Models({ view, jobs, onDelete, onGet, onCancel, onDismiss }: { view: View; jobs: DownloadView[]; onDelete: (m: ModelView) => void; onGet: (repo: string, about?: string) => void; onCancel: (id: number) => void; onDismiss: (id: number) => void }) {
  if (!view.gateway) return <EmptyState icon="folder">The model service is not answering, so the installed models cannot be listed.</EmptyState>;
  const running = new Set(jobs.filter((j) => j.state === "running").map((j) => j.repo));
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 24 }} data-test="models">
      <Downloads jobs={jobs} onCancel={onCancel} onDismiss={onDismiss} />
      {view.models.length ? (
        <Group title="On this computer">
          {view.models.map((m) => (
            <HxRow key={m.id} test={`model:${m.id}`} title={shortName(m.id)} sub={bytes(m.bytes)}
              trailing={<div style={{ display: "flex", gap: 8, alignItems: "center" }}>
                {m.vision ? <Chip>Reads pictures</Chip> : null}
                {m.loaded ? <Chip tone="ok">In use</Chip> : null}
                <Button small kind="destructive" data-test={`delete:${m.id}`} onClick={() => onDelete(m)}>Delete</Button>
              </div>} />
          ))}
        </Group>
      ) : (
        <div className="setting-about" data-test="no-models">No models are on this computer yet. Get one below, or put GGUF files in {view.snapshot.model_dir ?? "the models folder"}.</div>
      )}
      <Group title="Get models">
        {CATALOGUE.map((c) => (
          <HxRow key={c.repo} test={`catalogue:${c.repo}`} title={c.title} sub={c.about}
            trailing={<div style={{ display: "flex", gap: 8, alignItems: "center" }}>
              {c.pictures ? <Chip>Reads pictures</Chip> : null}
              <Button small data-test={`get:${c.repo}`} disabled={running.has(c.repo)} onClick={() => onGet(c.repo, c.about)}>{running.has(c.repo) ? "Getting…" : "Get…"}</Button>
            </div>} />
        ))}
        <HxRow test="from-hub" title="From Hugging Face…" sub="Any repository with GGUF files, by its name."
          trailing={<Button small data-test="get-other" onClick={() => onGet("")}>Choose…</Button>} />
      </Group>
      <div className="setting-about">Models are kept in {view.snapshot.model_dir ?? "the models folder"}. They are fetched by the model service from huggingface.co, checked against the repository's checksums, and only when you ask. Deleting one removes its files from this computer.</div>
    </div>
  );
}

function GetSheet({ draft, setDraft, view, memory, onLookUp, onDownload, onCancel }: { draft: GetDraft; setDraft: (d: GetDraft) => void; view: View; memory: number; onLookUp: () => void; onDownload: () => void; onCancel: () => void }) {
  const choices = draft.files ? choicesOf(draft.files) : [];
  const chosen = choices.find((c) => c.path === draft.pick);
  const hasProjector = !!draft.files?.some((f) => f.path.startsWith("mmproj"));
  const projector = hasProjector ? Math.min(...draft.files!.filter((f) => f.path.startsWith("mmproj")).map((f) => f.size)) : 0;
  const total = (chosen?.size ?? 0) + (draft.projector && hasProjector ? projector : 0);
  const hint = chosen ? fit(chosen.size, view, memory) : null;
  return (
    <Sheet title={draft.fixed ? draft.repo.split("/").pop() ?? draft.repo : "From Hugging Face"} test="get-sheet" onCancel={onCancel}
      actions={<>
        <Button data-test="get-cancel" onClick={onCancel}>Cancel</Button>
        {draft.files ? <Button kind="primary" data-test="get-download" disabled={!chosen} onClick={onDownload}>Download{total ? ` ${bytes(total)}` : ""}</Button>
          : <Button kind="primary" data-test="get-lookup" disabled={!draft.repo.includes("/") || draft.looking} onClick={onLookUp}>{draft.looking ? "Looking…" : "Look up"}</Button>}
      </>}>
      {draft.fixed ? (draft.about ? <p>{draft.about}</p> : null) : (
        <>
          <input className="text-input" data-test="get-repo" autoFocus value={draft.repo} placeholder="owner/name-GGUF" autoComplete="off" spellCheck={false} style={{ width: "100%" }}
            onChange={(e) => setDraft({ ...draft, repo: e.target.value, files: null, error: "" })}
            onKeyDown={(e) => { if (e.key === "Enter" && draft.repo.includes("/") && !draft.files) onLookUp(); }} />
          <div className="setting-about">The repository's name on huggingface.co, such as Qwen/Qwen3-4B-GGUF.</div>
        </>
      )}
      {draft.looking && draft.fixed ? <div className="setting-about" data-test="get-looking">Looking up the files…</div> : null}
      {draft.error ? <Banner tone="error" test="get-error">{draft.error}</Banner> : null}
      {draft.files && !choices.length ? <Banner tone="warn" test="get-none">This repository has no GGUF model files.</Banner> : null}
      {choices.length ? (
        <Group>
          <HxRow title="Version" sub={hint ? <span data-test="get-fit"><Chip tone={hint.tone === "danger" ? "danger" : hint.tone}>{hint.label}</Chip></span> : undefined}
            trailing={<Popup label="Version" test="get-version" value={draft.pick} onChange={(pick) => setDraft({ ...draft, pick })}
              options={choices.map((c) => ({ id: c.path, label: `${c.label} · ${bytes(c.size)}` }))} />} />
          {hasProjector ? (
            <HxRow title="Include picture support" sub={`Adds ${bytes(projector)} so the model can read images.`}
              trailing={<HxSwitch test="get-projector" checked={draft.projector} onChange={(projector) => setDraft({ ...draft, projector })} label="Include picture support" />} />
          ) : null}
        </Group>
      ) : null}
      {choices.length ? <div className="setting-about">Smaller versions are quicker and lighter but a little less capable. Q4_K_M is a good balance.</div> : null}
    </Sheet>
  );
}

// --- Routing --------------------------------------------------------------------------------------------------------------------------

const OTHER = "\u0000other";

function Routing({ view, probes, onSlot, onEscalate, onCustom, onBattery }: { view: View; probes: Record<string, Probe>; onSlot: (slot: string, backend: string, model: string) => void; onEscalate: (slot: string, backend: string, model: string) => void; onCustom: (slot: string, backend: string, model: string, escalate: boolean) => void; onBattery: (allow: boolean) => void }) {
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 24 }} data-test="routing">
      <Group>
        {view.slots.map((slot) => {
          const backend = view.backends.find((b) => b.id === slot.backend);
          const local = !backend || backend.kind === "local";
          const choices = probes[slot.backend]?.models ?? [];
          const info = SLOT_TITLES[slot.slot];
          return (
            <HxRow key={slot.slot} test={`slot:${slot.slot}`} title={info?.title ?? slot.slot} sub={info?.about}
              >
              <div style={{ display: "flex", gap: 8, alignItems: "center", flexWrap: "wrap", marginTop: 8 }}>
                <Popup label={`${info?.title ?? slot.slot}: where it runs`} test={`slot-backend:${slot.slot}`} value={slot.backend}
                  options={view.backends.map((b) => ({ id: b.id, label: b.label, disabled: slot.local_only && b.kind !== "local", title: slot.local_only && b.kind !== "local" ? "Acting for the desktop only runs on this computer" : undefined }))}
                  onChange={(id) => onSlot(slot.slot, id, "")} />
                <Popup label={`${info?.title ?? slot.slot}: model`} test={`slot-model:${slot.slot}`} value={slot.model} placeholder={local ? "First installed" : "Choose a model"}
                  options={[
                    ...(local ? [{ id: "", label: "First installed" }] : []),
                    ...(local ? view.models.map((m) => ({ id: m.id, label: shortName(m.id) })) : choices.map((m) => ({ id: m, label: m }))),
                    ...(slot.model && !(local ? view.models.map((m) => m.id) : choices).includes(slot.model) ? [{ id: slot.model, label: `${shortName(slot.model)}${local ? " (not installed)" : ""}` }] : []),
                    ...(local ? [] : [{ id: OTHER, label: "Other…" }]),
                  ]}
                  onChange={(id) => (id === OTHER ? onCustom(slot.slot, slot.backend, slot.model, false) : onSlot(slot.slot, slot.backend, id))} />
              </div>
              {slot.local_only ? null : (
                <div style={{ display: "flex", gap: 8, alignItems: "center", flexWrap: "wrap", marginTop: 8 }} data-test={`escalate:${slot.slot}`}>
                  <span className="setting-about">When it is not enough</span>
                  <Popup label={`${info?.title ?? slot.slot}: stronger model backend`} test={`escalate-backend:${slot.slot}`} value={slot.escalate_backend}
                    options={[{ id: "", label: "Nothing" }, ...view.backends.filter((b) => b.kind !== "local").map((b) => ({ id: b.id, label: b.label }))]}
                    onChange={(id) => onEscalate(slot.slot, id, "")} />
                  {slot.escalate_backend ? (
                    <Popup label={`${info?.title ?? slot.slot}: stronger model`} test={`escalate-model:${slot.slot}`} value={slot.escalate_model} placeholder="Choose a model"
                      options={[
                        ...(probes[slot.escalate_backend]?.models ?? []).map((m) => ({ id: m, label: m })),
                        ...(slot.escalate_model && !(probes[slot.escalate_backend]?.models ?? []).includes(slot.escalate_model) ? [{ id: slot.escalate_model, label: slot.escalate_model }] : []),
                        { id: OTHER, label: "Other…" },
                      ]}
                      onChange={(id) => (id === OTHER ? onCustom(slot.slot, slot.escalate_backend, slot.escalate_model, true) : onEscalate(slot.slot, slot.escalate_backend, id))} />
                  ) : null}
                </div>
              )}
            </HxRow>
          );
        })}
      </Group>
      <Group title="Safety">
        <HxRow title="Use other computers and services on battery saver" sub="Off by default. While the computer is saving power, only models on this computer are used."
          trailing={<HxSwitch label="Use other computers and services on battery saver" checked={view.allow_remote_on_battery_saver} test="battery-remote" onChange={onBattery} />} />
        <HxRow title="Local by default" sub="Nothing leaves this computer unless you route a slot to a backend here. If that backend is unreachable the request fails; it never quietly goes somewhere else." />
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
    if (page !== "models" && !anyRunning) return;
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
  useEffect(() => { invoke<number>("machine_memory").then(setMemory, () => {}); }, []);

  const lookUp = (draft: GetDraft) => {
    setGet({ ...draft, looking: true, error: "" });
    invoke<HubFile[]>("hub_files", { repo: draft.repo.trim() }).then(
      (files) => setGet((g) => g && g.repo === draft.repo ? { ...g, looking: false, files, pick: preferred(choicesOf(files)), projector: files.some((f) => f.path.startsWith("mmproj")) } : g),
      (e) => setGet((g) => g && g.repo === draft.repo ? { ...g, looking: false, error: errorText(e) } : g),
    );
  };
  const openGet = (repo: string, about?: string) => {
    const draft: GetDraft = { repo, fixed: !!repo, files: null, pick: "", projector: false, looking: false, error: "", about };
    if (repo) lookUp(draft); else setGet(draft);
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
  let body: ReactNode = null;
  if (view) {
    if (page === "overview") body = <Overview view={view} goto={setPage} onUnload={() => run(invoke("engine_unload"))} />;
    if (page === "backends") {
      body = (
        <Backends view={view} probes={probes} probing={probing} onProbe={probe}
          onAdd={() => { setDraftError(""); setDraft({ ...NEW_DRAFT }); }}
          onEdit={(b) => { setDraftError(""); setDraft({ id: b.id, label: b.label, base_url: b.base_url, key: "", editing: true, had_key: b.has_key }); }}
          onClearKey={(b) => setConfirm({ title: `Forget the key for ${b.label}?`, body: "The saved key is deleted from this computer. You can enter it again later.", action: "Forget key", test: "confirm-clear-key", run: () => invoke("key_clear", { id: b.id }) })}
          onRemove={(b) => setConfirm({ title: `Remove ${b.label}?`, body: "The backend and its saved key are removed. Slots that use it must be moved first.", action: "Remove", test: "confirm-remove", run: () => invoke("backend_remove", { id: b.id }) })} />
      );
    }
    if (page === "models") {
      body = <Models view={view} jobs={jobs.filter((j) => !dismissed.includes(j.id))} onGet={openGet}
        onCancel={(id) => run(invoke("download_cancel", { id }))} onDismiss={(id) => setDismissed((d) => [...d, id])}
        onDelete={(m) => setConfirm({ title: `Delete ${shortName(m.id)}?`, body: `${bytes(m.bytes)} are removed from this computer. You can get the model again later.`, action: "Delete", test: "confirm-delete", run: () => invoke("model_delete", { id: m.id }) })} />;
    }
    if (page === "routing") {
      body = <Routing view={view} probes={probes} onSlot={(slot, backend, model) => run(invoke("slot_set", { slot, backend, model }))} onEscalate={(slot, backend, model) => run(invoke("slot_escalate", { slot, backend, model }))} onCustom={(slot, backend, model, escalate) => setCustom({ slot, backend, value: model, escalate })} onBattery={(allow) => run(invoke("battery_remote", { allow }))} />;
      // The models a backend offers are fetched once, so the model field can suggest them.
      view.slots.forEach((s) => { for (const id of [s.backend, s.escalate_backend]) { if (id && id !== "local" && !probes[id] && probing !== id) setTimeout(() => probe(id), 0); } });
    }
  }

  return (
    <>
      <AppFrame
        title={title}
        largeTitle
        sidebar={<Sidebar>{PAGES.map((p) => <SidebarItem key={p.id} icon={p.icon} label={p.title} test={`page-${p.id}`} current={page === p.id} onClick={() => setPage(p.id)} />)}</Sidebar>}
      >
        {view && !view.gateway ? <Banner tone="warn" test="gateway-down">{view.error || "The model service is not answering."}</Banner> : null}
        {error ? <Banner tone="error" test="error">{error}</Banner> : null}
        {body}
      </AppFrame>
      {draft ? <BackendSheet draft={draft} setDraft={setDraft} onSave={saveDraft} onCancel={() => setDraft(null)} error={draftError} /> : null}
      {get && view ? <GetSheet draft={get} setDraft={setGet} view={view} memory={memory} onLookUp={() => lookUp(get)} onDownload={startDownload} onCancel={() => setGet(null)} /> : null}
      {custom ? (
        <Sheet title="Model name" test="custom-sheet" onCancel={() => setCustom(null)}
          actions={<><Button data-test="custom-cancel" onClick={() => setCustom(null)}>Cancel</Button><Button kind="primary" data-test="custom-save" onClick={() => { const c = custom; setCustom(null); run(invoke(c.escalate ? "slot_escalate" : "slot_set", { slot: c.slot, backend: c.backend, model: c.value })); }}>Use</Button></>}>
          <input className="text-input" data-test="custom-model" autoFocus value={custom.value} placeholder="Model name" autoComplete="off" style={{ width: "100%" }}
            onChange={(e) => setCustom({ ...custom, value: e.target.value })} />
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
  document.body.classList.add("hx");
  createRoot(host).render(<Models_ />);
}

(window as unknown as { HelixModelsReact: unknown }).HelixModelsReact = { mount };
