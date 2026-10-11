import { createRoot } from "react-dom/client";
import type { NotificationsView } from "@helix/ui";
import { Shade, Toasts } from "./notifications";
import { Launcher, type LauncherHandle } from "./launcher";
import { Today, type TodayHandle } from "./today";

type Subscriber = (set: (view: NotificationsView) => void) => void;

/** A host element and a way to push the newest view into the component once it has mounted (none are lost before that). */
function island(host: HTMLElement, Component: (props: { subscribe: Subscriber }) => React.ReactElement) {
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
