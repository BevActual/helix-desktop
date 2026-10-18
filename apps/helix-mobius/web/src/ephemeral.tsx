import { useState } from "react";

/** Ephemeral UI: a reply can ask for a small piece of interface (a fenced block labelled `ui` holding JSON) that exists for this moment:
 * choices to pick from, a short form, a row of figures. The model writes a description; this draws it from a fixed set of components
 * (never markup, never script) and what the person does with it goes back into the chat as an ordinary message. A block that does not
 * parse stays a code block. Only the newest reply is live; older ones are read-only records of what was offered. */
export type UiSpec =
  | { type: "choices"; question: string; options: string[] }
  | { type: "form"; title: string; submit: string; fields: Field[] }
  | { type: "stats"; title: string; items: { label: string; value: string }[] }
  | { type: "checklist"; title: string; items: { label: string; done: boolean }[] };
export type Field = { name: string; label: string; kind: "text" | "number" | "select" | "toggle"; options: string[]; initial: string };

const text = (v: unknown, max: number) => (typeof v === "string" || typeof v === "number" ? String(v).slice(0, max) : "");

export function parseUi(source: string): UiSpec | null {
  let raw: unknown;
  try { raw = JSON.parse(source); } catch { return null; }
  if (!raw || typeof raw !== "object") return null;
  const o = raw as Record<string, unknown>;
  if (o.type === "choices") {
    const options = Array.isArray(o.options) ? o.options.map((x) => text(x, 80)).filter(Boolean) : [];
    if (options.length < 2 || options.length > 8) return null;
    return { type: "choices", question: text(o.question, 200), options };
  }
  if (o.type === "form") {
    if (!Array.isArray(o.fields) || o.fields.length === 0 || o.fields.length > 8) return null;
    const fields: Field[] = [];
    for (const f of o.fields) {
      const r = (f ?? {}) as Record<string, unknown>;
      const label = text(r.label ?? r.name, 80);
      if (!label) return null;
      const kind = r.type === "number" || r.type === "select" || r.type === "toggle" ? r.type : "text";
      const options = Array.isArray(r.options) ? r.options.map((x) => text(x, 60)).filter(Boolean).slice(0, 12) : [];
      if (kind === "select" && options.length < 2) return null;
      fields.push({ name: text(r.name ?? label, 40), label, kind, options, initial: text(r.default, 120) || (kind === "select" ? options[0] : "") });
    }
    return { type: "form", title: text(o.title, 120), submit: text(o.submit, 30) || "Send", fields };
  }
  if (o.type === "checklist") {
    if (!Array.isArray(o.items) || o.items.length === 0 || o.items.length > 20) return null;
    const items = o.items.map((i) => typeof i === "string" || typeof i === "number"
      ? { label: text(i, 120), done: false }
      : { label: text((i as Record<string, unknown>)?.label, 120), done: (i as Record<string, unknown>)?.done === true });
    if (items.some((i) => !i.label)) return null;
    return { type: "checklist", title: text(o.title, 120), items };
  }
  if (o.type === "stats") {
    if (!Array.isArray(o.items) || o.items.length === 0 || o.items.length > 8) return null;
    const items = o.items.map((i) => ({ label: text((i as Record<string, unknown>)?.label, 40), value: text((i as Record<string, unknown>)?.value, 40) }));
    if (items.some((i) => !i.label || !i.value)) return null;
    return { type: "stats", title: text(o.title, 120), items };
  }
  return null;
}

/** Ticks survive a reload (kept in this browser only, keyed by the list's own text); losing them costs nothing. */
function ticksKey(spec: { title: string; items: { label: string }[] }) {
  return "mobius-ticks:" + [spec.title, ...spec.items.map((i) => i.label)].join("|").slice(0, 400);
}
function loadTicks(spec: Extract<UiSpec, { type: "checklist" }>): boolean[] {
  try {
    const saved = JSON.parse(localStorage.getItem(ticksKey(spec)) ?? "null");
    if (Array.isArray(saved) && saved.length === spec.items.length) return saved.map(Boolean);
  } catch { /* none saved */ }
  return spec.items.map((i) => i.done);
}

export function Ephemeral({ spec, live, onSend }: { spec: UiSpec; live: boolean; onSend: (text: string) => void }) {
  const [chosen, setChosen] = useState("");
  const [values, setValues] = useState<Record<string, string>>(() => spec.type === "form" ? Object.fromEntries(spec.fields.map((f) => [f.label, f.initial])) : {});
  const [ticks, setTicks] = useState<boolean[]>(() => spec.type === "checklist" ? loadTicks(spec) : []);
  const done = chosen !== "";
  if (spec.type === "checklist") {
    const toggle = (i: number) => {
      const next = ticks.map((t, j) => (j === i ? !t : t));
      setTicks(next);
      try { localStorage.setItem(ticksKey(spec), JSON.stringify(next)); } catch { /* not kept */ }
    };
    const count = ticks.filter(Boolean).length;
    return <section className="eph eph-checklist" data-test="eph-checklist" aria-label={spec.title || "Checklist"}>
      <h4>{spec.title || "Checklist"} <span className="eph-count">{count} of {spec.items.length}</span></h4>
      <div className="eph-progress" aria-hidden="true"><i style={{ width: `${(count / spec.items.length) * 100}%` }} /></div>
      <ul>{spec.items.map((item, i) => <li key={i}><label className={ticks[i] ? "ticked" : ""}>
        <input type="checkbox" data-test={`eph-tick-${i}`} checked={!!ticks[i]} onChange={() => toggle(i)} /><span>{item.label}</span></label></li>)}</ul>
      {live && !done ? <div className="eph-actions"><button type="button" data-test="eph-progress-send" onClick={() => {
        setChosen("sent");
        const donePart = spec.items.filter((_, i) => ticks[i]).map((i) => i.label);
        const left = spec.items.filter((_, i) => !ticks[i]).map((i) => i.label);
        onSend(`${spec.title || "Checklist"}\nDone: ${donePart.join("; ") || "nothing yet"}\nLeft: ${left.join("; ") || "nothing"}`);
      }}>Tell Mobius where I am</button></div> : null}
    </section>;
  }
  if (spec.type === "stats") {
    return <section className="eph eph-stats" data-test="eph-stats" aria-label={spec.title || "Figures"}>
      {spec.title ? <h4>{spec.title}</h4> : null}
      <div className="eph-cards">{spec.items.map((item, i) => <div key={i} className="eph-card"><span>{item.label}</span><strong>{item.value}</strong></div>)}</div>
    </section>;
  }
  if (spec.type === "choices") {
    return <section className="eph eph-choices" data-test="eph-choices" role="group" aria-label={spec.question || "Choose one"}>
      {spec.question ? <p className="eph-question">{spec.question}</p> : null}
      <div className="eph-options">{spec.options.map((option, i) => <button key={i} type="button" data-test={`eph-option-${i}`} className={chosen === option ? "chosen" : ""}
        disabled={!live || done} onClick={() => { setChosen(option); onSend(option); }}>{option}</button>)}</div>
    </section>;
  }
  const send = () => {
    const lines = spec.fields.map((f) => `- ${f.label}: ${values[f.label] || (f.kind === "toggle" ? "no" : "(blank)")}`);
    setChosen("sent");
    onSend(`${spec.title || "Details"}\n${lines.join("\n")}`);
  };
  return <form className="eph eph-form" data-test="eph-form" onSubmit={(e) => { e.preventDefault(); if (live && !done) send(); }}>
    {spec.title ? <h4>{spec.title}</h4> : null}
    {spec.fields.map((f, i) => <label key={i} className={`eph-field${f.kind === "toggle" ? " eph-toggle" : ""}`}>
      <span>{f.label}</span>
      {f.kind === "select"
        ? <select disabled={!live || done} value={values[f.label]} onChange={(e) => setValues({ ...values, [f.label]: e.target.value })}>{f.options.map((o) => <option key={o}>{o}</option>)}</select>
        : f.kind === "toggle"
          ? <input type="checkbox" className="switch" disabled={!live || done} checked={values[f.label] === "yes"} onChange={(e) => setValues({ ...values, [f.label]: e.target.checked ? "yes" : "no" })} />
          : <input type={f.kind === "number" ? "number" : "text"} data-test={`eph-field-${i}`} disabled={!live || done} value={values[f.label]} onChange={(e) => setValues({ ...values, [f.label]: e.target.value })} />}
    </label>)}
    <div className="eph-actions"><button type="submit" className="primary" data-test="eph-submit" disabled={!live || done}>{done ? "Sent" : spec.submit}</button></div>
  </form>;
}
