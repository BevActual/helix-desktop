import { useEffect, useRef, useState } from "react";
import { invoke, listen } from "@helix/ui";
import { Icon } from "./icon";

/** What Rust says about Writing Tools (`writing.rs`). */
export interface WritingView {
  excerpt: string;
  chars: number;
  tool: string;
  phase: "ready" | "working" | "done" | "failed";
  result: string;
  error: string;
  replaceable: boolean;
  local: boolean;
}

export interface WritingHandle {
  open: () => void;
}

const TOOLS: { id: string; title: string; icon: string }[] = [
  { id: "proofread", title: "Proofread", icon: "check" },
  { id: "friendly", title: "Friendly", icon: "chat" },
  { id: "professional", title: "Professional", icon: "doc" },
  { id: "concise", title: "Concise", icon: "fork" },
  { id: "summarize", title: "Summary", icon: "grid" },
  { id: "key-points", title: "Key points", icon: "clipboard" },
  { id: "explain", title: "Explain", icon: "sparkle" },
];

/** Writing Tools: what is selected, the tools as tiles, then the answer with Replace or Copy. Everything happens in Rust; this draws it. */
export function Writing({ register }: { register: (handle: WritingHandle) => void }) {
  const [view, setView] = useState<WritingView | null>(null);
  const [question, setQuestion] = useState("");
  const [copied, setCopied] = useState(false);
  const field = useRef<HTMLInputElement>(null);
  useEffect(() => {
    const stop = listen<WritingView>("writing-view", (payload) => setView(payload));
    return () => { stop.then((f) => f()); };
  }, []);
  useEffect(() => {
    register({
      open: () => {
        setQuestion("");
        setCopied(false);
        invoke<WritingView>("writing_view").then(setView);
        setTimeout(() => field.current?.focus(), 60);
      },
    });
  }, [register]);
  const run = (tool: string, q?: string) => {
    setCopied(false);
    invoke("writing_run", { tool, question: q ?? null }).catch(() => undefined);
  };
  const working = view?.phase === "working";
  const empty = !view || view.chars === 0;
  return (
    <>
      <div className="wt-head">
        <span className="wt-orb"><Icon name="sparkle" /></span>
        <div className="wt-title">
          <h3>Writing Tools</h3>
          <div className="wt-source" data-test="writing-source">
            {empty ? "Select some text in any app, then open Writing Tools." : `“${view?.excerpt}”`}
          </div>
        </div>
      </div>
      <div className="wt-ask">
        <input
          ref={field}
          data-test="writing-ask"
          placeholder="Ask about this, or describe a change…"
          autoComplete="off"
          value={question}
          disabled={empty || working}
          onChange={(event) => setQuestion(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === "Enter" && question.trim()) {
              event.preventDefault();
              run("ask", question.trim());
            }
          }}
        />
      </div>
      <div className="wt-tools">
        {TOOLS.map((tool) => (
          <button
            key={tool.id}
            type="button"
            className={`wt-tool${view?.tool === tool.id ? " current" : ""}`}
            data-test={`writing-${tool.id}`}
            disabled={empty || working}
            onClick={() => run(tool.id)}
          >
            <Icon name={tool.icon} />
            <span>{tool.title}</span>
          </button>
        ))}
      </div>
      {view && view.phase !== "ready" ? (
        <div className={`wt-result${working ? " working" : ""}`} data-test="writing-result" data-phase={view.phase}>
          {view.phase === "failed" ? <div className="wt-error">{view.error}</div> : <div className="wt-text">{view.result || "Writing…"}</div>}
        </div>
      ) : null}
      {view && view.phase === "done" ? (
        <div className="wt-actions">
          <span className="wt-note">{view.local ? "On this computer" : "Answered online"}</span>
          <button type="button" data-test="writing-copy" onClick={() => invoke("writing_copy").then(() => setCopied(true)).catch(() => undefined)}>
            {copied ? "Copied" : "Copy"}
          </button>
          {view.replaceable ? (
            <button type="button" className="primary" data-test="writing-replace" onClick={() => invoke("writing_replace").catch(() => undefined)}>Replace</button>
          ) : null}
        </div>
      ) : null}
    </>
  );
}
