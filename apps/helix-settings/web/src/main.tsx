import { createRoot } from "react-dom/client";
import { invoke, type Change, type RowView } from "@helix/ui";
import { RowsPage, type PageHandle, type PageSpec } from "./pages/RowsPage";

export interface MountOptions {
  /** Rows the host page already has (service settings arrive as events, not from a command). */
  initialRows: RowView[] | null;
  /** Called with every list of rows the page shows (the search page reads them). */
  onRows: (rows: RowView[]) => void;
}

export interface Controller {
  setRows: (rows: RowView[]) => void;
  /** Reads the page's rows again (the page was shown again). */
  refresh: () => void;
}

const report = (text: string) => {
  invoke("ui_error", { text }).catch(() => undefined);
};

const serviceChange = (page: string) => (id: string, change: Change) => invoke<void>("setting_change", { page, id, change });

/** Every page that is only rows. Names match the host page's `PAGES`. */
const specs: Record<string, PageSpec> = {
  dock: {
    load: () => invoke<RowView[]>("dock_rows"),
    change: (id, change) => invoke<RowView[]>("dock_change", { id, change }),
  },
  power: {
    load: () => invoke<RowView[]>("power_rows"),
    change: (id, change) => invoke<RowView[]>("power_change", { id, change }),
  },
  displays: {
    load: () => invoke<RowView[]>("displays_rows"),
    empty: "No displays reported.",
    note: "Resolution, scale and arrangement are set in the compositor's configuration.",
  },
  appearance: { change: serviceChange("appearance"), empty: "There is nothing to adjust here on this desktop." },
  accessibility: { change: serviceChange("accessibility"), empty: "There is nothing to adjust here on this desktop." },
};

function mountPage(name: string, host: HTMLElement, options: MountOptions): Controller | null {
  const spec = specs[name];
  if (!spec) return null;
  let handle: PageHandle | null = null;
  // Rows can arrive before React has committed the page; the newest are kept and applied the moment it is ready.
  let pending: RowView[] | null = null;
  const controller: Controller = {
    setRows: (rows) => {
      if (handle) handle.setRows(rows);
      else pending = rows;
    },
    refresh: () => handle?.refresh(),
  };
  const register = (ready: PageHandle) => {
    handle = ready;
    if (pending) {
      const rows = pending;
      pending = null;
      ready.setRows(rows);
    }
  };
  createRoot(host).render(
    <RowsPage spec={spec} initialRows={options.initialRows} register={register} onRows={options.onRows} report={report} />,
  );
  return controller;
}

(window as unknown as { HelixReact: { mountPage: typeof mountPage; pages: string[] } }).HelixReact = {
  mountPage,
  pages: Object.keys(specs),
};
