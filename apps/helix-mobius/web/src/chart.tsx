import { useMemo } from "react";

/** A chart a reply asks for: a fenced block labelled `chart` holding JSON. The model only writes data; this draws it (an SVG from fixed
 * shapes, nothing it sends is run). Anything that does not parse is left as an ordinary code block. */
export interface ChartSpec { type: "bar" | "line" | "pie"; title: string; labels: string[]; series: { name: string; values: number[] }[] }

const MAX_POINTS = 60;
const MAX_SERIES = 6;

export function parseChart(text: string): ChartSpec | null {
  let raw: unknown;
  try { raw = JSON.parse(text); } catch { return null; }
  if (!raw || typeof raw !== "object") return null;
  const o = raw as Record<string, unknown>;
  const type = o.type === "line" || o.type === "pie" ? o.type : o.type === "bar" || o.type === undefined ? "bar" : null;
  if (!type) return null;
  const title = typeof o.title === "string" ? o.title.slice(0, 120) : "";
  let labels: string[] = [];
  let series: { name: string; values: number[] }[] = [];
  const nums = (v: unknown) => Array.isArray(v) && v.length > 0 && v.every((n) => typeof n === "number" && Number.isFinite(n)) ? (v as number[]) : null;
  if (Array.isArray(o.labels) && o.labels.every((l) => typeof l === "string" || typeof l === "number")) labels = o.labels.map(String);
  if (Array.isArray(o.series)) {
    for (const s of o.series) {
      const values = nums((s as { values?: unknown })?.values);
      if (!values) return null;
      const name = (s as { name?: unknown }).name;
      series.push({ name: typeof name === "string" ? name.slice(0, 40) : "", values });
    }
  } else if (nums(o.values)) {
    series = [{ name: "", values: o.values as number[] }];
  } else if (Array.isArray(o.data) && o.data.every((d) => Array.isArray(d) && d.length === 2 && typeof d[1] === "number" && Number.isFinite(d[1]))) {
    labels = (o.data as [unknown, number][]).map((d) => String(d[0]));
    series = [{ name: "", values: (o.data as [unknown, number][]).map((d) => d[1]) }];
  } else return null;
  const width = series[0]?.values.length ?? 0;
  if (!width || width > MAX_POINTS || series.length > MAX_SERIES || series.some((s) => s.values.length !== width)) return null;
  if (labels.length !== width) labels = Array.from({ length: width }, (_, i) => String(i + 1));
  if (type === "pie" && (series.length !== 1 || series[0].values.some((v) => v < 0))) return null;
  return { type, title, labels: labels.map((l) => l.slice(0, 24)), series };
}

const W = 560, H = 260, L = 44, R = 12, T = 14, B = 42;
const colour = (i: number) => `hsl(${(210 + i * 67) % 360} 65% 58%)`;

/** A round top for the axis: 0, then steps of 1, 2 or 5 times a power of ten. */
function niceMax(max: number): { top: number; step: number } {
  if (max <= 0) return { top: 1, step: 0.25 };
  const power = Math.pow(10, Math.floor(Math.log10(max)));
  const f = max / power;
  const unit = f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10;
  const top = unit * power;
  return { top, step: top / 4 };
}
const fmt = (n: number) => Math.abs(n) >= 1000 ? `${+(n / 1000).toFixed(1)}k` : `${+n.toFixed(2)}`;

export function Chart({ spec }: { spec: ChartSpec }) {
  const body = useMemo(() => {
    if (spec.type === "pie") {
      const values = spec.series[0].values;
      const total = values.reduce((a, b) => a + b, 0) || 1;
      let angle = -Math.PI / 2;
      const cx = 130, cy = 130, r = 100;
      return <>
        {values.map((v, i) => {
          const a2 = angle + (v / total) * Math.PI * 2;
          const large = a2 - angle > Math.PI ? 1 : 0;
          const d = values.length === 1
            ? `M${cx - r},${cy}a${r},${r} 0 1,0 ${2 * r},0a${r},${r} 0 1,0 ${-2 * r},0`
            : `M${cx},${cy}L${cx + r * Math.cos(angle)},${cy + r * Math.sin(angle)}A${r},${r} 0 ${large} 1 ${cx + r * Math.cos(a2)},${cy + r * Math.sin(a2)}Z`;
          angle = a2;
          return <path key={i} d={d} fill={colour(i)} stroke="var(--bg, #1e1e24)" strokeWidth="1.5"><title>{`${spec.labels[i]}: ${fmt(v)} (${Math.round((v / total) * 100)}%)`}</title></path>;
        })}
        {values.map((v, i) => <g key={`l${i}`} transform={`translate(270 ${20 + i * 20})`}><rect width="11" height="11" rx="2" fill={colour(i)} /><text x="17" y="10" className="chart-text">{`${spec.labels[i]} · ${Math.round((v / total) * 100)}%`}</text></g>)}
      </>;
    }
    const all = spec.series.flatMap((s) => s.values);
    const { top, step } = niceMax(Math.max(0, ...all));
    const min = Math.min(0, ...all);
    const lo = min < 0 ? -niceMax(-min).top : 0;
    const y = (v: number) => T + (H - T - B) * (1 - (v - lo) / (top - lo));
    const n = spec.labels.length;
    const inner = W - L - R;
    const slot = inner / n;
    const ticks: number[] = [];
    for (let t = lo; t <= top + 1e-9; t += step) ticks.push(t);
    const every = Math.ceil(n / 12);
    return <>
      {ticks.map((t) => <g key={t}><line x1={L} x2={W - R} y1={y(t)} y2={y(t)} className="chart-grid" /><text x={L - 6} y={y(t) + 4} textAnchor="end" className="chart-text">{fmt(t)}</text></g>)}
      {spec.labels.map((label, i) => i % every === 0 ? <text key={i} x={L + slot * (i + 0.5)} y={H - B + 16} textAnchor="middle" className="chart-text">{label}</text> : null)}
      {spec.type === "bar"
        ? spec.series.map((s, si) => s.values.map((v, i) => {
            const bw = (slot * 0.7) / spec.series.length;
            const x = L + slot * i + slot * 0.15 + bw * si;
            return <rect key={`${si}-${i}`} x={x} y={Math.min(y(v), y(0))} width={bw} height={Math.abs(y(v) - y(0))} rx="2" fill={colour(si)}><title>{`${spec.labels[i]}: ${fmt(v)}`}</title></rect>;
          }))
        : spec.series.map((s, si) => <g key={si}>
            <polyline fill="none" stroke={colour(si)} strokeWidth="2.5" strokeLinejoin="round" points={s.values.map((v, i) => `${L + slot * (i + 0.5)},${y(v)}`).join(" ")} />
            {s.values.map((v, i) => <circle key={i} cx={L + slot * (i + 0.5)} cy={y(v)} r="3.5" fill={colour(si)}><title>{`${spec.labels[i]}: ${fmt(v)}`}</title></circle>)}
          </g>)}
      {spec.series.length > 1 ? spec.series.map((s, si) => <g key={`k${si}`} transform={`translate(${L + si * 110} ${H - 14})`}><rect width="10" height="10" rx="2" fill={colour(si)} /><text x="15" y="9" className="chart-text">{s.name || `Series ${si + 1}`}</text></g>) : null}
    </>;
  }, [spec]);
  const summary = `${spec.title || "Chart"}: ${spec.labels.map((l, i) => `${l} ${spec.series.map((s) => fmt(s.values[i])).join(", ")}`).join("; ")}`;
  return <figure className="chart" data-test="chart">
    {spec.title ? <figcaption>{spec.title}</figcaption> : null}
    <svg viewBox={`0 0 ${W} ${spec.type === "pie" ? Math.max(260, 30 + spec.labels.length * 20) : H}`} role="img" aria-label={summary}>{body}</svg>
  </figure>;
}
