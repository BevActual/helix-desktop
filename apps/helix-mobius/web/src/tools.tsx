import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";
interface Snapshot { id: string; tools_enabled: boolean; workspace: string; streaming: boolean; craft?: string }
/** Named operations of one craft. The buttons a person presses and the steps a model proposes are this same list. A terminal command always waits, the way a terminal agent does. */
const CRAFTS: Record<string, { title: string; note: string; tools: [string, string][] }> = {
  write: {
    title: "Write",
    note: "Suggestions land on the document. Nothing changes until you accept.",
    tools: [
      ["Replace", "One passage, or every copy of it"],
      ["Delete", "Remove a passage"],
      ["Insert", "Add text after a line"],
      ["Replace section", "Rewrite everything under a heading"],
      ["Append", "Add to the end"],
    ],
  },
  image: {
    title: "Image",
    note: "Suggestions are previewed on the picture. The original stays one undo away.",
    tools: [
      ["Crop", "Keep a rectangle"],
      ["Resize", "By percent or by size"],
      ["Rotate", "Quarter turns"],
      ["Flip", "Left to right, or top to bottom"],
      ["Adjust", "Brightness, contrast, black and white, blur, sharpen"],
      ["Box", "Draw a rectangle in a chosen colour"],
    ],
  },
  code: {
    title: "Code",
    note: "A file edit shows its diff first. A command runs only after you allow it, and only inside the project folder.",
    tools: [
      ["Read", "The file in view goes with your next message"],
      ["Edit", "Replace one exact passage, then undo if the file is unchanged since"],
      ["Terminal", "A command in the folder. No network, no home folder. Always asks"],
    ],
  },
};
export interface ToolsHandle { open: (view: Snapshot) => void }
export function ChatTools({ register }: { register: (handle: ToolsHandle) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [view, setView] = useState<Snapshot | null>(null);
  const [enabled, setEnabled] = useState(false);
  const [folder, setFolder] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [rules, setRules] = useState<{ file: string; text: string; total_chars: number } | null>(null);
  useEffect(() => {
    if (!folder) { setRules(null); return; }
    invoke<{ file: string; text: string; total_chars: number } | null>("tools_rules", { folder }).then(setRules, () => setRules(null));
  }, [folder]);
  useEffect(() => { register({ open: (snapshot) => { setView(snapshot); setEnabled(snapshot.tools_enabled); setFolder(snapshot.workspace || ""); setError(""); dialog.current?.showModal(); } }); }, [register]);
  const choose = async () => { setBusy(true); setError(""); try { const path = await invoke<string | null>("tools_pick_folder"); if (path) setFolder(path); } catch (reason) { setError(String(reason)); } finally { setBusy(false); } };
  const save = async () => { if (!view) return; setBusy(true); setError(""); try { await invoke("tools_configure", { chatId: view.id, enabled, workspace: folder }); dialog.current?.close(); } catch (reason) { setError(String(reason)); } finally { setBusy(false); } };
  return <dialog ref={dialog} className="sheet" data-test="tools-panel" aria-labelledby="tools-title"><div className="sheet-body">
    <header className="sheet-head"><h3 id="tools-title">Tools for this chat</h3></header>
    <div className="sheet-scroll">
      <label className="tools-row">
        <span className="tools-row-text"><strong>Research and project tools</strong><span>Search the web, read pages, and run commands in a folder. You approve every lookup and command first.</span></span>
        <input type="checkbox" className="switch" data-test="tools-enabled" checked={enabled} disabled={busy || view?.streaming} onChange={(event) => setEnabled(event.target.checked)} />
      </label>
      <div className="tools-row tools-folder">
        <span className="tools-row-text"><strong>Project folder</strong><code data-test="tools-folder">{folder || "None: web only"}</code></span>
        <span className="tools-row-actions"><button type="button" data-test="tools-choose" disabled={busy || view?.streaming} onClick={choose}>Choose…</button><button type="button" disabled={!folder || busy || view?.streaming} onClick={() => setFolder("")}>Clear</button></span>
      </div>
      {rules && <details className="tools-rules" data-test="tools-rules">
        <summary><strong>Project rules</strong> · {rules.file} · {rules.total_chars.toLocaleString()} characters{rules.total_chars > rules.text.length ? " (the first 4,000 are used)" : ""}</summary>
        <pre>{rules.text}</pre>
        <p className="sheet-note">The project's own guidance for assistants. Tools mode follows it as guidance; every command and edit still waits for your review.</p>
      </details>}
      <p className="sheet-note">Commands can read and change this folder. They get no network, home folder or desktop session. Tool results are sent to the chat model. A terminal command always waits for you, and you can stop it while it runs.</p>
      {CRAFTS[view?.craft || ""] && <section className="craft-tools" data-test="craft-tools">
        <h4>{CRAFTS[view?.craft || ""].title}</h4>
        <p className="sheet-note">{CRAFTS[view?.craft || ""].note}</p>
        <ul>{CRAFTS[view?.craft || ""].tools.map(([name, about]) => <li key={name}><strong>{name}</strong><span>{about}</span></li>)}</ul>
      </section>}
      <details className="tools-desk">
        <summary>Agent desk</summary>
        <p className="sheet-note">Desktop changes still use Act. With agent-desk placement on, the first approved command also opens a terminal there.</p>
        <div className="sheet-actions">
          <button type="button" data-test="tools-show-desk" disabled={busy} onClick={() => invoke("agent_desk_focus").catch((reason) => setError(String(reason)))}>Show agent desk</button>
          <button type="button" data-test="tools-open-desk-terminal" disabled={busy || !folder} onClick={() => invoke("tools_open_desk_terminal").catch((reason) => setError(String(reason)))}>Open folder on agent desk</button>
          <button type="button" data-test="tools-agent-settings" disabled={busy} onClick={() => invoke("open_settings_agents").catch((reason) => setError(String(reason)))}>Agent desk settings</button>
        </div>
      </details>
      {view?.streaming && <p role="status">Stop or finish the current task before changing its tools.</p>}
      {error && <p className="sheet-warning" role="alert">{error}</p>}
    </div><footer className="sheet-actions"><button type="button" disabled={busy} onClick={() => dialog.current?.close()}>Cancel</button><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="tools-save" disabled={busy || view?.streaming} onClick={save}>{busy ? "Saving…" : "Save"}</button></footer>
  </div></dialog>;
}
