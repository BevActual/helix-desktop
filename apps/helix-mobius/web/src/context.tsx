import { useEffect, useRef, useState } from "react";
import { invoke, listen } from "@helix/ui";

export interface ContextInfo {
  included_messages: number; omitted_messages: number;
  included_images: number; omitted_images: number;
  used_chars: number; budget_chars: number; truncated_chars: number;
  first_message: number | null;
  model_context_tokens?: number;
}
interface Snapshot { id: string; brief: string; context: ContextInfo; streaming: boolean; temporary: boolean }
export interface ContextHandle { open: (view: Snapshot) => void }

export function ChatContext({ register }: { register: (handle: ContextHandle) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const openedId = useRef("");
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [open, setOpen] = useState(false);
  const [brief, setBrief] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => {
    register({ open: (view) => { openedId.current = view.id; setSnapshot(view); setBrief(view.brief || ""); setError(""); setOpen(true); } });
  }, [register]);
  useEffect(() => {
    const stop = listen<Snapshot>("chat-view", (view) => {
      if (openedId.current && openedId.current !== view.id) setOpen(false);
      setSnapshot(view);
    });
    return () => { stop.then((fn) => fn()); };
  }, []);
  useEffect(() => {
    if (open && !dialog.current?.open) dialog.current?.showModal();
    if (!open && dialog.current?.open) dialog.current?.close();
  }, [open]);
  const count = Array.from(brief).length;
  const info = snapshot?.context;
  const changed = brief !== (snapshot?.brief || "");
  const save = async () => {
    if (!snapshot) return;
    setBusy(true); setError("");
    try { await invoke("chat_set_brief", { id: snapshot.id, brief }); setOpen(false); }
    catch (reason) { setError(String(reason)); }
    finally { setBusy(false); }
  };
  return <dialog ref={dialog} className="sheet chat-context-sheet" data-test="context-panel" aria-labelledby="context-title" onClose={() => setOpen(false)}>
    <div className="sheet-body">
      <header className="sheet-head"><h3 id="context-title">Chat context</h3><p className="sheet-note">Keep important goals and decisions available as your conversation grows.</p></header>
      <div className="sheet-scroll">
        {info && <section className="context-history" aria-label="Recent history">
          <h4>Recent history</h4>
          <div className="context-stats"><div><strong>{info.included_messages}</strong><span>messages included</span></div><div><strong>{info.omitted_messages}</strong><span>earlier messages outside context</span></div></div>
          <div className="context-ring-row" aria-hidden="true"><span className="context-ring context-ring-lg" style={{ "--context-fill": String(info.model_context_tokens ? Math.min(1, info.used_chars / (info.model_context_tokens * 4)) : Math.min(1, info.used_chars / info.budget_chars)) } as React.CSSProperties} /><span>{info.model_context_tokens ? `${Math.round(100 * info.used_chars / (info.model_context_tokens * 4))}% of model window (~${info.model_context_tokens.toLocaleString()} tokens)` : `${Math.round(100 * info.used_chars / info.budget_chars)}% of recent-history budget`}</span></div>
          <progress value={info.used_chars} max={info.budget_chars} aria-label="Recent history space used" />
          <p className="sheet-note">{info.used_chars.toLocaleString()} of {info.budget_chars.toLocaleString()} characters used for recent history.</p>
          {info.omitted_messages > 0 && <p className="context-notice" data-test="context-history-notice">Earlier messages stay saved in this chat, but are not currently included. Add the details you still need to the brief below.</p>}
          {info.truncated_chars > 0 && <p className="context-notice">The latest included message is shortened by {info.truncated_chars.toLocaleString()} characters.</p>}
          {info.included_images + info.omitted_images > 0 && <p className="sheet-note">Attached pictures: {info.included_images} included, {info.omitted_images} outside context.</p>}
          <p className="sheet-note">Your next message also needs room, so the included history can change when you send. A reply still being written is excluded here.</p>
        </section>}
        <section className="context-brief">
          <label className="sheet-field" htmlFor="context-brief">Chat brief<textarea id="context-brief" data-test="context-brief" value={brief} onChange={(event) => setBrief(event.target.value)} maxLength={8000} disabled={busy || snapshot?.streaming} placeholder="Goal: …\nDecisions: …\nConstraints or preferences: …" aria-describedby="context-brief-help context-brief-count" /></label>
          <div className="context-brief-meta"><span id="context-brief-count" className={count > 2000 ? "context-over-limit" : ""}>{count.toLocaleString()} / 2,000 characters</span><button type="button" className="mini" data-test="context-brief-clear" disabled={!brief || busy || snapshot?.streaming} onClick={() => setBrief("")}>Clear brief</button></div>
          <p id="context-brief-help" className="sheet-note">You write this brief. Mobius includes it with every text chat reply in this conversation, separately from recent history. It stays with a forked chat.</p>
          {snapshot?.temporary && <p className="context-notice">This is a temporary chat. Its brief is not saved to disk.</p>}
          {snapshot?.streaming && <p className="sheet-note" role="status" data-test="context-streaming-notice">Wait for the reply to finish, or stop it, to edit the brief.</p>}
        </section>
        {error && <p className="sheet-warning" role="alert">{error}</p>}
      </div>
      <footer className="sheet-actions"><button type="button" data-test="context-close" disabled={busy} onClick={() => setOpen(false)}>{changed ? "Cancel" : "Done"}</button><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="context-save" disabled={busy || snapshot?.streaming || count > 2000 || !changed} onClick={save}>{busy ? "Saving…" : "Save brief"}</button></footer>
    </div>
  </dialog>;
}
