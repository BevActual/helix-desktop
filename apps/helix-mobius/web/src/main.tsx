import { createRoot } from "react-dom/client";
import { ChatList, type ChatItem, type ChatListHandle, type ChatListHooks } from "./chatlist";
import { Dialogs, type DialogHandle, type DialogHooks } from "./dialogs";
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
  dialogs: (host: HTMLElement, rail: HTMLElement | null, hooks: DialogHooks) => {
    const run = handled<DialogHandle>(host, (register) => <Dialogs register={register} hooks={hooks} rail={rail} />);
    return {
      openRename: (title: string) => run((h) => h.openRename(title)),
      openProject: (current: string, suggestions: string[]) => run((h) => h.openProject(current, suggestions)),
      openModel: () => run((h) => h.openModel()),
      openPersona: (create?: boolean) => run((h) => h.openPersona(create)),
    };
  },
  chats: (host: HTMLElement, hooks: ChatListHooks) => {
    const run = handled<ChatListHandle>(host, (register) => <ChatList register={register} hooks={hooks} />);
    return { set: (items: ChatItem[], currentId: string) => run((h) => h.set(items, currentId)) };
  },
  models: (host: HTMLElement, hooks: ModelsHooks) => {
    const run = handled<ModelsHandle>(host, (register) => <ModelsPage register={register} hooks={hooks} />);
    return { open: () => run((h) => h.open()) };
  },
};
