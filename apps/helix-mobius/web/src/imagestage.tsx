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
  const [panel, setPanel] = useState<"adjust" | "size" | "history">("adjust");
  const [zoom, setZoom] = useState<"fit" | "actual">("fit");
  const [room, setRoom] = useState({ w: 0, h: 0 });
  const canvas = useRef<HTMLDivElement>(null);
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

  useEffect(() => {
    const el = canvas.current; if (!el || typeof ResizeObserver === "undefined") return;
    const watch = new ResizeObserver(() => setRoom({ w: el.clientWidth - 24, h: el.clientHeight - 24 }));
    watch.observe(el); setRoom({ w: el.clientWidth - 24, h: el.clientHeight - 24 });
    return () => watch.disconnect();
  }, [doc?.id, panel]);

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
      <div className="img-body">
        <div className="img-rail" role="toolbar" aria-label="Tools" aria-orientation="vertical">
          <button type="button" className={tool === "crop" ? "on" : ""} aria-pressed={tool === "crop"} title="Crop: drag on the picture" data-test="img-tool-crop" onClick={() => setTool("crop")}><span className="rail-glyph">⌗</span>Crop</button>
          <button type="button" className={tool === "rect" ? "on" : ""} aria-pressed={tool === "rect"} title="Rectangle: drag on the picture" data-test="img-tool-rect" onClick={() => setTool("rect")}><span className="rail-glyph">▭</span>Box</button>
          <input type="color" className="rail-colour" aria-label="Rectangle colour" value={colour} onChange={(e) => setColour(e.target.value)} />
          <span className="rail-sep" />
          <button type="button" title="Rotate left" data-test="img-rotate-left" disabled={busy} onClick={() => run([{ op: "rotate", degrees: 270 }])}><span className="rail-glyph">↺</span>Left</button>
          <button type="button" title="Rotate right" data-test="img-rotate-right" disabled={busy} onClick={() => run([{ op: "rotate", degrees: 90 }])}><span className="rail-glyph">↻</span>Right</button>
          <button type="button" title="Flip left to right" disabled={busy} onClick={() => run([{ op: "flip", axis: "horizontal" }])}><span className="rail-glyph">⇋</span>Flip</button>
          <button type="button" title="Flip top to bottom" disabled={busy} onClick={() => run([{ op: "flip", axis: "vertical" }])}><span className="rail-glyph">⇅</span>Flop</button>
        </div>
        <div className="img-main">
          <div className="img-options" data-test="img-options">
            {box
              ? <><span>{tool === "crop" ? "Crop" : "Box"} {box.w}×{box.h}</span><span className="stage-spacer" /><button type="button" className="mini" onClick={() => setBox(null)}>Clear</button><button type="button" className="mini primary" data-test="img-apply-selection" disabled={busy} onClick={() => run([tool === "crop" ? { op: "crop", ...box } : { op: "rect", ...box, color: colour, thickness: Math.max(2, Math.round(Math.min(doc.width, doc.height) / 150)) }])}>{tool === "crop" ? "Crop to selection" : "Draw it"}</button></>
              : <><span className="hint">{tool === "crop" ? "Drag on the picture to choose what to keep." : "Drag on the picture to draw a box."}</span><span className="stage-spacer" /></>}
            <span className="img-zoom" role="group" aria-label="Zoom">
              <button type="button" className={zoom === "fit" ? "on" : ""} aria-pressed={zoom === "fit"} onClick={() => setZoom("fit")}>Fit</button>
              <button type="button" className={zoom === "actual" ? "on" : ""} aria-pressed={zoom === "actual"} onClick={() => setZoom("actual")}>100%</button>
            </span>
          </div>
          <div className="img-canvas" data-test="img-canvas" ref={canvas}>
            {src && <div className="img-wrap" onPointerDown={down} onPointerMove={moveDrag} onPointerUp={up}>
              <img ref={img} src={src} alt={doc.title} draggable={false} data-test="img-picture" onLoad={() => setBox((b) => b)}
                style={(() => { const k = zoom === "actual" ? 1 : Math.max(0.05, Math.min(room.w / doc.width, room.h / doc.height, 8)); return room.w > 0 || zoom === "actual" ? { width: Math.round(doc.width * k), height: Math.round(doc.height * k), maxWidth: "none", maxHeight: "none", imageRendering: k > 2 ? "pixelated" : "auto" } : undefined; })()} />
              {overlay && <div className={`img-selection ${tool}`} style={overlay} />}
            </div>}
          </div>
        </div>
      </div>
      <div className="img-panel">
        <div className="img-tabs" role="tablist" aria-label="Picture panels">
          {([["adjust", "Adjust"], ["size", "Size"], ["history", `History · ${doc.steps.length}`]] as const).map(([id, label]) => <button key={id} type="button" role="tab" aria-selected={panel === id} className={panel === id ? "on" : ""} data-test={`img-tab-${id}`} onClick={() => setPanel(id)}>{label}</button>)}
        </div>
        {panel === "adjust" ? <div className="img-adjust">
          <label>Brightness <input type="range" min={-100} max={100} value={brightness} data-test="img-brightness" onChange={(e) => setBrightness(Number(e.target.value))} onPointerUp={() => { if (brightness !== 0) run([{ op: "brightness", amount: brightness }]).then(() => setBrightness(0)); }} onKeyUp={() => { if (brightness !== 0) run([{ op: "brightness", amount: brightness }]).then(() => setBrightness(0)); }} /><output>{brightness}</output></label>
          <label>Contrast <input type="range" min={-100} max={100} value={contrast} data-test="img-contrast" onChange={(e) => setContrast(Number(e.target.value))} onPointerUp={() => { if (contrast !== 0) run([{ op: "contrast", amount: contrast }]).then(() => setContrast(0)); }} onKeyUp={() => { if (contrast !== 0) run([{ op: "contrast", amount: contrast }]).then(() => setContrast(0)); }} /><output>{contrast}</output></label>
          <div className="img-chips">
            <button type="button" data-test="img-grayscale" disabled={busy} onClick={() => run([{ op: "grayscale" }])}>Black and white</button>
            <button type="button" disabled={busy} onClick={() => run([{ op: "invert" }])}>Invert</button>
            <button type="button" disabled={busy} onClick={() => run([{ op: "blur", sigma: 3 }])}>Blur</button>
            <button type="button" disabled={busy} onClick={() => run([{ op: "sharpen", amount: 2 }])}>Sharpen</button>
          </div>
        </div> : null}
        {panel === "size" ? <div className="img-adjust">
          <div className="img-chips"><span className="stage-state">Now {doc.width}×{doc.height}</span></div>
          <label className="img-resize">Scale to <input type="number" min={1} max={800} value={percent} data-test="img-percent" onChange={(e) => setPercent(Number(e.target.value))} /> %
            <button type="button" data-test="img-resize" disabled={busy || percent === 100} onClick={() => run([{ op: "resize", percent }]).then(() => setPercent(100))}>Resize</button></label>
        </div> : null}
        {panel === "history" ? <div className="img-history" data-test="img-history">
          <button type="button" className={doc.cursor === 0 ? "now" : ""} onClick={() => jump(0)}>Original</button>
          {doc.steps.map((s, i) => <button type="button" key={i} className={doc.cursor === i + 1 ? "now" : i + 1 > doc.cursor ? "undone" : ""} data-test={`img-step-${i}`} title={s.by === "mobius" ? "Suggested by Mobius, applied by you" : "Made by you"} onClick={() => jump(i + 1)}>{s.label}{s.by === "mobius" ? <span className="img-by">Mobius</span> : null}</button>)}
        </div> : null}
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

/** The document operations the model may propose, said in words for the card. */
export function describeDocOp(o: Record<string, unknown>): string {
  const q = (t: unknown) => { const s = String(t ?? "").replace(/\s+/g, " ").trim(); return `“${s.length > 40 ? s.slice(0, 40) + "…" : s}”`; };
  switch (o.op) {
    case "replace": return `Change ${q(o.find)} to ${q(o.with)}${o.all ? " everywhere" : ""}`;
    case "delete": return `Remove ${q(o.find)}`;
    case "insert_after": return `Add a paragraph after ${q(o.anchor)}`;
    case "replace_section": return `Rewrite the section ${q(o.heading)}`;
    case "append": return "Add to the end";
    default: return String(o.op);
  }
}

export function DocEditsCard({ ops, onApply }: { ops: Record<string, unknown>[]; onApply: () => void }) {
  return <div className="artifact-card" data-test="docedits-card">
    <div className="artifact-head"><strong>Suggested document edits</strong><span>{ops.length} edit{ops.length === 1 ? "" : "s"}</span>
      <button type="button" className="mini primary" data-test="docedits-apply" onClick={onApply}>Review in the document</button></div>
    <ul className="docedits-list">{ops.map((o, i) => <li key={i}>{describeDocOp(o)}</li>)}</ul>
  </div>;
}
