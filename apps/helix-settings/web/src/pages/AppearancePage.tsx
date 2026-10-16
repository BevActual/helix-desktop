import { useState } from "react";
import { invoke, type RowView } from "@helix/ui";
import { RowsPage, type PageHandle, type PageSpec } from "./RowsPage";

/** Appearance rows, plus the action that copies the current theme onto the login screen. */
export function AppearancePage({
  spec,
  register,
  initialRows,
  onRows,
  report,
}: {
  spec: PageSpec;
  register: (handle: PageHandle) => void;
  initialRows: RowView[] | null;
  onRows: (rows: RowView[]) => void;
  report: (text: string) => void;
}) {
  const [note, setNote] = useState("");
  const [busy, setBusy] = useState(false);

  const sync = () => {
    if (busy) return;
    setBusy(true);
    setNote("Syncing the login screen…");
    invoke("greeter_sync").then(
      () => {
        setBusy(false);
        setNote("Login screen updated.");
      },
      (error) => {
        setBusy(false);
        setNote(String(error));
        report(String(error));
      },
    );
  };

  return (
    <>
      <RowsPage spec={spec} initialRows={initialRows} register={register} onRows={onRows} report={report} />
      <div className="setting-about">
        <button className="primary" type="button" data-test="greeter-sync" disabled={busy} onClick={sync}>
          Sync login screen
        </button>
        <p className="note" data-test="greeter-sync-note">
          {note || "Copies the current theme and wallpaper to the login screen. An administrator password is asked when those files are not writable."}
        </p>
      </div>
    </>
  );
}
