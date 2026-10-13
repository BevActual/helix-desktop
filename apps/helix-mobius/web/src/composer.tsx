import { useEffect, useRef, useState } from "react";
import { flushSync } from "react-dom";

export interface Attachment { name: string; path: string; chars: number; original_chars: number; included_chars: number; content: string; }
export interface SlashItem { cmd: string; desc: string; }

export interface ChipsHandle { set: (files: Attachment[], warnings: string[]) => void; }
export interface SlashHandle { set: (items: SlashItem[], active: number) => void; }

/** The composer's attachment chips. The composer's own state (the text, the files, what is queued) stays with the page; this draws the
 * chips it was given and says which one was removed. */
declare function icon(name: string): SVGElement;
function AttachmentIcon() {
  const host = useRef<HTMLSpanElement>(null);
  useEffect(() => { host.current?.replaceChildren(icon("paperclip")); }, []);
  return <span ref={host} className="attachment-file-icon" aria-hidden="true" />;
}

export function AttachChips({ register, onRemove, onDismiss }: { register: (handle: ChipsHandle) => void; onRemove: (index: number) => void; onDismiss: () => void }) {
  const [files, setFiles] = useState<Attachment[]>([]);
  const [warnings, setWarnings] = useState<string[]>([]);
  const [selected, setSelected] = useState("");
  const dialog = useRef<HTMLDialogElement>(null);
  const file = files.find((item) => item.path === selected);
  useEffect(() => {
    register({ set: (next, notices) => flushSync(() => { setFiles(next); setWarnings(notices); }) });
  }, [register]);
  useEffect(() => {
    if (file && !dialog.current?.open) dialog.current?.showModal();
    if (!file && dialog.current?.open) dialog.current?.close();
  }, [file]);
  const count = (value: number) => value.toLocaleString();
  const status = (item: Attachment) => item.original_chars === 0
    ? "Empty file"
    : item.included_chars === 0
      ? "Not included · attachment space is full"
    : item.included_chars < item.original_chars
      ? `Excerpt · ${count(item.included_chars)} of ${count(item.original_chars)} characters`
      : `${count(item.included_chars)} characters · full text`;
  const shortened = files.some((item) => item.included_chars < item.original_chars);
  return <>
    <div className="attachment-cards">
      {files.map((item, index) => <div key={item.path} className={`attachment-card${item.included_chars < item.original_chars ? " excerpt" : ""}`} data-test={`attach-chip-${index}`}>
        <button type="button" className="attachment-preview-open" data-test={`attach-preview-${index}`} aria-label={`Preview ${item.name}`} onClick={() => setSelected(item.path)}>
          <AttachmentIcon />
          <span className="attachment-file-label"><strong title={item.name}>{item.name}</strong><small data-test={`attach-status-${index}`}>{status(item)}</small></span>
        </button>
        <button type="button" className="attachment-remove" aria-label={`Remove ${item.name}`} title={`Remove ${item.name}`} data-test={`attach-remove-${index}`} onClick={() => onRemove(index)}>×</button>
      </div>)}
    </div>
    {shortened && <p className="attachment-note" role="status" data-test="attach-excerpt-note">Some files are excerpts. Preview a file to see the text included in your message. Remove earlier files to make room.</p>}
    {warnings.length > 0 && <div className="attachment-notices" role="status" data-test="attach-warnings"><div className="attachment-notices-head"><strong>Some files weren’t added</strong><button type="button" className="mini" data-test="attach-warnings-dismiss" onClick={onDismiss}>Dismiss</button></div><ul>{warnings.map((warning, index) => <li key={index}>{warning}</li>)}</ul></div>}
    <dialog ref={dialog} className="sheet attachment-preview-sheet" data-test="attachment-preview-panel" aria-labelledby="attachment-preview-title" onClose={() => setSelected("")}>
      <div className="sheet-body">
        <header className="sheet-head"><h3 id="attachment-preview-title">{file?.name}</h3><p className="sheet-note" data-test="attachment-preview-status">{file ? status(file) : ""}</p></header>
        <div className="sheet-scroll"><p className="sheet-note">This is the file text that will be included when you send. Your selected chat model receives this text.</p><pre className="attachment-preview-text" data-test="attachment-preview-text">{file ? Array.from(file.content).slice(0, file.included_chars).join("") : ""}</pre></div>
        <footer className="sheet-actions"><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="attachment-preview-close" onClick={() => setSelected("")}>Done</button></footer>
      </div>
    </dialog>
  </>;
}

/** The slash-command menu: the matching commands and which one is highlighted. Choosing one is a mousedown (so the textarea keeps
 * focus), reported by index. */
export function SlashMenu({ register, onPick }: { register: (handle: SlashHandle) => void; onPick: (index: number) => void }) {
  const [items, setItems] = useState<SlashItem[]>([]);
  const [active, setActive] = useState(0);
  useEffect(() => {
    register({ set: (next, index) => flushSync(() => { setItems(next); setActive(index); }) });
  }, [register]);
  return (
    <>
      {items.map((item, index) => (
        <button
          key={item.cmd}
          type="button"
          className={`slash-item${index === active ? " active" : ""}`}
          data-test={`slash-item-${index}`}
          onMouseDown={(event) => { event.preventDefault(); onPick(index); }}
        >
          <span className="slash-cmd">{item.cmd}</span>
          <span className="slash-desc">{item.desc}</span>
        </button>
      ))}
    </>
  );
}
