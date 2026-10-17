import { useState } from "react";

/** Ephemeral UI: a reply can ask for a small piece of interface (a fenced block labelled `ui` holding JSON) that exists for this moment:
 * choices to pick from, a short form, a row of figures. The model writes a description; this draws it from a fixed set of components
 * (never markup, never script) and what the person does with it goes back into the chat as an ordinary message. A block that does not
 * parse stays a code block. Only the newest reply is live; older ones are read-only records of what was offered. */
export type UiSpec =
  | { type: "choices"; question: string; options: string[] }
  | { type: "form"; title: string; submit: string; fields: Field[] }
  | { type: "stats"; title: string; items: { label: string; value: string }[] };
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
  if (o.type === "stats") {
    if (!Array.isArray(o.items) || o.items.length === 0 || o.items.length > 8) return null;
    const items = o.items.map((i) => ({ label: text((i as Record<string, unknown>)?.label, 40), value: text((i as Record<string, unknown>)?.value, 40) }));
    if (items.some((i) => !i.label || !i.value)) return null;
    return { type: "stats", title: text(o.title, 120), items };
  }
  return null;
}

export function Ephemeral({ spec, live, onSend }: { spec: UiSpec; live: boolean; onSend: (text: string) => void }) {
  const [chosen, setChosen] = useState("");
  const [values, setValues] = useState<Record<string, string>>(() => spec.type === "form" ? Object.fromEntries(spec.fields.map((f) => [f.label, f.initial])) : {});
  const done = chosen !== "";
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
