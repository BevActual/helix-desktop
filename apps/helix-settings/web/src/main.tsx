import type { ReactElement } from "react";
import { createRoot } from "react-dom/client";
import { invoke, type Change, type RowView } from "@helix/ui";
import { RowsPage, type PageHandle, type PageSpec } from "./pages/RowsPage";
import { AboutPage } from "./pages/AboutPage";
import { AppearancePage } from "./pages/AppearancePage";
import { KeyboardPage } from "./pages/KeyboardPage";
import { WallpaperPage } from "./pages/WallpaperPage";
import { NetworkPage } from "./pages/NetworkPage";
import { BluetoothPage } from "./pages/BluetoothPage";
import { SoundPage } from "./pages/SoundPage";
import { AgentsPage } from "./pages/AgentsPage";

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

const appearance: PageSpec = {
  change: serviceChange("appearance"),
  empty: "There is nothing to adjust here on this desktop.",
};

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
  accessibility: { change: serviceChange("accessibility"), empty: "There is nothing to adjust here on this desktop." },
};

/** Pages with their own components (not only rows). */
const custom: Record<string, (props: { register: (handle: PageHandle) => void; options: MountOptions }) => ReactElement> = {
  about: ({ register }) => <AboutPage register={register} />,
  appearance: ({ register, options }) => (
    <AppearancePage
      spec={appearance}
      register={register}
      initialRows={options.initialRows}
      onRows={options.onRows}
      report={report}
    />
  ),
  keyboard: ({ register }) => <KeyboardPage register={register} report={report} />,
  wallpaper: ({ register }) => <WallpaperPage register={register} report={report} />,
  network: ({ register }) => <NetworkPage register={register} />,
  bluetooth: ({ register }) => <BluetoothPage register={register} />,
  sound: ({ register }) => <SoundPage register={register} />,
  agents: ({ register }) => <AgentsPage register={register} />,
};

function mountPage(name: string, host: HTMLElement, options: MountOptions): Controller | null {
  const rows = specs[name];
  const page = custom[name];
  if (!rows && !page) return null;
  let handle: PageHandle | null = null;
  // Rows can arrive before React has committed the page; the newest are kept and applied the moment it is ready.
  let pending: RowView[] | null = null;
  const controller: Controller = {
    setRows: (next) => {
      if (handle) handle.setRows(next);
      else pending = next;
    },
    refresh: () => handle?.refresh(),
  };
  const register = (ready: PageHandle) => {
    handle = ready;
    if (pending) {
      const next = pending;
      pending = null;
      ready.setRows(next);
    }
  };
  const root = createRoot(host);
  if (rows) {
    root.render(<RowsPage spec={rows} initialRows={options.initialRows} register={register} onRows={options.onRows} report={report} />);
  } else {
    root.render(page({ register, options }));
  }
  return controller;
}

(window as unknown as { HelixReact: { mountPage: typeof mountPage; pages: string[] } }).HelixReact = {
  mountPage,
  pages: [...Object.keys(specs), ...Object.keys(custom)],
};
