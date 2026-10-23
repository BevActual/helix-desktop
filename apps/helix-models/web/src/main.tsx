import { useCallback, useEffect, useState, type ReactNode } from "react";
import { createRoot } from "react-dom/client";
import { AppFrame, Banner, Button, Chip, EmptyState, Group, HxRow, Popup, Sheet, Sidebar, SidebarItem, HxSwitch, invoke, useLayoutReport } from "@helix/ui";

type Page = "overview" | "backends" | "models" | "routing";
type Trust = "this" | "lan" | "cloud";

interface BackendView { id: string; label: string; kind: "local" | "openai"; base_url: string; trust: Trust; has_key: boolean }
interface SlotView { slot: string; backend: string; model: string; local_only: boolean }
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

function Models({ view, onDelete }: { view: View; onDelete: (m: ModelView) => void }) {
  if (!view.gateway) return <EmptyState icon="folder">The model service is not answering, so the installed models cannot be listed.</EmptyState>;
  if (!view.models.length) {
    return <EmptyState icon="folder"><span data-test="no-models">No models are installed. Put GGUF files in {view.snapshot.model_dir ?? "the models folder"}. Downloading from here is coming.</span></EmptyState>;
  }
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 16 }} data-test="models">
      <Group>
        {view.models.map((m) => (
          <HxRow key={m.id} test={`model:${m.id}`} title={shortName(m.id)} sub={bytes(m.bytes)}
            trailing={<div style={{ display: "flex", gap: 8, alignItems: "center" }}>
              {m.vision ? <Chip>Reads pictures</Chip> : null}
              {m.loaded ? <Chip tone="ok">In use</Chip> : null}
              <Button small kind="destructive" data-test={`delete:${m.id}`} onClick={() => onDelete(m)}>Delete</Button>
            </div>} />
        ))}
      </Group>
      <div className="setting-about">Models are read from {view.snapshot.model_dir ?? "the models folder"}. Deleting one removes its files from this computer.</div>
    </div>
  );
}

// --- Routing --------------------------------------------------------------------------------------------------------------------------

const OTHER = "\u0000other";

function Routing({ view, probes, onSlot, onCustom, onBattery }: { view: View; probes: Record<string, Probe>; onSlot: (slot: string, backend: string, model: string) => void; onCustom: (slot: string, backend: string, model: string) => void; onBattery: (allow: boolean) => void }) {
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
                  onChange={(id) => (id === OTHER ? onCustom(slot.slot, slot.backend, slot.model) : onSlot(slot.slot, slot.backend, id))} />
              </div>
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
  const [custom, setCustom] = useState<{ slot: string; backend: string; value: string } | null>(null);
  const [confirm, setConfirm] = useState<{ title: string; body: string; action: string; run: () => Promise<unknown>; test: string } | null>(null);

  const refresh = useCallback(() => {
    invoke<View>("models_view").then(setView, (e) => setError(errorText(e)));
  }, []);
  useEffect(() => {
    refresh();
    const timer = setInterval(refresh, 3000);
    return () => clearInterval(timer);
  }, [refresh]);

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
      body = <Models view={view} onDelete={(m) => setConfirm({ title: `Delete ${shortName(m.id)}?`, body: `${bytes(m.bytes)} are removed from this computer. You can get the model again later.`, action: "Delete", test: "confirm-delete", run: () => invoke("model_delete", { id: m.id }) })} />;
    }
    if (page === "routing") {
      body = <Routing view={view} probes={probes} onSlot={(slot, backend, model) => run(invoke("slot_set", { slot, backend, model }))} onCustom={(slot, backend, model) => setCustom({ slot, backend, value: model })} onBattery={(allow) => run(invoke("battery_remote", { allow }))} />;
      // The models a backend offers are fetched once, so the model field can suggest them.
      view.slots.forEach((s) => { if (s.backend !== "local" && !probes[s.backend] && probing !== s.backend) setTimeout(() => probe(s.backend), 0); });
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
      {custom ? (
        <Sheet title="Model name" test="custom-sheet" onCancel={() => setCustom(null)}
          actions={<><Button data-test="custom-cancel" onClick={() => setCustom(null)}>Cancel</Button><Button kind="primary" data-test="custom-save" onClick={() => { const c = custom; setCustom(null); run(invoke("slot_set", { slot: c.slot, backend: c.backend, model: c.value })); }}>Use</Button></>}>
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
