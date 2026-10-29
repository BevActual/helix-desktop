import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@helix/ui";
import { AgentChooser } from "./agents";

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
  /** The requests made in this chat, oldest first, with their state. */
  tasks: () => CodeTask[];
  /** Scrolls the conversation to a request. */
  reveal: (index: number) => void;
  /** The latest terminal command in this chat, if any. */
  terminal: () => { command: string; state: string; output: string } | null;
}
/** One request in this chat and what came of it: the unit of work the agent was given. */
export interface CodeTask { index: number; request: string; state: "pending" | "running" | "waiting" | "failed" | "done"; steps: number; changed: number; inputs: string[] }
export interface CodeHandle { open: () => void; refresh: () => void; update: () => void; close: () => void; show: (path: string, line?: number) => void }

/** A folder or a file in the project tree. Folders sort before the files inside them. */
interface TreeNode { name: string; path: string; dir: boolean; children: TreeNode[] }
function treeOf(paths: string[]): TreeNode[] {
  const roots: TreeNode[] = [];
  const index = new Map<string, TreeNode>();
  for (const path of paths) {
    const parts = path.split("/");
    let parent = roots;
    let acc = "";
    parts.forEach((name, i) => {
      acc = acc ? `${acc}/${name}` : name;
      let node = index.get(acc);
      if (!node) {
        node = { name, path: acc, dir: i < parts.length - 1, children: [] };
        index.set(acc, node);
        parent.push(node);
      }
      parent = node.children;
    });
  }
  const sort = (nodes: TreeNode[]) => {
    nodes.sort((a, b) => (a.dir === b.dir ? a.name.localeCompare(b.name) : a.dir ? -1 : 1));
    nodes.forEach((node) => sort(node.children));
  };
  sort(roots);
  return roots;
}

function mark(kind: string) {
  return <i title={kind}>{kind === "added" ? "+" : kind === "deleted" ? "−" : "●"}</i>;
}

/** One contiguous change, with a couple of unchanged lines around it, for the review before a save. */
function hunkOf(before: string, after: string) {
  const oldLines = before.split("\n");
  const newLines = after.split("\n");
  let start = 0;
  while (start < oldLines.length && start < newLines.length && oldLines[start] === newLines[start]) start++;
  let oldEnd = oldLines.length;
  let newEnd = newLines.length;
  while (oldEnd > start && newEnd > start && oldLines[oldEnd - 1] === newLines[newEnd - 1]) { oldEnd--; newEnd--; }
  const context = 2;
  return {
    omittedBefore: Math.max(0, start - context),
    pre: oldLines.slice(Math.max(0, start - context), start),
    removed: oldLines.slice(start, oldEnd),
    added: newLines.slice(start, newEnd),
    post: oldLines.slice(oldEnd, Math.min(oldLines.length, oldEnd + context)),
    omittedAfter: Math.max(0, oldLines.length - (oldEnd + context)),
  };
}

function FileRow({ path, name, dir, selected, changed, show }: {
  path: string; name: string; dir?: string; selected: string;
  changed: Map<string, { file: Change; index: number }>; show: (path: string) => void;
}) {
  const hit = changed.get(path);
  return (
    <li>
      <button type="button" className={`${path === selected ? "active " : ""}${hit ? `changed ${hit.file.kind}` : ""}`} data-test={`code-file-${path}`} onClick={() => show(path)} title={path}>
        <span className="code-name">{name}</span>
        {dir ? <span className="code-dir">{dir}</span> : null}
        {hit ? mark(hit.file.kind) : null}
      </button>
    </li>
  );
}

function FileTree({ nodes, closed, toggle, selected, changed, show }: {
  nodes: TreeNode[]; closed: Set<string>; toggle: (path: string) => void; selected: string;
  changed: Map<string, { file: Change; index: number }>; show: (path: string) => void;
}) {
  return (
    <ul>
      {nodes.map((node) => node.dir ? (
        <li key={node.path}>
          <button type="button" className="code-folder" aria-expanded={!closed.has(node.path)} data-test={`code-dir-${node.path}`} onClick={() => toggle(node.path)}>
            <span className="code-chevron" aria-hidden="true">{closed.has(node.path) ? "▸" : "▾"}</span>
            <span className="code-name">{node.name}</span>
          </button>
          {closed.has(node.path) ? null : <FileTree nodes={node.children} closed={closed} toggle={toggle} selected={selected} changed={changed} show={show} />}
        </li>
      ) : (
        <FileRow key={node.path} path={node.path} name={node.name} selected={selected} changed={changed} show={show} />
      ))}
    </ul>
  );
}

export function CodeStage({ register, hooks }: { register: (handle: CodeHandle) => void; hooks: CodeHooks }) {
  const [active, setActive] = useState(false);
  const [files, setFiles] = useState<string[]>([]);
  const [error, setError] = useState("");
  const [filter, setFilter] = useState("");
  const [hits, setHits] = useState<{ path: string; line: number; text: string; truncated?: boolean }[]>([]);
  const [hitNote, setHitNote] = useState("");
  const jump = useRef<number | null>(null);
  const [jumpToken, setJumpToken] = useState(0);
  const [focusLine, setFocusLine] = useState<number | null>(null);
  const [closed, setClosed] = useState<Set<string>>(new Set());
  const [selected, setSelected] = useState("");
  const [text, setText] = useState("");
  const [source, setSource] = useState("");
  const [truncated, setTruncated] = useState(false);
  // A file the person is typing: base is the file as opened, draft is the textarea. Nothing is written until they review and accept.
  const [base, setBase] = useState("");
  const [crlf, setCrlf] = useState(false);
  const [draft, setDraft] = useState<string | null>(null);
  const [reviewing, setReviewing] = useState(false);
  const draftRef = useRef<string | null>(null);
  const baseRef = useRef("");
  const selectedRef = useRef("");
  const [tick, setTick] = useState(0);
  const [side, setSide] = useState<"files" | "tasks" | "terminal">("files");
  const showRef = useRef<(path: string, at?: number) => void>(() => {});
  // Follow the agent: the viewer opens each file the agent touches while it works. Off until the person turns it on.
  const [follow, setFollow] = useState(() => { try { return localStorage.getItem("mobius-code-follow") === "1"; } catch { return false; } });
  const followed = useRef("");
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
  const save = useRef<HTMLButtonElement>(null);
  draftRef.current = draft;
  baseRef.current = base;
  selectedRef.current = selected;
  useEffect(() => { const end = () => { selecting.current = false; }; window.addEventListener("mouseup", end); return () => window.removeEventListener("mouseup", end); }, []);
  const leaveRef = useRef<(force: boolean) => void>(() => {});

  const loadTree = useCallback(async () => {
    setError("");
    try { const tree = await invoke<{ files: string[] }>("code_tree"); setFiles(tree.files); }
    catch (reason) { setFiles([]); setError(String(reason)); }
  }, []);
  useEffect(() => { register({ open: () => { setActive(true); loadTree(); }, refresh: () => { setTick((t) => t + 1); loadTree(); }, update: () => setTick((t) => t + 1), close: () => leaveRef.current(true), show: (path, line) => showRef.current(path, line) }); }, [register, loadTree]);

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
      const file = await invoke<{ text: string; truncated: boolean; source?: string }>("code_read", { path: selected });
      setText(file.text); setSource(file.source ?? ""); setTruncated(file.truncated); hooks.shown({ title: selected, text: file.text });
      loadTree();
    } catch (reason) { setProblem(String(reason)); setProposal(null); }
  };
  const undoApplied = async () => {
    try {
      await invoke("tools_undo_edit", { activityId: applied });
      setApplied("");
      const file = await invoke<{ text: string; truncated: boolean; source?: string }>("code_read", { path: selected });
      setText(file.text); setSource(file.source ?? ""); setTruncated(file.truncated); hooks.shown({ title: selected, text: file.text });
    } catch (reason) { setProblem(String(reason)); }
  };
  const dirty = () => draftRef.current !== null && draftRef.current !== baseRef.current;
  const show = async (path: string, at?: number) => {
    if (path === selectedRef.current && draftRef.current !== null) return;
    if (path !== selectedRef.current && dirty()) { setProblem("Review or cancel this edit before opening another file."); return; }
    setSelected(path); setRange(null); setProposal(null); setApplied(""); setProblem(""); setDraft(null); setReviewing(false); setFocusLine(null);
    setClosed((old) => {
      const next = new Set(old);
      path.split("/").slice(0, -1).reduce((acc, part) => {
        const dir = acc ? `${acc}/${part}` : part;
        next.delete(dir);
        return dir;
      }, "");
      return next;
    });
    const hit = at ? undefined : changed.get(path);
    if (hit) { setText(hit.file.diff); setTruncated(false); hooks.shown({ title: `${path} (changed in the last turn)`, text: hit.file.diff }); return; }
    try {
      const file = await invoke<{ text: string; truncated: boolean; source?: string }>("code_read", { path });
      setText(file.text); setSource(file.source ?? ""); setTruncated(file.truncated); hooks.shown({ title: path, text: file.text });
      if (at) { jump.current = at; setFocusLine(at); setJumpToken((n) => n + 1); }
    } catch (reason) { setText(""); hooks.report(reason); }
  };
  showRef.current = show;
  // Files the turn deleted are not in the tree any more, but they are in the changes.
  const all = useMemo(() => Array.from(new Set([...files, ...(changes?.files ?? []).map((f) => f.path)])).sort(), [files, changes]);
  const shown = all.filter((f) => !filter || f.toLowerCase().includes(filter.toLowerCase()));
  useEffect(() => {
    const query = filter.trim();
    if (!active || query.length < 2) { setHits([]); setHitNote(""); return; }
    const timer = window.setTimeout(() => {
      invoke<{ hits: { path: string; line: number; text: string }[]; truncated: boolean }>("code_search", { query })
        .then((found) => { setHits(found.hits); setHitNote(found.truncated ? "Showing the first 40 matches." : ""); })
        .catch(() => { setHits([]); setHitNote(""); });
    }, 250);
    return () => window.clearTimeout(timer);
  }, [filter, active]);
  useEffect(() => {
    const line = jump.current;
    if (!line || !text) return;
    jump.current = null;
    document.querySelector(`[data-test="code-line-${line}"]`)?.scrollIntoView({ block: "center" });
  }, [text, selected, jumpToken]);
  const tasks = hooks.tasks();
  const term = hooks.terminal();
  const running = [...tasks].reverse().find((t) => t.state === "running" || t.state === "waiting");
  const fileSet = useMemo(() => new Set(all), [all]);
  useEffect(() => {
    if (!follow || !running) return;
    // The newest step that names a file of the project.
    for (let i = running.inputs.length - 1; i >= 0; i--) {
      const hitPath = running.inputs[i].split(/[\s"'`]+/).map((t) => t.replace(/[,:;]+$/, "")).find((t) => fileSet.has(t));
      if (hitPath) { if (followed.current !== hitPath + ":" + running.inputs.length) { followed.current = hitPath + ":" + running.inputs.length; if (hitPath !== selected) show(hitPath); } return; }
    }
  }, [tick, follow, running?.inputs.length]);
  const toggleFollow = () => { const next = !follow; setFollow(next); try { localStorage.setItem("mobius-code-follow", next ? "1" : "0"); } catch { /* not remembered */ } };
  const beginEdit = () => { const lf = source.replace(/\r\n/g, "\n"); setCrlf(source.includes("\r\n")); setDraft(lf); setBase(lf); setRange(null); setProposal(null); setApplied(""); setProblem(""); setReviewing(false); };
  const cancelEdit = () => { setDraft(null); setReviewing(false); setProblem(""); };
  const openReview = () => { if (draft === null) return; if (draft === base) { setProblem("Nothing changed."); return; } setProblem(""); setReviewing(true); setTimeout(() => save.current?.focus(), 0); };
  const saveDraft = async () => {
    if (draft === null) return;
    try {
      const eol = (text: string) => (crlf ? text.replace(/\n/g, "\r\n") : text);
      const id = await invoke<string>("code_save", { path: selected, before: eol(base), after: eol(draft) });
      setApplied(id); setDraft(null); setReviewing(false);
      const file = await invoke<{ text: string; truncated: boolean; source?: string }>("code_read", { path: selected });
      setText(file.text); setSource(file.source ?? ""); setTruncated(file.truncated); hooks.shown({ title: selected, text: file.text });
      loadTree();
    } catch (reason) { setProblem(String(reason)); }
  };
  leaveRef.current = (force) => {
    if (dirty()) {
      // The person's typing is not thrown away: leaving by the × asks first, and switching mode only hides the stage, so it is all still here on return.
      if (!force) { setProblem("Review or cancel this edit before leaving."); return; }
      setActive(false); hooks.shown(null);
      return;
    }
    setDraft(null); setReviewing(false); setActive(false); setSelected(""); hooks.shown(null);
  };
  if (!active) return null;
  const hunk = draft !== null && reviewing ? hunkOf(base, draft) : null;
  const hit = changed.get(selected);
  const isDiff = !!hit;
  return <aside className="stage code-stage" data-test="code-stage" aria-label="Project">
    <header>
      <strong>Project</strong>
      <span className="stage-state" title={hooks.folder()}>{hooks.folder().split("/").filter(Boolean).pop() ?? ""}</span>
      <span className="stage-spacer" />
      <AgentChooser report={hooks.report} enabled={!!hooks.folder()} />
      <button type="button" className={`mini${follow ? " on" : ""}`} aria-pressed={follow} data-test="code-follow" title="Open each file the agent touches while it works" onClick={toggleFollow}>Follow</button>
      <button type="button" className="mini" onClick={() => { loadTree(); setTick((t) => t + 1); }}>Refresh</button>
      <button type="button" className="icon-btn" aria-label="Close the project view" data-test="code-close" onClick={() => leaveRef.current(false)}>×</button>
    </header>
    {error ? <div className="code-empty"><p>{error}</p><button type="button" className="primary" data-test="code-choose" onClick={hooks.chooseFolder}>Choose a project folder…</button></div> : <div className="code-body">
      <div className="code-files">
        <div className="code-side-tabs" role="tablist" aria-label="Project">
          <button type="button" role="tab" aria-selected={side === "files"} className={side === "files" ? "on" : ""} data-test="code-tab-files" onClick={() => setSide("files")}>Files</button>
          <button type="button" role="tab" aria-selected={side === "tasks"} className={side === "tasks" ? "on" : ""} data-test="code-tab-tasks" onClick={() => setSide("tasks")}>Tasks{tasks.length ? ` · ${tasks.length}` : ""}{running ? <i className="task-dot" aria-label="running" /> : null}</button>
          {term ? <button type="button" role="tab" aria-selected={side === "terminal"} className={side === "terminal" ? "on" : ""} data-test="code-tab-terminal" onClick={() => setSide("terminal")}>Terminal{term.state === "running" ? <i className="task-dot" aria-label="running" /> : null}</button> : null}
        </div>
        {side === "terminal" && term ? <div className="code-terminal" data-test="code-terminal">
          <div className="code-terminal-head"><strong data-test="code-terminal-cmd">{term.command.split("\n")[0]}</strong><span className="stage-state" data-test={`code-terminal-${term.state}`}>{({ waiting: "Needs you", running: "Running", completed: "Done", failed: "Failed", declined: "Skipped" } as Record<string, string>)[term.state] || term.state}</span></div>
          <pre className="code-text terminal-out" data-test="code-terminal-out">{term.output || (term.state === "running" ? "Waiting for output…" : "")}</pre>
        </div> : null}
        {side === "tasks" ? <ol className="code-tasks" data-test="code-tasks">
          {[...tasks].reverse().map((t) => <li key={t.index}><button type="button" className={`code-task ${t.state}`} data-test={`code-task-${t.index}`} onClick={() => hooks.reveal(t.index)} title="Show this request in the conversation">
            <span className="code-task-state" data-test={`code-task-${t.index}-${t.state}`}>{({ pending: "Queued", running: "Running", waiting: "Needs you", failed: "Failed", done: "Done" })[t.state]}</span>
            <span className="code-task-title">{t.request.split("\n")[0].slice(0, 90)}</span>
            <span className="code-task-meta">{t.steps ? `${t.steps} step${t.steps === 1 ? "" : "s"}` : "answered"}{t.changed ? ` · ${t.changed} file${t.changed === 1 ? "" : "s"} changed` : ""}</span>
          </button></li>)}
          {tasks.length === 0 && <li className="sheet-note">Requests you make in this chat are listed here with what they did.</li>}
        </ol> : side === "files" ? <>
        <input data-test="code-filter" placeholder="Filter files or search text" value={filter} onChange={(e) => setFilter(e.target.value)} aria-label="Filter files or search their text" />
        {filter
          ? <ul>{shown.map((f) => <FileRow key={f} path={f} name={f.split("/").pop() ?? f} dir={f.includes("/") ? f.slice(0, f.lastIndexOf("/")) : ""} selected={selected} changed={changed} show={show} />)}</ul>
          : <FileTree nodes={treeOf(all)} closed={closed} toggle={(path) => setClosed((old) => { const next = new Set(old); if (next.has(path)) next.delete(path); else next.add(path); return next; })} selected={selected} changed={changed} show={show} />}
        {hits.length > 0 && <ul className="code-hits" data-test="code-hits">{hits.map((h) => <li key={`${h.path}:${h.line}`}><button type="button" data-test={`code-hit-${h.path}:${h.line}`} onClick={() => show(h.path, h.line)} title={h.path}>
          <span className="code-name">{h.path.split("/").pop()}</span>
          <span className="code-dir">{h.line}</span>
          <span className="code-hit-text">{h.text}</span>
        </button></li>)}</ul>}
        {hitNote && <p className="sheet-note">{hitNote}</p>}
        {all.length === 0 && <p className="sheet-note">No text files found.</p>}
        </> : null}
      </div>
      <div className="code-view">
        {selected ? <>
          <div className="code-view-head"><strong>{selected}</strong>
            {draft !== null && !reviewing ? <button type="button" className="mini" data-test="code-review" onClick={openReview}>Review change</button> : null}
            {draft !== null ? <button type="button" className="mini" data-test="code-edit-cancel" onClick={cancelEdit}>Cancel</button> : null}
            {draft === null && !isDiff && !truncated ? <button type="button" className="mini" data-test="code-edit" onClick={beginEdit}>Edit</button> : null}
            {isDiff && hit && !hit.file.reverted ? <button type="button" className="mini" data-test="code-revert" onClick={() => invoke("tools_revert_change", { activityId: changes!.activityId, index: hit.index }).then(() => setTick((t) => t + 1)).catch(hooks.report)}>Revert this change</button> : null}
            {isDiff && hit?.file.reverted ? <span className="stage-state">reverted</span> : null}
          </div>
          {draft !== null && !reviewing
            ? <textarea className="code-editor" data-test="code-draft" aria-label={`Edit ${selected}`} spellCheck={false} value={draft} onChange={(e) => setDraft(e.target.value)} />
            : <pre className={`code-text${isDiff ? " diff" : ""}`} data-test="code-text">{isDiff
            ? text.split("\n").map((line, i) => <span key={i} className={line.startsWith("@@") ? "diff-head" : line.startsWith("+") ? "diff-add" : line.startsWith("-") ? "diff-del" : ""}>{line || " "}</span>)
            : text.split("\n").map((line, i) => <span key={i} className={`code-line${range && i + 1 >= lo && i + 1 <= hi ? " picked" : ""}${focusLine === i + 1 ? " found" : ""}`} data-test={`code-line-${i + 1}`}
              onMouseDown={(e) => { if (e.button !== 0 || (e.target as HTMLElement).tagName !== "I") return; e.preventDefault(); selecting.current = true; setProposal(null); setApplied(""); setRange(e.shiftKey && range ? { a: range.a, b: i + 1 } : { a: i + 1, b: i + 1 }); }}
              onMouseEnter={() => { if (selecting.current) setRange((r) => (r ? { a: r.a, b: i + 1 } : r)); }}><i title="Click or drag to select lines">{i + 1}</i>{line || " "}</span>)}</pre>}
          {truncated && draft === null && <p className="sheet-note">Only the first 3,000 lines are shown.</p>}
          {problem && <p className="stage-notice" role="alert" data-test="code-problem">{problem}</p>}
          {applied && <p className="stage-notice" role="status" data-test="code-applied">Applied. <button type="button" className="link" data-test="code-undo" onClick={undoApplied}>Undo</button></p>}
          {hunk ? <div className="stage-proposal code-proposal" data-test="code-save-review" role="group" aria-label="Your change" onKeyDown={(e) => { if (e.key === "Escape") { e.preventDefault(); setReviewing(false); } }}>
            <pre className="diff">
              {hunk.omittedBefore > 0 ? <span className="diff-head">{hunk.omittedBefore} lines unchanged</span> : null}
              {hunk.pre.map((l, i) => <span key={`p${i}`}>{`  ${l}`}</span>)}
              {hunk.removed.map((l, i) => <span key={`r${i}`} className="diff-del">{`- ${l}`}</span>)}
              {hunk.added.map((l, i) => <span key={`a${i}`} className="diff-add">{`+ ${l}`}</span>)}
              {hunk.post.map((l, i) => <span key={`t${i}`}>{`  ${l}`}</span>)}
              {hunk.omittedAfter > 0 ? <span className="diff-head">{hunk.omittedAfter} lines unchanged</span> : null}
            </pre>
            <div className="proposal-actions"><button type="button" ref={save} className="primary" data-test="code-save" onClick={saveDraft}>Accept</button><button type="button" data-test="code-save-back" onClick={() => setReviewing(false)}>Keep editing</button></div>
          </div> : null}
          {proposal && draft === null ? <div className="stage-proposal code-proposal" data-test="code-proposal" role="group" aria-label="Suggested change" onKeyDown={(e) => { if (e.key === "Escape") { e.preventDefault(); setProposal(null); } }}>
            <pre className="diff">{[...proposal.old.split("\n").map((l, i) => <span key={`o${i}`} className="diff-del">{`- ${l}`}</span>), ...proposal.text.split("\n").map((l, i) => <span key={`n${i}`} className="diff-add">{`+ ${l}`}</span>)]}</pre>
            <div className="proposal-actions"><button type="button" ref={accept} className="primary" data-test="code-accept" onClick={applyProposal}>Accept <kbd>Enter</kbd></button><button type="button" data-test="code-reject" onClick={() => setProposal(null)}>Reject <kbd>Esc</kbd></button></div>
          </div> : draft === null && range && !isDiff && <div className="stage-selbar" data-test="code-selbar" role="toolbar" aria-label="Edit the selected lines with Mobius">
            <span className="stage-state">Lines {lo}{hi > lo ? `–${hi}` : ""}</span>
            {[["Simplify", "Simplify this code without changing what it does."], ["Add comments", "Add short, useful comments."], ["Fix bug", "Find and fix the bug in this code."], ["Make it idiomatic", "Rewrite in the idiomatic style of this language, same behaviour."]].map(([label, what]) => <button key={label} type="button" disabled={working} onClick={() => ask(what)}>{label}</button>)}
            <input data-test="code-instruction" value={instruction} disabled={working} placeholder={working ? "Working…" : "Edit these lines with Mobius…"} maxLength={300} onChange={(e) => setInstruction(e.target.value)} onKeyDown={(e) => { if (e.key === "Enter" && instruction.trim()) { e.preventDefault(); ask(instruction); } }} />
          </div>}
        </> : <p className="code-hint">Pick a file. Edit writes it after you review the change. The one in view also goes to Mobius with your next message. Files the last turn changed are marked and show their diff.</p>}
      </div>
    </div>}
  </aside>;
}
