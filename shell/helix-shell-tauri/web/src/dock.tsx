import { useEffect, useState } from "react";
import { flushSync } from "react-dom";
import { invoke } from "@helix/ui";

export interface DockItem {
  id: string;
  name: string;
  running: boolean;
  focused: boolean;
  urgent: boolean;
  unverified: boolean;
  pinned: boolean;
  detail: string;
  icon_uri: string | null;
}
export interface DockHooks {
  menu: (item: DockItem, cell: HTMLElement) => void;
  tip: (item: DockItem, cell: HTMLElement) => void;
  untip: () => void;
}

export interface DockHandle {
  set: (items: DockItem[]) => void;
}

function hue(name: string): number {
  let hash = 0;
  for (const ch of name) hash = (hash * 31 + (ch.codePointAt(0) ?? 0)) >>> 0;
  return hash % 360;
}

/** The dock's icons. `shell.js` keeps what is geometric: the magnify lens (it sets `--s` and `--dx` on these cells directly, so each cell
 * keeps a stable element), the hide timer, the tooltip, the context menu and the regions and glass measured from the dock. */
export function DockItems({ hooks, register }: { hooks: DockHooks; register: (handle: DockHandle) => void }) {
  const [items, setItems] = useState<DockItem[]>([]);
  useEffect(() => {
    register({ set: (next) => flushSync(() => setItems(next)) });
  }, [register]);
  return (
    <>
      {items.map((item, index) => (
        <div
          key={item.id}
          className={`dock-item${item.running ? " running" : ""}${item.focused ? " focused" : ""}${item.urgent ? " urgent" : ""}${item.unverified ? " unverified" : ""}`}
          data-test={`dock-item-${index}`}
          data-name={item.name}
          onClick={() => invoke("dock_activate", { id: item.id })}
          onAuxClick={(event) => { if (event.button === 1) invoke("dock_new_window", { id: item.id }); }}
          onContextMenu={(event) => {
            event.preventDefault();
            hooks.menu(item, event.currentTarget);
          }}
          onPointerEnter={(event) => hooks.tip(item, event.currentTarget)}
          onPointerLeave={() => hooks.untip()}
        >
          {item.icon_uri ? (
            <img src={item.icon_uri} alt="" draggable={false} />
          ) : (
            <div className="dock-tile" style={{ background: `hsl(${hue(item.name)} 45% 42%)` }}>{Array.from(item.name)[0] || "•"}</div>
          )}
        </div>
      ))}
    </>
  );
}
