import { createRoot } from "react-dom/client";
import type { NotificationsView } from "@helix/ui";
import { Shade, Toasts } from "./notifications";
import { Launcher, type LauncherHandle } from "./launcher";
import { Today, type TodayHandle } from "./today";
import { ControlCenter, type ControlsHandle } from "./controls";
import { Mobius, type MobiusHandle } from "./mobius";
import { IslandBody, OsdBody, type IslandView, type OsdView } from "./indicators";
import { Tray, TrayMenu, type TrayHooks, type TrayMenuHandle } from "./tray";
import { DockItems, type DockHandle, type DockHooks, type DockItem } from "./dock";
import { QuickInsert, type QuickInsertHandle } from "./quickinsert";
import { Writing, type WritingHandle } from "./writing";

type Subscriber = (set: (view: NotificationsView) => void) => void;

/** A host element and a way to push the newest view into the component once it has mounted (none are lost before that). */
function island(host: HTMLElement, Component: (props: { subscribe: Subscriber }) => React.ReactElement | null) {
  let push: ((view: NotificationsView) => void) | null = null;
  let latest: NotificationsView | null = null;
  const subscribe: Subscriber = (set) => {
    push = set;
    if (latest) set(latest);
  };
  createRoot(host).render(<Component subscribe={subscribe} />);
  return {
    update(view: NotificationsView) {
      latest = view;
      push?.(view);
    },
  };
}

/** Mounts a component that registers a handle once it is ready; calls made before that wait and run when it is. */
function handled<H extends object>(host: HTMLElement, render: (register: (handle: H) => void) => React.ReactElement) {
  let handle: H | null = null;
  const waiting: ((h: H) => void)[] = [];
  createRoot(host).render(
    render((ready) => {
      handle = ready;
      waiting.splice(0).forEach((call) => call(ready));
    }),
  );
  return (call: (h: H) => void) => {
    if (handle) call(handle);
    else waiting.push(call);
  };
}

(window as unknown as { HelixShellReact: unknown }).HelixShellReact = {
  launcher: (host: HTMLElement) => {
    const run = handled<LauncherHandle>(host, (register) => <Launcher register={register} />);
    return { open: () => run((h) => h.open()) };
  },
  mobius: (host: HTMLElement) => {
    const run = handled<MobiusHandle>(host, (register) => <Mobius register={register} />);
    return { setMode: (mode: string) => run((h) => h.setMode(mode)) };
  },
  island: (host: HTMLElement) => {
    const run = handled<{ set: (view: IslandView) => void }>(host, (ready) => (
      <IslandBody register={(set) => ready({ set })} />
    ));
    return { set: (view: IslandView) => run((h) => h.set(view)) };
  },
  osd: (host: HTMLElement) => {
    const run = handled<{ set: (view: OsdView) => void }>(host, (ready) => (
      <OsdBody register={(set) => ready({ set })} />
    ));
    return { set: (view: OsdView) => run((h) => h.set(view)) };
  },
  tray: (host: HTMLElement, menuHost: HTMLElement, hooks: TrayHooks) => {
    createRoot(host).render(<Tray hooks={hooks} />);
    const run = handled<TrayMenuHandle>(menuHost, (register) => <TrayMenu register={register} />);
    return { showMenu: (key: string, entries: import("./tray").MenuEntry[], chosen: () => void) => run((h) => h.set(key, entries, chosen)) };
  },
  dock: (host: HTMLElement, hooks: DockHooks) => {
    const run = handled<DockHandle>(host, (register) => <DockItems hooks={hooks} register={register} />);
    return { set: (items: DockItem[]) => run((h) => h.set(items)) };
  },
  quickInsert: (host: HTMLElement) => {
    const run = handled<QuickInsertHandle>(host, (register) => <QuickInsert register={register} />);
    return { open: () => run((h) => h.open()) };
  },
  writing: (host: HTMLElement) => {
    const run = handled<WritingHandle>(host, (register) => <Writing register={register} />);
    return { open: () => run((h) => h.open()) };
  },
  controls: (host: HTMLElement) => {
    let ready: ControlsHandle | null = null;
    const waiting: ((h: ControlsHandle) => void)[] = [];
    createRoot(host).render(
      <ControlCenter
        register={(handle) => {
          ready = handle;
          waiting.splice(0).forEach((call) => call(handle));
        }}
      />,
    );
    const run = (call: (h: ControlsHandle) => void) => (ready ? call(ready) : waiting.push(call));
    return { open: () => run((h) => h.open()), tick: () => run((h) => h.tick()), back: () => (ready ? ready.back() : false) };
  },
  today: (host: HTMLElement) => {
    const run = handled<TodayHandle>(host, (register) => <Today register={register} />);
    return { open: () => run((h) => h.open()), tick: () => run((h) => h.tick()) };
  },
  notifications: (toasts: HTMLElement, shade: HTMLElement) => {
    const a = island(toasts, Toasts);
    const b = island(shade, Shade);
    return { update: (view: NotificationsView) => { a.update(view); b.update(view); } };
  },
};
