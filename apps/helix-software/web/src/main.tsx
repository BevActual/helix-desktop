import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { closeWindow, invoke, useLayoutReport } from "@helix/ui";

type Page = "explore" | "library" | "updates" | "sources";

interface PackageView {
  id: string;
  title: string;
  summary: string;
  kind: string;
  installed: boolean;
  update: boolean;
  remote: string;
}
interface PendingView { id: string; title: string; action: "install" | "remove" | "update"; }
interface PageView {
  explore: PackageView[];
  library: PackageView[];
  updates: PackageView[];
  pending: PendingView | null;
  sources: string[];
  update_count: number;
}

const PAGES: { id: Page; title: string; icon: string }[] = [
  { id: "explore", title: "Explore", icon: "sparkle" },
  { id: "library", title: "Library", icon: "grid" },
  { id: "updates", title: "Updates", icon: "sun" },
  { id: "sources", title: "Sources", icon: "wifi" },
];

declare function icon(name: string): SVGElement;
function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}

const VERB = { install: "Install", remove: "Remove", update: "Update" } as const;
const hue = (name: string) => {
  let hash = 0;
  for (const ch of name) hash = (hash * 31 + (ch.codePointAt(0) ?? 0)) >>> 0;
  return hash % 360;
};

function PackageRow({ pkg, busy, focus, onAction }: { pkg: PackageView; busy: boolean; focus: boolean; onAction: (action: "install" | "remove" | "update") => void }) {
  const row = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (focus) row.current?.scrollIntoView({ block: "center" });
  }, [focus]);
  const action = pkg.update ? "update" : pkg.installed ? "remove" : "install";
  const name = pkg.title || pkg.id;
  return (
    <div ref={row} className={`setting-row pkg-row${focus ? " focus" : ""}`} data-test={`pkg:${pkg.id}`}>
      <span className="pkg-icon" style={{ background: `hsl(${hue(name)} 45% 42%)` }}>{Array.from(name)[0]?.toUpperCase() || "•"}</span>
      <div className="setting-text pkg-text">
        <div className="setting-title">
          {name}
          {pkg.update ? <span className="chip update">Update available</span> : pkg.installed ? <span className="chip">Installed</span> : null}
        </div>
        {pkg.summary ? <div className="setting-about">{pkg.summary}</div> : null}
        {pkg.remote ? <div className="setting-about pkg-remote">{pkg.remote}</div> : null}
      </div>
      <button
        className={`segment pkg-action${action === "install" || action === "update" ? " primary-action" : ""}`}
        data-test={`action:${pkg.id}`}
        disabled={busy}
        onClick={() => onAction(action)}
      >
        {VERB[action]}
      </button>
    </div>
  );
}

function Software({ start }: { start: { page: Page | null; app: string | null } }) {
  const [page, setPage] = useState<Page>(start.page ?? "explore");
  const [query, setQuery] = useState("");
  const [view, setView] = useState<PageView | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [focusId, setFocusId] = useState<string | null>(start.app);
  const search = useRef<HTMLInputElement>(null);
  useLayoutReport("software");

  const read = useCallback(
    (nextPage: Page, nextQuery: string) =>
      invoke<PageView>("software_view", { page: nextPage, query: nextPage === "explore" ? nextQuery : "", kind: "flatpak" }).then(setView, (e) => setError(String(e))),
    [],
  );

  // A deep link opens on the page the package belongs to.
  useEffect(() => {
    if (!start.app) return;
    invoke<PackageView | null>("software_detail", { id: start.app }).then((detail) => {
      if (detail) setPage(detail.installed ? "library" : "explore");
    });
  }, [start.app]);

  // Typing is read after a short pause; a page change is read at once.
  useEffect(() => {
    const timer = setTimeout(() => { read(page, query); }, page === "explore" ? 120 : 0);
    return () => clearTimeout(timer);
  }, [page, query, read]);

  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      if (event.key === "/" && document.activeElement !== search.current) {
        event.preventDefault();
        search.current?.focus();
      }
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, []);

  const request = (id: string, action: "install" | "remove" | "update") => {
    setError("");
    invoke("software_request", { id, action }).then(() => read(page, query), (e) => setError(String(e)));
  };
  const confirm = (accept: boolean) => {
    setBusy(accept);
    setError("");
    invoke("software_confirm", { accept })
      .catch((e) => setError(String(e)))
      .finally(() => {
        setBusy(false);
        setFocusId(null);
        read(page, query);
      });
  };

  const rows = useMemo(() => {
    if (!view) return [];
    if (page === "explore") return view.explore;
    if (page === "updates") return view.updates;
    // The catalog search is Rust's; the library is short enough to filter here.
    const needle = query.trim().toLowerCase();
    return view.library.filter((row) => !needle || (row.title + " " + row.id).toLowerCase().includes(needle));
  }, [view, page, query]);

  const title = PAGES.find((entry) => entry.id === page)?.title ?? "";
  const pending = view?.pending ?? null;
  const emptyText =
    page === "explore" ? (query ? `Nothing matches “${query}”.` : "The catalog is empty. Check your sources.")
    : page === "library" ? (query ? `Nothing installed matches “${query}”.` : "Nothing is installed yet.")
    : "Everything is up to date.";

  return (
    <>
      <aside id="side">
        <div id="titlebar" data-tauri-drag-region>
          <button id="close" className="win-btn" data-test="close" title="Close" aria-label="Close" onClick={() => closeWindow()}>
            <Icon name="x" />
          </button>
        </div>
        <nav id="pages">
          {PAGES.map((entry) => (
            <button
              key={entry.id}
              className={`page-link${entry.id === page ? " current" : ""}`}
              data-page={entry.id}
              data-test={`page-${entry.id}`}
              onClick={() => { setPage(entry.id); setError(""); }}
            >
              <Icon name={entry.icon} />
              <span>{entry.title}</span>
              {entry.id === "updates" && view && view.update_count > 0 ? <span className="count-badge" data-test="updates-count">{view.update_count}</span> : null}
            </button>
          ))}
        </nav>
      </aside>
      <main id="content" data-test="content">
        <header data-tauri-drag-region>
          <h2 id="page-title">{title}</h2>
          {page === "explore" || page === "library" ? (
            <input
              id="search"
              data-test="search"
              ref={search}
              type="search"
              placeholder="Search"
              autoComplete="off"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              onKeyDown={(event) => { if (event.key === "Escape") { setQuery(""); event.currentTarget.blur(); } }}
            />
          ) : null}
        </header>
        {pending ? (
          <div id="confirm" data-test="confirm-bar" role="alertdialog" aria-label="Confirm">
            <div className="setting-row">
              <div className="setting-about confirm-text">{VERB[pending.action]} <b>{pending.title}</b>?</div>
              <button className="segment" data-test="confirm-no" disabled={busy} onClick={() => confirm(false)}>Cancel</button>
              <button className="segment primary-action" data-test="confirm-yes" disabled={busy} onClick={() => confirm(true)}>
                {busy ? "Working…" : "Allow"}
              </button>
            </div>
          </div>
        ) : null}
        {error ? <div className="banner error" data-test="error" role="alert">{error}</div> : null}
        <div id="list" data-test="list">
          {page === "sources" ? (
            <div className="rows-card">
              {(view?.sources ?? []).map((name) => (
                <div key={name} className="setting-row" data-test={`source:${name}`}>
                  <div className="setting-title">{name}</div>
                </div>
              ))}
            </div>
          ) : view && !rows.length ? (
            <div className="empty">{emptyText}</div>
          ) : (
            <div className="rows-card">
              {rows.map((pkg) => (
                <PackageRow key={pkg.id} pkg={pkg} busy={busy || !!pending} focus={focusId === pkg.id} onAction={(action) => request(pkg.id, action)} />
              ))}
            </div>
          )}
        </div>
      </main>
    </>
  );
}

function mount(host: HTMLElement) {
  const params = new URLSearchParams(location.search);
  const startPage = params.get("page");
  const page = PAGES.some((entry) => entry.id === startPage) ? (startPage as Page) : null;
  createRoot(host).render(<Software start={{ page, app: params.get("app") }} />);
}

(window as unknown as { HelixSoftwareReact: unknown }).HelixSoftwareReact = { mount };
