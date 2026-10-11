import { useEffect, useRef, useState } from "react";
import { flushSync } from "react-dom";
import { invoke, listen } from "@helix/ui";

export interface TrayItem {
  key: string;
  title: string;
  status: string;
  icon_uri: string | null;
  pixmap: { width: number; height: number; rgba: string } | null;
}
export interface MenuEntry {
  id: number | string;
  label: string;
  kind: string;
  enabled: boolean;
  checked: boolean;
  children?: MenuEntry[];
}
export interface TrayHooks {
  /** A right click: `shell.js` reads the application's menu and places it, because the input regions and glass are measured from it. */
  context: (item: TrayItem, button: HTMLElement, at: { key: string; x: number; y: number }) => void;
}

function hue(name: string): number {
  let hash = 0;
  for (const ch of name) hash = (hash * 31 + (ch.codePointAt(0) ?? 0)) >>> 0;
  return hash % 360;
}

/** Applications publish these icons and titles, so they are drawn as images (never as markup). */
function TrayIcon({ item }: { item: TrayItem }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const pixmap = item.pixmap;
  useEffect(() => {
    if (!pixmap || !canvas.current) return;
    const bytes = Uint8ClampedArray.from(atob(pixmap.rgba), (ch) => ch.charCodeAt(0));
    if (bytes.length === pixmap.width * pixmap.height * 4) {
      canvas.current.getContext("2d")?.putImageData(new ImageData(bytes, pixmap.width, pixmap.height), 0, 0);
    }
  }, [pixmap]);
  if (item.icon_uri) return <img src={item.icon_uri} alt="" draggable={false} />;
  if (pixmap) return <canvas ref={canvas} width={pixmap.width} height={pixmap.height} />;
  const name = item.title || "?";
  return (
    <div className="dock-tile tray-tile" style={{ background: `hsl(${hue(name)} 45% 42%)` }}>
      {Array.from(name)[0] || "•"}
    </div>
  );
}

export function Tray({ hooks }: { hooks: TrayHooks }) {
  const [items, setItems] = useState<TrayItem[]>([]);
  useEffect(() => {
    const stop = listen<TrayItem[]>("tray-view", setItems);
    invoke<TrayItem[]>("tray_view").then(setItems, () => undefined);
    return () => { stop.then((fn) => fn()); };
  }, []);
  return (
    <>
      {items.map((item, index) => {
        const where = (event: React.MouseEvent) => ({ key: item.key, x: Math.round(event.clientX), y: Math.round(event.clientY) });
        return (
          <button
            key={item.key}
            className={`tray-item${item.status === "attention" ? " attention" : ""}`}
            data-test={`tray-${index}`}
            title={item.title}
            aria-label={item.title}
            onClick={(event) => invoke("tray_activate", where(event))}
            onAuxClick={(event) => { if (event.button === 1) invoke("tray_secondary", where(event)); }}
            onContextMenu={(event) => {
              event.preventDefault();
              hooks.context(item, event.currentTarget, where(event));
            }}
          >
            <TrayIcon item={item} />
          </button>
        );
      })}
    </>
  );
}

export interface TrayMenuHandle {
  /** Draws the entries now (synchronously), so the caller can measure the menu to place it. */
  set: (key: string, entries: MenuEntry[], chosen: () => void) => void;
}

/** An application's own menu. Rust remembers exactly what it showed, so a choice is only passed on if it was an enabled entry of this menu. */
export function TrayMenu({ register }: { register: (handle: TrayMenuHandle) => void }) {
  const [state, setState] = useState<{ key: string; entries: MenuEntry[]; chosen: () => void } | null>(null);
  useEffect(() => {
    register({ set: (key, entries, chosen) => flushSync(() => setState({ key, entries, chosen })) });
  }, [register]);
  if (!state) return null;
  const rows = (entry: MenuEntry, sub: boolean): React.ReactNode => {
    if (entry.kind === "separator") return <div key={`s${entry.id}`} className="menu-sep" />;
    const header = entry.kind === "submenu";
    const disabled = !entry.enabled && !header;
    return [
      <div
        key={`r${entry.id}`}
        className={`menu-row${sub ? " sub" : ""}${header ? " header" : ""}${disabled ? " disabled" : ""}`}
        data-test={`tray-menu-${entry.id}`}
        onClick={header ? undefined : () => {
          if (!entry.enabled) return;
          state.chosen();
          invoke("tray_menu_activate", { key: state.key, id: entry.id });
        }}
      >
        <span className="tick">{entry.checked ? (entry.kind === "radio" ? "●" : "✓") : ""}</span>
        <span>{entry.label}</span>
      </div>,
      ...(entry.children ?? []).map((child) => rows(child, true)),
    ];
  };
  return <>{state.entries.map((entry) => rows(entry, false))}</>;
}
