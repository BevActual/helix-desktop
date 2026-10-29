import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { invoke, listen } from "@helix/ui";
import { Icon } from "./icon";

/** What Rust says about the Magic Pointer (`pointer.rs`). */
export interface PointerView {
  at: [number, number] | null;
  app: string;
  title: string;
  window: boolean;
  selection: string;
  offers: { id: string; title: string; app: string; confirm: boolean }[];
  ask: string;
  phase: "ready" | "working" | "done" | "failed";
  result: string;
  error: string;
  done: string;
  undoable: boolean;
  local: boolean;
}

export interface PointerHandle {
  open: () => void;
}

const WRITING: { id: string; title: string }[] = [
  { id: "proofread", title: "Proofread" },
  { id: "summarize", title: "Summarize" },
  { id: "explain", title: "Explain" },
];

/** Beside the pointer, at the corner nearest it, which stays put as the answer grows: below and right of the pointer, or above and left of
 *  it near an edge (whichever side has room). Never over the bar or off screen; a long answer scrolls inside the bubble. */
function place(host: HTMLElement, at: [number, number] | null) {
  const gap = 14, margin = 12;
  const sw = window.innerWidth, sh = window.innerHeight;
  const bar = document.getElementById("bar");
  const ceiling = Math.max(margin, (bar && bar.offsetHeight ? bar.getBoundingClientRect().bottom : 0) + 8);
  host.style.maxHeight = "none";
  const w = host.offsetWidth, h = host.offsetHeight;
  if (!at) {
    host.style.transformOrigin = "top center";
    host.style.left = `${Math.round((sw - w) / 2)}px`;
    host.style.top = `${Math.round(Math.max(ceiling, sh * 0.2))}px`;
    host.style.bottom = "auto";
    host.style.maxHeight = `${Math.round(sh - margin - Math.max(ceiling, sh * 0.2))}px`;
    return;
  }
  let x = at[0] + gap;
  const left = x + w > sw - margin;
  if (left) x = at[0] - gap - w;
  x = Math.min(Math.max(x, margin), Math.max(margin, sw - w - margin));
  const below = sh - margin - (at[1] + gap), above = at[1] - gap - ceiling;
  const down = h <= below || below >= above;
  host.style.transformOrigin = `${left ? "right" : "left"} ${down ? "top" : "bottom"}`;
  host.style.left = `${Math.round(x)}px`;
  if (down) {
    host.style.top = `${Math.round(Math.max(ceiling, at[1] + gap))}px`;
    host.style.bottom = "auto";
    host.style.maxHeight = `${Math.round(below)}px`;
  } else {
    host.style.top = "auto";
    host.style.bottom = `${Math.round(sh - (at[1] - gap))}px`;
    host.style.maxHeight = `${Math.round(above)}px`;
  }
}

/** The Magic Pointer's bubble: what is under the pointer, what can be done with it, and the answer. Everything happens in Rust. */
export function Pointer({ host, register }: { host: HTMLElement; register: (handle: PointerHandle) => void }) {
  const [view, setView] = useState<PointerView | null>(null);
  // Placed once per opening, where the pointer was (not again as the answer grows, so the bubble does not jump under the pointer).
  const placedFor = useRef<string>("");
  const [question, setQuestion] = useState("");
  const [confirming, setConfirming] = useState("");
  const [copied, setCopied] = useState(false);
  const field = useRef<HTMLInputElement>(null);
  useEffect(() => {
    const stop = listen<PointerView>("pointer-view", (payload) => setView(payload));
    return () => { stop.then((f) => f()); };
  }, []);
  useEffect(() => {
    register({
      open: () => {
        setQuestion("");
        setConfirming("");
        setCopied(false);
        placedFor.current = "";
        invoke<PointerView>("pointer_view").then(setView);
        setTimeout(() => field.current?.focus(), 60);
      },
    });
  }, [register]);
  useLayoutEffect(() => {
    if (!view) return;
    const key = JSON.stringify(view.at) + view.app + view.title;
    if (placedFor.current !== key) {
      placedFor.current = key;
      place(host, view.at);
    }
  });
  const ask = (kind: string, q?: string) => {
    setCopied(false);
    setConfirming("");
    invoke("pointer_ask", { ask: kind, question: q ?? null }).catch(() => undefined);
  };
  const offer = (id: string, confirm: boolean) => {
    if (confirm && confirming !== id) { setConfirming(id); return; }
    setConfirming("");
    invoke("pointer_offer", { id }).catch(() => undefined);
  };
  const working = view?.phase === "working";
  // An answer (or what an action did) takes the bubble's place; the back button returns to the menu.
  const answering = !!view && (!!view.ask || !!view.done || view.phase === "failed") && view.phase !== "ready";
  const where = view ? (view.app || view.title ? [view.app, view.title].filter(Boolean).join(" — ") : "The desktop") : "";
  const back = () => {
    setCopied(false);
    invoke("pointer_back").catch(() => undefined);
    setTimeout(() => field.current?.focus(), 60);
  };
  return (
    <>
      <div className="mp-head">
        {answering && !working ? (
          <button type="button" className="mp-back" data-test="pointer-back" aria-label="Back" onClick={back}><Icon name="chevron-left" /></button>
        ) : (
          <span className="mp-orb"><Icon name="mouse" /></span>
        )}
        <div className="mp-where" data-test="pointer-where">{where}</div>
      </div>
      {answering && view ? (
        <>
          {view.ask ? (
            <div className={`wt-result mp-result${working ? " working" : ""}`} data-test="pointer-result" data-phase={view.phase}>
              {view.phase === "failed" ? <div className="wt-error">{view.error}</div> : <div className="wt-text">{view.result || (view.ask === "text" ? "Reading…" : "Looking…")}</div>}
            </div>
          ) : (
            <div className="mp-done" data-test="pointer-done" data-phase={view.phase}>
              {view.phase === "failed" ? <span className="wt-error">{view.error}</span> : <><Icon name="check" /><span>{view.done}</span></>}
              {view.undoable ? (
                <button type="button" data-test="pointer-undo" onClick={() => invoke("pointer_undo").catch(() => undefined)}>Undo</button>
              ) : null}
            </div>
          )}
          {view.ask && view.phase === "done" ? (
            <div className="wt-actions">
              <span className="wt-note">{view.local ? "On this computer" : "Answered online"}</span>
              {view.ask === "text" ? (
                <span className="mp-copied" data-test="pointer-copied">Copied</span>
              ) : (
                <button type="button" data-test="pointer-copy" onClick={() => invoke("pointer_copy").then(() => setCopied(true)).catch(() => undefined)}>
                  {copied ? "Copied" : "Copy"}
                </button>
              )}
            </div>
          ) : null}
        </>
      ) : (
        <>
          {view?.window ? (
            <div className="mp-ask">
              <input
                ref={field}
                data-test="pointer-ask"
                placeholder="Ask about this…"
                autoComplete="off"
                value={question}
                onChange={(event) => setQuestion(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === "Enter" && question.trim()) {
                    event.preventDefault();
                    ask("ask", question.trim());
                  }
                }}
              />
            </div>
          ) : null}
          <div className="mp-list">
            {view?.window ? (
              <>
                <button type="button" className="mp-row" data-test="pointer-describe" onClick={() => ask("describe")}>
                  <Icon name="eye" /><span>What’s this?</span>
                </button>
                <button type="button" className="mp-row" data-test="pointer-text" onClick={() => ask("text")}>
                  <Icon name="doc" /><span>Copy text</span>
                </button>
              </>
            ) : null}
            {view?.offers.map((o) => (
              <button key={o.id} type="button" className={`mp-row${confirming === o.id ? " confirming" : ""}`} data-test={`pointer-offer-${o.id}`} onClick={() => offer(o.id, o.confirm)}>
                <Icon name="plus" />
                <span>{confirming === o.id ? `${o.title}? Click again` : o.title}</span>
                <span className="mp-app">{o.app}</span>
              </button>
            ))}
          </div>
          {view?.selection ? (
            <div className="mp-writing">
              <div className="mp-label"><Icon name="sparkle" /><span>“{view.selection}”</span></div>
              <div className="mp-chips">
                {WRITING.map((t) => (
                  <button key={t.id} type="button" data-test={`pointer-writing-${t.id}`} onClick={() => invoke("pointer_writing", { tool: t.id }).catch(() => undefined)}>{t.title}</button>
                ))}
                <button type="button" data-test="pointer-writing-more" onClick={() => invoke("pointer_writing", { tool: null }).catch(() => undefined)}>More…</button>
              </div>
            </div>
          ) : null}
          {!view?.window && !view?.selection && !view?.offers.length ? (
            <div className="mp-empty">Point at a window, or select some text, then press the pointer key.</div>
          ) : null}
        </>
      )}
    </>
  );
}
