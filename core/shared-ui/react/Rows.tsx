// The Helix settings row family. The markup and class names are the same as the hand-written pages use, so both look identical and the
// nested tests find the same controls by `data-test`. Text only ever goes through React children (never as HTML).
import { Fragment, useEffect, useRef, useState } from "react";
import type { Choice } from "./bindings/Choice";
import type { RowView } from "./bindings/RowView";

export type Change =
  | { kind: "switch"; value: boolean }
  | { kind: "number"; value: number }
  | { kind: "choice"; value: string }
  | { kind: "text"; value: string };

export function Switch({ id, on, onChange }: { id: string; on: boolean; onChange: (on: boolean) => void }) {
  return <input type="checkbox" className="switch" checked={on} data-test={`set-${id}`} onChange={(event) => onChange(event.target.checked)} />;
}

export function Slider({ id, min, max, integer, value, onCommit }: { id: string; min: number; max: number; integer: boolean; value: number; onCommit: (value: number) => void }) {
  const [draft, setDraft] = useState(value);
  const [typed, setTyped] = useState<string | null>(null);
  useEffect(() => setDraft(value), [value]);
  const format = (v: number) => (integer ? String(Math.round(v)) : v.toFixed(2));
  const step = integer ? 1 : (max - min) / 100;
  const percent = ((draft - min) / (max - min)) * 100;
  const entered = useRef(false);
  const commitTyped = () => {
    const text = typed ?? "";
    setTyped(null);
    const number = Number(text);
    if (text === "" || !Number.isFinite(number) || number < min || number > max) return;
    setDraft(number);
    onCommit(number);
  };
  return (
    <div className="slider-wrap">
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={draft}
        data-test={`set-${id}`}
        style={{ ["--pct" as string]: `${percent}%` }}
        onChange={(event) => setDraft(Number(event.target.value))}
        onPointerUp={() => onCommit(draft)}
        onKeyUp={(event) => { if (event.key.startsWith("Arrow") || event.key === "Home" || event.key === "End") onCommit(draft); }}
      />
      <input
        type="number"
        className="slider-input"
        min={min}
        max={max}
        step={step}
        value={typed ?? format(draft)}
        data-test={`set-${id}-number`}
        onFocus={(event) => { entered.current = true; event.target.select(); }}
        onMouseUp={(event) => { if (entered.current) { event.preventDefault(); entered.current = false; } }}
        onChange={(event) => setTyped(event.target.value)}
        onBlur={commitTyped}
        onKeyDown={(event) => { if (event.key === "Enter") event.currentTarget.blur(); }}
      />
    </div>
  );
}

export function Segments({ id, options, current, onChange }: { id: string; options: Choice[]; current: string; onChange: (value: string) => void }) {
  return (
    <div className="segments">
      {options.map((option) => (
        <button key={option.value} className={`segment${option.value === current ? " current" : ""}`} data-test={`set-${id}-${option.value}`} onClick={() => onChange(option.value)}>
          {option.label}
        </button>
      ))}
    </div>
  );
}

/** A choice with too many options for a row of buttons (time zones): the current one on a button that opens a short list with a filter. */
export function ChoiceList({ id, options, current, onChange }: { id: string; options: Choice[]; current: string; onChange: (value: string) => void }) {
  const [open, setOpen] = useState(false);
  const [needle, setNeedle] = useState("");
  const shown = options.filter((o) => !needle || `${o.label} ${o.value}`.toLowerCase().includes(needle.toLowerCase())).slice(0, 80);
  const label = options.find((o) => o.value === current)?.label ?? (current || "Choose…");
  return (
    <div className="choice-list">
      <button type="button" className="choice-current" aria-expanded={open} data-test={`set-${id}`} onClick={() => setOpen(!open)}>{label}<span aria-hidden="true"> ▾</span></button>
      {open ? (
        <div className="choice-pop" role="listbox" aria-label={id}>
          <input type="search" autoFocus placeholder="Type to find" spellCheck={false} data-test={`set-${id}-filter`} value={needle} onChange={(e) => setNeedle(e.target.value)} onKeyDown={(e) => { if (e.key === "Escape") setOpen(false); }} />
          <ul>
            {shown.map((o) => (
              <li key={o.value}>
                <button type="button" role="option" aria-selected={o.value === current} className={o.value === current ? "current" : ""} data-test={`set-${id}-${o.value}`} onClick={() => { setOpen(false); setNeedle(""); if (o.value !== current) onChange(o.value); }}>{o.label}</button>
              </li>
            ))}
            {shown.length === 0 ? <li className="choice-none">Nothing matches.</li> : null}
          </ul>
        </div>
      ) : null}
    </div>
  );
}

export function Row({ row, onChange }: { row: RowView; onChange: (id: string, change: Change) => void }) {
  const control = row.control;
  return (
    <div className="setting-row">
      <div className="setting-text">
        <div className="setting-title">{row.title}</div>
        {row.description ? <div className="setting-about">{row.description}</div> : null}
      </div>
      {control.kind === "switch" && <Switch id={row.id} on={control.on} onChange={(value) => onChange(row.id, { kind: "switch", value })} />}
      {control.kind === "slider" && (
        <Slider id={row.id} min={control.min} max={control.max} integer={control.integer} value={control.value} onCommit={(value) => onChange(row.id, { kind: "number", value })} />
      )}
      {control.kind === "choice" && (control.options.length > 6 || control.current === ""
        ? <ChoiceList id={row.id} options={control.options} current={control.current} onChange={(value) => onChange(row.id, { kind: "choice", value })} />
        : <Segments id={row.id} options={control.options} current={control.current} onChange={(value) => onChange(row.id, { kind: "choice", value })} />)}
      {control.kind === "text" && <div className="setting-value">{control.value}</div>}
    </div>
  );
}

/** Rows under their group headings, each group in one rounded card. */
export function RowGroups({ rows, onChange }: { rows: RowView[]; onChange: (id: string, change: Change) => void }) {
  const groups: { name: string; rows: RowView[] }[] = [];
  for (const row of rows) {
    const last = groups[groups.length - 1];
    if (last && last.name === row.group) last.rows.push(row);
    else groups.push({ name: row.group, rows: [row] });
  }
  return (
    <>
      {groups.map((group) => (
        // A fragment, not a wrapper: the page's spacing rules are written for heading and card as siblings.
        <Fragment key={group.name}>
          <h4>{group.name}</h4>
          <div className="rows-card">
            {group.rows.map((row) => (
              <Row key={row.id} row={row} onChange={onChange} />
            ))}
          </div>
        </Fragment>
      ))}
    </>
  );
}
