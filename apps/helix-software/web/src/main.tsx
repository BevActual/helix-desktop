import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { AppFrame, Banner, Button, Chip, EmptyState, Group, HxRow, IconTile, SearchField, Sidebar, SidebarItem, Sheet, hueOf, invoke, useLayoutReport } from "@helix/ui";

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

const VERB = { install: "Install", remove: "Remove", update: "Update" } as const;

function PackageRow({ pkg, busy, focus, onAction }: { pkg: PackageView; busy: boolean; focus: boolean; onAction: (action: "install" | "remove" | "update") => void }) {
  const action = pkg.update ? "update" : pkg.installed ? "remove" : "install";
  const name = pkg.title || pkg.id;
  return (
    <HxRow
      test={`pkg:${pkg.id}`}
      focus={focus}
      leading={<IconTile large letter={Array.from(name)[0]?.toUpperCase() || "•"} hue={hueOf(name)} />}
      title={<>{name} {pkg.update ? <Chip tone="accent">Update available</Chip> : pkg.installed ? <Chip>Installed</Chip> : null}</>}
      sub={<>{pkg.summary}{pkg.summary && pkg.remote ? " · " : ""}{pkg.remote}</>}
      trailing={
        <Button kind={action === "remove" ? undefined : "primary"} small data-test={`action:${pkg.id}`} disabled={busy} onClick={() => onAction(action)}>
          {VERB[action]}
        </Button>
      }
    />
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
  const confirm = useCallback((accept: boolean) => {
    setBusy(accept);
    setError("");
    invoke("software_confirm", { accept })
      .catch((e) => setError(String(e)))
      .finally(() => {
        setBusy(false);
        setFocusId(null);
        read(page, query);
      });
  }, [page, query, read]);

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
    : page === "library" ? (query ? `Nothing installed matches “${query}”.` : "Nothing is installed yet. Find apps in Explore.")
    : "Everything is up to date.";
  const emptyIcon = page === "updates" ? "shield" : "search";

  return (
    <>
      <AppFrame
        title={title}
        sidebar={
          <Sidebar>
            {PAGES.map((entry) => (
              <SidebarItem
                key={entry.id}
                icon={entry.icon}
                label={entry.title}
                test={`page-${entry.id}`}
                current={entry.id === page}
                count={entry.id === "updates" ? view?.update_count : undefined}
                onClick={() => { setPage(entry.id); setError(""); }}
              />
            ))}
          </Sidebar>
        }
        toolbar={page === "explore" || page === "library" ? <SearchField test="search" value={query} onChange={setQuery} inputRef={search} /> : null}
      >
        {error ? <Banner tone="error" test="error">{error}</Banner> : null}
        <div data-test="list">
          {page === "sources" ? (
            <Group title="Where software comes from">
              {(view?.sources ?? []).map((name) => (
                <HxRow key={name} test={`source:${name}`} title={name} />
              ))}
            </Group>
          ) : view && !rows.length ? (
            <EmptyState icon={emptyIcon}>{emptyText}</EmptyState>
          ) : (
            <Group>
              {rows.map((pkg) => (
                <PackageRow key={pkg.id} pkg={pkg} busy={busy || !!pending} focus={focusId === pkg.id} onAction={(action) => request(pkg.id, action)} />
              ))}
            </Group>
          )}
        </div>
      </AppFrame>
      {pending ? (
        <Sheet
          alert
          test="confirm-bar"
          title={`${VERB[pending.action]} “${pending.title}”?`}
          onCancel={() => confirm(false)}
          actions={
            <>
              <Button data-test="confirm-no" disabled={busy} onClick={() => confirm(false)}>Cancel</Button>
              <Button kind={pending.action === "remove" ? "destructive" : "primary"} data-test="confirm-yes" disabled={busy} onClick={() => confirm(true)}>
                {busy ? "Working…" : VERB[pending.action]}
              </Button>
            </>
          }
        >
          <p>{pending.action === "remove" ? "It will be removed from this computer." : pending.action === "update" ? "The new version replaces the installed one." : "It will be installed from its source."}</p>
        </Sheet>
      ) : null}
    </>
  );
}

function mount(host: HTMLElement) {
  // The hand-written page styles `body.settings #app`; the React page brings its own frame.
  document.body.classList.remove("settings");
  document.body.classList.add("hx");
  const params = new URLSearchParams(location.search);
  const startPage = params.get("page");
  const page = PAGES.some((entry) => entry.id === startPage) ? (startPage as Page) : null;
  createRoot(host).render(<Software start={{ page, app: params.get("app") }} />);
}

(window as unknown as { HelixSoftwareReact: unknown }).HelixSoftwareReact = { mount };
