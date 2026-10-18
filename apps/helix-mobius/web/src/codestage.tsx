import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** The Code stage: the chat's project folder beside the conversation. A file list (the files the last turn changed are marked), a viewer, and,
 * for a changed file, its diff with Revert. The file shown goes to the model with the next message as a visible chip, the same as a document. */
interface Change { path: string; kind: string; diff: string; added: number; removed: number; reverted: boolean }
export interface CodeHooks {
  report: (error: unknown) => void;
  /** The last turn's changes: the activity they came from and the files. */
  changes: () => { activityId: string; files: Change[] } | null;
  /** The file in view changed (null = none); the page keeps its chip. */
  shown: (open: { title: string; text: string } | null) => void;
  chooseFolder: () => void;
  folder: () => string;
}
export interface CodeHandle { open: () => void; refresh: () => void; close: () => void }

export function CodeStage({ register, hooks }: { register: (handle: CodeHandle) => void; hooks: CodeHooks }) {
  const [active, setActive] = useState(false);
  const [files, setFiles] = useState<string[]>([]);
  const [error, setError] = useState("");
  const [filter, setFilter] = useState("");
  const [selected, setSelected] = useState("");
  const [text, setText] = useState("");
  const [truncated, setTruncated] = useState(false);
  const [tick, setTick] = useState(0);
  // Edit with Mobius on lines: a range chosen by dragging over the line numbers (or a click, shift-click to extend), the bar for an
  // instruction, and the proposal for those lines to accept (Enter) or reject (Esc).
  const [range, setRange] = useState<{ a: number; b: number } | null>(null);
  const selecting = useRef(false);
  const [instruction, setInstruction] = useState("");
  const [working, setWorking] = useState(false);
  const [proposal, setProposal] = useState<{ start: number; end: number; old: string; text: string } | null>(null);
  const [applied, setApplied] = useState<string>("");
  const [problem, setProblem] = useState("");
  const accept = useRef<HTMLButtonElement>(null);
  useEffect(() => { const end = () => { selecting.current = false; }; window.addEventListener("mouseup", end); return () => window.removeEventListener("mouseup", end); }, []);

  const loadTree = useCallback(async () => {
    setError("");
    try { const tree = await invoke<{ files: string[] }>("code_tree"); setFiles(tree.files); }
    catch (reason) { setFiles([]); setError(String(reason)); }
  }, []);
  useEffect(() => { register({ open: () => { setActive(true); loadTree(); }, refresh: () => { setTick((t) => t + 1); loadTree(); }, close: () => { setActive(false); setSelected(""); hooks.shown(null); } }); }, [register, loadTree, hooks]);

  const changes = hooks.changes();
  const changed = useMemo(() => new Map((changes?.files ?? []).map((f, i) => [f.path, { file: f, index: i }])), [changes, tick]);
  const lo = range ? Math.min(range.a, range.b) : 0;
  const hi = range ? Math.max(range.a, range.b) : 0;
  const ask = async (what: string) => {
    if (!range || working || !selected) return;
    setWorking(true); setProblem(""); setApplied("");
    try {
      const out = await invoke<{ old: string; text: string }>("code_transform", { path: selected, start: lo, end: hi, instruction: what });
      setProposal({ start: lo, end: hi, ...out });
      setInstruction("");
      setTimeout(() => accept.current?.focus(), 0);
    } catch (reason) { setProblem(String(reason)); } finally { setWorking(false); }
  };
  const applyProposal = async () => {
    if (!proposal) return;
    try {
      const id = await invoke<string>("code_apply", { path: selected, start: proposal.start, end: proposal.end, old: proposal.old, text: proposal.text });
      setApplied(id); setProposal(null); setRange(null);
      const file = await invoke<{ text: string; truncated: boolean }>("code_read", { path: selected });
      setText(file.text); hooks.shown({ title: selected, text: file.text });
      loadTree();
    } catch (reason) { setProblem(String(reason)); setProposal(null); }
  };
  const undoApplied = async () => {
    try {
      await invoke("tools_undo_edit", { activityId: applied });
      setApplied("");
      const file = await invoke<{ text: string; truncated: boolean }>("code_read", { path: selected });
      setText(file.text); hooks.shown({ title: selected, text: file.text });
    } catch (reason) { setProblem(String(reason)); }
  };
  const show = async (path: string) => {
    setSelected(path); setRange(null); setProposal(null); setApplied(""); setProblem("");
    const hit = changed.get(path);
    if (hit) { setText(hit.file.diff); setTruncated(false); hooks.shown({ title: `${path} (changed in the last turn)`, text: hit.file.diff }); return; }
    try {
      const file = await invoke<{ text: string; truncated: boolean }>("code_read", { path });
      setText(file.text); setTruncated(file.truncated); hooks.shown({ title: path, text: file.text });
    } catch (reason) { setText(""); hooks.report(reason); }
  };
  // Files the turn deleted are not in the tree any more, but they are in the changes.
  const all = useMemo(() => Array.from(new Set([...files, ...(changes?.files ?? []).map((f) => f.path)])).sort(), [files, changes]);
  const shown = all.filter((f) => !filter || f.toLowerCase().includes(filter.toLowerCase()));
  if (!active) return null;
  const hit = changed.get(selected);
  const isDiff = !!hit;
  return <aside className="stage code-stage" data-test="code-stage" aria-label="Project">
    <header>
      <strong>Project</strong>
      <span className="stage-state" title={hooks.folder()}>{hooks.folder().split("/").filter(Boolean).pop() ?? ""}</span>
      <span className="stage-spacer" />
      <button type="button" className="mini" onClick={() => { loadTree(); setTick((t) => t + 1); }}>Refresh</button>
      <button type="button" className="icon-btn" aria-label="Close the project view" data-test="code-close" onClick={() => { setActive(false); setSelected(""); hooks.shown(null); }}>×</button>
    </header>
    {error ? <div className="code-empty"><p>{error}</p><button type="button" className="primary" data-test="code-choose" onClick={hooks.chooseFolder}>Choose a project folder…</button></div> : <div className="code-body">
      <div className="code-files">
        <input data-test="code-filter" placeholder="Filter files" value={filter} onChange={(e) => setFilter(e.target.value)} aria-label="Filter files" />
        <ul>{shown.map((f) => {
          const c = changed.get(f);
          return <li key={f}><button type="button" className={`${f === selected ? "active " : ""}${c ? "changed " + c.file.kind : ""}`} data-test={`code-file-${f}`} onClick={() => show(f)} title={f}>
            <span className="code-name">{f.split("/").pop()}</span><span className="code-dir">{f.includes("/") ? f.slice(0, f.lastIndexOf("/")) : ""}</span>{c ? <i title={c.file.kind}>{c.file.kind === "added" ? "+" : c.file.kind === "deleted" ? "−" : "●"}</i> : null}
          </button></li>;
        })}</ul>
        {all.length === 0 && <p className="sheet-note">No text files found.</p>}
      </div>
      <div className="code-view">
        {selected ? <>
          <div className="code-view-head"><strong>{selected}</strong>
            {isDiff && hit && !hit.file.reverted ? <button type="button" className="mini" data-test="code-revert" onClick={() => invoke("tools_revert_change", { activityId: changes!.activityId, index: hit.index }).then(() => setTick((t) => t + 1)).catch(hooks.report)}>Revert this change</button> : null}
            {isDiff && hit?.file.reverted ? <span className="stage-state">reverted</span> : null}
          </div>
          <pre className={`code-text${isDiff ? " diff" : ""}`} data-test="code-text">{isDiff
            ? text.split("\n").map((line, i) => <span key={i} className={line.startsWith("@@") ? "diff-head" : line.startsWith("+") ? "diff-add" : line.startsWith("-") ? "diff-del" : ""}>{line || " "}</span>)
            : text.split("\n").map((line, i) => <span key={i} className={`code-line${range && i + 1 >= lo && i + 1 <= hi ? " picked" : ""}`} data-test={`code-line-${i + 1}`}
              onMouseDown={(e) => { if (e.button !== 0 || (e.target as HTMLElement).tagName !== "I") return; e.preventDefault(); selecting.current = true; setProposal(null); setApplied(""); setRange(e.shiftKey && range ? { a: range.a, b: i + 1 } : { a: i + 1, b: i + 1 }); }}
              onMouseEnter={() => { if (selecting.current) setRange((r) => (r ? { a: r.a, b: i + 1 } : r)); }}><i title="Click or drag to select lines">{i + 1}</i>{line || " "}</span>)}</pre>
          {truncated && <p className="sheet-note">Only the first 3,000 lines are shown.</p>}
          {problem && <p className="stage-notice" role="alert" data-test="code-problem">{problem}</p>}
          {applied && <p className="stage-notice" role="status" data-test="code-applied">Applied. <button type="button" className="link" data-test="code-undo" onClick={undoApplied}>Undo</button></p>}
          {proposal ? <div className="stage-proposal code-proposal" data-test="code-proposal" role="group" aria-label="Suggested change" onKeyDown={(e) => { if (e.key === "Escape") { e.preventDefault(); setProposal(null); } }}>
            <pre className="diff">{[...proposal.old.split("\n").map((l, i) => <span key={`o${i}`} className="diff-del">{`- ${l}`}</span>), ...proposal.text.split("\n").map((l, i) => <span key={`n${i}`} className="diff-add">{`+ ${l}`}</span>)]}</pre>
            <div className="proposal-actions"><button type="button" ref={accept} className="primary" data-test="code-accept" onClick={applyProposal}>Accept <kbd>Enter</kbd></button><button type="button" data-test="code-reject" onClick={() => setProposal(null)}>Reject <kbd>Esc</kbd></button></div>
          </div> : range && !isDiff && <div className="stage-selbar" data-test="code-selbar" role="toolbar" aria-label="Edit the selected lines with Mobius">
            <span className="stage-state">Lines {lo}{hi > lo ? `–${hi}` : ""}</span>
            {[["Simplify", "Simplify this code without changing what it does."], ["Add comments", "Add short, useful comments."], ["Fix bug", "Find and fix the bug in this code."], ["Make it idiomatic", "Rewrite in the idiomatic style of this language, same behaviour."]].map(([label, what]) => <button key={label} type="button" disabled={working} onClick={() => ask(what)}>{label}</button>)}
            <input data-test="code-instruction" value={instruction} disabled={working} placeholder={working ? "Working…" : "Edit these lines with Mobius…"} maxLength={300} onChange={(e) => setInstruction(e.target.value)} onKeyDown={(e) => { if (e.key === "Enter" && instruction.trim()) { e.preventDefault(); ask(instruction); } }} />
          </div>}
        </> : <p className="code-hint">Pick a file. The one in view goes to Mobius with your next message, so you can ask about it. Files the last turn changed are marked and show their diff.</p>}
      </div>
    </div>}
  </aside>;
}
