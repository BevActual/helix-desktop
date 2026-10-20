import { useMemo, useState } from "react";
import { matches, priceLine, validId, type CloudModel } from "./cloudlist";
import { Icon } from "./settingsui";
import type { CloudView } from "./cloudmodels";

/** One model in the list: where it runs, and whether the chat's model menu offers it. */
interface Row { key: string; source: "local" | "chatgpt" | "openrouter"; id: string; name: string; detail: string; shown: boolean }
const SOURCE_NAME = { local: "This computer", chatgpt: "ChatGPT plan", openrouter: "OpenRouter" };
const SEARCH_LIMIT = 30;

/** Which models appear in the chat's model menu: a single list with a switch for each, a box to search or add by id, and a refresh. The jobs a
 * model is used for (chat, code, pictures...) are in Models; this page only decides what the menu offers. */
export function ModelPicker({ local, titles, chatgpt, cloud, hidden, loading, onToggleLocal, onPin, onRefresh, onTasks }: {
  local: string[]; titles: Record<string, string>; chatgpt: string[]; cloud: CloudView | null; hidden: string[]; loading: boolean;
  onToggleLocal: (key: string, shown: boolean) => void; onPin: (id: string, on: boolean) => void; onRefresh: () => void; onTasks: () => void;
}) {
  const [needle, setNeedle] = useState("");
  const pins = cloud?.pins ?? [];
  const catalog = cloud?.models ?? [];
  const off = useMemo(() => new Set(hidden), [hidden]);
  const rows = useMemo(() => {
    const q = needle.trim().toLowerCase();
    const say = (r: Row) => !q || `${r.name} ${r.id} ${SOURCE_NAME[r.source]}`.toLowerCase().includes(q);
    const out: Row[] = [];
    for (const id of local) out.push({ key: `local:${id}`, source: "local", id, name: titles[id] || id.replace(/\.gguf$/i, ""), detail: id, shown: !off.has(`local:${id}`) });
    for (const id of chatgpt) out.push({ key: `chatgpt:${id}`, source: "chatgpt", id, name: id, detail: "Uses your ChatGPT plan", shown: !off.has(`chatgpt:${id}`) });
    const byId = new Map<string, CloudModel>(catalog.map((m) => [m.id, m]));
    const cloudIds = new Set<string>(pins);
    // Without a search only what is in the menu is listed; searching reaches the whole catalog.
    if (q) for (const m of catalog.filter((m) => matches(m, q, "all", pins)).slice(0, SEARCH_LIMIT)) cloudIds.add(m.id);
    for (const id of cloudIds) {
      const m = byId.get(id);
      out.push({ key: `openrouter:${id}`, source: "openrouter", id, name: m?.name ?? id.split("/").pop() ?? id, detail: m ? [id, priceLine(m)].filter(Boolean).join(" · ") : `${id} · not in the list we have`, shown: pins.includes(id) });
    }
    return out.filter(say).sort((a, b) => Number(b.shown) - Number(a.shown));
  }, [local, titles, chatgpt, catalog, pins, off, needle]);
  const typed = needle.trim();
  const canAdd = validId(typed) && !pins.includes(typed);
  const toggle = (r: Row, on: boolean) => (r.source === "openrouter" ? onPin(r.id, on) : onToggleLocal(r.key, on));

  return (
    <div className="picker" data-test="picker">
      <div className="picker-bar">
        <label className="route-search" htmlFor="picker-search">
          <Icon name="search" />
          <input id="picker-search" data-test="picker-search" type="search" autoComplete="off" spellCheck={false} placeholder="Add or search model" value={needle} onChange={(e) => setNeedle(e.target.value)}
            onKeyDown={(e) => { if (e.key === "Enter" && canAdd) { e.preventDefault(); onPin(typed, true); setNeedle(""); } }} />
        </label>
        <button type="button" className="icon-btn" data-test="picker-refresh" aria-label="Refresh the OpenRouter list" title="Refresh the OpenRouter list" disabled={loading} onClick={onRefresh}>↻</button>
      </div>
      {canAdd ? <button type="button" className="picker-add" data-test="picker-add" onClick={() => { onPin(typed, true); setNeedle(""); }}>Add <code>{typed}</code> from OpenRouter</button> : null}
      <ul className="picker-list" data-test="picker-list">
        {rows.map((r) => (
          <li key={r.key} className="picker-row" data-test={`picker-row-${r.key}`}>
            <div className="picker-text"><div className="picker-name">{r.name}</div><div className="picker-detail">{r.detail}</div></div>
            <span className={`picker-source ${r.source}`}>{SOURCE_NAME[r.source]}</span>
            <button type="button" role="switch" aria-checked={r.shown} aria-label={`Show ${r.name} in the model picker`} className={`set-switch${r.shown ? " on" : ""}`} data-test={`picker-switch-${r.key}`} onClick={() => toggle(r, !r.shown)}><span /></button>
          </li>
        ))}
        {rows.length === 0 ? <li className="picker-empty" data-test="picker-empty">{needle ? "Nothing matches. A model id like vendor/model-name can be added." : "No models yet. Get one in the Library, or search OpenRouter above."}</li> : null}
      </ul>
      <p className="set-hint">Turned-off models stay installed and can still be chosen in Settings. The picker is the model menu in a chat. <button type="button" className="set-link" data-test="picker-to-tasks" onClick={onTasks}>Which model does which job →</button></p>
    </div>
  );
}
