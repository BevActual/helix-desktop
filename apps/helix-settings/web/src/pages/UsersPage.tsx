import { useEffect, useState } from "react";
import { invoke } from "@helix/ui";
import type { PageHandle } from "./RowsPage";

interface UserView { name: string; username: string; kind: string; picture: string | null; editable: boolean }

/** Users: the signed-in account's name and picture, kept by the account service, so the lock screen and the login screen show the same. */
export function UsersPage({ register, report }: { register: (handle: PageHandle) => void; report: (text: string) => void }) {
  const [user, setUser] = useState<UserView | null>(null);
  const [name, setName] = useState("");
  const [note, setNote] = useState("");
  const [busy, setBusy] = useState(false);
  const apply = (next: UserView) => { setUser(next); setName(next.name); };
  useEffect(() => {
    const refresh = () => { invoke<UserView>("users_view").then(apply, () => undefined); };
    register({ setRows: () => undefined, refresh });
    refresh();
  }, [register]);
  if (!user) return null;
  const run = (command: string, args?: Record<string, unknown>) => {
    setBusy(true);
    setNote("");
    invoke<UserView>(command, args).then(
      (next) => { setBusy(false); apply(next); },
      (error) => { setBusy(false); setNote(String(error)); report(`users: ${String(error)}`); },
    );
  };
  const initials = user.name.split(/\s+/).filter(Boolean).slice(0, 2).map((w) => w[0].toUpperCase()).join("") || "?";
  return (
    <>
      <div className="rows-card user-card">
        <div className="user-avatar" data-test="user-avatar" aria-label="Your picture">
          {user.picture ? <img src={user.picture} alt="" /> : <span>{initials}</span>}
        </div>
        <div className="user-text">
          <div className="setting-title" data-test="user-name-shown">{user.name}</div>
          <div className="setting-about">{user.username}{user.kind ? ` · ${user.kind}` : ""}</div>
          <div className="user-actions">
            <button type="button" data-test="user-choose" disabled={busy || !user.editable} onClick={() => run("users_choose_picture")}>Choose picture…</button>
            <button type="button" data-test="user-remove" disabled={busy || !user.editable || !user.picture} onClick={() => run("users_clear_picture")}>Remove</button>
          </div>
        </div>
      </div>
      <h4>Name</h4>
      <div className="rows-card">
        <div className="setting-row">
          <div className="setting-text">
            <div className="setting-title">Full name</div>
            <div className="setting-about">Shown on the lock screen and the login screen. Your username stays {user.username}.</div>
          </div>
          <input className="text-input" data-test="user-name" value={name} maxLength={64} disabled={!user.editable} onChange={(e) => setName(e.target.value)}
            onKeyDown={(e) => { if (e.key === "Enter" && name.trim() && name !== user.name) run("users_set_name", { name }); }}
            onBlur={() => { if (name.trim() && name !== user.name) run("users_set_name", { name }); }} />
        </div>
      </div>
      {note ? <div className="setting-about warn" data-test="user-note" role="alert">{note}</div> : null}
      {!user.editable ? <div className="setting-about note">The account service is not available, so this account cannot be changed here.</div> : null}
    </>
  );
}
