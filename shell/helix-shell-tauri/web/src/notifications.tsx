import { useEffect, useState } from "react";
import { invoke, type NotificationItem, type NotificationsView } from "@helix/ui";
import { Icon } from "./icon";

/** A coloured tile with the application's first letter, the colour taken from its name: a stable, quiet stand-in for an app icon. */
function AppBadge({ name }: { name: string }) {
  let hash = 0;
  for (const ch of name) hash = (hash * 31 + (ch.codePointAt(0) ?? 0)) >>> 0;
  return (
    <div className="nico" style={{ background: `hsl(${hash % 360} 52% 46%)` }}>
      {Array.from(name)[0] || "•"}
    </div>
  );
}

function Body({ item, children }: { item: NotificationItem; children?: React.ReactNode }) {
  return (
    <div className="ntext">
      <div className="napp">{item.app}</div>
      <div className="nsum">{item.summary}</div>
      {item.body ? <div className="nbody">{item.body}</div> : null}
      {children}
    </div>
  );
}

/** The toasts over the desktop. They never take the pointer: there is nothing to click here. */
export function Toasts({ subscribe }: { subscribe: (set: (view: NotificationsView) => void) => void }) {
  const [view, setView] = useState<NotificationsView | null>(null);
  useEffect(() => subscribe(setView), [subscribe]);
  if (!view) return null;
  return (
    <>
      {view.toasts.map((item) => (
        <div key={`${item.id}:${item.revision}`} className={`toast${item.urgency === "critical" ? " critical" : ""}`}>
          <AppBadge name={item.app} />
          <Body item={item} />
        </div>
      ))}
    </>
  );
}

/** The shade: every notification, with its actions and a quiet dismiss. Actions send an index into the list Rust offered. */
export function Shade({ subscribe }: { subscribe: (set: (view: NotificationsView) => void) => void }) {
  const [view, setView] = useState<NotificationsView | null>(null);
  useEffect(() => subscribe(setView), [subscribe]);
  if (!view) return null;
  return (
    <>
      {view.items.map((item, index) => (
        <div key={`${item.id}:${item.revision}`} className={`nrow${item.urgency === "critical" ? " critical" : ""}${item.live ? "" : " stale"}`}>
          <AppBadge name={item.app} />
          <Body item={item}>
            {item.actions.length ? (
              <div className="nacts">
                {item.actions.map((action, j) => (
                  <button
                    key={j}
                    data-test={`notif-${index}-action-${j}`}
                    onClick={(event) => {
                      event.stopPropagation();
                      invoke("notification_invoke", { id: item.id, revision: item.revision, index: j });
                    }}
                  >
                    {action.label}
                  </button>
                ))}
              </div>
            ) : null}
          </Body>
          <button className="ndismiss" title="Dismiss" aria-label="Dismiss" data-test={`notif-${index}-dismiss`} onClick={() => invoke("notification_dismiss", { id: item.id })}>
            <Icon name="x" />
          </button>
        </div>
      ))}
    </>
  );
}
