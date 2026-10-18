import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

declare function icon(name: string): SVGElement;
function ToolIcon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span className="welcome-tool-icon" ref={host} aria-hidden="true" />;
}
export type WelcomeAction = "web" | "tools" | "files" | "picture" | "image" | "voice" | "analysis" | "brief" | "models" | "write" | "code";
export interface Recent { id: string; title: string; project?: string }
export interface WelcomeHandle { set: (starters: string[], listening: boolean, busy: string) => void; recents: (list: Recent[]) => void }
export interface WelcomeHooks { action: (action: WelcomeAction) => void; starter: (text: string) => void; open: (id: string) => void }

/** The first things shown in a new chat: a greeting above the composer (which sits in the middle of the window while the chat is empty) and,
 * below it, a few ways to begin. The rest of the tools are one disclosure away, so nothing competes with the box the person is about to type in. */
const quick: { action: WelcomeAction; icon: string; title: string; detail: string }[] = [
  { action: "tools", icon: "sparkle", title: "Research", detail: "Search and read pages here; you approve each step." },
  { action: "write", icon: "doc", title: "Write", detail: "A document beside the chat." },
  { action: "image", icon: "image", title: "Create an image", detail: "Described, then made on this computer." },
  { action: "code", icon: "code", title: "Work on code", detail: "Your project's files beside the chat." },
];
const more: { action: WelcomeAction; icon: string; title: string; detail: string }[] = [
  { action: "web", icon: "search", title: "Pick sources yourself", detail: "Choose pages, then ask about the excerpts." },
  { action: "files", icon: "paperclip", title: "Work with files", detail: "Add text or source files to your message." },
  { action: "picture", icon: "image", title: "Ask about a picture", detail: "Attach a picture, then ask about it." },
  { action: "voice", icon: "mic", title: "Dictate a message", detail: "Record here; review the words before sending." },
  { action: "analysis", icon: "eye", title: "Inspect Linux software", detail: "Analyse a compiled program locally." },
  { action: "brief", icon: "pin", title: "Set a chat brief", detail: "Keep goals and decisions as the chat grows." },
];
const greeting = () => { const h = new Date().getHours(); return h < 5 ? "Still up?" : h < 12 ? "Good morning" : h < 18 ? "Good afternoon" : "Good evening"; };

export function Welcome({ register, hooks }: { register: (handle: WelcomeHandle) => void; hooks: WelcomeHooks }) {
  const [starters, setStarters] = useState<string[]>([]);
  const [listening, setListening] = useState(false);
  const [busy, setBusy] = useState("");
  const [recent, setRecent] = useState<Recent[]>([]);
  const [showMore, setShowMore] = useState(false);
  useEffect(() => { register({ set: (next, recording, pending) => { setStarters(next); setListening(recording); setBusy(pending); }, recents: setRecent }); }, [register]);
  const below = document.getElementById("home-below");
  const card = (tool: { action: WelcomeAction; icon: string; title: string; detail: string }) => <button key={tool.action} type="button" className={`welcome-tool${tool.action === "voice" && listening ? " recording" : ""}`} data-test={`welcome-${tool.action}`} onClick={() => hooks.action(tool.action)} disabled={tool.action === "voice" && !!busy} aria-pressed={tool.action === "voice" ? listening : undefined}>
    <ToolIcon name={tool.icon} />
    <span className="welcome-tool-text"><strong>{tool.action === "voice" && busy ? (busy === "starting" ? "Preparing dictation…" : "Transcribing…") : tool.action === "voice" && listening ? "Finish dictation" : tool.title}</strong><span>{tool.action === "voice" && listening ? <span data-test="welcome-recording" role="status">The microphone is on. Finish to transcribe your message.</span> : tool.detail}</span></span>
  </button>;
  return <>
    <h2 className="empty-title" data-test="welcome-greeting">{greeting()}</h2>
    <p className="empty-note">What would you like to do?</p>
    {below && createPortal(<div className="home-below">
      <div className="welcome-quick" role="group" aria-label="Start something">{quick.map(card)}</div>
      <div className="welcome-morebar">
        <button type="button" className="mini" data-test="welcome-more" aria-expanded={showMore} onClick={() => setShowMore(!showMore)}>{showMore ? "Fewer ways to start" : "More ways to start"}</button>
        <button type="button" className="mini" data-test="welcome-models" onClick={() => hooks.action("models")}>Model library</button>
      </div>
      {showMore && <div className="welcome-tools" aria-label="More ways to start">{more.map(card)}</div>}
      {starters.length > 0 && <section className="welcome-starters" aria-label="Writing starters"><div className="suggestions">{starters.map((text, index) => <button key={`${index}:${text}`} type="button" className="suggestion" data-test={`suggestion-${index}`} onClick={() => hooks.starter(text)}>{text}</button>)}</div></section>}
      {recent.length > 0 && <section className="welcome-recent" aria-label="Pick up where you left off"><h3>Pick up where you left off</h3>
        {recent.slice(0, 3).map((r) => <button key={r.id} type="button" data-test={`recent-${r.id}`} onClick={() => hooks.open(r.id)}><span>{r.title}</span>{r.project ? <em>{r.project}</em> : null}</button>)}
      </section>}
    </div>, below)}
  </>;
}
