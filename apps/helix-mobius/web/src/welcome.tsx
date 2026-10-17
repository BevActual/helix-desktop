import { useEffect, useRef, useState } from "react";

declare function icon(name: string): SVGElement;
function ToolIcon({ name }: { name: string }) {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon(name)); }, [name]);
  return <span className="welcome-tool-icon" ref={host} aria-hidden="true" />;
}
export type WelcomeAction = "web" | "tools" | "files" | "picture" | "image" | "voice" | "analysis" | "brief" | "models";
export interface WelcomeHandle { set: (starters: string[], listening: boolean, busy: string) => void }
export interface WelcomeHooks { action: (action: WelcomeAction) => void; starter: (text: string) => void }
const tools: { action: WelcomeAction; icon: string; title: string; detail: string }[] = [
  { action: "tools", icon: "sparkle", title: "Research in chat", detail: "Search and read pages here; you approve each step." },
  { action: "web", icon: "search", title: "Pick sources yourself", detail: "Choose pages, then ask about the excerpts." },
  { action: "files", icon: "paperclip", title: "Work with files", detail: "Add text or source files to your message." },
  { action: "picture", icon: "image", title: "Ask about a picture", detail: "Attach a picture, then ask about it." },
  { action: "image", icon: "sparkle", title: "Create an image", detail: "Describe it; it is made on this computer." },
  { action: "voice", icon: "mic", title: "Dictate a message", detail: "Record here; review the words before sending." },
  { action: "analysis", icon: "eye", title: "Inspect Linux software", detail: "Analyse a compiled program locally." },
  { action: "brief", icon: "pin", title: "Set a chat brief", detail: "Keep goals and decisions as the chat grows." },
];
export function Welcome({ register, hooks }: { register: (handle: WelcomeHandle) => void; hooks: WelcomeHooks }) {
  const [starters, setStarters] = useState<string[]>([]);
  const [listening, setListening] = useState(false);
  const [busy, setBusy] = useState("");
  useEffect(() => { register({ set: (next, recording, pending) => { setStarters(next); setListening(recording); setBusy(pending); } }); }, [register]);
  return <>
    <div className="empty-kicker">Mobius</div>
    <h2 className="empty-title">What would you like to do?</h2>
    <p className="empty-note">Ask a question below, or start with a tool.</p>
    <div className="welcome-tools" aria-label="Start a task">
      {tools.map((tool) => <button key={tool.action} type="button" className={`welcome-tool${tool.action === "voice" && listening ? " recording" : ""}`} data-test={`welcome-${tool.action}`} onClick={() => hooks.action(tool.action)} disabled={tool.action === "voice" && !!busy} aria-pressed={tool.action === "voice" ? listening : undefined}>
        <ToolIcon name={tool.icon} />
        <span className="welcome-tool-text"><strong>{tool.action === "voice" && busy ? (busy === "starting" ? "Preparing dictation…" : "Transcribing…") : tool.action === "voice" && listening ? "Finish dictation" : tool.title}</strong><span>{tool.action === "voice" && listening ? <span data-test="welcome-recording" role="status">The microphone is on. Finish to transcribe your message.</span> : tool.detail}</span></span>
      </button>)}
    </div>
    <div className="welcome-models"><span>Local tools may need a model or setup.</span><button type="button" className="mini" data-test="welcome-models" onClick={() => hooks.action("models")}>Model library</button></div>
    {starters.length > 0 && <section className="welcome-starters" aria-label="Writing starters"><h3>Or start a conversation</h3><div className="suggestions">{starters.map((text, index) => <button key={`${index}:${text}`} type="button" className="suggestion" data-test={`suggestion-${index}`} onClick={() => hooks.starter(text)}>{text}</button>)}</div></section>}
    <p className="welcome-hint">Starters fill your draft for review. Type / for slash commands, or open Apps and search <strong>act</strong> for desktop actions.</p>
  </>;
}
