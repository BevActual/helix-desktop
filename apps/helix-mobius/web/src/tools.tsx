import { useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";
interface Snapshot { id: string; tools_enabled: boolean; workspace: string; streaming: boolean }
export interface ToolsHandle { open: (view: Snapshot) => void }
export function ChatTools({ register }: { register: (handle: ToolsHandle) => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [view, setView] = useState<Snapshot | null>(null);
  const [enabled, setEnabled] = useState(false);
  const [folder, setFolder] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => { register({ open: (snapshot) => { setView(snapshot); setEnabled(snapshot.tools_enabled); setFolder(snapshot.workspace || ""); setError(""); dialog.current?.showModal(); } }); }, [register]);
  const choose = async () => { setBusy(true); setError(""); try { const path = await invoke<string | null>("tools_pick_folder"); if (path) setFolder(path); } catch (reason) { setError(String(reason)); } finally { setBusy(false); } };
  const save = async () => { if (!view) return; setBusy(true); setError(""); try { await invoke("tools_configure", { chatId: view.id, enabled, workspace: folder }); dialog.current?.close(); } catch (reason) { setError(String(reason)); } finally { setBusy(false); } };
  return <dialog ref={dialog} className="sheet" data-test="tools-panel" aria-labelledby="tools-title"><div className="sheet-body">
    <header className="sheet-head"><h3 id="tools-title">Tools for this chat</h3><p className="sheet-note">Each send can search the web, read pages, and optionally run shell commands in a project folder. Steps appear in the thread; you approve lookups and commands before they run.</p></header>
    <div className="sheet-scroll">
      <label className="sheet-field"><span><input type="checkbox" data-test="tools-enabled" checked={enabled} disabled={busy || view?.streaming} onChange={(event) => setEnabled(event.target.checked)} /> Enable Tools mode</span></label>
      <p>With Tools on, Mobius plans in the background, then answers with source links. Desktop changes still use <strong>Act</strong>. When agent-desk placement is on, the first approved shell command also opens a terminal on the agent desk (sandboxed runs still happen in the background).</p>
      <div className="sheet-actions">
        <button type="button" data-test="tools-show-desk" disabled={busy} onClick={() => invoke("agent_desk_focus").catch((reason) => setError(String(reason)))}>Show agent desk</button>
        <button type="button" data-test="tools-open-desk-terminal" disabled={busy || !folder} onClick={() => invoke("tools_open_desk_terminal").catch((reason) => setError(String(reason)))}>Open folder on agent desk</button>
        <button type="button" data-test="tools-agent-settings" disabled={busy} onClick={() => invoke("open_settings_agents").catch((reason) => setError(String(reason)))}>Agent desk settings</button>
      </div>
      <label className="sheet-field">Project folder (optional for web-only)<code data-test="tools-folder">{folder || "No folder selected"}</code></label>
      <div className="sheet-actions"><button type="button" data-test="tools-choose" disabled={busy || view?.streaming} onClick={choose}>Choose folder</button><button type="button" disabled={!folder || busy || view?.streaming} onClick={() => setFolder("")}>Clear</button></div>
      <p className="sheet-note">Commands can read and change this folder. You review the exact command before every run. Commands have no network access, home folder, or desktop session.</p>
      <p className="sheet-note">Web requests using attachments or command results also need review. Tool results are sent to the selected chat model.</p>
      {view?.streaming && <p role="status">Stop or finish the current task before changing its tools.</p>}
      {error && <p className="sheet-warning" role="alert">{error}</p>}
    </div><footer className="sheet-actions"><button type="button" disabled={busy} onClick={() => dialog.current?.close()}>Cancel</button><span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="tools-save" disabled={busy || view?.streaming} onClick={save}>{busy ? "Saving…" : "Save tools"}</button></footer>
  </div></dialog>;
}
