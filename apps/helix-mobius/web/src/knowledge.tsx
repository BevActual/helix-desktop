import { useCallback, useEffect, useState } from "react";
import { invoke } from "@helix/ui";

interface FolderView { path: string; indexed: number; pages: number; passages: number; with_meaning: number; }
interface Running { folder: string; done: number; total: number; }
export interface KnowledgeStatus { meaning: boolean; folders: FolderView[]; running: Running | null; error: string; total_pages: number; }
interface Found { title: string; heading: string; snippet: string; key: string; by_meaning: boolean; }

const home = (path: string) => path.replace(/^\/home\/[^/]+/, "~");
function when(seconds: number): string {
  if (!seconds) return "not finished yet";
  const days = Math.floor((Date.now() / 1000 - seconds) / 86400);
  return days <= 0 ? "read today" : days === 1 ? "read yesterday" : `read ${days} days ago`;
}

/** Folders Mobius reads so their files can be searched by words and by meaning. All of it stays on this computer. */
export function KnowledgePanel({ report, onGetModel }: { report: (error: unknown) => void; onGetModel: () => void }) {
  const [status, setStatus] = useState<KnowledgeStatus | null>(null);
  const [query, setQuery] = useState("");
  const [found, setFound] = useState<Found[] | null>(null);
  const [searching, setSearching] = useState(false);
  const [adding, setAdding] = useState(false);

  const refresh = useCallback(() => {
    invoke<KnowledgeStatus>("knowledge_status").then(setStatus, () => undefined);
  }, []);
  useEffect(() => {
    refresh();
    const tauri = (window as unknown as { __TAURI__?: { event?: { listen: (name: string, cb: (e: { payload: KnowledgeStatus }) => void) => Promise<() => void> } } }).__TAURI__;
    let stop: (() => void) | undefined;
    tauri?.event?.listen("knowledge-changed", (event) => setStatus(event.payload)).then((off) => { stop = off; });
    return () => stop?.();
  }, [refresh]);

  const add = () => {
    setAdding(true);
    invoke<string | null>("knowledge_add_folder").catch(report).finally(() => { setAdding(false); refresh(); });
  };
  const search = () => {
    if (query.trim().length < 2) return;
    setSearching(true);
    invoke<Found[]>("knowledge_search", { query }).then(setFound, (e) => { setFound([]); report(e); }).finally(() => setSearching(false));
  };
  const running = status?.running;
  const empty = !status?.folders.length;

  return (
    <>
      <h3 className="models-h3">Your folders</h3>
      <p className="models-lead">Choose folders and Mobius reads their text files into a search index on this computer, so you can find things by what they mean as well as by the words in them. Nothing leaves this computer: the index is one file in your data folder, and removing a folder removes everything read from it.</p>
      {status && !status.meaning ? (
        <div className="set-offer" data-test="kn-nomeaning">
          <div className="set-offer-text"><div><strong>Search by meaning needs a small model</strong></div><span>Until it is installed, files can only be found by their words.</span></div>
          <button type="button" className="primary" data-test="kn-get-model" onClick={onGetModel}>Get it in the Library</button>
        </div>
      ) : null}
      {status?.error ? <p className="lib-error" data-test="kn-error">{status.error}</p> : null}
      <div className="set-list kn-list" data-test="kn-folders">
        {status?.folders.map((folder) => {
          const mine = running?.folder === folder.path ? running : null;
          const percent = mine && mine.total ? Math.min(100, Math.round((mine.done / mine.total) * 100)) : 0;
          return (
            <div key={folder.path} className="set-row" data-test={`kn-folder-${folder.path}`}>
              <div className="set-row-main">
                <div className="set-row-text">
                  <div className="set-row-title kn-path">{home(folder.path)}</div>
                  <div className="set-row-note">{folder.pages} file{folder.pages === 1 ? "" : "s"} · {folder.passages} passage{folder.passages === 1 ? "" : "s"}{folder.passages && folder.with_meaning < folder.passages ? ` · ${folder.with_meaning} searchable by meaning` : ""} · {mine ? "reading now" : when(folder.indexed)}</div>
                </div>
                <div className="kn-actions">
                  {mine ? <button type="button" data-test="kn-stop" onClick={() => invoke("knowledge_cancel")}>Stop</button> : (
                    <>
                      <button type="button" data-test={`kn-reindex-${folder.path}`} disabled={!!running} onClick={() => invoke("knowledge_reindex", { path: folder.path }).catch(report)}>Read again</button>
                      <button type="button" className="lib-remove" data-test={`kn-remove-${folder.path}`} disabled={!!running}
                        onClick={() => { if (window.confirm(`Forget ${home(folder.path)}? Your files are not touched; only what Mobius read from them is removed.`)) invoke("knowledge_remove_folder", { path: folder.path }).catch(report).finally(refresh); }}>Remove</button>
                    </>
                  )}
                </div>
              </div>
              {mine ? (
                <div className="lib-progress" data-test="kn-progress">
                  <div className="lib-bar"><div style={{ width: `${percent}%` }} /></div>
                  <span>{mine.done} of {mine.total} files · {percent}%</span>
                </div>
              ) : null}
            </div>
          );
        })}
        {empty ? <div className="set-row"><div className="set-row-note" data-test="kn-empty">No folders yet. Add a project, a notes folder, or documentation.</div></div> : null}
      </div>
      <div className="lib-add">
        <button type="button" className="primary" data-test="kn-add" disabled={adding || !!running} onClick={add}>{adding ? "Choosing…" : "Add a folder…"}</button>
        <span className="set-hint">Text and code files under 300 KB, up to 3000 per folder. Hidden folders, build output and links are skipped.</span>
      </div>

      <h3 className="models-h3 set-gap">Try a search</h3>
      <p className="models-lead">See what Mobius would find in your folders for a question.</p>
      <div className="hub-bar">
        <input data-test="kn-query" type="text" spellCheck={false} placeholder="What do the notes say about…" autoComplete="off" value={query}
          onChange={(e) => setQuery(e.target.value)} onKeyDown={(e) => { if (e.key === "Enter") search(); }} />
        <button type="button" className="primary" data-test="kn-search" disabled={searching || empty || query.trim().length < 2} onClick={search}>{searching ? "Searching…" : "Search"}</button>
      </div>
      {found ? (
        <div className="hub-results" data-test="kn-results">
          {found.length ? found.map((hit, i) => (
            <div key={`${hit.key}:${i}`} className="hub-repo kn-hit" data-test={`kn-hit-${i}`}>
              <span className="hub-repo-name">{hit.title}{hit.heading ? ` › ${hit.heading}` : ""}</span>
              <span className="lib-tags">{hit.by_meaning ? <span className="lib-tag vision">By meaning</span> : null}</span>
              <span className="hub-repo-meta kn-snippet">{hit.snippet}</span>
            </div>
          )) : <p className="route-model-empty" data-test="kn-none">Nothing found.</p>}
        </div>
      ) : null}
    </>
  );
}
