import { useState } from "react";
import { Picker, type Option } from "./settingsui";

export interface RouteOptions { prefer: string; fallbacks: string[]; private_only: boolean; }

const PREFER: { id: string; label: string; note: string }[] = [
  { id: "", label: "Best match", note: "OpenRouter balances cost, speed and reliability" },
  { id: "price", label: "Lowest price", note: "The cheapest provider of the model" },
  { id: "speed", label: "Fastest", note: "The provider that writes the most tokens a second" },
];

/** How one job's cloud requests are routed: which provider of the model to prefer, which models to fall back to, and whether the provider
 * may keep the prompt. Saved as it changes. */
export function RouteEditor({ id, primary, route, choices, onChange }: {
  id: string; primary: string; route: RouteOptions; choices: { id: string; name: string }[]; onChange: (next: RouteOptions) => void;
}) {
  const [open, setOpen] = useState(false);
  const name = (model: string) => choices.find((c) => c.id === model)?.name ?? model;
  const addable: Option[] = choices
    .filter((c) => c.id !== primary && !route.fallbacks.includes(c.id))
    .map((c) => ({ key: c.id, group: "", title: c.name, detail: c.name === c.id ? "" : c.id, backend: "openrouter", model: c.id, test: `job-fb-add-${id}-${c.id}` }));
  const summary = [PREFER.find((p) => p.id === route.prefer)?.label ?? "Best match", route.fallbacks.length ? `${route.fallbacks.length} fallback${route.fallbacks.length === 1 ? "" : "s"}` : "", route.private_only ? "private providers only" : "any provider"].filter(Boolean).join(" · ");
  return (
    <div className="route-edit" data-test={`job-route-${id}`}>
      <button type="button" className="set-link" aria-expanded={open} data-test={`job-route-toggle-${id}`} onClick={() => setOpen(!open)}>
        {open ? "Hide cloud routing" : "Cloud routing"} <span className="set-hint">{summary}</span>
      </button>
      {open ? (
        <div className="route-body">
          <div className="route-field">
            <div className="route-label">Prefer</div>
            <div className="route-seg" role="radiogroup" aria-label="Provider preference">
              {PREFER.map((p) => (
                <button key={p.id || "best"} type="button" role="radio" aria-checked={route.prefer === p.id} className={route.prefer === p.id ? "on" : ""} title={p.note}
                  data-test={`job-prefer-${id}-${p.id || "best"}`} onClick={() => onChange({ ...route, prefer: p.id })}>{p.label}</button>
              ))}
            </div>
          </div>
          <div className="route-field">
            <div className="route-label">If it fails, try</div>
            <div className="route-chips">
              {route.fallbacks.map((m, i) => (
                <span key={m} className="route-chip" data-test={`job-fb-${id}-${i}`}>
                  {name(m)}
                  <button type="button" aria-label={`Remove ${name(m)}`} data-test={`job-fb-remove-${id}-${i}`} onClick={() => onChange({ ...route, fallbacks: route.fallbacks.filter((f) => f !== m) })}>×</button>
                </span>
              ))}
              {route.fallbacks.length < 3 && addable.length ? (
                <Picker id={`fb-${id}`} label="Add a fallback" options={addable} current="" onPick={(o) => onChange({ ...route, fallbacks: [...route.fallbacks, o.model] })} />
              ) : null}
              {!route.fallbacks.length && !addable.length ? <span className="set-hint">Add models to your menu in Cloud models to choose fallbacks.</span> : null}
            </div>
            <div className="set-hint">Used when the first model errors, is rate limited, or the conversation is too long for it. You pay for the model that answers, and the reply says which one it was.</div>
          </div>
          <label className="sheet-check set-check route-private">
            <input type="checkbox" data-test={`job-private-${id}`} checked={route.private_only} onChange={(e) => onChange({ ...route, private_only: e.target.checked })} />
            <span><strong>Only providers that promise not to keep or train on my prompts</strong><br /><span className="set-hint">On by default. Some models, mostly free ones, are only offered by providers that keep prompts, and will not work while this is on.</span></span>
          </label>
        </div>
      ) : null}
    </div>
  );
}
