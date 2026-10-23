import { useEffect, useLayoutEffect, useRef, useState, type ReactNode } from "react";
import { closeWindow } from "./tauri";

// `icon()` is the shared hand-drawn icon set (core/shared-ui/icons.js), loaded by the page before the bundle.
declare function icon(name: string): SVGElement;

export function Icon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => {
    host.current?.replaceChildren(icon(name));
  }, [name]);
  return <span ref={host} style={{ display: "contents" }} />;
}

/** Marks the document active or inactive as the window gains and loses focus; inactive windows recede (sidebar selection, accent). */
function useWindowState() {
  useEffect(() => {
    const set = (active: boolean) => { document.documentElement.dataset.window = active ? "active" : "inactive"; };
    const on = () => set(true);
    const off = () => set(false);
    set(document.hasFocus());
    window.addEventListener("focus", on);
    window.addEventListener("blur", off);
    return () => {
      window.removeEventListener("focus", on);
      window.removeEventListener("blur", off);
    };
  }, []);
}

export interface AppFrameProps {
  title: string;
  sidebar: ReactNode;
  toolbar?: ReactNode;
  children: ReactNode;
  /** The scrolling content is a single centred column of at most 720 px. */
  column?: boolean;
  /** Before the title in the toolbar row (a Back button). */
  leading?: ReactNode;
  /** A large title at the top of the content; the toolbar title appears only once it has scrolled away. */
  largeTitle?: boolean;
}

/** A window: close control, material sidebar, unified titlebar and toolbar over a solid scrolling content area. */
export function AppFrame({ title, sidebar, toolbar, children, column = true, leading, largeTitle }: AppFrameProps) {
  useWindowState();
  const [scrolled, setScrolled] = useState(false);
  return (
    <div className="hx-app" data-test="app">
      <aside className="hx-sidebar" data-test="side">
        <div className="hx-window-controls" data-tauri-drag-region>
          <button className="hx-close" data-test="close" title="Close" aria-label="Close" onClick={() => closeWindow()}>
            <Icon name="x" />
          </button>
        </div>
        {sidebar}
      </aside>
      <section className="hx-main">
        <header className={`hx-titlebar${scrolled ? " scrolled" : ""}`} data-tauri-drag-region>
          {leading}
          <h1 className="hx-title" id="page-title" style={largeTitle && !scrolled ? { opacity: 0 } : undefined}>{title}</h1>
          <div className="hx-toolbar">{toolbar}</div>
        </header>
        <div className="hx-scroll" data-test="content" onScroll={(event) => setScrolled(event.currentTarget.scrollTop > (largeTitle ? 34 : 4))}>
          {column ? (
            <div className="hx-column">
              {largeTitle ? <h2 className="hx-large-title" data-test="large-title">{title}</h2> : null}
              {children}
            </div>
          ) : children}
        </div>
      </section>
    </div>
  );
}

export function Sidebar({ children }: { children: ReactNode }) {
  return <nav className="hx-nav">{children}</nav>;
}

export function SidebarItem({ icon: name, label, current, count, test, onClick }: { icon: string; label: string; current: boolean; count?: number; test?: string; onClick: () => void }) {
  return (
    <button className={`hx-side-item${current ? " current" : ""}`} data-test={test} aria-current={current ? "page" : undefined} onClick={onClick}>
      <Icon name={name} />
      <span>{label}</span>
      {count ? <span className="hx-count" data-test={test ? `${test}-count` : undefined}>{count}</span> : null}
    </button>
  );
}

export function Group({ title, children, test }: { title?: string; children: ReactNode; test?: string }) {
  return (
    <div>
      {title ? <h2 className="hx-group-title">{title}</h2> : null}
      <div className="hx-group" data-test={test}>{children}</div>
    </div>
  );
}

export function Row({ title, sub, leading, trailing, test, focus, children }: { title: ReactNode; sub?: ReactNode; leading?: ReactNode; trailing?: ReactNode; test?: string; focus?: boolean; children?: ReactNode }) {
  const row = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (focus) row.current?.scrollIntoView({ block: "center" });
  }, [focus]);
  return (
    <div ref={row} className={`hx-row${focus ? " focus" : ""}`} data-test={test}>
      {leading}
      <div className="hx-row-text">
        <div className="hx-row-title">{title}</div>
        {sub ? <div className="hx-row-sub">{sub}</div> : null}
        {children}
      </div>
      {trailing ? <div className="hx-row-trail">{trailing}</div> : null}
    </div>
  );
}

/** A coloured rounded square with a glyph or a letter (apps, sections). */
export function IconTile({ name, letter, hue, large }: { name?: string; letter?: string; hue: number; large?: boolean }) {
  return (
    <span className={`hx-tile${large ? " large" : ""}`} style={{ background: `hsl(${hue} 52% 46%)` }} aria-hidden="true">
      {name ? <Icon name={name} /> : letter}
    </span>
  );
}

export function hueOf(text: string): number {
  let hash = 0;
  for (const ch of text) hash = (hash * 31 + (ch.codePointAt(0) ?? 0)) >>> 0;
  return hash % 360;
}

export function Button({ kind, small, children, ...rest }: { kind?: "primary" | "plain" | "destructive"; small?: boolean } & React.ButtonHTMLAttributes<HTMLButtonElement>) {
  return (
    <button {...rest} className={`hx-btn${kind ? ` ${kind}` : ""}${small ? " small" : ""}${rest.className ? ` ${rest.className}` : ""}`}>
      {children}
    </button>
  );
}

export function Segmented<T extends string>({ value, options, onChange, test }: { value: T; options: { id: T; label: string }[]; onChange: (id: T) => void; test?: string }) {
  const host = useRef<HTMLDivElement>(null);
  const [thumb, setThumb] = useState<{ left: number; width: number } | null>(null);
  useLayoutEffect(() => {
    const current = host.current?.querySelector<HTMLElement>("button.current");
    if (current) setThumb({ left: current.offsetLeft, width: current.offsetWidth });
  }, [value, options.length]);
  return (
    <div className="hx-seg" ref={host} role="tablist">
      {thumb ? <span className="thumb" style={{ left: thumb.left, width: thumb.width }} /> : null}
      {options.map((option) => (
        <button key={option.id} role="tab" aria-selected={option.id === value} className={option.id === value ? "current" : ""} data-test={test ? `${test}-${option.id}` : undefined} onClick={() => onChange(option.id)}>
          {option.label}
        </button>
      ))}
    </div>
  );
}

/**
 * A pop-up button, as on macOS: the current choice with a chevron, and a menu of the others that opens beneath it. Arrow keys move, Enter chooses,
 * Escape and a click elsewhere close it. One choice is marked with a tick. An option can be disabled (greyed, with a reason as its title).
 */
export function Popup<T extends string>({ value, options, onChange, test, label, placeholder }: {
  value: T;
  options: { id: T; label: string; disabled?: boolean; title?: string }[];
  onChange: (id: T) => void;
  test?: string;
  label: string;
  placeholder?: string;
}) {
  const [open, setOpen] = useState(false);
  const [active, setActive] = useState(0);
  const host = useRef<HTMLDivElement>(null);
  const current = options.find((option) => option.id === value);
  useEffect(() => {
    if (!open) return;
    const outside = (event: MouseEvent) => {
      if (!host.current?.contains(event.target as Node)) setOpen(false);
    };
    window.addEventListener("mousedown", outside, true);
    return () => window.removeEventListener("mousedown", outside, true);
  }, [open]);
  const choose = (option: { id: T; disabled?: boolean }) => {
    if (option.disabled) return;
    setOpen(false);
    if (option.id !== value) onChange(option.id);
  };
  const move = (by: number) => {
    const count = options.length;
    for (let step = 1; step <= count; step++) {
      const next = (active + by * step + count * step) % count;
      if (!options[next].disabled) {
        setActive(next);
        return;
      }
    }
  };
  return (
    <div className="hx-popup" ref={host}>
      <button
        type="button"
        className="hx-popup-button"
        aria-haspopup="menu"
        aria-expanded={open}
        aria-label={label}
        data-test={test}
        onClick={() => { setActive(Math.max(0, options.findIndex((option) => option.id === value))); setOpen(!open); }}
        onKeyDown={(event) => {
          if (event.key === "ArrowDown" || event.key === "ArrowUp") {
            event.preventDefault();
            if (!open) { setActive(Math.max(0, options.findIndex((option) => option.id === value))); setOpen(true); } else move(event.key === "ArrowDown" ? 1 : -1);
          } else if (event.key === "Enter" && open) {
            event.preventDefault();
            choose(options[active]);
          } else if (event.key === "Escape" && open) {
            event.stopPropagation();
            setOpen(false);
          }
        }}
      >
        <span className="hx-popup-value">{current ? current.label : placeholder ?? "Choose"}</span>
        <Icon name="chevron-down" />
      </button>
      {open ? (
        <div className="hx-menu" role="menu" aria-label={label}>
          {options.map((option, index) => (
            <button
              type="button"
              key={option.id}
              role="menuitemradio"
              aria-checked={option.id === value}
              disabled={option.disabled}
              title={option.title}
              className={`hx-menu-item${index === active ? " active" : ""}`}
              data-test={test ? `${test}-${option.id}` : undefined}
              onMouseEnter={() => setActive(index)}
              onClick={() => choose(option)}
            >
              <span className="hx-menu-tick">{option.id === value ? "✓" : ""}</span>
              {option.label}
            </button>
          ))}
        </div>
      ) : null}
    </div>
  );
}

export function Switch({ checked, onChange, label, test }: { checked: boolean; onChange: (next: boolean) => void; label: string; test?: string }) {
  return <button className="hx-switch" role="switch" aria-checked={checked} aria-label={label} data-test={test} onClick={() => onChange(!checked)} />;
}

export function SearchField({ value, onChange, placeholder = "Search", test, inputRef }: { value: string; onChange: (value: string) => void; placeholder?: string; test?: string; inputRef?: React.Ref<HTMLInputElement> }) {
  return (
    <label className="hx-search">
      <Icon name="search" />
      <input ref={inputRef} data-test={test} type="search" placeholder={placeholder} autoComplete="off" value={value} onChange={(event) => onChange(event.target.value)} onKeyDown={(event) => { if (event.key === "Escape") onChange(""); }} />
    </label>
  );
}

export function Chip({ tone, children }: { tone?: "accent" | "ok" | "warn" | "danger"; children: ReactNode }) {
  return <span className={`hx-chip${tone ? ` ${tone}` : ""}`}>{children}</span>;
}

export function Banner({ tone, children, action, test }: { tone?: "error" | "warn"; children: ReactNode; action?: ReactNode; test?: string }) {
  return (
    <div className={`hx-banner${tone ? ` ${tone}` : ""}`} role={tone === "error" ? "alert" : "status"} data-test={test}>
      <span>{children}</span>
      {action}
    </div>
  );
}

export function EmptyState({ icon: name, children, action }: { icon: string; children: ReactNode; action?: ReactNode }) {
  return (
    <div className="hx-empty">
      <Icon name={name} />
      <p>{children}</p>
      {action}
    </div>
  );
}

export function Meter({ percent, live, label }: { percent: number; live?: boolean; label?: string }) {
  const value = Math.max(0, Math.min(100, percent));
  return (
    <div
      className={live ? "hx-meter live" : "hx-meter"}
      role={live ? "meter" : undefined}
      aria-hidden={live ? undefined : true}
      aria-label={live ? label : undefined}
      aria-valuemin={live ? 0 : undefined}
      aria-valuemax={live ? 100 : undefined}
      aria-valuenow={live ? Math.round(value) : undefined}
    >
      <i style={{ width: `${value}%` }} />
    </div>
  );
}

/** A modal sheet that drops from the titlebar, or a centred alert. Escape cancels; Enter does not confirm (the primary must be clicked). */
export function Sheet({ title, children, actions, alert, onCancel, test }: { title: string; children?: ReactNode; actions: ReactNode; alert?: boolean; onCancel: () => void; test?: string }) {
  const ref = useRef<HTMLDivElement>(null);
  // The latest onCancel, without re-running the effect: focusing the sheet again on every render would take the keyboard from a field being typed in.
  const cancel = useRef(onCancel);
  cancel.current = onCancel;
  useEffect(() => {
    ref.current?.focus();
    const key = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        event.stopPropagation();
        cancel.current();
      }
    };
    window.addEventListener("keydown", key, true);
    return () => window.removeEventListener("keydown", key, true);
  }, []);
  return (
    <div className="hx-scrim" onMouseDown={(event) => { if (event.target === event.currentTarget) onCancel(); }}>
      <div ref={ref} tabIndex={-1} role={alert ? "alertdialog" : "dialog"} aria-label={title} className={`hx-sheet${alert ? " hx-alert" : ""}`} data-test={test}>
        <h3>{title}</h3>
        {children}
        <div className="hx-actions">{actions}</div>
      </div>
    </div>
  );
}

export function Spinner({ label = "Working" }: { label?: string }) {
  return <span className="hx-spinner" role="progressbar" aria-label={label} />;
}

export function SidebarLabel({ children }: { children: ReactNode }) {
  return <div className="hx-nav-label">{children}</div>;
}
