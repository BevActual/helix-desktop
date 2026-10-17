import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** The stage: the panel beside the chat where the work lives. Today it holds documents: the model drafts them (an `artifact:Title` block in a
 * reply), the person edits them here, every change is a version, and the open document goes to the model with the next message as a visible
 * chip. Later stages (code, image, vector, browser) share this panel; see docs/design/mobius-workspaces.md. */
interface Version { body: string; by: string; at: number }
interface Document { title: string; versions: Version[] }
interface Store { documents: Document[] }
export interface StageHooks {
  chatId: () => string;
  report: (error: unknown) => void;
  /** The open document changed (or closed: null); the page keeps its chip and the layout in step. */
  changed: (open: { title: string; text: string } | null) => void;
}
export interface StageHandle {
  /** Opens a document; `body` is the model's text for it when it is not stored yet. */
  open: (title: string, body?: string) => void;
  /** A reply finished: the page recorded its documents; show the newest text of the open one. */
  refresh: () => void;
  close: () => void;
}

export function Stage({ register, hooks }: { register: (handle: StageHandle) => void; hooks: StageHooks }) {
  const [store, setStore] = useState<Store>({ documents: [] });
  const [title, setTitle] = useState("");
  const [text, setText] = useState("");
  const [version, setVersion] = useState(-1);
  const [notice, setNotice] = useState("");
  const [saved, setSaved] = useState(true);
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const current = useRef({ title: "", text: "" });
  current.current = { title, text };

  const load = useCallback(async (want: string, body?: string) => {
    const chat = hooks.chatId();
    let next: Store = { documents: [] };
    try {
      if (body !== undefined) next = await invoke<Store>("stage_record", { chatId: chat, title: want, body, by: "model" });
      else next = await invoke<Store>("stage_get", { chatId: chat });
    } catch (reason) { hooks.report(reason); }
    setStore(next);
    const doc = next.documents.find((d) => d.title.toLowerCase() === want.toLowerCase());
    if (doc) {
      const latest = doc.versions[doc.versions.length - 1];
      setTitle(doc.title); setText(latest?.body ?? ""); setVersion(doc.versions.length - 1); setSaved(true);
      hooks.changed({ title: doc.title, text: latest?.body ?? "" });
    }
  }, [hooks]);

  useEffect(() => {
    register({
      open: (want, body) => { setNotice(""); load(want, body); },
      refresh: () => {
        const open = current.current.title;
        if (!open) return;
        invoke<Store>("stage_get", { chatId: hooks.chatId() }).then((next) => {
          setStore(next);
          const doc = next.documents.find((d) => d.title.toLowerCase() === open.toLowerCase());
          const latest = doc?.versions[doc.versions.length - 1];
          if (doc && latest && latest.body !== current.current.text) {
            setText(latest.body); setVersion(doc.versions.length - 1); setSaved(true);
            setNotice(latest.by === "model" ? "Mobius revised this document." : "");
            hooks.changed({ title: doc.title, text: latest.body });
          }
        }, () => undefined);
      },
      close: () => { setTitle(""); setText(""); hooks.changed(null); },
    });
  }, [register, load, hooks]);

  const edit = (value: string) => {
    setText(value); setSaved(false); setNotice("");
    hooks.changed({ title, text: value });
    clearTimeout(timer.current);
    // Kept as the person pauses, so a version is a thought rather than a keystroke.
    timer.current = setTimeout(async () => {
      try { setStore(await invoke<Store>("stage_record", { chatId: hooks.chatId(), title, body: value, by: "person" })); setSaved(true); }
      catch (reason) { hooks.report(reason); }
    }, 1500);
  };
  const doc = store.documents.find((d) => d.title === title);
  const versions = doc?.versions ?? [];
  const pick = (index: number) => { const v = versions[index]; if (!v) return; setVersion(index); setText(v.body); setSaved(false); hooks.changed({ title, text: v.body }); };
  if (!title) return null;
  return <aside className="stage" data-test="stage" aria-label="Document">
    <header>
      <strong data-test="stage-title">{title}</strong>
      <span className="stage-state" data-test="stage-state">{saved ? "Saved" : "Editing…"}</span>
      <span className="stage-spacer" />
      {versions.length > 1 && <select aria-label="Version" data-test="stage-version" value={version} onChange={(e) => pick(Number(e.target.value))}>
        {versions.map((v, i) => <option key={i} value={i}>{`Version ${i + 1} · ${v.by === "model" ? "Mobius" : "you"}`}</option>)}
      </select>}
      <button type="button" className="mini" onClick={() => invoke("chat_copy", { text })}>Copy</button>
      <button type="button" className="mini" data-test="stage-save" onClick={() => invoke("chat_reply_save", { content: text, defaultFilename: `${title.replace(/[^\w.-]+/g, "-")}.md` }).catch(hooks.report)}>Save as file</button>
      <button type="button" className="icon-btn" aria-label="Close the document" data-test="stage-close" onClick={() => { setTitle(""); setText(""); hooks.changed(null); }}>×</button>
    </header>
    {notice && <p className="stage-notice" role="status" data-test="stage-notice">{notice}</p>}
    <textarea data-test="stage-text" aria-label={`Edit ${title}`} spellCheck value={text} onChange={(e) => edit(e.target.value)} />
    <footer>{text.split(/\s+/).filter(Boolean).length} words · It goes to Mobius with your next message, so you can ask for changes to what is here.</footer>
  </aside>;
}

export function ArtifactCard({ title, body, writing, onOpen }: { title: string; body: string; writing: boolean; onOpen: () => void }) {
  const first = body.split("\n").filter((l) => l.trim()).slice(0, 3).join("\n");
  return <div className="artifact-card" data-test="artifact-card">
    <div className="artifact-head"><strong>{title}</strong><span>Document · {body.split(/\s+/).filter(Boolean).length} words</span>
      <button type="button" className="mini primary" data-test="artifact-open" disabled={writing} onClick={onOpen}>{writing ? "Writing…" : "Open"}</button></div>
    <pre>{first}{body.split("\n").length > 3 ? "\n…" : ""}</pre>
  </div>;
}
