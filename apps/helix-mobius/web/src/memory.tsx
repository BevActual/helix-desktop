import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

interface Item { id: number; text: string; created: number }
export interface MemoryHandle { open: () => void }

/** What Mobius has been asked to remember: always visible, only the person adds to it, any line can go. Used with models on this computer only. */
export function Memory({ register }: { register: (handle: MemoryHandle) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [items, setItems] = useState<Item[]>([]);
  const [text, setText] = useState("");
  const [error, setError] = useState("");
  useEffect(() => {
    register({ open: () => { setError(""); setText(""); invoke<Item[]>("memory_list").then(setItems, () => setItems([])); dialog.current?.showModal(); } });
  }, [register]);
  const add = async () => {
    setError("");
    try { setItems(await invoke<Item[]>("memory_add", { text })); setText(""); } catch (reason) { setError(String(reason)); }
  };
  const remove = async (id: number) => setItems(await invoke<Item[]>("memory_remove", { id }));
  return <dialog ref={dialog} className="sheet" data-test="memory-panel" aria-labelledby="memory-title"><div className="sheet-body">
    <header className="sheet-head"><h3 id="memory-title">Memory</h3><p className="sheet-note">Things you asked Mobius to remember in every chat. Only you add to this list. It is kept on this computer and sent to models on this computer only, never to a cloud model.</p></header>
    <div className="sheet-scroll">
      <form className="memory-add" onSubmit={(e) => { e.preventDefault(); if (text.trim()) add(); }}>
        <input data-test="memory-input" value={text} placeholder="For example: I use Fedora with niri; answer in British English" maxLength={300} onChange={(e) => setText(e.target.value)} />
        <button type="submit" className="primary" data-test="memory-add" disabled={!text.trim()}>Remember</button>
      </form>
      {error && <p className="sheet-warning" role="alert">{error}</p>}
      {items.length === 0 ? <p className="sheet-note" data-test="memory-empty">Nothing yet. Try “/remember I prefer short answers” in any chat.</p>
        : <ul className="memory-list">{items.map((item) => <li key={item.id} data-test={`memory-item-${item.id}`}><span>{item.text}</span><button type="button" className="mini" aria-label={`Forget: ${item.text}`} data-test={`memory-forget-${item.id}`} onClick={() => remove(item.id)}>Forget</button></li>)}</ul>}
    </div>
    <footer className="sheet-actions"><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="memory-done" onClick={() => dialog.current?.close()}>Done</button></footer>
  </div></dialog>;
}
