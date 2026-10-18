import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** Regenerate, and a small menu beside it to redo the reply with another model, for this reply only: the chat keeps the model it has, so the two
 * answers can be compared without changing anything else. */
interface Card { file: string; title: string; bytes: number; loaded: boolean; installed: boolean; fit: string; speed: number | null; vision: boolean }
interface Cloud { has_key: boolean; models: string[] }

export function Regenerate({ disabled, report }: { disabled: boolean; report: (error: unknown) => void }) {
  const [open, setOpen] = useState(false);
  const [local, setLocal] = useState<Card[]>([]);
  const [cloud, setCloud] = useState<Cloud>({ has_key: false, models: [] });
  const box = useRef<HTMLSpanElement>(null);
  useEffect(() => {
    if (!open) return;
    Promise.all([
      invoke<string[]>("routing_list_models").catch(() => [] as string[]),
      invoke<Card[]>("library_list").catch(() => [] as Card[]),
      invoke<Cloud>("routing_cloud_menu").catch(() => ({ has_key: false, models: [] as string[] })),
    ]).then(([files, cards, remote]) => {
      const installed = new Map(cards.filter((c) => c.installed).map((c) => [c.file, c]));
      setLocal((Array.isArray(files) ? files : []).map((f) => installed.get(f) ?? { file: f, title: f.replace(/\.gguf$/, ""), bytes: 0, loaded: false, installed: true, fit: "", speed: null, vision: false }));
      setCloud(remote);
    });
    const away = (e: MouseEvent) => { if (!box.current?.contains(e.target as Node)) setOpen(false); };
    const esc = (e: KeyboardEvent) => { if (e.key === "Escape") setOpen(false); };
    window.addEventListener("mousedown", away); window.addEventListener("keydown", esc);
    return () => { window.removeEventListener("mousedown", away); window.removeEventListener("keydown", esc); };
  }, [open]);
  const redo = (backend: string, model: string) => { setOpen(false); invoke("chat_regenerate", { backend, model }).catch(report); };
  return <span className="regen" ref={box}>
    <button className="mini" data-test="regenerate" disabled={disabled} onClick={() => invoke("chat_regenerate").catch(report)}>Regenerate</button>
    <button className="mini regen-more" data-test="regenerate-more" aria-label="Regenerate with another model" aria-haspopup="menu" aria-expanded={open} title="Regenerate with another model" disabled={disabled} onClick={() => setOpen(!open)}>▾</button>
    {open && <div className="hx-menu regen-menu" role="menu" data-test="regen-menu">
      <div className="hx-menu-label">Redo this reply with…</div>
      {local.map((c) => <button key={c.file} type="button" role="menuitem" className="hx-menu-item" data-test={`regen-model-${c.file}`} onClick={() => redo("local", c.file)}>
        <span className="mi-text"><span className="mi-title">{c.title}</span><span className="mi-sub">{[c.bytes ? `${(c.bytes / 1e9).toFixed(1)} GB` : "", c.vision ? "sees pictures" : "", c.loaded ? "ready now" : "loads first", c.speed ? `${Math.round(c.speed)} tok/s here` : ""].filter(Boolean).join(" · ")}</span></span>
      </button>)}
      {local.length === 0 && <div className="hx-menu-label">No local models are installed.</div>}
      {cloud.has_key && <><div className="hx-menu-sep" /><div className="hx-menu-label">OpenRouter <span className="hx-menu-hint">leaves this computer</span></div>
        {cloud.models.slice(0, 5).map((id) => <button key={id} type="button" role="menuitem" className="hx-menu-item" data-test={`regen-cloud-${id}`} onClick={() => redo("openrouter", id)}><span className="mi-text"><span className="mi-title">{id.split("/").pop()}</span><span className="mi-sub">{id.split("/")[0]} · cloud</span></span></button>)}</>}
    </div>}
  </span>;
}
