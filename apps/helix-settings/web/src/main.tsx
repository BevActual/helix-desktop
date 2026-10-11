import { createRoot } from "react-dom/client";
import { invoke, type RowView } from "@helix/ui";
import { DockPage, type DockHandle } from "./pages/DockPage";

export interface MountOptions {
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

function mountDock(host: HTMLElement, options: MountOptions): Controller {
  let handle: DockHandle | null = null;
  const controller: Controller = { setRows: (rows) => handle?.setRows(rows), refresh: () => handle?.refresh() };
  createRoot(host).render(<DockPage register={(h) => { handle = h; }} onRows={options.onRows} report={report} />);
  return controller;
}

(window as unknown as { HelixReact: { mountDock: typeof mountDock } }).HelixReact = { mountDock };
