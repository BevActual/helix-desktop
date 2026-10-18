import { useEffect, useRef, useState } from "react";

declare function icon(name: string): SVGElement;
export function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}


export interface Option { key: string; group: string; title: string; detail: string; backend: string; model: string; test: string; }

/** A button that shows the current choice and opens a short list to change it. */
export function Picker({ id, label, options, current, onPick }: {
  id: string; label: string; options: Option[]; current: string; onPick: (option: Option) => void;
}) {
  const [open, setOpen] = useState(false);
  const host = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return undefined;
    const away = (event: MouseEvent) => { if (!host.current?.contains(event.target as Node)) setOpen(false); };
    const key = (event: KeyboardEvent) => { if (event.key === "Escape") { event.stopPropagation(); setOpen(false); } };
    document.addEventListener("mousedown", away);
    document.addEventListener("keydown", key, true);
    return () => { document.removeEventListener("mousedown", away); document.removeEventListener("keydown", key, true); };
  }, [open]);
  const groups = [...new Set(options.map((o) => o.group))];
  return (
    <div className="set-picker" ref={host}>
      <button type="button" className="set-picker-btn" data-test={`job-pick-${id}`} aria-haspopup="menu" aria-expanded={open} onClick={() => setOpen(!open)}>
        <span>{label}</span><Icon name="chevron-down" />
      </button>
      {open ? (
        <div className="set-picker-menu" role="menu" data-test={`job-menu-${id}`}>
          {groups.map((group) => (
            <div key={group} className="set-picker-group">
              {group ? <div className="set-picker-head">{group}</div> : null}
              {options.filter((o) => o.group === group).map((option) => (
                <button key={option.key} type="button" role="menuitemradio" aria-checked={option.key === current} className={`set-picker-item${option.key === current ? " on" : ""}`}
                  data-test={option.test} onClick={() => { setOpen(false); onPick(option); }}>
                  <span className="set-picker-title">{option.title}</span>
                  {option.detail ? <span className="set-picker-detail">{option.detail}</span> : null}
                  {option.key === current ? <Icon name="check" /> : null}
                </button>
              ))}
            </div>
          ))}
        </div>
      ) : null}
    </div>
  );
}

