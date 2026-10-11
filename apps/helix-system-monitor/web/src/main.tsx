import { useCallback, useEffect, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { closeWindow, invoke, listen, useLayoutReport } from "@helix/ui";

type Page = "overview" | "processes" | "screen-time";
type Sort = "cpu" | "memory" | "name" | "pid";

interface Overview {
  cpu_percent: number;
  mem_percent: number;
  mem_used: number;
  mem_total: number;
  swap_percent: number;
  net_rx: number;
  net_tx: number;
  load_1: number;
  load_5: number;
  load_15: number;
}
interface ProcessRow { pid: number; name: string; state: string; threads: number; rss: number; cpu_ticks: number; }
interface ScreenTimeRow { app: string; seconds: number; }
interface MonitorView { overview: Overview; processes: ProcessRow[]; screen_time: ScreenTimeRow[]; gpu_shown: boolean; }

const PAGES: { id: Page; title: string; icon: string }[] = [
  { id: "overview", title: "Overview", icon: "sun" },
  { id: "processes", title: "Processes", icon: "grid" },
  { id: "screen-time", title: "Screen Time", icon: "sparkle" },
];
const SORTS: { id: Sort; title: string }[] = [
  { id: "cpu", title: "CPU" },
  { id: "memory", title: "Memory" },
  { id: "name", title: "Name" },
  { id: "pid", title: "PID" },
];
const HISTORY = 60;
const TICKS_PER_SECOND = 100;

declare function icon(name: string): SVGElement;
function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}

const pct = (n: number) => `${(Math.round(n * 10) / 10).toFixed(1)}%`;
function bytes(n: number): string {
  if (n >= 1e9) return `${(n / 1e9).toFixed(1)} GB`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(1)} MB`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(0)} KB`;
  return `${n} B`;
}
function duration(seconds: number): string {
  if (seconds >= 3600) return `${Math.floor(seconds / 3600)} h ${Math.round((seconds % 3600) / 60)} min`;
  if (seconds >= 60) return `${Math.round(seconds / 60)} min`;
  return `${seconds} s`;
}
function cpuTime(ticks: number): string {
  const seconds = ticks / TICKS_PER_SECOND;
  return seconds >= 60 ? duration(Math.round(seconds)) : `${seconds.toFixed(1)} s`;
}

/** A thin line of the last minute of samples. */
function Spark({ values, test }: { values: number[]; test: string }) {
  const width = 120;
  const height = 28;
  const points = values.map((value, index) => {
    const x = (index / (HISTORY - 1)) * width;
    const y = height - (Math.max(0, Math.min(100, value)) / 100) * height;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  });
  return (
    <svg className="spark" data-test={test} viewBox={`0 0 ${width} ${height}`} width={width} height={height} aria-hidden="true">
      {points.length > 1 ? <polyline points={points.join(" ")} fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinejoin="round" strokeLinecap="round" /> : null}
    </svg>
  );
}

function Meter({ percent }: { percent: number }) {
  return (
    <div className="meter" aria-hidden="true">
      <div className="meter-fill" style={{ width: `${Math.max(0, Math.min(100, percent))}%` }} />
    </div>
  );
}

function Stat({ test, title, value, percent, history }: { test: string; title: string; value: string; percent?: number; history?: number[] }) {
  return (
    <div className="setting-row stat-row" data-test={test}>
      <div className="setting-text stat-text">
        <div className="setting-title">{title}</div>
        <div className="setting-about">{value}</div>
        {percent !== undefined ? <Meter percent={percent} /> : null}
      </div>
      {history ? <Spark values={history} test={`${test}-history`} /> : null}
    </div>
  );
}

function OverviewPage({ view, cpu, mem }: { view: MonitorView; cpu: number[]; mem: number[] }) {
  const o = view.overview;
  return (
    <div className="rows-card">
      <Stat test="stat-cpu" title="CPU" value={pct(o.cpu_percent)} percent={o.cpu_percent} history={cpu} />
      <Stat test="stat-mem" title="Memory" value={`${pct(o.mem_percent)} · ${bytes(o.mem_used)} / ${bytes(o.mem_total)}`} percent={o.mem_percent} history={mem} />
      <Stat test="stat-swap" title="Swap" value={pct(o.swap_percent)} percent={o.swap_percent} />
      <Stat test="stat-net" title="Network" value={`↓ ${bytes(o.net_rx)}   ↑ ${bytes(o.net_tx)}`} />
      <Stat test="stat-load" title="Load" value={`${o.load_1} / ${o.load_5} / ${o.load_15}`} />
      <Stat test="stat-gpu" title="GPU" value={view.gpu_shown ? "Probing while Overview is open" : "Hidden"} />
    </div>
  );
}

function ProcessesPage({ view, sort, setSort, memTotal }: { view: MonitorView; sort: Sort; setSort: (sort: Sort) => void; memTotal: number }) {
  return (
    <>
      <div className="segments sort-segments" role="tablist" aria-label="Sort by">
        {SORTS.map((entry) => (
          <button key={entry.id} className={`segment${entry.id === sort ? " current" : ""}`} data-test={`sort-${entry.id}`} onClick={() => setSort(entry.id)}>
            {entry.title}
          </button>
        ))}
      </div>
      {view.processes.length ? (
        <div className="rows-card">
          {view.processes.slice(0, 80).map((row) => (
            <div key={row.pid} className="setting-row" data-test={`proc:${row.pid}`}>
              <div className="setting-text stat-text">
                <div className="setting-title">{row.name} <span className="pid">{row.pid}</span></div>
                <div className="setting-about">RSS {bytes(row.rss)} · CPU time {cpuTime(row.cpu_ticks)} · {row.threads} {row.threads === 1 ? "thread" : "threads"}</div>
                {memTotal > 0 ? <Meter percent={(row.rss / memTotal) * 100 * 4} /> : null}
              </div>
            </div>
          ))}
        </div>
      ) : (
        <div className="empty">No processes.</div>
      )}
    </>
  );
}

function ScreenTimePage({ rows }: { rows: ScreenTimeRow[] }) {
  if (!rows.length) return <div className="empty">No screen-time report yet.</div>;
  const longest = Math.max(...rows.map((row) => row.seconds), 1);
  return (
    <div className="rows-card">
      {rows.map((row) => (
        <div key={row.app} className="setting-row" data-test={`screen:${row.app}`}>
          <div className="setting-text stat-text">
            <div className="setting-title">{row.app}</div>
            <div className="setting-about">{duration(row.seconds)}</div>
            <Meter percent={(row.seconds / longest) * 100} />
          </div>
        </div>
      ))}
    </div>
  );
}

function Monitor({ startPage }: { startPage: Page | null }) {
  const [page, setPage] = useState<Page>(startPage ?? "overview");
  const [query, setQuery] = useState("");
  const [sort, setSort] = useState<Sort>("cpu");
  const [view, setView] = useState<MonitorView | null>(null);
  const [cpu, setCpu] = useState<number[]>([]);
  const [mem, setMem] = useState<number[]>([]);
  const current = useRef({ page, query, sort });
  current.current = { page, query, sort };
  useLayoutReport("system-monitor");

  const read = useCallback(() => {
    const { query: q, sort: s } = current.current;
    invoke<MonitorView>("monitor_view", { query: q, sort: s }).then((next) => {
      setView(next);
      setCpu((old) => [...old.slice(-(HISTORY - 1)), next.overview.cpu_percent]);
      setMem((old) => [...old.slice(-(HISTORY - 1)), next.overview.mem_percent]);
    });
  }, []);

  // Rust samples on its own timer and says so; the page reads when told, and at once when the page or filter changes.
  useEffect(() => {
    const stop = listen<null>("monitor", read);
    return () => { stop.then((fn) => fn()); };
  }, [read]);
  useEffect(() => {
    invoke("monitor_set_gpu", { shown: page === "overview" });
    read();
  }, [page, query, sort, read]);

  const title = PAGES.find((entry) => entry.id === page)?.title ?? "";
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
            <button key={entry.id} className={`page-link${entry.id === page ? " current" : ""}`} data-page={entry.id} data-test={`page-${entry.id}`} onClick={() => setPage(entry.id)}>
              <Icon name={entry.icon} />
              <span>{entry.title}</span>
            </button>
          ))}
        </nav>
      </aside>
      <main id="content" data-test="content">
        <header data-tauri-drag-region>
          <h2 id="page-title">{title}</h2>
          {page === "processes" ? (
            <input
              id="search"
              data-test="search"
              type="search"
              placeholder="Filter processes"
              autoComplete="off"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              onKeyDown={(event) => { if (event.key === "Escape") setQuery(""); }}
            />
          ) : null}
        </header>
        <div id="body" data-test="body">
          {!view ? null : page === "overview" ? (
            <OverviewPage view={view} cpu={cpu} mem={mem} />
          ) : page === "processes" ? (
            <ProcessesPage view={view} sort={sort} setSort={setSort} memTotal={view.overview.mem_total} />
          ) : (
            <ScreenTimePage rows={view.screen_time} />
          )}
        </div>
      </main>
    </>
  );
}

function mount(host: HTMLElement) {
  const start = new URLSearchParams(location.search).get("page");
  const page = PAGES.some((entry) => entry.id === start) ? (start as Page) : null;
  createRoot(host).render(<Monitor startPage={page} />);
}

(window as unknown as { HelixMonitorReact: unknown }).HelixMonitorReact = { mount };
