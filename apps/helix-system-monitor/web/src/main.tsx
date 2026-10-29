import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
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

const PAGES: { id: Page; title: string; icon: string; tint: string }[] = [
  { id: "overview", title: "Overview", icon: "sun", tint: "#0a84ff" },
  { id: "processes", title: "Processes", icon: "grid", tint: "#8e8e93" },
  { id: "screen-time", title: "Screen Time", icon: "clock", tint: "#5e5ce6" },
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

function Meter({ percent }: { percent: number }) {
  return (
    <div className="meter" aria-hidden="true">
      <div className="meter-fill" style={{ width: `${Math.max(0, Math.min(100, percent))}%` }} />
    </div>
  );
}

/** The last minute as a filled area, its line on top: the shape of a widget's history. */
function Area({ values, test, tint }: { values: number[]; test: string; tint: string }) {
  const width = 300;
  const height = 64;
  const id = `area-${test}`;
  const points = values.map((value, index) => [
    (index / (HISTORY - 1)) * width,
    height - 2 - (Math.max(0, Math.min(100, value)) / 100) * (height - 4),
  ]);
  const line = points.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(" ");
  const area = points.length > 1 ? `0,${height} ${line} ${points[points.length - 1][0].toFixed(1)},${height}` : "";
  return (
    <svg className="area" data-test={test} viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" aria-hidden="true">
      <defs>
        <linearGradient id={id} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor={tint} stopOpacity=".45" />
          <stop offset="1" stopColor={tint} stopOpacity="0" />
        </linearGradient>
      </defs>
      {points.length > 1 ? <polygon points={area} fill={`url(#${id})`} /> : null}
      {points.length > 1 ? <polyline points={line} fill="none" stroke={tint} strokeWidth="2" strokeLinejoin="round" strokeLinecap="round" vectorEffect="non-scaling-stroke" /> : null}
    </svg>
  );
}

function Ring({ percent, tint }: { percent: number; tint: string }) {
  const r = 22;
  const c = 2 * Math.PI * r;
  return (
    <svg className="ring" width="56" height="56" viewBox="0 0 56 56" fill="none" strokeWidth="6" aria-hidden="true">
      <circle cx="28" cy="28" r={r} stroke="var(--fill-2)" />
      <circle cx="28" cy="28" r={r} stroke={tint} strokeLinecap="round" strokeDasharray={c} strokeDashoffset={c * (1 - Math.max(0, Math.min(100, percent)) / 100)} transform="rotate(-90 28 28)" />
    </svg>
  );
}

function Widget({ test, icon: name, label, tint, wide, children }: { test: string; icon: string; label: string; tint: string; wide?: boolean; children: ReactNode }) {
  return (
    <div className={`hx-widget${wide ? " wide" : ""}`} style={{ ["--tint" as string]: tint }} data-test={test}>
      <div className="label"><Icon name={name} />{label}</div>
      {children}
    </div>
  );
}

const CPU = "#0a84ff", MEMORY = "#30d158", SWAP = "#ff9f0a", NETWORK = "#5e5ce6", LOAD = "#bf5af2", GPU = "#ff375f";

function OverviewPage({ view, cpu, mem }: { view: MonitorView; cpu: number[]; mem: number[] }) {
  const o = view.overview;
  const [whole, part] = pct(o.cpu_percent).replace("%", "").split(".");
  return (
    <div className="monitor-widgets">
      <Widget test="stat-cpu" icon="bolt" label="CPU" tint={CPU} wide>
        <div className="big">{whole}<small className="fraction">.{part}%</small></div>
        <Area values={cpu} test="stat-cpu-history" tint={CPU} />
      </Widget>
      <Widget test="stat-mem" icon="grid" label="Memory" tint={MEMORY} wide>
        <div className="big">{bytes(o.mem_used).split(" ")[0]}<small>{bytes(o.mem_used).split(" ")[1]} of {bytes(o.mem_total)}</small></div>
        <Area values={mem} test="stat-mem-history" tint={MEMORY} />
      </Widget>
      <Widget test="stat-swap" icon="folder" label="Swap" tint={SWAP}>
        <Ring percent={o.swap_percent} tint={SWAP} />
        <div className="big">{Math.round(o.swap_percent)}<small>%</small></div>
      </Widget>
      <Widget test="stat-net" icon="wifi" label="Network" tint={NETWORK}>
        <div className="flow"><span className="down">↓</span>{bytes(o.net_rx)}</div>
        <div className="flow"><span className="up">↑</span>{bytes(o.net_tx)}</div>
      </Widget>
      <Widget test="stat-load" icon="sun" label="Load" tint={LOAD}>
        <div className="big">{o.load_1}</div>
        <div className="line">{o.load_5} · {o.load_15} over 5 and 15 min</div>
      </Widget>
      <Widget test="stat-gpu" icon="eye" label="Graphics" tint={GPU}>
        <div className="big small-big">{view.gpu_shown ? "Watching" : "Hidden"}</div>
        <div className="line">{view.gpu_shown ? "Probed while Overview is open" : "Shown on Overview"}</div>
      </Widget>
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
              <span className="page-tile" style={{ ["--tile" as string]: entry.tint }}><Icon name={entry.icon} /></span>
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
  document.body.classList.add("hx-modern", "monitor");
  createRoot(host).render(<Monitor startPage={page} />);
}

(window as unknown as { HelixMonitorReact: unknown }).HelixMonitorReact = { mount };
