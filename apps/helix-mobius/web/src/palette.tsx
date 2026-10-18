import { useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** The command palette (Ctrl+K): one box for everything. Type to find a chat (by title or by what was said in it), a command, a mode or a
 * model, or start a new chat with the words typed. Up and down choose, Enter does it, Escape leaves. */
export interface PaletteItem { id: string; section: string; title: string; sub?: string; hint?: string; run: () => void }
export interface PaletteHooks {
  /** Chats and commands the page offers for this text; the palette adds full-text matches from the saved chats. */
  items: (query: string) => PaletteItem[];
  openChat: (id: string) => void;
  newChatWith: (text: string) => void;
}
export interface PaletteHandle { open: () => void }

interface Hit { id: string; title: string; snippet?: string; project?: string }

export function Palette({ register, hooks }: { register: (handle: PaletteHandle) => void; hooks: PaletteHooks }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const field = useRef<HTMLInputElement>(null);
  const [query, setQuery] = useState("");
  const [hits, setHits] = useState<Hit[]>([]);
  const [index, setIndex] = useState(0);
  useEffect(() => {
    register({ open: () => { setQuery(""); setHits([]); setIndex(0); dialog.current?.showModal(); setTimeout(() => field.current?.focus(), 0); } });
  }, [register]);
  useEffect(() => {
    const needle = query.trim();
    if (needle.length < 2) { setHits([]); return; }
    const timer = setTimeout(() => {
      invoke<Hit[]>("chat_search", { query: needle }).then((found) => setHits(Array.isArray(found) ? found.slice(0, 6) : []), () => setHits([]));
    }, 150);
    return () => clearTimeout(timer);
  }, [query]);
  const rows = useMemo(() => {
    const base = hooks.items(query);
    const seen = new Set(base.filter((r) => r.section === "Chats").map((r) => r.id));
    const found: PaletteItem[] = hits.filter((h) => !seen.has(`chat:${h.id}`)).map((h) => ({ id: `chat:${h.id}`, section: "Chats", title: h.title, sub: h.snippet, run: () => hooks.openChat(h.id) }));
    const out = [...base.filter((r) => r.section === "Chats"), ...found, ...base.filter((r) => r.section !== "Chats")];
    if (query.trim()) out.push({ id: "ask", section: "Start", title: `Ask Mobius: ${query.trim()}`, sub: "In a new chat", run: () => hooks.newChatWith(query.trim()) });
    return out;
  }, [query, hits, hooks]);
  useEffect(() => { setIndex(0); }, [query]);
  const run = (row: PaletteItem | undefined) => { if (!row) return; dialog.current?.close(); row.run(); };
  const onKey = (event: React.KeyboardEvent) => {
    if (event.key === "ArrowDown") { event.preventDefault(); setIndex((i) => Math.min(rows.length - 1, i + 1)); }
    else if (event.key === "ArrowUp") { event.preventDefault(); setIndex((i) => Math.max(0, i - 1)); }
    else if (event.key === "Enter") { event.preventDefault(); run(rows[index]); }
  };
  useEffect(() => { dialog.current?.querySelector(".palette-row.active")?.scrollIntoView({ block: "nearest" }); }, [index, rows]);
  let lastSection = "";
  return <dialog ref={dialog} className="palette" data-test="palette" aria-label="Command palette" onClick={(e) => { if (e.target === dialog.current) dialog.current?.close(); }}>
    <input ref={field} data-test="palette-input" value={query} placeholder="Search chats, run a command, switch model or mode…" onChange={(e) => setQuery(e.target.value)} onKeyDown={onKey} aria-label="Search" />
    <div className="palette-rows" role="listbox">
      {rows.length === 0 && <p className="palette-empty">Nothing matches.</p>}
      {rows.map((row, i) => {
        const header = row.section !== lastSection ? <div className="palette-section" key={`s-${row.section}`}>{row.section}</div> : null;
        lastSection = row.section;
        return <div key={row.id}>{header}<button type="button" role="option" aria-selected={i === index} data-test={`palette-row-${row.id}`} className={`palette-row${i === index ? " active" : ""}`}
          onMouseMove={() => setIndex(i)} onClick={() => run(row)}>
          <span className="palette-title">{row.title}</span>
          {row.sub ? <span className="palette-sub">{row.sub}</span> : null}
          {row.hint ? <kbd>{row.hint}</kbd> : null}
        </button></div>;
      })}
    </div>
  </dialog>;
}
