import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";
import { Blocks, type Block } from "./thread";

/** The other answers to the latest question (kept when a reply is redone) and a side-by-side view to choose between them: the current answer on the
 * left, another on the right with arrows when there are more, and "Keep this one" swaps it in (the one it replaces is kept, so a choice can be undone). */
interface Alt { text: string; model: string; tokens: number; ms: number }
interface Current { text: string; model: string; tokens: number; ms: number }
const speed = (a: { tokens: number; ms: number }) => (a.ms > 0 && a.tokens >= 20 ? ` · ${Math.round((a.tokens * 1000) / a.ms)} tok/s` : "");
const label = (a: { model: string; tokens: number; ms: number }) => `${a.model || "Default model"}${a.tokens ? ` · ${a.tokens} tokens` : ""}${speed(a)}`;

export function CompareBar({ index, current, streaming, report }: { index: number; current: Current; streaming: boolean; report: (error: unknown) => void }) {
  const [alts, setAlts] = useState<Alt[]>([]);
  const [open, setOpen] = useState(false);
  useEffect(() => {
    if (streaming) return;
    let live = true;
    invoke<Alt[]>("alt_list", { index }).then((list) => live && setAlts(Array.isArray(list) ? list : []), () => live && setAlts([]));
    return () => { live = false; };
  }, [index, streaming, current.text]);
  if (streaming || alts.length === 0) return null;
  return <>
    <div className="compare-bar" data-test="compare-bar"><span>{alts.length === 1 ? "Another answer is kept" : `${alts.length} other answers are kept`}</span>
      <button type="button" className="mini" data-test="compare-open" onClick={() => setOpen(true)}>Compare</button></div>
    {open && <CompareView index={index} current={current} alts={alts} onClose={() => setOpen(false)} report={report} />}
  </>;
}

function CompareView({ index, current, alts, onClose, report }: { index: number; current: Current; alts: Alt[]; onClose: () => void; report: (error: unknown) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [which, setWhich] = useState(0);
  const [left, setLeft] = useState<Block[]>([]);
  const [right, setRight] = useState<Block[]>([]);
  const alt = alts[Math.min(which, alts.length - 1)];
  useEffect(() => { dialog.current?.showModal(); }, []);
  useEffect(() => { invoke<Block[]>("markdown_blocks", { text: current.text }).then(setLeft, () => setLeft([])); }, [current.text]);
  useEffect(() => { invoke<Block[]>("markdown_blocks", { text: alt.text }).then(setRight, () => setRight([])); }, [alt.text]);
  const keep = () => invoke("alt_keep", { index, which }).then(onClose, report);
  return <dialog ref={dialog} className="compare-dialog" data-test="compare-view" aria-label="Compare answers" onClose={onClose} onClick={(e) => { if (e.target === dialog.current) onClose(); }}>
    <div className="compare-cols">
      <section aria-label="The answer now">
        <header><strong>This answer</strong><span>{label(current)}</span></header>
        <div className="compare-body" data-test="compare-current"><Blocks blocks={left} needle="" report={report} /></div>
        <footer><button type="button" className="primary" data-test="compare-keep-current" onClick={onClose}>Keep this one</button></footer>
      </section>
      <section aria-label="Another answer">
        <header><strong>{alts.length > 1 ? `Another answer ${which + 1} of ${alts.length}` : "Another answer"}</strong><span>{label(alt)}</span>
          {alts.length > 1 && <span className="compare-arrows"><button type="button" aria-label="Previous" disabled={which === 0} onClick={() => setWhich(which - 1)}>‹</button><button type="button" aria-label="Next" disabled={which >= alts.length - 1} onClick={() => setWhich(which + 1)}>›</button></span>}</header>
        <div className="compare-body" data-test="compare-other"><Blocks blocks={right} needle="" report={report} /></div>
        <footer><button type="button" className="primary" data-test="compare-keep-other" onClick={keep}>Keep this one</button></footer>
      </section>
    </div>
    <button type="button" className="compare-close" aria-label="Close" data-test="compare-close" onClick={onClose}>×</button>
  </dialog>;
}
