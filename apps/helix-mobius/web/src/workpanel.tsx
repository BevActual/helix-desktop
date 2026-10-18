import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@helix/ui";
import { Blocks, type Block } from "./thread";
import { continueList, findAll, indent, makeLink, outline, readingMinutes, setHeading, toggleLines, toggleWrap, type Edit } from "./markdownEdit";

/** The stage: the panel beside the chat where the work lives. Today it holds documents: the model drafts them (an `artifact:Title` block in a
 * reply), the person edits them here, every change is a version, and the open document goes to the model with the next message as a visible
 * chip. Later stages (code, image, vector, browser) share this panel; see docs/design/mobius-workspaces.md. */
interface Version { body: string; by: string; at: number }
interface Document { title: string; versions: Version[] }
interface Store { documents: Document[] }
export interface StageHooks {
  chatId: () => string;
  report: (error: unknown) => void;
  /** The open document changed (or closed: null); the page keeps its chip and the layout in step. */
  changed: (open: { title: string; text: string } | null) => void;
}
export interface StageHandle {
  /** Opens a document; `body` is the model's text for it when it is not stored yet. */
  open: (title: string, body?: string) => void;
  /** A reply finished: the page recorded its documents; show the newest text of the open one. */
  refresh: () => void;
  close: () => void;
}

type Font = "sans" | "serif" | "mono";
const remembered = <T extends string | number>(key: string, fallback: T): T => { try { const v = localStorage.getItem(key); return v === null ? fallback : (typeof fallback === "number" ? (Number(v) as T) : (v as T)); } catch { return fallback; } };
const remember = (key: string, value: string | number) => { try { localStorage.setItem(key, String(value)); } catch { /* not kept */ } };
const ago = (secs: number) => { const d = Math.max(0, Date.now() / 1000 - secs); return d < 90 ? "just now" : d < 5400 ? `${Math.round(d / 60)} min ago` : d < 129600 ? `${Math.round(d / 3600)} h ago` : `${Math.round(d / 86400)} days ago`; };

/** Replaces only what changed between two texts, through the browser's own editing command, so the person's Undo and Redo keep working
 * across toolbar edits and accepted suggestions. Falls back to setting the value when the command is not available. */
function replaceText(area: HTMLTextAreaElement, next: string, selection: { start: number; end: number }) {
  const old = area.value;
  let a = 0;
  while (a < old.length && a < next.length && old[a] === next[a]) a++;
  let b = 0;
  while (b < old.length - a && b < next.length - a && old[old.length - 1 - b] === next[next.length - 1 - b]) b++;
  area.focus();
  area.setSelectionRange(a, old.length - b);
  const ok = a === old.length - b && a === next.length - b ? true : document.execCommand("insertText", false, next.slice(a, next.length - b));
  if (!ok || area.value !== next) {
    const setter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, "value")?.set;
    setter?.call(area, next);
    area.dispatchEvent(new Event("input", { bubbles: true }));
  }
  area.setSelectionRange(selection.start, selection.end);
}

/** Where the caret is on the screen (the line below it), found by laying the text up to it out in a hidden copy of the box. */
function caretPoint(area: HTMLTextAreaElement, index: number): { x: number; y: number } {
  const cs = getComputedStyle(area);
  const mirror = document.createElement("div");
  for (const p of ["fontFamily", "fontSize", "fontWeight", "lineHeight", "letterSpacing", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft", "boxSizing", "tabSize"] as const) mirror.style[p] = cs[p];
  Object.assign(mirror.style, { position: "absolute", visibility: "hidden", whiteSpace: "pre-wrap", overflowWrap: "anywhere", width: `${area.clientWidth}px`, top: "0", left: "-9999px" });
  mirror.textContent = area.value.slice(0, index);
  const mark = document.createElement("span");
  mark.textContent = "\u200b";
  mirror.appendChild(mark);
  document.body.appendChild(mirror);
  const box = area.getBoundingClientRect();
  const line = parseFloat(cs.lineHeight) || parseFloat(cs.fontSize) * 1.7;
  const point = { x: box.left + mark.offsetLeft, y: box.top + mark.offsetTop - area.scrollTop + line };
  mirror.remove();
  return point;
}

export function Stage({ register, hooks }: { register: (handle: StageHandle) => void; hooks: StageHooks }) {
  const [store, setStore] = useState<Store>({ documents: [] });
  const [title, setTitle] = useState("");
  const [text, setText] = useState("");
  const [notice, setNotice] = useState("");
  const [saved, setSaved] = useState(true);
  const [mode, setMode] = useState<"write" | "read">("write");
  const [blocks, setBlocks] = useState<Block[]>([]);
  const [font, setFont] = useState<Font>(() => remembered<Font>("mobius-write-font", "sans"));
  const [size, setSize] = useState<number>(() => remembered("mobius-write-size", 16));
  // The outline is shown by default only where there is room (a wide stage); a choice the person makes is remembered.
  const [outlinePref, setOutlinePref] = useState<boolean | null>(() => { const v = remembered("mobius-write-outline", -1); return v === -1 ? null : v === 1; });
  const [wide, setWide] = useState(false);
  const shell = useRef<HTMLElement>(null);
  const [menu, setMenu] = useState<"" | "docs" | "type" | "more">("");
  const [history, setHistory] = useState(false);
  const [picked, setPicked] = useState(-1);
  const [diff, setDiff] = useState("");
  const [find, setFind] = useState(false);
  const [needle, setNeedle] = useState("");
  const [replacement, setReplacement] = useState("");
  const [hit, setHit] = useState(0);
  // Ask on the selection: the selected range, the bar's own instruction field, and a proposal waiting to be accepted or rejected.
  const [sel, setSel] = useState<{ start: number; end: number } | null>(null);
  const [custom, setCustom] = useState("");
  const [working, setWorking] = useState(false);
  const [proposal, setProposal] = useState<{ start: number; end: number; old: string; text: string } | null>(null);
  const [problem, setProblem] = useState("");
  const [draftTitle, setDraftTitle] = useState("");
  const [focus, setFocus] = useState(false);
  // The block menu: typing "/" at the start of a line offers headings, lists, a quote, code, a divider, and "Continue writing".
  const [slash, setSlash] = useState<{ from: number; to: number; query: string } | null>(null);
  const [slashAt, setSlashAt] = useState(0);
  const [slashPoint, setSlashPoint] = useState({ x: 0, y: 0 });
  const lastRename = useRef("");
  const area = useRef<HTMLTextAreaElement>(null);
  const page = useRef<HTMLDivElement>(null);
  const customField = useRef<HTMLInputElement>(null);
  const findField = useRef<HTMLInputElement>(null);
  const accept = useRef<HTMLButtonElement>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const current = useRef({ title: "", text: "" });
  current.current = { title, text };

  useEffect(() => {
    const el = shell.current; if (!el) return;
    const watch = new ResizeObserver(([entry]) => setWide(entry.contentRect.width >= 820));
    watch.observe(el);
    return () => watch.disconnect();
  }, [title]);
  const showOutline = outlinePref ?? wide;
  // A menu that opens takes the keyboard: the first item is focused, Up and Down move, Escape closes (and gives the keyboard back).
  useEffect(() => { if (menu) shell.current?.querySelector<HTMLButtonElement>(".write-menu button")?.focus(); }, [menu]);

  const adopt = useCallback((next: Store, want: string) => {
    setStore(next);
    const doc = next.documents.find((d) => d.title.toLowerCase() === want.toLowerCase());
    if (!doc) return;
    const latest = doc.versions[doc.versions.length - 1];
    setTitle(doc.title); setDraftTitle(doc.title); setText(latest?.body ?? ""); setSaved(true); setPicked(-1); setDiff(""); setProposal(null); setSel(null);
    hooks.changed({ title: doc.title, text: latest?.body ?? "" });
  }, [hooks]);

  const load = useCallback(async (want: string, body?: string) => {
    const chat = hooks.chatId();
    try {
      adopt(body !== undefined ? await invoke<Store>("stage_record", { chatId: chat, title: want, body, by: "model" }) : await invoke<Store>("stage_get", { chatId: chat }), want);
    } catch (reason) { hooks.report(reason); }
  }, [hooks, adopt]);

  useEffect(() => {
    register({
      open: (want, body) => { setNotice(""); load(want, body); },
      refresh: () => {
        const open = current.current.title;
        if (!open) return;
        invoke<Store>("stage_get", { chatId: hooks.chatId() }).then((next) => {
          setStore(next);
          const doc = next.documents.find((d) => d.title.toLowerCase() === open.toLowerCase());
          const latest = doc?.versions[doc.versions.length - 1];
          if (doc && latest && latest.body !== current.current.text) {
            setText(latest.body); setSaved(true);
            setNotice(latest.by === "model" ? "Mobius revised this document. Every earlier version is in History." : "");
            hooks.changed({ title: doc.title, text: latest.body });
          }
        }, () => undefined);
      },
      close: () => { setTitle(""); setText(""); hooks.changed(null); },
    });
  }, [register, load, hooks]);

  // The Read view is the same Markdown the thread draws, so a document looks the same in both places.
  useEffect(() => {
    if (mode !== "read") return;
    let live = true;
    invoke<Block[]>("markdown_blocks", { text }).then((b) => live && setBlocks(b), () => undefined);
    return () => { live = false; };
  }, [mode, text]);

  const remember3 = (next: string) => {
    setText(next); setSaved(false); setNotice("");
    hooks.changed({ title, text: next });
    clearTimeout(timer.current);
    // Kept as the person pauses, so a version is a thought rather than a keystroke.
    timer.current = setTimeout(async () => {
      try { setStore(await invoke<Store>("stage_record", { chatId: hooks.chatId(), title, body: next, by: "person" })); setSaved(true); }
      catch (reason) { hooks.report(reason); }
    }, 1500);
  };
  const noteSelection = () => {
    const el = area.current; if (!el) return;
    const { selectionStart: start, selectionEnd: end } = el;
    setSel(end - start >= 3 ? { start, end } : null);
  };
  // A change made by the toolbar, a shortcut or an accepted suggestion goes through the editing command so Undo still works.
  const applyEdit = (edit: Edit) => { const el = area.current; if (el) { replaceText(el, edit.text, { start: edit.start, end: edit.end }); noteSelection(); } };
  const format = (fn: (t: string, s: number, e: number) => Edit) => { const el = area.current; if (!el) return; applyEdit(fn(el.value, el.selectionStart, el.selectionEnd)); };

  const ask = async (kind: string, instruction = "") => {
    if (!sel || working) return;
    const old = text.slice(sel.start, sel.end);
    setWorking(true); setProblem("");
    try {
      const edited = await invoke<string>("stage_transform", { kind, custom: instruction, text: old });
      setProposal({ start: sel.start, end: sel.end, old, text: edited });
      setCustom("");
      setTimeout(() => accept.current?.focus(), 0);
    } catch (reason) { setProblem(String(reason)); } finally { setWorking(false); }
  };
  const applyProposal = () => {
    if (!proposal) return;
    const el = area.current;
    if (!el || text.slice(proposal.start, proposal.end) !== proposal.old) { setProblem("The text changed while Mobius worked, so the suggestion was dropped."); setProposal(null); return; }
    const next = text.slice(0, proposal.start) + proposal.text + text.slice(proposal.end);
    setProposal(null);
    replaceText(el, next, { start: proposal.start, end: proposal.start + proposal.text.length });
    noteSelection();
  };
  const rejectProposal = () => { setProposal(null); setTimeout(() => area.current?.focus(), 0); };

  const doc = store.documents.find((d) => d.title === title);
  const versions = doc?.versions ?? [];
  const words = useMemo(() => text.split(/\s+/).filter(Boolean).length, [text]);
  const headings = useMemo(() => outline(text), [text]);
  const matches = useMemo(() => findAll(text, needle), [text, needle]);

  const jumpTo = (offset: number) => {
    if (mode === "write") {
      const el = area.current; if (!el) return;
      el.focus(); el.setSelectionRange(offset, offset);
      const lines = el.value.slice(0, offset).split("\n").length - 1;
      el.scrollTop = Math.max(0, lines * (size * 1.7) - 60);
    } else {
      const index = headings.findIndex((h) => h.offset === offset);
      page.current?.querySelectorAll("h3, h4, h5, h6")[index]?.scrollIntoView({ block: "start", behavior: "smooth" });
    }
  };
  const openHistory = async (index: number) => {
    setPicked(index);
    const v = versions[index], before = versions[index - 1];
    try { setDiff(await invoke<string>("text_diff", { old: before?.body ?? "", new: v.body })); } catch { setDiff(""); }
  };
  const restore = (index: number) => {
    const el = area.current, v = versions[index]; if (!v) return;
    setMode("write"); setHistory(false);
    setTimeout(() => { const a = area.current ?? el; if (a) { replaceText(a, v.body, { start: 0, end: 0 }); noteSelection(); } }, 0);
  };
  const goTo = (n: number) => { if (matches.length === 0) return; const i = (n + matches.length) % matches.length; setHit(i); const el = area.current; if (el && mode === "write") { el.focus(); el.setSelectionRange(matches[i], matches[i] + needle.length); const lines = el.value.slice(0, matches[i]).split("\n").length - 1; el.scrollTop = Math.max(0, lines * (size * 1.7) - 80); } };
  const replaceOne = () => { const el = area.current; if (!el || matches.length === 0) return; const at = matches[hit % matches.length]; replaceText(el, text.slice(0, at) + replacement + text.slice(at + needle.length), { start: at, end: at + replacement.length }); };
  const replaceAll = () => { const el = area.current; if (!el || matches.length === 0) return; const re = new RegExp(needle.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi"); replaceText(el, text.replace(re, () => replacement), { start: 0, end: 0 }); };
  const rename = async (to: string) => {
    // Enter and the blur that follows it both ask; the second is the same request.
    if (to.trim() === "" || to.trim() === title || to.trim() === lastRename.current) return;
    lastRename.current = to.trim();
    try { const next = await invoke<Store>("stage_rename", { chatId: hooks.chatId(), from: title, to }); adopt(next, to); } catch (reason) { hooks.report(reason); }
  };
  const newDocument = async () => {
    setMenu("");
    let n = 1; const names = new Set(store.documents.map((d) => d.title.toLowerCase()));
    while (names.has(`untitled ${n}`.toLowerCase())) n += 1;
    const name = store.documents.length === 0 ? "Untitled" : `Untitled ${n}`;
    try { adopt(await invoke<Store>("stage_record", { chatId: hooks.chatId(), title: name, body: "", by: "person" }), name); setMode("write"); setTimeout(() => area.current?.focus(), 50); } catch (reason) { hooks.report(reason); }
  };
  const closeAll = () => { setTitle(""); setText(""); hooks.changed(null); };

  const slashItems = useMemo(() => {
    const all: { id: string; label: string; hint: string }[] = [
      { id: "continue", label: "Continue writing", hint: "Mobius adds a paragraph or two" },
      { id: "h1", label: "Heading 1", hint: "#" }, { id: "h2", label: "Heading 2", hint: "##" }, { id: "h3", label: "Heading 3", hint: "###" },
      { id: "bullet", label: "Bulleted list", hint: "-" }, { id: "number", label: "Numbered list", hint: "1." }, { id: "quote", label: "Quote", hint: ">" },
      { id: "code", label: "Code block", hint: "```" }, { id: "divider", label: "Divider", hint: "---" },
    ];
    const q = slash?.query.toLowerCase() ?? "";
    return all.filter((i) => !q || i.label.toLowerCase().includes(q) || i.id.startsWith(q));
  }, [slash]);
  const noteSlash = (value: string, caret: number) => {
    const from = value.lastIndexOf("\n", caret - 1) + 1;
    const m = /^\/([a-z0-9 ]{0,20})$/i.exec(value.slice(from, caret));
    if (m) { setSlash({ from, to: caret, query: m[1] }); setSlashAt(0); if (area.current) setSlashPoint(caretPoint(area.current, caret)); } else setSlash(null);
  };
  const runSlash = (id: string) => {
    const el = area.current; if (!el || !slash) return;
    const { from, to } = slash;
    setSlash(null);
    const without = el.value.slice(0, from) + el.value.slice(to);
    if (id === "continue") {
      replaceText(el, without, { start: from, end: from });
      const before = without.slice(Math.max(0, from - 3000), from);
      if (!before.trim()) { setProblem("Write something first, and Mobius can continue it."); return; }
      setWorking(true); setProblem("");
      invoke<string>("stage_transform", { kind: "continue", custom: "", text: before })
        .then((out) => { setProposal({ start: from, end: from, old: "", text: (before.endsWith("\n") ? "" : "\n\n") + out.trim() }); setTimeout(() => accept.current?.focus(), 0); })
        .catch((reason) => setProblem(String(reason))).finally(() => setWorking(false));
      return;
    }
    const insert: Record<string, string> = { h1: "# ", h2: "## ", h3: "### ", bullet: "- ", number: "1. ", quote: "> ", code: "```\n\n```", divider: "---\n" };
    const text = insert[id] ?? "";
    const caret = from + (id === "code" ? 4 : text.length);
    replaceText(el, without.slice(0, from) + text + without.slice(from), { start: caret, end: caret });
  };
  const keys = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (slash && slashItems.length) {
      if (e.key === "ArrowDown") { e.preventDefault(); setSlashAt((i) => (i + 1) % slashItems.length); return; }
      if (e.key === "ArrowUp") { e.preventDefault(); setSlashAt((i) => (i + slashItems.length - 1) % slashItems.length); return; }
      if (e.key === "Enter" || e.key === "Tab") { e.preventDefault(); runSlash(slashItems[slashAt].id); return; }
      if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); setSlash(null); return; }
    }
    const mod = e.ctrlKey || e.metaKey, k = e.key.toLowerCase();
    if (mod && k === "b") { e.preventDefault(); format((t, s, en) => toggleWrap(t, s, en, "**")); }
    else if (mod && k === "i" && !sel) { e.preventDefault(); format((t, s, en) => toggleWrap(t, s, en, "*")); }
    else if (mod && k === "i" && sel) { e.preventDefault(); customField.current?.focus(); }
    else if (mod && k === "k") { e.preventDefault(); e.stopPropagation(); format(makeLink); }
    else if (mod && k === "f") { e.preventDefault(); e.stopPropagation(); setFind(true); setTimeout(() => findField.current?.select(), 0); }
    else if (e.key === "Tab") { e.preventDefault(); format((t, s, en) => indent(t, s, en, e.shiftKey)); }
    else if (e.key === "Enter" && !e.shiftKey && !mod) { const el = e.currentTarget; if (el.selectionStart === el.selectionEnd) { const c = continueList(el.value, el.selectionStart); if (c) { e.preventDefault(); applyEdit(c); } } }
  };

  if (!title) return null;
  const fontFamily = font === "serif" ? 'ui-serif, "New York", "Iowan Old Style", Georgia, serif' : font === "mono" ? 'ui-monospace, "JetBrains Mono", "SF Mono", monospace' : 'system-ui, -apple-system, "SF Pro Text", "Inter", sans-serif';
  const pageStyle = { "--write-font": fontFamily, "--write-size": `${size}px` } as React.CSSProperties;
  const tool = (label: string, aria: string, run: () => void, extra = "") => <button type="button" className={`wt ${extra}`} aria-label={aria} title={aria} onMouseDown={(e) => e.preventDefault()} onClick={run} data-test={`fmt-${aria.toLowerCase().replace(/[^a-z0-9]+/g, "-")}`}>{label}</button>;
  return <aside ref={shell} className={`stage write-stage${focus ? " focus" : ""}`} data-test="stage" aria-label="Document" style={pageStyle} onClick={() => { if (menu) setMenu(""); }}
    onKeyDown={(e) => {
      if (menu && (e.key === "ArrowDown" || e.key === "ArrowUp")) {
        const items = Array.from(shell.current?.querySelectorAll<HTMLButtonElement>(".write-menu button") ?? []);
        const at = items.indexOf(document.activeElement as HTMLButtonElement);
        e.preventDefault();
        items[(at + (e.key === "ArrowDown" ? 1 : items.length - 1)) % items.length]?.focus();
        return;
      }
      if (e.key === "Escape" && !e.defaultPrevented) { if (menu) { setMenu(""); area.current?.focus(); } else if (find) setFind(false); else if (history) setHistory(false); else if (focus) setFocus(false); } }}>
    <header className="write-head">
      <div className="write-docs">
        <button type="button" className="write-docbtn" data-test="stage-docs" aria-haspopup="menu" aria-expanded={menu === "docs"} onClick={(e) => { e.stopPropagation(); setMenu(menu === "docs" ? "" : "docs"); }}>
          <span className="write-docname" data-test="stage-title">{title}</span><span className="chev">▾</span></button>
        {menu === "docs" && <div className="write-menu" role="menu" onClick={(e) => e.stopPropagation()}>
          {store.documents.map((d) => <button key={d.title} type="button" role="menuitem" className={d.title === title ? "current" : ""} onClick={() => { setMenu(""); adopt(store, d.title); }}>{d.title}<span>{d.versions.length} version{d.versions.length === 1 ? "" : "s"}</span></button>)}
          <div className="write-sep" />
          <button type="button" role="menuitem" data-test="stage-new" onClick={newDocument}>New document</button>
        </div>}
      </div>
      <span className={`stage-state${saved ? "" : " busy"}`} data-test="stage-state">{saved ? "Saved" : "Saving…"}</span>
      <span className="stage-spacer" />
      <div className="write-seg" role="tablist" aria-label="View">
        <button type="button" role="tab" aria-selected={mode === "write"} className={mode === "write" ? "on" : ""} data-test="stage-mode-write" onClick={() => setMode("write")}>Write</button>
        <button type="button" role="tab" aria-selected={mode === "read"} className={mode === "read" ? "on" : ""} data-test="stage-mode-read" onClick={() => setMode("read")}>Read</button>
      </div>
      <button type="button" className={`wt${showOutline ? " on" : ""}`} aria-label="Outline" title="Outline" data-test="stage-outline-toggle" onClick={() => { setOutlinePref(!showOutline); remember("mobius-write-outline", showOutline ? 0 : 1); }}>☰</button>
      <button type="button" className={`wt${history ? " on" : ""}`} aria-label="History" title="Version history" data-test="stage-history-toggle" onClick={() => { setHistory(!history); setPicked(-1); setDiff(""); }}>◷</button>
      <div className="write-docs">
        <button type="button" className="wt" aria-label="More" title="More" aria-haspopup="menu" data-test="stage-more" onClick={(e) => { e.stopPropagation(); setMenu(menu === "more" ? "" : "more"); }}>⋯</button>
        {menu === "more" && <div className="write-menu right" role="menu" onClick={(e) => e.stopPropagation()}>
          <button type="button" role="menuitem" data-test="stage-find-open" onClick={() => { setMenu(""); setFind(true); setTimeout(() => findField.current?.focus(), 0); }}>Find and replace<span>Ctrl+F</span></button>
          <button type="button" role="menuitem" onClick={() => { setMenu(""); invoke("chat_copy", { text }); }}>Copy as Markdown</button>
          <button type="button" role="menuitem" data-test="stage-focus" onClick={() => { setMenu(""); setFocus(!focus); }}>{focus ? "Leave focus mode" : "Focus mode"}</button>
          <button type="button" role="menuitem" data-test="stage-html" onClick={() => { setMenu(""); invoke("stage_export_html", { title, text }).catch(hooks.report); }}>Export as web page…</button>
          <button type="button" role="menuitem" data-test="stage-print" onClick={() => { setMenu(""); setMode("read"); setTimeout(() => window.print(), 400); }}>Print or save as PDF…</button>
          <button type="button" role="menuitem" data-test="stage-save" onClick={() => { setMenu(""); invoke("chat_reply_save", { content: text, defaultFilename: `${title.replace(/[^\w.-]+/g, "-")}.md` }).catch(hooks.report); }}>Save as file…</button>
          <div className="write-sep" />
          <button type="button" role="menuitem" onClick={() => { setMenu(""); if (window.confirm(`Delete “${title}” and its ${versions.length} version${versions.length === 1 ? "" : "s"}?`)) invoke<Store>("stage_delete", { chatId: hooks.chatId(), title }).then((n) => { setStore(n); closeAll(); }, hooks.report); }}>Delete document…</button>
        </div>}
      </div>
      <button type="button" className="icon-btn" aria-label="Close the document" data-test="stage-close" onClick={closeAll}>×</button>
    </header>
    {mode === "write" && <div className="write-format" role="toolbar" aria-label="Format">
      {tool("B", "Bold", () => format((t, s, e) => toggleWrap(t, s, e, "**")), "bold")}
      {tool("I", "Italic", () => format((t, s, e) => toggleWrap(t, s, e, "*")), "italic")}
      {tool("</>", "Code", () => format((t, s, e) => toggleWrap(t, s, e, "`")), "mono")}
      <span className="wsep" />
      {tool("H1", "Heading 1", () => format((t, s, e) => setHeading(t, s, e, 1)))}
      {tool("H2", "Heading 2", () => format((t, s, e) => setHeading(t, s, e, 2)))}
      {tool("H3", "Heading 3", () => format((t, s, e) => setHeading(t, s, e, 3)))}
      <span className="wsep" />
      {tool("•", "Bulleted list", () => format((t, s, e) => toggleLines(t, s, e, "bullet")))}
      {tool("1.", "Numbered list", () => format((t, s, e) => toggleLines(t, s, e, "number")))}
      {tool("❝", "Quote", () => format((t, s, e) => toggleLines(t, s, e, "quote")))}
      {tool("🔗", "Link", () => format(makeLink))}
      <span className="write-spacer" />
      <div className="write-docs">
        <button type="button" className="wt" aria-label="Text style" title="Text style" data-test="stage-type" onClick={(e) => { e.stopPropagation(); setMenu(menu === "type" ? "" : "type"); }}>Aa</button>
        {menu === "type" && <div className="write-menu right type" role="menu" onClick={(e) => e.stopPropagation()}>
          {(["sans", "serif", "mono"] as Font[]).map((f) => <button key={f} type="button" role="menuitemradio" aria-checked={font === f} className={font === f ? "current" : ""} onClick={() => { setFont(f); remember("mobius-write-font", f); }}>{f === "sans" ? "Sans" : f === "serif" ? "Serif" : "Mono"}</button>)}
          <div className="write-sep" />
          <div className="write-size"><button type="button" aria-label="Smaller" onClick={() => { const n = Math.max(13, size - 1); setSize(n); remember("mobius-write-size", n); }}>A−</button><span>{size}</span><button type="button" aria-label="Larger" onClick={() => { const n = Math.min(26, size + 1); setSize(n); remember("mobius-write-size", n); }}>A+</button></div>
        </div>}
      </div>
    </div>}
    {find && <div className="write-find" role="search" data-test="stage-find">
      <input ref={findField} data-test="find-needle" placeholder="Find" value={needle} onChange={(e) => { setNeedle(e.target.value); setHit(0); }} onKeyDown={(e) => { if (e.key === "Enter") { e.preventDefault(); goTo(hit + (e.shiftKey ? -1 : 1)); } else if (e.key === "Escape") { setFind(false); area.current?.focus(); } }} />
      <span className="count">{needle ? (matches.length ? `${(hit % matches.length) + 1} of ${matches.length}` : "No matches") : ""}</span>
      <button type="button" aria-label="Previous match" onClick={() => goTo(hit - 1)}>↑</button><button type="button" aria-label="Next match" onClick={() => goTo(hit + 1)}>↓</button>
      <input data-test="find-replacement" placeholder="Replace with" value={replacement} onChange={(e) => setReplacement(e.target.value)} />
      <button type="button" data-test="find-replace" onClick={replaceOne} disabled={!matches.length}>Replace</button><button type="button" data-test="find-replace-all" onClick={replaceAll} disabled={!matches.length}>All</button>
      <button type="button" aria-label="Close find" onClick={() => { setFind(false); area.current?.focus(); }}>×</button>
    </div>}
    {notice && <p className="stage-notice" role="status" data-test="stage-notice">{notice}</p>}
    {problem && <p className="stage-notice" role="alert" data-test="stage-problem">{problem}</p>}
    <div className="write-body">
      {showOutline && headings.length > 0 && <nav className="write-outline" aria-label="Outline" data-test="stage-outline">
        {headings.map((h, i) => <button key={i} type="button" style={{ paddingLeft: 8 + (h.level - 1) * 12 }} data-test={`outline-${i}`} onClick={() => jumpTo(h.offset)}>{h.title}</button>)}
      </nav>}
      <div className="write-page" ref={page}>
        {proposal ? <div className="stage-proposal" data-test="stage-proposal" role="group" aria-label="Suggested change" onKeyDown={(e) => { if (e.key === "Escape") { e.preventDefault(); rejectProposal(); } }}>
          <div className="proposal-old" data-test="proposal-old"><span>Now</span>{proposal.old}</div>
          <div className="proposal-new" data-test="proposal-new"><span>Suggested</span>{proposal.text}</div>
          <div className="proposal-actions"><button type="button" ref={accept} className="primary" data-test="proposal-accept" onClick={applyProposal}>Accept <kbd>Enter</kbd></button><button type="button" data-test="proposal-reject" onClick={rejectProposal}>Reject <kbd>Esc</kbd></button></div>
        </div> : sel && mode === "write" && <div className="stage-selbar" data-test="stage-selbar" role="toolbar" aria-label="Ask Mobius about the selection" onMouseDown={(e) => { if ((e.target as HTMLElement).tagName !== "INPUT") e.preventDefault(); }}>
          {[["improve", "Improve"], ["shorter", "Shorter"], ["longer", "Longer"], ["simplify", "Simplify"], ["fix", "Fix grammar"]].map(([kind, label]) => <button key={kind} type="button" disabled={working} data-test={`sel-${kind}`} onClick={() => ask(kind)}>{label}</button>)}
          <input ref={customField} data-test="sel-custom" value={custom} disabled={working} placeholder={working ? "Working…" : "Ask Mobius to…  (Ctrl+I)"} maxLength={300} onChange={(e) => setCustom(e.target.value)} onKeyDown={(e) => { if (e.key === "Enter" && custom.trim()) { e.preventDefault(); ask("custom", custom); } }} />
        </div>}
        {mode === "write" && <input className="write-title-input" data-test="stage-title-input" aria-label="Title" value={draftTitle} maxLength={80} placeholder="Untitled"
          onChange={(e) => setDraftTitle(e.target.value)} onBlur={() => rename(draftTitle)}
          onKeyDown={(e) => { if (e.key === "Enter") { e.preventDefault(); rename(draftTitle); area.current?.focus(); } else if (e.key === "Escape") { setDraftTitle(title); area.current?.focus(); } }} />}
        {slash && slashItems.length > 0 && mode === "write" && <div className="write-slash" role="listbox" aria-label="Insert" data-test="write-slash" style={{ left: Math.min(slashPoint.x, window.innerWidth - 280), top: Math.min(slashPoint.y + 4, window.innerHeight - 260) }}>
          {slashItems.map((i, n) => <button key={i.id} type="button" role="option" aria-selected={n === slashAt} className={n === slashAt ? "on" : ""} data-test={`slash-${i.id}`} onMouseDown={(e) => e.preventDefault()} onMouseEnter={() => setSlashAt(n)} onClick={() => runSlash(i.id)}><span>{i.label}</span><kbd>{i.hint}</kbd></button>)}
        </div>}
        {mode === "write"
          ? <textarea ref={area} className="write-text" data-test="stage-text" aria-label={`Edit ${title}`} spellCheck placeholder="Start writing. Press / for blocks, or ask Mobius in the chat to draft something." value={text}
              onChange={(e) => { remember3(e.target.value); setSel(null); noteSlash(e.target.value, e.target.selectionStart); }} onSelect={noteSelection} onMouseUp={noteSelection} onKeyUp={noteSelection} onKeyDown={keys} />
          : <article className="write-read" data-test="stage-read"><h1 className="write-title">{title}</h1><Blocks blocks={blocks} needle="" report={hooks.report} /></article>}
      </div>
      {history && <aside className="write-history" data-test="stage-history" aria-label="Version history">
        <h4>History</h4>
        <ul>{versions.map((_, i) => i).reverse().map((i) => { const v = versions[i]; return <li key={i}><button type="button" className={picked === i ? "on" : ""} data-test={`version-${i}`} onClick={() => openHistory(i)}><strong>Version {i + 1}</strong><span>{v.by === "model" ? "Mobius" : "You"} · {ago(v.at)}</span></button></li>; })}</ul>
        {picked >= 0 && <div className="write-diff" data-test="stage-diff">
          <pre className="diff">{(diff || "No changes from the version before it.").split("\n").map((l, i) => <span key={i} className={l.startsWith("@@") ? "diff-head" : l.startsWith("+") ? "diff-add" : l.startsWith("-") ? "diff-del" : ""}>{l || " "}</span>)}</pre>
          <button type="button" className="primary" data-test="version-restore" onClick={() => restore(picked)}>Restore this version</button>
        </div>}
      </aside>}
    </div>
    <footer className="write-status">{words.toLocaleString()} word{words === 1 ? "" : "s"} · {readingMinutes(words)} min read{sel ? ` · ${sel.end - sel.start} characters selected` : ""}{working ? " · Mobius is writing…" : ""}<span className="write-spacer" /><span className="write-hint">Goes to Mobius with your next message</span></footer>
  </aside>;
}

export function ArtifactCard({ title, body, writing, onOpen }: { title: string; body: string; writing: boolean; onOpen: () => void }) {
  const first = body.split("\n").filter((l) => l.trim()).slice(0, 3).join("\n");
  return <div className="artifact-card" data-test="artifact-card">
    <div className="artifact-head"><strong>{title}</strong><span>Document · {body.split(/\s+/).filter(Boolean).length} words</span>
      <button type="button" className="mini primary" data-test="artifact-open" disabled={writing} onClick={onOpen}>{writing ? "Writing…" : "Open"}</button></div>
    <pre>{first}{body.split("\n").length > 3 ? "\n…" : ""}</pre>
  </div>;
}
