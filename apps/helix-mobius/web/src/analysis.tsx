import { useEffect, useRef, useState } from "react";
import { invoke, listen } from "@helix/ui";

interface EvidenceRecord { reference: string; tool: string; evidence: { evidence_id: string; normalized_result: unknown; [key: string]: unknown } }
interface Job { id: string; target: string; procedure: string; sha256: string; state: string; stage: string; started: number; error: string; records: EvidenceRecord[] }
interface Readiness { ready: boolean; detail: string }
export interface AnalysisHandle { open: (id?: string) => void }

function Evidence({ record }: { record: EvidenceRecord }) {
  const text = JSON.stringify(record.evidence, null, 2);
  const result = record.evidence.normalized_result;
  const pseudocode = result && typeof result === "object" && "pseudocode" in result && typeof result.pseudocode === "string" ? result.pseudocode : "";
  const labels: Record<string, string> = { binary_overview: "Binary overview", list_procedures: "Functions", analyze_function: "Function pseudocode and references", list_strings: "Strings and locations" };
  return <details className="analysis-evidence" id={`analysis-${record.reference}`} data-test={`analysis-${record.reference}`}>
    <summary><strong>[{record.reference}]</strong> {labels[record.tool] || record.tool}</summary>
    {pseudocode && <><p className="sheet-note">Reconstructed pseudocode</p><pre>{pseudocode.slice(0, 60000)}</pre></>}
    <p className="sheet-note">Evidence: {record.evidence.evidence_id}</p><pre>{text.slice(0, 60000)}</pre>
    {text.length > 60000 && <p className="sheet-note">Preview limited to 60,000 characters. The complete record is saved with this analysis.</p>}
  </details>;
}

export function Analysis({ register, report }: { register: (handle: AnalysisHandle) => void; report: (error: unknown) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [open, setOpen] = useState(false);
  const [readiness, setReadiness] = useState<Readiness | null>(null);
  const [job, setJob] = useState<Job | null>(null);
  const [path, setPath] = useState("");
  const [procedure, setProcedure] = useState("");
  const [models, setModels] = useState<string[]>([]);
  const [model, setModel] = useState("");
  const [error, setError] = useState("");
  const [pending, setPending] = useState(false);
  const [elapsed, setElapsed] = useState(0);
  const busy = pending || !!job && ["preparing", "analyzing", "cancelling"].includes(job.state);
  useEffect(() => {
    register({ open: (id) => {
      setError(""); setOpen(true);
      invoke<Readiness>("analysis_readiness").then(setReadiness, (e) => setError(String(e)));
      invoke<string[]>("routing_list_models").then((ids) => { setModels(ids); setModel((current) => ids.includes(current) ? current : ids[0] || ""); }, report);
      (id ? invoke<Job>("analysis_load", { id }) : invoke<Job>("analysis_view"))
        .then((next) => { setJob(next); if (next.target) setPath(next.target); setProcedure(next.procedure); }, (e) => setError(String(e)));
    } });
  }, [register, report]);
  useEffect(() => { const subscription = listen<Job>("analysis-view", setJob); return () => { subscription.then((stop) => stop()); }; }, []);
  useEffect(() => { if (open && !dialog.current?.open) dialog.current?.showModal(); if (!open && dialog.current?.open) dialog.current?.close(); }, [open]);
  useEffect(() => {
    const tick = () => setElapsed(job?.started ? Math.max(0, Math.floor(Date.now() / 1000) - job.started) : 0);
    tick(); if (!busy) return;
    const timer = setInterval(tick, 1000); return () => clearInterval(timer);
  }, [busy, job?.started]);
  const pick = async () => { setError(""); try { const selected = await invoke<string | null>("analysis_pick"); if (selected) setPath(selected); } catch (e) { setError(String(e)); } };
  const start = async () => {
    setError(""); setPending(true);
    try { await invoke<Job>("analysis_start", { path, procedure }); setJob(await invoke<Job>("analysis_view")); }
    catch (e) { setError(String(e)); } finally { setPending(false); }
  };
  const explain = async () => {
    if (!job) return; setError(""); setPending(true);
    try { await invoke("analysis_explain", { id: job.id, model }); setOpen(false); }
    catch (e) { setError(String(e)); } finally { setPending(false); }
  };
  const inventory = job?.records.find((record) => record.tool === "list_procedures")?.evidence.normalized_result;
  const functions = Array.isArray(inventory) ? inventory as { value: string; address: string }[] : [];
  return <dialog ref={dialog} className="sheet analysis-sheet" data-test="analysis-panel" onClose={() => setOpen(false)}>
    <div className="sheet-body">
      <header className="sheet-head"><h3>Analyze software</h3><p className="sheet-note">Inspect a Linux binary locally with REA and Ghidra. The selected program is never run.</p></header>
      <div className="sheet-scroll">
        <p className="sheet-note" data-test="analysis-readiness">{readiness?.detail || "Checking analysis tools…"}</p>
        <section className="sheet-section">
          <label className="sheet-field">Linux ELF file<div className="analysis-target"><input data-test="analysis-path" value={path} onChange={(e) => setPath(e.target.value)} disabled={busy} placeholder="/absolute/path/to/program" /><button type="button" data-test="analysis-pick" onClick={pick} disabled={busy}>Choose file</button></div></label>
          <label className="sheet-field">Function name or address (optional)<input data-test="analysis-procedure" value={procedure} onChange={(e) => setProcedure(e.target.value)} disabled={busy} maxLength={160} list="analysis-functions" placeholder="Default: main or entry point" /><datalist id="analysis-functions">{functions.slice(0, 200).map((fn) => <option key={fn.address} value={fn.address}>{fn.value}</option>)}</datalist></label>
          <p className="sheet-note">Up to 64 MiB. Analysis may take several minutes on the first import.</p>
        </section>
        {job?.id && <section className="sheet-section" aria-live="polite"><strong data-test="analysis-status">{job.stage}{busy ? ` · ${elapsed}s` : ""}</strong>{busy && <progress aria-label="Analysis running" />}{job.sha256 && <p className="analysis-digest">SHA-256: {job.sha256}</p>}<p className="sheet-note">Job {job.id} · {job.records.length} evidence records · decompiled code is pseudocode</p></section>}
        {(error || job?.error) && <pre className="analysis-error" role="alert" data-test="analysis-error">{error || job?.error}</pre>}
        {job?.records.map((record) => <Evidence key={record.reference} record={record} />)}
        {job?.state === "ready" && <section className="sheet-section"><label className="sheet-field">Explain with a model on this computer<select data-test="analysis-model" value={model} onChange={(e) => setModel(e.target.value)} disabled={busy}>{!models.length && <option value="">Install a local text model in Models &amp; routing</option>}{models.map((id) => <option key={id} value={id}>{id}</option>)}</select></label><p className="sheet-note">Starts a conversation with evidence references. This conversation and its follow-up questions stay local.</p></section>}
      </div>
      <footer className="sheet-actions"><button type="button" data-test="analysis-close" onClick={() => setOpen(false)}>Back to chat</button><span className="sheet-actions-spacer" />
        {busy ? <button type="button" data-test="analysis-cancel" disabled={pending || job?.state === "cancelling"} onClick={() => invoke("analysis_cancel").catch((e) => setError(String(e)))}>Cancel analysis</button> : <button type="button" className="primary" data-test="analysis-start" disabled={!readiness?.ready || !path} onClick={start}>Analyze</button>}
        {job?.state === "ready" && <button type="button" className="primary" data-test="analysis-explain" disabled={busy || !model} onClick={explain}>Explain locally</button>}
      </footer>
    </div>
  </dialog>;
}
