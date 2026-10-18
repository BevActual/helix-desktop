import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { AppFrame, Banner, Button, Chip, EmptyState, Group, HxRow, Icon, SearchField, Sheet, Sidebar, SidebarItem, SidebarLabel, Spinner, invoke, listen, useLayoutReport } from "@helix/ui";
import { Detail } from "./detail";
import { forgetIcons } from "./hooks";
import { JobsBar } from "./jobs";
import { AppTile, CardRow, Hero, PackageRow, SkeletonRows } from "./rows";
import { CATEGORIES, VERB, type JobView, type Page, type PackageView, type PageView } from "./types";

const PAGE_SIZE = 60;
const MINE: { id: Page; title: string; icon: string }[] = [
  { id: "library", title: "Library", icon: "grid" },
  { id: "updates", title: "Updates", icon: "shield" },
  { id: "sources", title: "Sources", icon: "wifi" },
];

function Software({ start }: { start: { page: Page | null; app: string | null } }) {
  const [page, setPage] = useState<Page>(start.page ?? "discover");
  const [category, setCategory] = useState("");
  const [query, setQuery] = useState("");
  const [limit, setLimit] = useState(PAGE_SIZE);
  const [view, setView] = useState<PageView | null>(null);
  const [detail, setDetail] = useState<PackageView | null>(null);
  const [detailId, setDetailId] = useState<string | null>(start.app);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [listFallback, setListFallback] = useState(false);
  const search = useRef<HTMLInputElement>(null);
  const sentinel = useRef<HTMLDivElement>(null);
  const current = useRef({ page, query, limit, category, listFallback });
  current.current = { page, query, limit, category, listFallback };
  useLayoutReport("software");

  const read = useCallback(() => {
    const { page: p, query: q, limit: l, category: c, listFallback: fallback } = current.current;
    const asked = p === "discover" && fallback && !q ? "explore" : p;
    invoke<PageView>("software_view", { page: asked, query: p === "discover" || p === "category" || p === "library" ? q : "", category: c, offset: 0, limit: l }).then(setView, (e) => setError(String(e)));
  }, []);

  // Rust says when anything changed (a catalog read, the index, a job's progress, a result); the page reads then, and at once on its own changes.
  useEffect(() => {
    const stop = listen<null>("software-changed", read);
    return () => { stop.then((fn) => fn()); };
  }, [read]);
  useEffect(() => {
    const timer = setTimeout(read, query ? 120 : 0);
    return () => clearTimeout(timer);
  }, [page, category, query, limit, listFallback, read]);
  useEffect(() => { setLimit(PAGE_SIZE); setListFallback(false); }, [page, category, query]);

  // Without appstream data (no remotes, or the index failed) Discover has nothing to curate: fall back to the plain list.
  useEffect(() => {
    if (page === "discover" && !query && view && view.discover && !view.discover.hero && !view.discover.shelves.length && !view.indexing && !view.loading && view.installed_count + view.update_count >= 0) {
      setListFallback(true);
    }
  }, [page, query, view]);

  // Icons are read again once a catalog read has finished (an install may have brought new ones), not on every progress tick.
  const wasLoading = useRef(false);
  useEffect(() => {
    if (wasLoading.current && view && !view.loading) forgetIcons();
    wasLoading.current = !!view?.loading;
  }, [view]);

  // A deep link opens the package.
  useEffect(() => {
    if (!start.app) return;
    invoke<PackageView | null>("software_detail", { id: start.app }).then((value) => {
      if (value) {
        setDetail(value);
        setPage(value.installed ? "library" : "discover");
      }
    });
  }, [start.app]);

  // The open package is kept current from the catalog as it changes.
  useEffect(() => {
    if (!detailId) return;
    invoke<PackageView | null>("software_detail", { id: detailId }).then((value) => setDetail(value), () => undefined);
  }, [detailId, view]);

  // More rows as the end of a long list scrolls into view.
  const total = view?.total ?? 0;
  const shown = view?.rows.length ?? 0;
  useEffect(() => {
    const node = sentinel.current;
    if (!node) return;
    const observer = new IntersectionObserver((entries) => {
      if (entries.some((entry) => entry.isIntersecting)) setLimit((old) => (old < total ? old + PAGE_SIZE : old));
    });
    observer.observe(node);
    return () => observer.disconnect();
  }, [page, total, shown, detailId]);

  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      const typing = event.target instanceof HTMLInputElement || event.target instanceof HTMLTextAreaElement;
      if (event.key === "/" && !typing) {
        event.preventDefault();
        search.current?.focus();
      } else if (event.key === "Escape" && detailId && !view?.pending) {
        setDetailId(null);
      }
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, [detailId, view?.pending]);

  const request = (id: string, action: "install" | "remove" | "update") => {
    setError("");
    invoke("software_request", { id, action }).then(read, (e) => setError(String(e)));
  };
  const open = (id: string) => invoke("software_open", { id }).catch((e) => setError(String(e)));
  const confirm = useCallback((accept: boolean) => {
    setBusy(true);
    invoke("software_confirm", { accept })
      .catch((e) => setError(String(e)))
      .finally(() => { setBusy(false); read(); });
  }, [read]);
  const goto = (next: Page, nextCategory = "") => {
    setPage(next);
    setCategory(nextCategory);
    setDetailId(null);
    setQuery("");
    setError("");
  };

  const jobFor = useMemo(() => {
    const byPackage = new Map<string, JobView>();
    (view?.jobs ?? []).forEach((job) => {
      const existing = byPackage.get(job.package_id);
      if (!existing || job.state === "running") byPackage.set(job.package_id, job);
    });
    return (id: string) => byPackage.get(id);
  }, [view?.jobs]);
  const running = (view?.jobs ?? []).some((job) => job.state === "running");
  const blocked = running || !!view?.pending;

  const pending = view?.pending ?? null;
  const rows = view?.rows ?? [];
  const categoryTitle = CATEGORIES.find((entry) => entry.id === category)?.title ?? "";
  const pageTitle = page === "discover" ? "Discover" : page === "category" ? categoryTitle : MINE.find((entry) => entry.id === page)?.title ?? "";
  const title = detailId && detail ? detail.title || detail.id : pageTitle;
  const allError = error || view?.error || "";
  const searching = (page === "discover" || page === "category" || page === "library") && query.trim() !== "";
  const curated = page === "discover" && !query && !listFallback;
  const emptyText =
    page === "library" ? (query ? `Nothing installed matches “${query}”.` : "Nothing is installed yet. Find apps in Discover.")
    : page === "updates" ? "Everything is up to date."
    : query ? `Nothing matches “${query}”.` : "The catalog is empty. Check your sources.";
  const wire = (pkg: PackageView) => ({
    pkg,
    job: jobFor(pkg.id),
    blocked,
    onDetail: () => { setDetail(pkg); setDetailId(pkg.id); },
    onOpen: () => open(pkg.id),
    onRequest: (action: "install" | "update") => request(pkg.id, action),
  });

  const toolbar = detailId ? null : page === "updates" ? (
    <>
      <Button data-test="check-updates" disabled={view?.checking} onClick={() => { setError(""); invoke("software_refresh", { network: true }); }}>
        {view?.checking ? <><Spinner /> Checking…</> : "Check for Updates"}
      </Button>
      {view && view.update_count > 0 ? (
        <Button kind="primary" data-test="update-all" disabled={blocked} onClick={() => request("*", "update")}>Update All ({view.update_count})</Button>
      ) : null}
    </>
  ) : page === "sources" ? (
    <Button data-test="refresh" disabled={view?.checking || view?.loading} onClick={() => invoke("software_refresh", { network: true })}>Refresh</Button>
  ) : (
    <SearchField test="search" value={query} onChange={setQuery} inputRef={search} />
  );

  const results = (
    <>
      <h2 className="hx-group-title" data-test="count">{total.toLocaleString()} {page === "library" ? "installed" : searching ? "results" : "apps"}</h2>
      {page === "library" ? (
        <Group>
          {rows.map((pkg) => (
            <PackageRow key={pkg.id} pkg={pkg} job={jobFor(pkg.id)} blocked={blocked} focus={start.app === pkg.id && !detailId} library onOpen={() => open(pkg.id)}
              onDetail={() => { setDetail(pkg); setDetailId(pkg.id); }} onRequest={(action) => request(pkg.id, action)} />
          ))}
        </Group>
      ) : (
        <div className="hx-grid">{rows.map((pkg) => <CardRow key={pkg.id} {...wire(pkg)} />)}</div>
      )}
      {total > shown ? <div ref={sentinel} className="hx-more"><Spinner /></div> : <div ref={sentinel} />}
    </>
  );

  return (
    <>
      <AppFrame
        title={title}
        largeTitle={!detailId}
        leading={detailId ? (
          <Button kind="plain" data-test="back" aria-label="Back" onClick={() => setDetailId(null)}>
            <Icon name="chevron-left" /> {pageTitle}
          </Button>
        ) : undefined}
        sidebar={
          <Sidebar>
            <SidebarItem icon="sparkle" label="Discover" test="page-discover" current={page === "discover"} onClick={() => goto("discover")} />
            <SidebarLabel>Categories</SidebarLabel>
            {CATEGORIES.map((entry) => (
              <SidebarItem key={entry.id} icon={entry.icon} label={entry.title} test={`cat-${entry.id}`} current={page === "category" && category === entry.id} onClick={() => goto("category", entry.id)} />
            ))}
            <SidebarLabel>Your Apps</SidebarLabel>
            {MINE.map((entry) => (
              <SidebarItem key={entry.id} icon={entry.icon} label={entry.title} test={`page-${entry.id}`} current={entry.id === page}
                count={entry.id === "updates" ? view?.update_count : undefined} onClick={() => goto(entry.id)} />
            ))}
          </Sidebar>
        }
        toolbar={toolbar}
      >
        {allError ? (
          <Banner tone="error" test="error" action={<Button small onClick={() => { setError(""); invoke("software_refresh", { network: false }); }}>Try Again</Button>}>
            {allError}
          </Banner>
        ) : null}

        {detailId && detail ? (
          <Detail pkg={detail} job={jobFor(detail.id)} blocked={blocked} onOpen={() => open(detail.id)} onRequest={(action) => request(detail.id, action)} />
        ) : (
          <div data-test="list" style={{ display: "flex", flexDirection: "column", gap: 24 }}>
            {page === "sources" ? (
              <Group title="Where software comes from">
                {(view?.sources ?? []).map((remote) => (
                  <HxRow key={`${remote.installation}:${remote.name}`} test={`source:${remote.name}`} title={remote.title} sub={remote.kind === "rpm" ? "Fedora repository" : remote.url}
                    trailing={<Chip>{remote.installation === "user" ? "This user" : "System"}</Chip>} />
                ))}
              </Group>
            ) : curated ? (
              !view || (!view.discover?.hero && !view.discover?.shelves.length) ? (
                view && (view.indexing || view.loading) ? (
                  <>
                    <SkeletonRows count={3} />
                    <div className="hx-more"><Spinner /> &nbsp;Preparing the catalog…</div>
                  </>
                ) : <SkeletonRows />
              ) : (
                <>
                  {view.discover?.hero ? <Hero {...wire(view.discover.hero)} /> : null}
                  {view.discover?.shelves.map((shelf) => (
                    <section key={shelf.title} className="hx-shelf" data-test={`shelf:${shelf.category || "recent"}`}>
                      <div className="hx-shelf-head">
                        <h3>{shelf.title}</h3>
                        {shelf.category ? <Button kind="plain" small data-test={`see-all:${shelf.category}`} onClick={() => goto("category", shelf.category)}>See All</Button> : null}
                      </div>
                      <div className="hx-rail">
                        {shelf.rows.map((pkg) => <AppTile key={pkg.id} test={`tile:${shelf.category || "recent"}:${pkg.id}`} {...wire(pkg)} />)}
                      </div>
                    </section>
                  ))}
                </>
              )
            ) : !view || (view.loading && !rows.length) ? (
              <SkeletonRows />
            ) : !rows.length ? (
              <EmptyState icon={page === "updates" ? "shield" : "search"}>{emptyText}</EmptyState>
            ) : page === "updates" ? (
              <Group>
                {rows.map((pkg) => (
                  <PackageRow key={pkg.id} pkg={pkg} job={jobFor(pkg.id)} blocked={blocked} focus={false} library={false} onOpen={() => open(pkg.id)}
                    onDetail={() => { setDetail(pkg); setDetailId(pkg.id); }} onRequest={(action) => request(pkg.id, action)} />
                ))}
              </Group>
            ) : (
              results
            )}
          </div>
        )}
      </AppFrame>
      <JobsBar jobs={view?.jobs ?? []} onOpen={open} />
      {pending ? (
        <Sheet
          alert
          test="confirm-bar"
          title={pending.id === "*" ? "Update all?" : `${VERB[pending.action] === "Get" ? "Install" : VERB[pending.action]} “${pending.title}”?`}
          onCancel={() => confirm(false)}
          actions={
            <>
              <Button data-test="confirm-no" disabled={busy} onClick={() => confirm(false)}>Cancel</Button>
              <Button kind={pending.action === "remove" ? "destructive" : "primary"} data-test="confirm-yes" disabled={busy} onClick={() => confirm(true)}>
                {pending.id === "*" ? "Update All" : pending.action === "install" ? "Install" : VERB[pending.action]}
              </Button>
            </>
          }
        >
          <p>
            {pending.kind === "firmware"
              ? "Firmware updates are applied by the system and may need a restart."
              : pending.id === "*"
                ? `${view?.update_count ?? 0} updates will be installed.`
                : pending.action === "remove"
                  ? "It will be removed from this computer. Its settings and data are kept."
                  : pending.action === "update"
                    ? pending.id === "system-updates"
                      ? "System packages are updated now; some, such as the kernel, take effect after a restart."
                      : "The new version replaces the installed one."
                    : "It will be installed from its source."}
          </p>
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
  const mapped = startPage === "explore" ? "discover" : startPage;
  const known: Page[] = ["discover", "library", "updates", "sources"];
  const page = known.includes(mapped as Page) ? (mapped as Page) : null;
  createRoot(host).render(<Software start={{ page, app: params.get("app") }} />);
}

(window as unknown as { HelixSoftwareReact: unknown }).HelixSoftwareReact = { mount };
