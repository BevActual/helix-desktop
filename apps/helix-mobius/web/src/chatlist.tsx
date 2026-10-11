import { useEffect, useMemo, useRef, useState } from "react";
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
  opened: () => void;
  /** The project names now in the list (the project dialog suggests them). */
  projects: (names: string[]) => void;
  report: (error: unknown) => void;
}
export interface ChatListHandle {
  set: (items: ChatItem[], currentId: string) => void;
}

/** The sidebar's search, project filter and chat list. Rust owns the chats and the full-text search; this filters what it was given and
 * sends back the id of the chat opened or deleted. */
export function ChatList({ register, hooks }: { register: (handle: ChatListHandle) => void; hooks: ChatListHooks }) {
  const [items, setItems] = useState<ChatItem[]>([]);
  const [currentId, setCurrentId] = useState("");
  const [filter, setFilter] = useState("");
  const [project, setProject] = useState("");
  const [hits, setHits] = useState<Hit[] | null>(null);
  const hooksRef = useRef(hooks);
  hooksRef.current = hooks;

  useEffect(() => {
    register({ set: (next, id) => { setItems(next); setCurrentId(id); } });
  }, [register]);

  const projects = useMemo(() => Array.from(new Set(items.map((item) => item.project).filter(Boolean))).sort((a, b) => a.localeCompare(b)), [items]);
  useEffect(() => { hooksRef.current.projects(projects); }, [projects]);

  // Full-text search is Rust's; it is asked a moment after typing stops.
  const needle = filter.trim();
  useEffect(() => {
    if (needle.length < 2) {
      setHits(null);
      return;
    }
    const timer = setTimeout(() => {
      invoke<Hit[]>("chat_search", { query: needle })
        .then((found) => setHits(Array.isArray(found) ? found : []))
        .catch(() => setHits(null));
    }, 160);
    return () => clearTimeout(timer);
  }, [needle]);

  const lower = needle.toLowerCase();
  const snippets: Record<string, string> = {};
  let shown: ChatItem[];
  if (lower.length >= 2 && hits) {
    shown = hits.map((hit) => {
      snippets[hit.id] = hit.snippet || "";
      return { id: hit.id, title: hit.title, updated: hit.updated, pinned: hit.pinned, project: hit.project || "", preview: hit.snippet || "" };
    });
    if (project) shown = shown.filter((item) => item.project === project);
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
      <nav id="chats" data-test="chats">
        {!shown.length ? <div className="chat-empty">{lower || project ? "No matching chats" : "No chats yet"}</div> : null}
        {shown.map((item, index) => {
          const snippet = snippets[item.id] || (lower && item.preview) || "";
          return (
            <div key={item.id} className={`chat-row${item.id === currentId ? " current" : ""}${item.pinned ? " pinned" : ""}`} data-test={`chat-${index}`}>
              <button
                className="chat-open"
                onClick={() => {
                  hooksRef.current.opened();
                  invoke("chat_open", { id: item.id }).catch(hooksRef.current.report);
                }}
              >
                {item.pinned ? <span className="chat-pin-mark"><Icon name="pin" /></span> : null}
                <span className="chat-open-title">{item.title}</span>
                {item.project ? <span className="chat-open-project">{item.project}</span> : null}
                {snippet && lower ? <span className="chat-open-snippet">{snippet}</span> : null}
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
          );
        })}
      </nav>
    </>
  );
}
