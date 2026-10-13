import { useEffect, useRef, useState } from "react";
import { invoke, listen } from "@helix/ui";

interface Card { id: string; file: string; title: string; tasks: string[]; installed: boolean; license: string; bytes: number; progress: { phase: string; done: number; total: number; error: string } | null }
export interface CreateImageHandle { open: (prompt?: string) => void }

export function CreateImage({ register }: { register: (handle: CreateImageHandle) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [open, setOpen] = useState(false);
  const [prompt, setPrompt] = useState("");
  const [cards, setCards] = useState<Card[]>([]);
  const [model, setModel] = useState("");
  const [shape, setShape] = useState("512x512");
  const [seed, setSeed] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const update = (next: Card[]) => { const images = next.filter((c) => c.tasks.includes("image")); setCards(images); setModel((current) => images.some((c) => c.file === current && c.installed) ? current : images.find((c) => c.installed)?.file || ""); };
  useEffect(() => {
    register({ open: (text) => { setOpen(true); setError(""); if (text) setPrompt(text); invoke<Card[]>("library_list").then(update, (e) => setError(String(e))); } });
  }, [register]);
  useEffect(() => { const stop = listen<Card[]>("library-changed", update); return () => { stop.then((fn) => fn()); }; }, []);
  useEffect(() => { if (open && !dialog.current?.open) dialog.current?.showModal(); if (!open && dialog.current?.open) dialog.current?.close(); }, [open]);
  const generate = async () => {
    setBusy(true); setError("");
    const [width, height] = shape.split("x").map(Number);
    const chosenSeed = seed.trim() ? Number(seed) : null;
    if (chosenSeed !== null && (!Number.isInteger(chosenSeed) || chosenSeed < 0 || chosenSeed > 2147483647)) { setError("Use a whole-number seed from 0 to 2147483647, or leave it blank."); setBusy(false); return; }
    try { await invoke("chat_generate_image", { prompt, model, width, height, seed: chosenSeed }); setOpen(false); }
    catch (e) { setError(String(e)); } finally { setBusy(false); }
  };
  const starter = cards.find((c) => c.id === "sd-turbo");
  const download = async () => {
    if (!starter || !window.confirm(`Download ${starter.title} (${(starter.bytes / 1e9).toFixed(2)} GB) from GPUStack on Hugging Face? License: ${starter.license}.`)) return;
    try { await invoke("library_download", { id: starter.id }); } catch (e) { setError(String(e)); }
  };
  return <dialog ref={dialog} className="sheet create-image-sheet" data-test="create-image-panel" onClose={() => setOpen(false)}>
    <div className="sheet-body">
      <header className="sheet-head"><h3>Create an image</h3><p className="sheet-note">Generated on this computer. The chat model releases GPU memory while your image is made.</p></header>
      <div className="sheet-scroll">
        <label className="sheet-field">Describe your image<textarea data-test="create-image-prompt" value={prompt} onChange={(e) => setPrompt(e.target.value)} maxLength={1000} placeholder="A blue ceramic mug on a wooden table, soft daylight…" disabled={busy} /></label>
        <label className="sheet-field">Local image model<select data-test="create-image-model" value={model} onChange={(e) => setModel(e.target.value)} disabled={busy}>{!model && <option value="">Download an image model first</option>}{cards.filter((c) => c.installed).map((c) => <option key={c.id} value={c.file}>{c.title}</option>)}</select></label>
        {starter && !starter.installed && <section className="sheet-section"><p className="sheet-note">SD-Turbo · {(starter.bytes / 1e9).toFixed(2)} GB · GPUStack on Hugging Face<br />{starter.license}</p>
          {starter.progress ? <><progress value={starter.progress.done} max={starter.progress.total || 1} /><p className="sheet-note">{starter.progress.phase}{starter.progress.error ? `: ${starter.progress.error}` : ""}</p><button type="button" onClick={() => invoke("library_cancel", { id: starter.id })}>Cancel download</button></> : <button type="button" data-test="create-image-download" onClick={download}>Download SD-Turbo</button>}
        </section>}
        <div className="image-options"><label className="sheet-field">Size<select data-test="create-image-size" value={shape} onChange={(e) => setShape(e.target.value)}><option value="512x512">Square · 512 × 512</option><option value="768x512">Landscape · 768 × 512</option><option value="512x768">Portrait · 512 × 768</option></select></label><label className="sheet-field">Seed (optional)<input data-test="create-image-seed" value={seed} onChange={(e) => setSeed(e.target.value)} placeholder="Random" inputMode="numeric" /></label></div>
        <p className="sheet-note">SD-Turbo uses four steps. Square images work best. This first version creates images from text; reference-image editing comes later.</p>
        {error && <p role="alert" className="sheet-warning" data-test="create-image-error">{error}</p>}
      </div>
      <footer className="sheet-actions"><button type="button" data-test="create-image-close" onClick={() => setOpen(false)}>Cancel</button><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="create-image-generate" disabled={busy || !model || !prompt.trim()} onClick={generate}>Generate image</button></footer>
    </div>
  </dialog>;
}
