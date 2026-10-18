import { useState } from "react";
import { ageLabel, contextLabel, matches, priceLine, sorted, validId, type CloudFilter, type CloudModel, type CloudSort } from "./cloudlist";
import { Icon, Picker, type Option } from "./settingsui";

export interface CloudView { models: CloudModel[]; fetched: number; error: string; pins: string[]; }

type Task = "chat" | "act" | "code" | "summarize" | "vision";
interface Profiles { [task: string]: { backend: string; model: string } }

const FILTERS: { id: CloudFilter; label: string }[] = [
  { id: "all", label: "All" },
  { id: "pinned", label: "In my menu" },
  { id: "free", label: "Free" },
  { id: "vision", label: "Sees pictures" },
  { id: "reasoning", label: "Thinks first" },
  { id: "tools", label: "Uses tools" },
  { id: "long", label: "Long documents" },
];
const SHOWN = 40;

/** What a cloud model can be used for here. Act never uses one, and Pictures needs one that reads pictures. */
const USES: { task: Task; title: string; needs?: (m: CloudModel) => boolean }[] = [
  { task: "chat", title: "Chat" },
  { task: "code", title: "Code" },
  { task: "vision", title: "Pictures", needs: (m) => m.vision },
  { task: "summarize", title: "Summaries" },
];

/** OpenRouter's catalog, narrowed and sorted. Pinning a model puts it in the chat's model menu; the menu holds only what is pinned. */
export function CloudModels({ view, loading, hasKey, profiles, now, onRefresh, onPin, onAssign, onProviders }: {
  view: CloudView | null; loading: boolean; hasKey: boolean | null; profiles: Profiles; now: number;
  onRefresh: () => void; onPin: (id: string, on: boolean) => void; onAssign: (task: Task, backend: string, model: string) => void; onProviders: () => void;
}) {
  const [needle, setNeedle] = useState("");
  const [filter, setFilter] = useState<CloudFilter>("all");
  const [sort, setSort] = useState<CloudSort>("newest");
  const [more, setMore] = useState(false);
  const [custom, setCustom] = useState("");
  const pins = view?.pins ?? [];
  const all = view?.models ?? [];
  const shown = sorted(all.filter((m) => matches(m, needle, filter, pins)), sort);
  const visible = more ? shown : shown.slice(0, SHOWN);
  const usedFor = (id: string) => USES.filter((u) => profiles[u.task]?.backend === "openrouter" && profiles[u.task]?.model === id);
  const customOk = validId(custom.trim());
  // A pinned model the catalog does not list (offline, retired, or added by id) still gets a row, so it can be taken out of the menu.
  const unlisted = pins.filter((id) => !all.some((m) => m.id === id));

  return (
    <>
      <div className="cloud-bar">
        <span className="cloud-age" data-test="cloud-age">{view ? ageLabel(view.fetched, now) : "Loading…"}</span>
        <button type="button" data-test="cloud-refresh" disabled={loading} onClick={onRefresh}>{loading ? "Fetching…" : "Refresh the list"}</button>
      </div>
      {hasKey === false ? (
        <div className="set-offer" data-test="cloud-nokey">
          <div className="set-offer-text"><div><strong>No OpenRouter key yet</strong></div><span>You can browse the list now. To use a model, add your key in Providers.</span></div>
          <button type="button" className="primary" data-test="cloud-to-providers" onClick={onProviders}>Add a key</button>
        </div>
      ) : null}
      {view?.error ? <p className="lib-error" data-test="cloud-error">{view.error}</p> : null}
      <div className="lib-tools">
        <label className="route-search" htmlFor="cloud-search">
          <Icon name="search" />
          <input id="cloud-search" data-test="cloud-search" type="search" placeholder="Search by name, maker or what it does" autoComplete="off" value={needle} onChange={(e) => { setNeedle(e.target.value); setMore(false); }} />
        </label>
        <label className="lib-sort">Sort
          <select data-test="cloud-sort" value={sort} onChange={(e) => setSort(e.target.value as CloudSort)}>
            <option value="newest">Newest</option>
            <option value="cheapest">Cheapest</option>
            <option value="context">Longest documents</option>
            <option value="name">Name</option>
          </select>
        </label>
      </div>
      <div className="lib-chips" role="group" aria-label="Filter cloud models">
        {FILTERS.map((f) => (
          <button key={f.id} type="button" className={`lib-chip${filter === f.id ? " on" : ""}`} aria-pressed={filter === f.id} data-test={`cloud-filter-${f.id}`} onClick={() => { setFilter(f.id); setMore(false); }}>
            {f.label}{f.id === "pinned" && pins.length ? ` · ${pins.length}` : ""}
          </button>
        ))}
      </div>
      <div className="lib-grid" data-test="cloud-list">
        {(filter === "all" || filter === "pinned" ? unlisted : []).map((id) => (
          <article key={id} className="lib-card cloud-card is-installed" data-test={`cloud-card-${id}`}>
            <header><div><div className="lib-title">{id.split("/").pop()}</div><div className="lib-sub">{id} · not in the list we have</div></div></header>
            <div className="lib-uses"><button type="button" data-test={`cloud-pin-${id}`} onClick={() => onPin(id, false)}>Remove from my menu</button></div>
          </article>
        ))}
        {visible.map((m) => {
          const pinned = pins.includes(m.id);
          const uses = usedFor(m.id);
          const options: Option[] = USES.filter((u) => !u.needs || u.needs(m)).map((u) => ({
            key: u.task, group: "", title: u.title, detail: uses.some((x) => x.task === u.task) ? "In use now" : "", backend: "openrouter", model: m.id, test: `cloud-use-${m.id}-${u.task}`,
          }));
          return (
            <article key={m.id} className={`lib-card cloud-card${pinned ? " is-installed" : ""}${uses.length ? " in-use" : ""}`} data-test={`cloud-card-${m.id}`}>
              <header>
                <div className="cloud-head">
                  <div className="lib-title">{m.name}</div>
                  <div className="lib-sub">{[m.id, contextLabel(m.context) ? `${contextLabel(m.context)} context` : "", priceLine(m)].filter(Boolean).join(" · ")}</div>
                </div>
                <div className="lib-tags">
                  {m.free ? <span className="lib-tag fit-gpu">Free</span> : null}
                  {m.vision ? <span className="lib-tag vision">Sees pictures</span> : null}
                  {m.reasoning ? <span className="lib-tag">Thinks first</span> : null}
                  {m.tools ? <span className="lib-tag">Uses tools</span> : null}
                  {m.makes_images ? <span className="lib-tag">Makes pictures</span> : null}
                </div>
              </header>
              {m.blurb ? <p className="lib-blurb">{m.blurb}</p> : null}
              <div className="lib-uses">
                <button type="button" className={`route-use-btn${pinned ? " active" : ""}`} aria-pressed={pinned} data-test={`cloud-pin-${m.id}`} onClick={() => onPin(m.id, !pinned)}>
                  {pinned ? "In my menu" : "Add to my menu"}
                </button>
                <Picker id={`cloud-${m.id}`} label={uses.length ? `Used for ${uses.map((u) => u.title).join(", ")}` : "Use for…"} options={options} current={""}
                  onPick={(o) => onAssign(o.key as Task, "openrouter", m.id)} />
              </div>
            </article>
          );
        })}
        {!shown.length && !unlisted.length ? <p className="route-model-empty" data-test="cloud-empty">{all.length ? "Nothing matches. Try another word or filter." : view ? "No list yet. Press Refresh the list to fetch it from openrouter.ai." : ""}</p> : null}
      </div>
      {shown.length > visible.length ? (
        <button type="button" className="set-link" data-test="cloud-more" onClick={() => setMore(true)}>Show {shown.length - visible.length} more</button>
      ) : null}
      <h4 className="models-h4">Add one by its id</h4>
      <div className="cloud-add">
        <input data-test="cloud-add-id" type="text" spellCheck={false} placeholder="vendor/model-name" value={custom} onChange={(e) => setCustom(e.target.value)} />
        <button type="button" disabled={!customOk} data-test="cloud-add" onClick={() => { onPin(custom.trim(), true); setCustom(""); }}>Add to my menu</button>
      </div>
      <p className="set-hint">Chats with a cloud model leave this computer, and Act never uses one. The list comes from openrouter.ai without your key.</p>
    </>
  );
}
