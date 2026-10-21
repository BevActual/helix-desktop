import { useEffect, useState } from "react";
import { invoke, type RowView } from "@helix/ui";
import { RowsPage, type PageHandle, type PageSpec } from "./RowsPage";

const ACCENTS: [string, string][] = [["blue", "Blue"], ["purple", "Purple"], ["pink", "Pink"], ["red", "Red"], ["orange", "Orange"], ["yellow", "Yellow"], ["green", "Green"], ["graphite", "Graphite"]];

/** The accent colour: a row of swatches. The choice is the desktop service's `theme.accent`, which every Helix application follows. */
function AccentRow({ report }: { report: (text: string) => void }) {
  const [current, setCurrent] = useState("blue");
  useEffect(() => { invoke<string>("accent_get").then(setCurrent, () => undefined); }, []);
  const pick = (name: string) => {
    const before = current;
    setCurrent(name);
    invoke("accent_set", { name }).catch((error) => { setCurrent(before); report(`accent: ${String(error)}`); });
  };
  return (
    <>
      <h4>Accent colour</h4>
      <div className="rows-card">
        <div className="setting-row stacked">
          <div className="setting-text">
            <div className="setting-title">Accent</div>
            <div className="setting-about">The colour of buttons, switches and selections in Helix applications.</div>
          </div>
          <div className="accent-swatches" role="radiogroup" aria-label="Accent colour">
            {ACCENTS.map(([name, label]) => (
              <button key={name} type="button" role="radio" aria-checked={current === name} aria-label={label} title={label} className="accent-swatch" data-accent={name} data-test={`accent-${name}`} onClick={() => pick(name)} />
            ))}
          </div>
        </div>
      </div>
    </>
  );
}

/** Appearance rows, the accent colour, and the action that copies the current theme onto the login screen. */
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
      <AccentRow report={report} />
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
