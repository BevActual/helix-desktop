import { createRoot } from "react-dom/client";
import { ChatList, type ChatItem, type ChatListHandle, type ChatListHooks } from "./chatlist";
import { AttachChips, SlashMenu, type Attachment, type ChipsHandle, type SlashHandle, type SlashItem } from "./composer";
import { Dialogs, type DialogHandle, type DialogHooks } from "./dialogs";
import { Thread, type Block, type Message, type ThreadHandle, type ThreadHooks } from "./thread";
import { ModelsPage, type ModelsHandle, type ModelsHooks } from "./models";

/** Mounts a component that registers a handle once it is ready; calls made before that wait and run when it is. */
function handled<H extends object>(host: HTMLElement, render: (register: (handle: H) => void) => React.ReactElement) {
  let handle: H | null = null;
  const waiting: ((h: H) => void)[] = [];
  createRoot(host).render(
    render((ready) => {
      handle = ready;
      waiting.splice(0).forEach((call) => queueMicrotask(() => call(ready)));
    }),
  );
  return (call: (h: H) => void) => {
    if (handle) call(handle);
    else waiting.push(call);
  };
}

(window as unknown as { HelixMobiusReact: unknown }).HelixMobiusReact = {
  chips: (host: HTMLElement, onRemove: (index: number) => void) => {
    const run = handled<ChipsHandle>(host, (register) => <AttachChips register={register} onRemove={onRemove} />);
    return { set: (files: Attachment[]) => run((h) => h.set(files)) };
  },
  slash: (host: HTMLElement, onPick: (index: number) => void) => {
    const run = handled<SlashHandle>(host, (register) => <SlashMenu register={register} onPick={onPick} />);
    return { set: (items: SlashItem[], active: number) => run((h) => h.set(items, active)) };
  },
  thread: (host: HTMLElement, hooks: ThreadHooks) => {
    const run = handled<ThreadHandle>(host, (register) => <Thread register={register} hooks={hooks} />);
    return {
      set: (messages: Message[], streaming: boolean) => run((h) => h.set(messages, streaming)),
      stream: (blocks: Block[]) => run((h) => h.stream(blocks)),
      find: (needle: string) => run((h) => h.find(needle)),
    };
  },
  dialogs: (host: HTMLElement, rail: HTMLElement | null, hooks: DialogHooks) => {
    const run = handled<DialogHandle>(host, (register) => <Dialogs register={register} hooks={hooks} rail={rail} />);
    return {
      openRename: (title: string) => run((h) => h.openRename(title)),
      openProject: (current: string, suggestions: string[]) => run((h) => h.openProject(current, suggestions)),
      openModel: () => run((h) => h.openModel()),
      openPersona: (create?: boolean) => run((h) => h.openPersona(create)),
      openEdit: (index: number, text: string) => run((h) => h.openEdit(index, text)),
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
