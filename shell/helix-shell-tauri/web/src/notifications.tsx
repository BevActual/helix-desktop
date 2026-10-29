import { useEffect, useState } from "react";
import { invoke, listen, type NotificationItem, type NotificationsView } from "@helix/ui";
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

interface Summary {
  app: string;
  key: string;
  text: string;
}

/** One notification in the shade, with its buttons and a quiet dismiss. `index` is its place in Rust's list (what tests and buttons name). */
function Row({ item, index }: { item: NotificationItem; index: number }) {
  return (
    <div className={`nrow${item.urgency === "critical" ? " critical" : ""}${item.live ? "" : " stale"}`}>
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
  );
}

/** The shade: notifications stacked by app (newest app first). A stack of two or more is one card with a summary line written on this
 *  computer (or its newest notification, until the summary is ready); a click opens it. Actions send an index into the list Rust offered. */
export function Shade({ subscribe }: { subscribe: (set: (view: NotificationsView) => void) => void }) {
  const [view, setView] = useState<NotificationsView | null>(null);
  const [summaries, setSummaries] = useState<Summary[]>([]);
  const [open, setOpen] = useState<string[]>([]);
  useEffect(() => subscribe(setView), [subscribe]);
  useEffect(() => {
    invoke<Summary[]>("notification_summaries").then(setSummaries).catch(() => undefined);
    const stop = listen<Summary[]>("notification-summaries", (payload) => setSummaries(payload));
    return () => { stop.then((f) => f()); };
  }, []);
  if (!view) return null;
  const stacks: { app: string; rows: { item: NotificationItem; index: number }[] }[] = [];
  view.items.forEach((item, index) => {
    const stack = stacks.find((s) => s.app === item.app);
    if (stack) stack.rows.push({ item, index });
    else stacks.push({ app: item.app, rows: [{ item, index }] });
  });
  return (
    <>
      {stacks.map((stack) => {
        if (stack.rows.length < 2) return <Row key={`${stack.rows[0].item.id}:${stack.rows[0].item.revision}`} {...stack.rows[0]} />;
        const key = stack.rows.map((r) => `${r.item.id}:${r.item.revision}`).join(",");
        const summary = summaries.find((s) => s.app === stack.app && s.key === key);
        const newest = stack.rows[0].item;
        const tid = stack.app.replace(/[^A-Za-z0-9]+/g, "-");
        const toggle = () => setOpen((o) => (o.includes(stack.app) ? o.filter((a) => a !== stack.app) : [...o, stack.app]));
        if (open.includes(stack.app)) {
          return (
            <div key={`stack:${stack.app}`} className="nstack-open">
              <div className="nstack-head">
                <span className="nstack-title">{stack.app}</span>
                <button type="button" data-test={`notif-stack-${tid}-less`} onClick={toggle}>Show less</button>
                <button type="button" title="Clear these" aria-label="Clear these" data-test={`notif-stack-${tid}-clear`}
                  onClick={() => stack.rows.forEach((r) => invoke("notification_dismiss", { id: r.item.id }))}>
                  <Icon name="x" />
                </button>
              </div>
              {stack.rows.map((r) => <Row key={`${r.item.id}:${r.item.revision}`} {...r} />)}
            </div>
          );
        }
        return (
          <button key={`stack:${stack.app}`} type="button" className={`nstack${stack.rows.length > 2 ? " deep" : ""}`} data-test={`notif-stack-${tid}`} onClick={toggle}>
            <AppBadge name={stack.app} />
            <div className="ntext">
              <div className="napp">{stack.app}<span className="ncount">{stack.rows.length}</span></div>
              {summary ? (
                <div className="nsummary" data-test={`notif-summary-${tid}`}><Icon name="sparkle" /><span>{summary.text}</span></div>
              ) : (
                <>
                  <div className="nsum">{newest.summary}</div>
                  {newest.body ? <div className="nbody">{newest.body}</div> : null}
                </>
              )}
            </div>
          </button>
        );
      })}
    </>
  );
}
