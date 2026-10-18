import { useCallback, useEffect, useMemo, useState } from "react";
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

  const loadTree = useCallback(async () => {
    setError("");
    try { const tree = await invoke<{ files: string[] }>("code_tree"); setFiles(tree.files); }
    catch (reason) { setFiles([]); setError(String(reason)); }
  }, []);
  useEffect(() => { register({ open: () => { setActive(true); loadTree(); }, refresh: () => { setTick((t) => t + 1); loadTree(); }, close: () => { setActive(false); setSelected(""); hooks.shown(null); } }); }, [register, loadTree, hooks]);

  const changes = hooks.changes();
  const changed = useMemo(() => new Map((changes?.files ?? []).map((f, i) => [f.path, { file: f, index: i }])), [changes, tick]);
  const show = async (path: string) => {
    setSelected(path);
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
            : text.split("\n").map((line, i) => <span key={i} className="code-line"><i>{i + 1}</i>{line || " "}</span>)}</pre>
          {truncated && <p className="sheet-note">Only the first 3,000 lines are shown.</p>}
        </> : <p className="code-hint">Pick a file. The one in view goes to Mobius with your next message, so you can ask about it. Files the last turn changed are marked and show their diff.</p>}
      </div>
    </div>}
  </aside>;
}
