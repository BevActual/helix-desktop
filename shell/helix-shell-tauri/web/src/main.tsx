import { createRoot } from "react-dom/client";
import type { NotificationsView } from "@helix/ui";
import { Shade, Toasts } from "./notifications";

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

(window as unknown as { HelixShellReact: unknown }).HelixShellReact = {
  notifications: (toasts: HTMLElement, shade: HTMLElement) => {
    const a = island(toasts, Toasts);
    const b = island(shade, Shade);
    return { update: (view: NotificationsView) => { a.update(view); b.update(view); } };
  },
};
