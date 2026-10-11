import { useCallback, useEffect, useState } from "react";
import { RowGroups, type Change, type RowView } from "@helix/ui";

/** What differs between the settings pages that are only rows. */
export interface PageSpec {
  /** Reads the rows from a command. Pages whose rows come from the service's list are given them by the host page instead. */
  load?: () => Promise<RowView[]>;
  /** Sends a change. A command that answers with the new rows returns them; otherwise rows arrive from the host (`setRows`). */
  change?: (id: string, change: Change) => Promise<RowView[] | void>;
  /** Shown when there are no rows. */
  empty?: string;
  /** A line under the rows. */
  note?: string;
}

export interface PageHandle {
  setRows: (rows: RowView[]) => void;
  refresh: () => void;
}

export function RowsPage({ spec, initialRows, register, onRows, report }: { spec: PageSpec; initialRows: RowView[] | null; register: (handle: PageHandle) => void; onRows: (rows: RowView[]) => void; report: (text: string) => void }) {
  const [rows, setRows] = useState<RowView[] | null>(initialRows);

  const apply = useCallback(
    (next: RowView[]) => {
      setRows(next);
      onRows(next);
    },
    [onRows],
  );

  useEffect(() => {
    const refresh = () => {
      spec.load?.().then(apply, (error) => report(`load: ${String(error)}`));
    };
    register({ setRows: apply, refresh });
    refresh();
  }, [spec, apply, register, report]);

  const change = (id: string, change: Change) => {
    spec.change?.(id, change).then(
      (next) => {
        if (next) apply(next);
      },
      (error) => report(`${id}: ${String(error)}`),
    );
  };

  if (rows === null) return null;
  return (
    <div data-test="react-page">
      {rows.length === 0 && spec.empty ? <div className="empty">{spec.empty}</div> : <RowGroups rows={rows} onChange={change} />}
      {spec.note ? <div className="setting-about note">{spec.note}</div> : null}
    </div>
  );
}
