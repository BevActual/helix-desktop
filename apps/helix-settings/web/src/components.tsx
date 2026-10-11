import { useEffect, useRef, type ReactNode } from "react";

// `icon()` is the shared hand-drawn icon set (core/shared-ui/icons.js), loaded by the page before this bundle.
declare function icon(name: string): SVGElement;

/** One of the shared icons. The SVG is made by the icon set with the SVG DOM (no markup is parsed). */
export function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => {
    host.current?.replaceChildren(icon(name));
  }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}

export function SwitchRow({ title, about, on, test, disabled, onChange }: { title: string; about?: string; on: boolean; test: string; disabled?: boolean; onChange: (on: boolean) => void }) {
  return (
    <div className="setting-row">
      <div className="setting-text">
        <div className="setting-title">{title}</div>
        {about ? <div className="setting-about">{about}</div> : null}
      </div>
      <input type="checkbox" className="switch" checked={on} disabled={disabled} data-test={test} onChange={(event) => onChange(event.target.checked)} />
    </div>
  );
}

export function ListRow({ title, tag, test, icon: iconName, active, clickable, dim, onClick, children }: { title: string; tag?: string; test: string; icon?: string; active?: boolean; clickable?: boolean; dim?: boolean; onClick?: () => void; children?: ReactNode }) {
  const classes = ["setting-row", "list-row", active ? "active" : "", clickable ? "clickable" : "", dim ? "dim" : ""].filter(Boolean).join(" ");
  return (
    <div className={classes} data-test={test} onClick={onClick}>
      <div className="row-lead">
        {iconName ? <Icon name={iconName} /> : null}
        <div className="setting-text">
          <div className="setting-title">{title}</div>
          {tag ? <div className="setting-about">{tag}</div> : null}
        </div>
      </div>
      {children}
    </div>
  );
}

/** A small button inside a row: pressing it must not also press the row. */
export function ActionButton({ label, test, onClick, kind, disabled }: { label: string; test: string; onClick: () => void; kind?: string; disabled?: boolean }) {
  return (
    <button className={kind ?? ""} data-test={test} disabled={disabled} onClick={(event) => { event.stopPropagation(); onClick(); }}>
      {label}
    </button>
  );
}
