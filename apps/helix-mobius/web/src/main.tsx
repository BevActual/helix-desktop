import { createRoot } from "react-dom/client";
import { ModelsPage, type ModelsHandle, type ModelsHooks } from "./models";

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

(window as unknown as { HelixMobiusReact: unknown }).HelixMobiusReact = {
  models: (host: HTMLElement, hooks: ModelsHooks) => {
    const run = handled<ModelsHandle>(host, (register) => <ModelsPage register={register} hooks={hooks} />);
    return { open: () => run((h) => h.open()) };
  },
};
