import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";
import { Icon } from "./settingsui";

/** The Image stage: a picture beside the chat with a small fixed set of edits. Every edit is a description (crop, rotate, brightness, a
 * rectangle...) applied by Mobius to a copy, so the original is one undo away and a model's suggestion is the same kind of description the
 * buttons make, readable before it is applied. See docs/design/mobius-workspaces.md. */
interface Step { label: string; by?: string }
export interface ImgDoc { id: string; title: string; steps: Step[]; cursor: number; current: string; width: number; height: number; can_undo: boolean; can_redo: boolean }
export type Op = Record<string, unknown> & { op: string };
export interface ImageHooks {
  chatId: () => string;
  report: (error: unknown) => void;
  /** Send the picture as it stands with the next message (a copy, so removing it there cannot touch the edit). */
  ask: (picture: string) => void;
  active: (open: boolean) => void;
}
export interface ImageHandle {
  open: (picture?: string, title?: string) => void;
  /** A model's suggestion, applied after the person pressed Apply on it. */
  apply: (ops: Op[]) => Promise<void>;
  close: () => void;
}

const urls = new Map<string, string>();
function pictureUrl(id: string): Promise<string> {
  const known = urls.get(id);
  if (known) return Promise.resolve(known);
  return invoke<ArrayBuffer>("image_data", { id }).then((bytes) => { const url = URL.createObjectURL(new Blob([bytes])); urls.set(id, url); return url; });
}

type Tool = "crop" | "rect";
interface Box { x: number; y: number; w: number; h: number }

export function ImageStage({ register, hooks }: { register: (handle: ImageHandle) => void; hooks: ImageHooks }) {
  const [active, setActive] = useState(false);
  const [doc, setDoc] = useState<ImgDoc | null>(null);
  const [src, setSrc] = useState("");
  const [tool, setTool] = useState<Tool>("crop");
  const [box, setBox] = useState<Box | null>(null);
  const [brightness, setBrightness] = useState(0);
  const [contrast, setContrast] = useState(0);
  const [percent, setPercent] = useState(100);
  const [colour, setColour] = useState("#ff3b30");
  const [format, setFormat] = useState("png");
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState("");
  const img = useRef<HTMLImageElement>(null);
  const drag = useRef<{ x: number; y: number } | null>(null);
  const current = useRef<ImgDoc | null>(null);
  // The documents are kept under the chat as it was when the picture was opened (a new chat has no id until its first message).
  const key = useRef("");
  current.current = doc;

  const show = useCallback((next: ImgDoc | null) => { setDoc(next); setBox(null); setBrightness(0); setContrast(0); setNote(""); }, []);
  useEffect(() => { if (doc) pictureUrl(doc.current).then(setSrc, () => setSrc("")); else setSrc(""); }, [doc?.current]);

  const open = useCallback(async (picture?: string, title?: string) => {
    setActive(true); hooks.active(true);
    if (!current.current || picture) key.current = hooks.chatId();
    if (!picture) { const existing = await invoke<ImgDoc[]>("imgstage_list", { chatId: key.current }).catch(() => []); if (existing.length) show(existing[existing.length - 1]); return; }
    try { show(await invoke<ImgDoc>("imgstage_open", { chatId: key.current, picture, title: title ?? "Picture" })); } catch (reason) { hooks.report(reason); }
  }, [hooks, show]);

  const run = useCallback(async (ops: Op[], by: "you" | "mobius" = "you") => {
    const d = current.current;
    if (!d) { hooks.report("Open a picture on the Image stage first."); return; }
    setBusy(true);
    try { show(await invoke<ImgDoc>("imgstage_apply", { chatId: key.current, docId: d.id, ops, by })); } catch (reason) { hooks.report(reason); } finally { setBusy(false); }
  }, [hooks, show]);

  useEffect(() => { register({ open, apply: (ops: Op[]) => run(ops, "mobius"), close: () => { setActive(false); setDoc(null); hooks.active(false); } }); }, [register, open, run, hooks]);

  const move = async (delta: number) => { if (!doc) return; try { show(await invoke<ImgDoc>("imgstage_move", { chatId: key.current, docId: doc.id, delta })); } catch (reason) { hooks.report(reason); } };
  const jump = (cursor: number) => doc && move(cursor - doc.cursor);
  const pick = async () => { try { const ids = await invoke<string[]>("image_pick"); if (ids[0]) await open(ids[0], "Picture"); } catch (reason) { hooks.report(reason); } };
  const exportPicture = async () => { if (!doc) return; try { const path = await invoke<string | null>("imgstage_export", { chatId: key.current, docId: doc.id, format }); if (path) setNote(`Saved ${path}`); } catch (reason) { hooks.report(reason); } };
  const close = () => { setActive(false); setDoc(null); hooks.active(false); };

  // A rectangle dragged on the picture, kept in the picture's own pixels.
  const toPicture = (event: React.PointerEvent) => {
    const el = img.current; if (!el || !doc) return null;
    const r = el.getBoundingClientRect();
    const sx = doc.width / r.width, sy = doc.height / r.height;
    return { x: Math.max(0, Math.min(doc.width, Math.round((event.clientX - r.left) * sx))), y: Math.max(0, Math.min(doc.height, Math.round((event.clientY - r.top) * sy))) };
  };
  const down = (event: React.PointerEvent) => { const p = toPicture(event); if (!p) return; drag.current = p; setBox({ x: p.x, y: p.y, w: 0, h: 0 }); (event.target as Element).setPointerCapture?.(event.pointerId); };
  const moveDrag = (event: React.PointerEvent) => { const start = drag.current, p = toPicture(event); if (!start || !p) return; setBox({ x: Math.min(start.x, p.x), y: Math.min(start.y, p.y), w: Math.abs(p.x - start.x), h: Math.abs(p.y - start.y) }); };
  const up = () => { drag.current = null; setBox((b) => (b && b.w >= 2 && b.h >= 2 ? b : null)); };
  const overlay = (() => {
    const el = img.current; if (!box || !el || !doc) return null;
    const k = el.getBoundingClientRect().width / doc.width;
    return { left: el.offsetLeft + box.x * k, top: el.offsetTop + box.y * k, width: box.w * k, height: box.h * k };
  })();

  if (!active) return null;
  return <aside className="stage image-stage" data-test="image-stage" aria-label="Picture">
    <header>
      <strong>{doc?.title ?? "Image"}</strong>
      {doc && <span className="stage-state">{doc.width}×{doc.height}</span>}
      <span className="stage-spacer" />
      <button type="button" className="mini" data-test="img-undo" disabled={!doc?.can_undo || busy} onClick={() => move(-1)}>Undo</button>
      <button type="button" className="mini" data-test="img-redo" disabled={!doc?.can_redo || busy} onClick={() => move(1)}>Redo</button>
      <button type="button" className="icon-btn" aria-label="Close the picture" data-test="img-close" onClick={close}>×</button>
    </header>
    {!doc ? <div className="stage-empty" data-test="img-empty">
      <span className="stage-empty-icon"><Icon name="image" /></span>
      <h3>Edit a picture</h3>
      <p>Crop, rotate, resize or mark it up. Edits are made on a copy, and the original stays one Undo away.</p>
      <div className="stage-empty-actions">
        <button type="button" className="primary" data-test="img-open" onClick={pick}>Open a picture…</button>
        <button type="button" data-test="img-create" onClick={() => window.dispatchEvent(new CustomEvent("mobius-create-image"))}>Create one instead</button>
      </div>
    </div> : <>
      <div className="img-toolbar" role="toolbar" aria-label="Edit">
        <div className="img-group" role="group" aria-label="Select">
          <button type="button" className={tool === "crop" ? "on" : ""} data-test="img-tool-crop" onClick={() => setTool("crop")}>Crop</button>
          <button type="button" className={tool === "rect" ? "on" : ""} data-test="img-tool-rect" onClick={() => setTool("rect")}>Rectangle</button>
          {tool === "rect" && <input type="color" aria-label="Rectangle colour" value={colour} onChange={(e) => setColour(e.target.value)} />}
          {box && <button type="button" className="primary" data-test="img-apply-selection" disabled={busy} onClick={() => run([tool === "crop" ? { op: "crop", ...box } : { op: "rect", ...box, color: colour, thickness: Math.max(2, Math.round(Math.min(doc.width, doc.height) / 150)) }])}>{tool === "crop" ? "Crop to selection" : "Draw it"}</button>}
        </div>
        <div className="img-group" role="group" aria-label="Transform">
          <button type="button" data-test="img-rotate-left" disabled={busy} onClick={() => run([{ op: "rotate", degrees: 270 }])}>↺</button>
          <button type="button" data-test="img-rotate-right" disabled={busy} onClick={() => run([{ op: "rotate", degrees: 90 }])}>↻</button>
          <button type="button" disabled={busy} onClick={() => run([{ op: "flip", axis: "horizontal" }])}>Flip ↔</button>
          <button type="button" disabled={busy} onClick={() => run([{ op: "flip", axis: "vertical" }])}>Flip ↕</button>
          <label className="img-resize">Size <input type="number" min={1} max={800} value={percent} data-test="img-percent" onChange={(e) => setPercent(Number(e.target.value))} />%</label>
          <button type="button" data-test="img-resize" disabled={busy || percent === 100} onClick={() => run([{ op: "resize", percent }]).then(() => setPercent(100))}>Resize</button>
        </div>
        <div className="img-group" role="group" aria-label="Adjust">
          <label>Brightness <input type="range" min={-100} max={100} value={brightness} data-test="img-brightness" onChange={(e) => setBrightness(Number(e.target.value))} onPointerUp={() => brightness !== 0 && run([{ op: "brightness", amount: brightness }])} onKeyUp={() => brightness !== 0 && run([{ op: "brightness", amount: brightness }])} /></label>
          <label>Contrast <input type="range" min={-100} max={100} value={contrast} data-test="img-contrast" onChange={(e) => setContrast(Number(e.target.value))} onPointerUp={() => contrast !== 0 && run([{ op: "contrast", amount: contrast }])} onKeyUp={() => contrast !== 0 && run([{ op: "contrast", amount: contrast }])} /></label>
          <button type="button" data-test="img-grayscale" disabled={busy} onClick={() => run([{ op: "grayscale" }])}>Black and white</button>
          <button type="button" disabled={busy} onClick={() => run([{ op: "invert" }])}>Invert</button>
          <button type="button" disabled={busy} onClick={() => run([{ op: "blur", sigma: 3 }])}>Blur</button>
          <button type="button" disabled={busy} onClick={() => run([{ op: "sharpen", amount: 2 }])}>Sharpen</button>
        </div>
      </div>
      <div className="img-canvas" data-test="img-canvas">
        {src && <div className="img-wrap" onPointerDown={down} onPointerMove={moveDrag} onPointerUp={up}>
          <img ref={img} src={src} alt={doc.title} draggable={false} data-test="img-picture" onLoad={() => setBox((b) => b)} />
          {overlay && <div className={`img-selection ${tool}`} style={overlay} />}
        </div>}
      </div>
      <div className="img-history" data-test="img-history">
        <button type="button" className={doc.cursor === 0 ? "now" : ""} onClick={() => jump(0)}>Original</button>
        {doc.steps.map((s, i) => <button type="button" key={i} className={doc.cursor === i + 1 ? "now" : i + 1 > doc.cursor ? "undone" : ""} data-test={`img-step-${i}`} title={s.by === "mobius" ? "Suggested by Mobius, applied by you" : "Made by you"} onClick={() => jump(i + 1)}>{s.label}{s.by === "mobius" ? <span className="img-by">Mobius</span> : null}</button>)}
      </div>
      <footer className="img-footer">
        <button type="button" className="mini" data-test="img-ask" onClick={() => invoke<string>("image_clone", { id: doc.current }).then(hooks.ask, hooks.report)}>Ask Mobius about this</button>
        <span className="stage-spacer" />
        <select aria-label="Format" value={format} onChange={(e) => setFormat(e.target.value)}><option value="png">PNG</option><option value="jpeg">JPEG</option><option value="webp">WebP</option></select>
        <button type="button" className="mini" data-test="img-export" onClick={exportPicture}>Save as…</button>
        {note && <span className="stage-state" role="status">{note}</span>}
      </footer>
    </>}
  </aside>;
}

/** A model's suggested edits, shown as a card in the reply: what they are, and a button that applies them. */
export function EditsCard({ ops, onApply }: { ops: Op[]; onApply: () => void }) {
  const names = ops.map((o) => String(o.op).replace("_", " ")).join(", ");
  return <div className="artifact-card" data-test="edits-card">
    <div className="artifact-head"><strong>Suggested picture edits</strong><span>{ops.length} edit{ops.length === 1 ? "" : "s"}: {names}</span>
      <button type="button" className="mini primary" data-test="edits-apply" onClick={onApply}>Apply to the open picture</button></div>
    <pre>{JSON.stringify(ops, null, 1)}</pre>
  </div>;
}
