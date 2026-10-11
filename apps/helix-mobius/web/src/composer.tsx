import { useEffect, useState } from "react";
import { flushSync } from "react-dom";

export interface Attachment { name: string; chars: number; }
export interface SlashItem { cmd: string; desc: string; }

export interface ChipsHandle { set: (files: Attachment[]) => void; }
export interface SlashHandle { set: (items: SlashItem[], active: number) => void; }

/** The composer's attachment chips. The composer's own state (the text, the files, what is queued) stays with the page; this draws the
 * chips it was given and says which one was removed. */
export function AttachChips({ register, onRemove }: { register: (handle: ChipsHandle) => void; onRemove: (index: number) => void }) {
  const [files, setFiles] = useState<Attachment[]>([]);
  useEffect(() => {
    register({ set: (next) => flushSync(() => setFiles(next)) });
  }, [register]);
  return (
    <>
      {files.map((file, index) => (
        <div key={`${file.name}:${index}`} className="attach-chip" data-test={`attach-chip-${index}`}>
          <span>{file.name} · {file.chars} ch</span>
          <button type="button" title="Remove" data-test={`attach-remove-${index}`} onClick={() => onRemove(index)}>×</button>
        </div>
      ))}
    </>
  );
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
