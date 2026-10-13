import { useEffect, useState } from "react";
import { Icon } from "./icon";

export interface IslandView { kind: string; text: string; detail: string; }
export interface OsdView { kind: string; percent: number; muted: boolean; text: string | null; }

/** The live-status pill's content. `shell.js` keeps the pill itself (visibility, kind class, title, click), because the bar's regions and
 * glass are measured from it; Rust still decides what is going on. */
export function IslandBody({ register }: { register: (set: (view: IslandView) => void) => void }) {
  const [view, setView] = useState<IslandView>({ kind: "none", text: "", detail: "" });
  useEffect(() => register(setView), [register]);
  const icon = view.kind === "approval" ? "shield" : view.kind === "media" ? "music" : view.kind === "agentdesk" ? "grid" : "sparkle";
  return (
    <>
      <Icon name={icon} />
      <span id="island-text">{view.text}</span>
      <span id="island-detail">{view.detail}</span>
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
