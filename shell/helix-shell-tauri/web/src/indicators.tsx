import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";
import { Icon } from "./icon";

export interface IslandView { kind: string; text: string; detail: string; }
export interface OsdView { kind: string; percent: number; muted: boolean; text: string | null; }

const clockText = (when: Date) => (window as unknown as { helixClockTime?: (d: Date) => string }).helixClockTime?.(when) ?? when.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });

/** The island: the live-status pill, exactly in the middle of the bar, over the clock's place. `shell.js` keeps the pill itself (visibility, kind
 * class, enter and leave, the click on the main part), because the bar's regions and glass are measured from it; Rust decides what is going on.
 * It carries the time (a click on it opens the calendar, which the clock it covers would have done) and, for music, the transport controls, which
 * open out while the pointer or the keyboard is on it. A change of what it says slides the new words in. */
export function IslandBody({ register }: { register: (set: (view: IslandView) => void) => void }) {
  const [view, setView] = useState<IslandView>({ kind: "none", text: "", detail: "" });
  const [now, setNow] = useState(() => new Date());
  const [swap, setSwap] = useState(0);
  const last = useRef("");
  useEffect(() => register(setView), [register]);
  useEffect(() => {
    const tick = () => setNow(new Date());
    const timer = setInterval(tick, 5000);
    window.addEventListener("helix-clock", tick);
    return () => { clearInterval(timer); window.removeEventListener("helix-clock", tick); };
  }, []);
  // New words slide in; the same words redrawn do not move.
  useEffect(() => {
    const key = `${view.kind}|${view.text}|${view.detail}`;
    if (key !== last.current) { last.current = key; setSwap((n) => n + 1); }
  }, [view]);
  const media = view.kind === "media";
  const changed = view.kind === "changed";
  const icon = changed ? "check" : view.kind === "approval" ? "shield" : view.kind === "agentdesk" ? "grid" : view.kind === "agentdisplay" ? "eye" : view.kind === "agenthelp" ? "alert" : "sparkle";
  const stop = (event: { stopPropagation: () => void }) => event.stopPropagation();
  return (
    <>
      <span className="isl-main" data-test="island-main">
        {media ? (
          <span className="isl-eq" aria-hidden="true"><i /><i /><i /><i /></span>
        ) : (
          <span className="isl-icon"><Icon name={icon} /></span>
        )}
        <span className="isl-words" key={swap}>
          <span id="island-text">{view.text}</span>
          <span id="island-detail">{view.detail}</span>
        </span>
      </span>
      {media ? (
        <span className="isl-controls" onClick={stop}>
          <button type="button" className="isl-btn" data-test="island-prev" title="Previous" aria-label="Previous" onClick={() => invoke("media_previous")}><Icon name="skip-back" /></button>
          <button type="button" className="isl-btn" data-test="island-toggle" title="Pause" aria-label="Pause" onClick={() => invoke("media_toggle")}><Icon name="pause" /></button>
          <button type="button" className="isl-btn" data-test="island-next" title="Next" aria-label="Next" onClick={() => invoke("media_next")}><Icon name="skip-forward" /></button>
        </span>
      ) : null}
      {changed ? (
        <span className="isl-controls isl-undo-wrap" onClick={stop}>
          <button type="button" className="isl-undo" data-test="island-undo" onClick={() => invoke("island_undo")}>Undo</button>
        </span>
      ) : null}
      <button type="button" className="isl-time" data-test="island-time" title="Calendar" onClick={(event) => { stop(event); invoke("panel_open", { id: "today" }); }}>{clockText(now)}</button>
    </>
  );
}

/** The volume and brightness indicator's content (the show/hide timing and the fill variable stay with the element in `shell.js`). */
export function OsdBody({ register }: { register: (set: (view: OsdView) => void) => void }) {
  const [view, setView] = useState<OsdView | null>(null);
  useEffect(() => register(setView), [register]);
  if (!view) return null;
  const notice = view.kind === "notice";
  const icon = notice ? "shield" : view.kind === "brightness" ? "sun" : view.muted || view.percent === 0 ? "volume-mute" : "volume";
  return (
    <>
      <Icon name={icon} />
      <div id="osd-bar"><div id="osd-fill" /></div>
      <span id="osd-val">{notice ? "" : view.muted ? "Muted" : `${view.percent}%`}</span>
      <span id="osd-text">{notice ? view.text ?? "" : ""}</span>
    </>
  );
}
