import { useMemo } from "react";

/** A flow diagram from a few lines of text: a fenced block labelled `diagram`, one edge per line (`Build -> Test : on push`), nodes named
 * by their words or declared as `id[Label]`; a first line `LR` lays it out left to right. Mobius lays it out in layers and draws an SVG; the
 * model only writes the text. A block with no edges, or too large, stays a code block. */
export interface Diagram { horizontal: boolean; nodes: { id: string; label: string; layer: number; slot: number }[]; edges: { from: number; to: number; label: string }[]; back: { from: number; to: number; label: string }[] }

const MAX_NODES = 30;
const MAX_EDGES = 60;

export function parseDiagram(source: string): Diagram | null {
  const lines = source.split("\n").map((l) => l.trim()).filter(Boolean);
  let horizontal = false;
  if (lines[0] && /^(LR|TD|TB)$/i.test(lines[0])) { horizontal = lines[0].toUpperCase() === "LR"; lines.shift(); }
  const index = new Map<string, number>();
  const labels: string[] = [];
  const node = (raw: string): number | null => {
    const m = /^([^[\]]+?)\s*\[(.+)\]$/.exec(raw.trim());
    const id = (m ? m[1] : raw).trim();
    const label = (m ? m[2] : raw).trim().slice(0, 40);
    if (!id || id.length > 60) return null;
    if (!index.has(id)) { index.set(id, labels.length); labels.push(label); } else if (m) labels[index.get(id)!] = label;
    return index.get(id)!;
  };
  const edges: { from: number; to: number; label: string }[] = [];
  for (const line of lines) {
    const m = /^(.+?)\s*(?:-+>|→)\s*(.+?)(?:\s*:\s*(.+))?$/.exec(line);
    if (!m) { if (/^[^[\]]+\[.+\]$/.test(line)) node(line); else return null; continue; }
    const a = node(m[1]), b = node(m[2]);
    if (a === null || b === null) return null;
    edges.push({ from: a, to: b, label: (m[3] ?? "").trim().slice(0, 30) });
  }
  if (!edges.length || edges.length > MAX_EDGES || labels.length > MAX_NODES) return null;
  // Layers: the longest path from a start, ignoring an edge that would close a loop.
  const layer = labels.map(() => 0);
  const forward: typeof edges = [];
  const back: typeof edges = [];
  const reach = (from: number, to: number, seen = new Set<number>()): boolean => from === to || (!seen.has(from) && (seen.add(from), forward.some((e) => e.from === from && reach(e.to, to, seen))));
  for (const e of edges) if (e.from !== e.to) (reach(e.to, e.from) ? back : forward).push(e);
  for (let pass = 0; pass < labels.length; pass++) for (const e of forward) layer[e.to] = Math.max(layer[e.to], layer[e.from] + 1);
  const count: number[] = [];
  const nodes = labels.map((label, i) => ({ id: String(i), label, layer: layer[i], slot: (count[layer[i]] = (count[layer[i]] ?? 0) + 1) - 1 }));
  return { horizontal, nodes, edges: forward, back };
}

const BOX_W = 128, BOX_H = 38, GAP_X = 46, GAP_Y = 54;

export function FlowDiagram({ diagram }: { diagram: Diagram }) {
  const view = useMemo(() => {
    const layers = Math.max(...diagram.nodes.map((n) => n.layer)) + 1;
    const widest = Math.max(...Array.from({ length: layers }, (_, l) => diagram.nodes.filter((n) => n.layer === l).length));
    const h = diagram.horizontal;
    const along = (BOX_W + GAP_X), across = (BOX_H + GAP_Y);
    const pos = diagram.nodes.map((n) => {
      const inLayer = diagram.nodes.filter((o) => o.layer === n.layer).length;
      const offset = (widest - inLayer) / 2;
      return h
        ? { x: 10 + n.layer * along, y: 10 + (n.slot + offset) * across }
        : { x: 10 + (n.slot + offset) * along, y: 10 + n.layer * across };
    });
    const loops = diagram.back.length ? 130 : 0;
    const width = (h ? 20 + layers * along - GAP_X : 20 + widest * along - GAP_X) + (h ? 0 : loops);
    const height = (h ? 20 + widest * across - GAP_Y : 20 + layers * across - GAP_Y) + (h ? loops : 0);
    return { pos, width, height };
  }, [diagram]);
  const end = (i: number, towards: { x: number; y: number }) => {
    const p = view.pos[i];
    const cx = p.x + BOX_W / 2, cy = p.y + BOX_H / 2;
    const dx = towards.x + BOX_W / 2 - cx, dy = towards.y + BOX_H / 2 - cy;
    const scale = Math.min(dx ? (BOX_W / 2) / Math.abs(dx) : Infinity, dy ? (BOX_H / 2) / Math.abs(dy) : Infinity);
    return { x: cx + dx * scale, y: cy + dy * scale };
  };
  const summary = [...diagram.edges, ...diagram.back].map((e) => `${diagram.nodes[e.from].label} to ${diagram.nodes[e.to].label}${e.label ? ` (${e.label})` : ""}`).join("; ");
  return <figure className="chart diagram" data-test="diagram">
    <svg viewBox={`0 0 ${view.width} ${view.height}`} role="img" aria-label={`Diagram: ${summary}`} style={{ maxWidth: view.width }}>
      <defs><marker id="arrowhead" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" className="diagram-arrow" /></marker></defs>
      {diagram.edges.map((e, i) => {
        const a = end(e.from, view.pos[e.to]), b = end(e.to, view.pos[e.from]);
        return <g key={i}><line x1={a.x} y1={a.y} x2={b.x} y2={b.y} className="diagram-edge" markerEnd="url(#arrowhead)" />
          {e.label ? <text x={(a.x + b.x) / 2} y={(a.y + b.y) / 2 - 4} textAnchor="middle" className="chart-text">{e.label}</text> : null}</g>;
      })}
      {diagram.back.map((e, i) => {
        // A line that goes back to an earlier step leaves the side of its box and curves round the others.
        const a = view.pos[e.from], b = view.pos[e.to], h = diagram.horizontal;
        const x1 = h ? a.x + BOX_W / 2 : a.x + BOX_W, y1 = h ? a.y + BOX_H : a.y + BOX_H / 2;
        const x2 = h ? b.x + BOX_W / 2 : b.x + BOX_W, y2 = h ? b.y + BOX_H : b.y + BOX_H / 2;
        const bend = 56 + (i % 3) * 8;
        const d = h ? `M${x1},${y1} C${x1},${y1 + bend} ${x2},${y2 + bend} ${x2},${y2}` : `M${x1},${y1} C${x1 + bend},${y1} ${x2 + bend},${y2} ${x2},${y2}`;
        return <g key={`b${i}`}><path d={d} fill="none" className="diagram-edge diagram-back" markerEnd="url(#arrowhead)" />
          {e.label ? <text x={h ? (x1 + x2) / 2 : Math.max(x1, x2) + bend * 0.75 + 4} y={h ? Math.max(y1, y2) + bend * 0.75 + 12 : (y1 + y2) / 2} textAnchor={h ? "middle" : "start"} className="chart-text">{e.label}</text> : null}</g>;
      })}
      {diagram.nodes.map((n, i) => <g key={n.id}><rect x={view.pos[i].x} y={view.pos[i].y} width={BOX_W} height={BOX_H} rx="9" className="diagram-node" />
        <text x={view.pos[i].x + BOX_W / 2} y={view.pos[i].y + BOX_H / 2 + 4} textAnchor="middle" className="diagram-label">{n.label.length > 18 ? n.label.slice(0, 17) + "…" : n.label}<title>{n.label}</title></text></g>)}
    </svg>
  </figure>;
}
