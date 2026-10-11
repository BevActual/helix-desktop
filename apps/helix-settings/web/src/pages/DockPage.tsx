import { useEffect, useState } from "react";
import { invoke, RowGroups, type Change, type RowView } from "@helix/ui";

export interface DockHandle {
  setRows: (rows: RowView[]) => void;
  refresh: () => void;
}

/** The Dock page: settings the shell honours, kept in a file it watches (`dock_rows` / `dock_change`). */
export function DockPage({ register, onRows, report }: { register: (handle: DockHandle) => void; onRows: (rows: RowView[]) => void; report: (text: string) => void }) {
  const [rows, setRows] = useState<RowView[] | null>(null);

  useEffect(() => {
    const apply = (next: RowView[]) => {
      setRows(next);
      onRows(next);
    };
    const refresh = () => {
      invoke<RowView[]>("dock_rows").then(apply, (error) => report(`dock_rows: ${String(error)}`));
    };
    register({ setRows: apply, refresh });
    refresh();
  }, [register, onRows, report]);

  const change = (id: string, change: Change) => {
    invoke<RowView[]>("dock_change", { id, change }).then(
      (next) => {
        setRows(next);
        onRows(next);
      },
      (error) => report(`dock_change ${id}: ${String(error)}`),
    );
  };

  if (!rows) return null;
  return (
    <div data-test="react-page">
      <RowGroups rows={rows} onChange={change} />
    </div>
  );
}
