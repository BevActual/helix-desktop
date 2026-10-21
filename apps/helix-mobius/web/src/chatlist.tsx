import { Fragment, useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@helix/ui";

declare function icon(name: string): SVGElement;
function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}

export interface ChatItem {
  id: string;
  title: string;
  updated?: number | string;
  pinned: boolean;
  project: string;
  preview?: string;
}
interface Hit { id: string; title: string; updated?: number | string; pinned: boolean; project?: string; snippet?: string; }

export interface ChatListHooks {
  /** A chat was opened from the list: the page clears its attachments and closes the narrow drawer. */
  opened: (id: string, query: string) => void;
  /** The project names now in the list (the project dialog suggests them). */
  projects: (names: string[]) => void;
  report: (error: unknown) => void;
}
export interface ChatListLive {
  streaming?: boolean;
  waiting?: boolean;
}
export interface ChatListHandle {
  set: (items: ChatItem[], currentId: string, live?: ChatListLive) => void;
}

/** The date section a chat opens: Pinned, Today, Yesterday, Previous 7 Days, Earlier; shown once, above its first chat. */
function bucket(item: ChatItem, now: number): string {
  if (item.pinned) return "Pinned";
  const age = now - Number(item.updated || 0);
  const day = 86_400;
  const startOfToday = now - (now % day);
  const updated = Number(item.updated || 0);
  if (updated >= startOfToday) return "Today";
  if (updated >= startOfToday - day) return "Yesterday";
  if (age < 7 * day) return "Previous 7 days";
  if (age < 30 * day) return "Previous 30 days";
  return "Earlier";
}
/** When a chat was last touched, in the fewest words that place it: the time today, the weekday this week, the date after that. */
function when(updated: number | string | undefined, now: number): string {
  const t = Number(updated || 0);
  if (!t) return "";
  const date = new Date(t * 1000), age = now - t, day = 86_400;
  if (age < day && date.toDateString() === new Date(now * 1000).toDateString()) return date.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
  if (age < 7 * day) return date.toLocaleDateString([], { weekday: "short" });
  return date.toLocaleDateString([], { month: "short", day: "numeric" });
}
function sectionOf(item: ChatItem, index: number, all: ChatItem[], now: number): string {
  const mine = bucket(item, now);
  return index === 0 || bucket(all[index - 1], now) !== mine ? mine : "";
}

/** The sidebar's search, project filter and chat list. Rust owns the chats and the full-text search; this filters what it was given and
 * sends back the id of the chat opened or deleted. */
export function ChatList({ register, hooks }: { register: (handle: ChatListHandle) => void; hooks: ChatListHooks }) {
  const [items, setItems] = useState<ChatItem[]>([]);
  const [currentId, setCurrentId] = useState("");
  const [live, setLive] = useState<ChatListLive>({});
  const [filter, setFilter] = useState("");
  const [project, setProject] = useState("");
  const [result, setResult] = useState<{ query: string; hits: Hit[] } | null>(null);
  const [searchError, setSearchError] = useState("");
  const hooksRef = useRef(hooks);
  hooksRef.current = hooks;

  useEffect(() => {
    register({ set: (next, id, flags) => { setItems(next); setCurrentId(id); setLive(flags || {}); } });
  }, [register]);

  const projects = useMemo(() => Array.from(new Set(items.map((item) => item.project).filter(Boolean))).sort((a, b) => a.localeCompare(b)), [items]);
  useEffect(() => { hooksRef.current.projects(projects); }, [projects]);

  // Full-text search is Rust's; it is asked a moment after typing stops.
  const needle = filter.trim();
  useEffect(() => {
    if (needle.length < 2) {
      setResult(null);
      setSearchError("");
      return;
    }
    let active = true;
    setSearchError("");
    const timer = setTimeout(() => {
      invoke<Hit[]>("chat_search", { query: needle })
        .then((found) => { if (active) setResult({ query: needle, hits: Array.isArray(found) ? found : [] }); })
        .catch(() => { if (active) { setResult(null); setSearchError("Search failed. Change the query to retry."); } });
    }, 160);
    return () => { active = false; clearTimeout(timer); };
  }, [needle, items]);

  const lower = needle.toLowerCase();
  const searching = lower.length >= 2;
  const hits = result?.query === needle ? result.hits : null;
  const pending = searching && !hits && !searchError;
  const now = Math.floor(Date.now() / 1000);
  const snippets: Record<string, string> = {};
  let shown: ChatItem[];
  if (lower.length >= 2 && hits) {
    shown = hits.map((hit) => {
      snippets[hit.id] = hit.snippet || "";
      return { id: hit.id, title: hit.title, updated: hit.updated, pinned: hit.pinned, project: hit.project || "", preview: hit.snippet || "" };
    });
    if (project) shown = shown.filter((item) => item.project === project);
  } else if (searching) {
    shown = [];
  } else {
    shown = project ? items.filter((item) => item.project === project) : items;
    if (lower) {
      shown = shown.filter((item) =>
        (item.title || "").toLowerCase().includes(lower) || (item.project || "").toLowerCase().includes(lower) || (item.preview || "").toLowerCase().includes(lower));
    }
  }

  return (
    <>
      <label className="chat-search" htmlFor="chat-search">
        <Icon name="search" />
        <input id="chat-search" data-test="chat-search" type="search" placeholder="Search chats &amp; messages" autoComplete="off" value={filter} onChange={(event) => setFilter(event.target.value)} />
      </label>
      <div id="project-filter" className="project-filter" data-test="project-filter" hidden={!projects.length && !project}>
        <button type="button" className={`project-pill${!project ? " active" : ""}`} onClick={() => setProject("")}>All</button>
        {projects.map((name) => (
          <button key={name} type="button" className={`project-pill${project === name ? " active" : ""}`} onClick={() => setProject(name)}>{name}</button>
        ))}
      </div>
      <nav id="chats" data-test="chats" onKeyDown={(event) => {
        // Up and Down walk the list, Home and End jump to its ends; Enter opens the one that has the focus (it is a button).
        const keys = ["ArrowDown", "ArrowUp", "Home", "End"];
        if (!keys.includes(event.key)) return;
        const rows = Array.from(event.currentTarget.querySelectorAll<HTMLButtonElement>(".chat-open"));
        const at = rows.indexOf(document.activeElement as HTMLButtonElement);
        if (rows.length === 0) return;
        event.preventDefault();
        const next = event.key === "Home" ? 0 : event.key === "End" ? rows.length - 1 : event.key === "ArrowDown" ? Math.min(rows.length - 1, at + 1) : Math.max(0, at - 1);
        rows[next]?.focus();
      }}>
        {!shown.length ? <div className="chat-empty" role="status" data-test="chat-search-status">{pending ? "Searching chats…" : searchError || (lower || project ? "No matching chats" : "No chats yet")}</div> : null}
        {shown.map((item, index) => {
          const snippet = snippets[item.id] || (lower && item.preview) || "";
          const label = !searching ? sectionOf(item, index, shown, now) : "";
          return (<Fragment key={item.id}>
            {label ? <div className="chat-section">{label}</div> : null}
            <div className={`chat-row${item.id === currentId ? " current" : ""}${item.pinned ? " pinned" : ""}`} data-test={`chat-${index}`}>
              <button
                className="chat-open"
                onClick={() => {
                  invoke("chat_open", { id: item.id })
                    .then(() => hooksRef.current.opened(item.id, searching ? needle : ""))
                    .catch(hooksRef.current.report);
                }}
              >
                <span className="chat-open-line">
                  {item.id === currentId && live.waiting ? <span className="chat-waiting" data-test="chat-waiting" title="Needs your review" /> : null}
                  {item.id === currentId && live.streaming && !live.waiting ? <span className="chat-working" data-test="chat-working" title="Writing" /> : null}
                  {item.pinned ? <span className="chat-pin-mark"><Icon name="pin" /></span> : null}
                  <span className="chat-open-title">{item.title}</span>
                  {!searching ? <span className="chat-open-when">{when(item.updated, now)}</span> : null}
                </span>
                {snippet && lower ? <span className="chat-open-snippet">{snippet}</span>
                  : !searching && item.preview && item.preview.trim() !== (item.title || "").trim() ? <span className="chat-open-preview">{item.preview}</span> : null}
                {item.project ? <span className="chat-open-project">{item.project}</span> : null}
              </button>
              <button
                className="chat-delete"
                data-test={`chat-delete-${index}`}
                title="Delete"
                aria-label="Delete chat"
                onClick={(event) => {
                  event.stopPropagation();
                  invoke("chat_delete", { id: item.id }).catch(hooksRef.current.report);
                }}
              >
                <Icon name="trash" />
              </button>
            </div>
          </Fragment>);
        })}
      </nav>
    </>
  );
}
