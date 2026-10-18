import { ChatTools, type ToolsHandle } from "./tools";
import { ActTask, type ActHandle, type ActHooks, type ActState } from "./acttask";
import { createRoot } from "react-dom/client";
import { ChatList, type ChatItem, type ChatListHandle, type ChatListHooks } from "./chatlist";
import { AttachChips, SlashMenu, type Attachment, type ChipsHandle, type SlashHandle, type SlashItem } from "./composer";
import { Dialogs, type DialogHandle, type DialogHooks, type ModeState } from "./dialogs";
import { Thread, type Block, type DesktopTask, type Message, type ThreadHandle, type ThreadHooks } from "./thread";
import { ModelsPage, type ModelsHandle, type ModelsHooks } from "./models";
import { Analysis, type AnalysisHandle } from "./analysis";
import { OnlineResearch, type WebHandle } from "./web";
import { Welcome, type WelcomeHandle, type WelcomeHooks } from "./welcome";
import { ChatContext, type ContextHandle, type ContextInfo } from "./context";
import { CodeStage, type CodeHandle, type CodeHooks } from "./codestage";
import { Stage, type StageHandle, type StageHooks } from "./workpanel";
import { ImageStage, type ImageHandle, type ImageHooks } from "./imagestage";
import { Projects, type ProjectsHandle, type ProjectsHooks } from "./projects";
import { Scheduled, type ScheduledHandle } from "./scheduled";
import { Palette, type PaletteHandle, type PaletteHooks } from "./palette";
import { Memory, type MemoryHandle } from "./memory";
import { CreateImage, type CreateImageHandle } from "./create-image";

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
  acttask: (host: HTMLElement, hooks: ActHooks) => {
    const run = handled<ActHandle>(host, (register) => <ActTask register={register} hooks={hooks} />);
    return { set: (state: ActState | null) => run((h) => h.set(state)) };
  },
  tools: (host: HTMLElement) => {
    const run = handled<ToolsHandle>(host, (register) => <ChatTools register={register} />);
    return { open: (view: Parameters<ToolsHandle["open"]>[0]) => run((h) => h.open(view)) };
  },
  web: (host: HTMLElement, answered: () => void) => {
    const run = handled<WebHandle>(host, (register) => <OnlineResearch register={register} answered={answered} />);
    return { open: (chat: Parameters<WebHandle["open"]>[0], query?: string) => run((h) => h.open(chat, query)) };
  },
  welcome: (host: HTMLElement, hooks: WelcomeHooks) => {
    const run = handled<WelcomeHandle>(host, (register) => <Welcome register={register} hooks={hooks} />);
    return { set: (starters: string[], listening: boolean, busy: string) => run((h) => h.set(starters, listening, busy)) };
  },
  context: (host: HTMLElement) => {
    const run = handled<ContextHandle>(host, (register) => <ChatContext register={register} />);
    return { open: (view: Parameters<ContextHandle["open"]>[0]) => run((h) => h.open(view)) };
  },
  codeStage: (host: HTMLElement, hooks: CodeHooks) => {
    const run = handled<CodeHandle>(host, (register) => <CodeStage register={register} hooks={hooks} />);
    return { open: () => run((h) => h.open()), refresh: () => run((h) => h.refresh()), close: () => run((h) => h.close()) };
  },
  stage: (host: HTMLElement, hooks: StageHooks) => {
    const run = handled<StageHandle>(host, (register) => <Stage register={register} hooks={hooks} />);
    return { open: (title: string, body?: string) => run((h) => h.open(title, body)), refresh: () => run((h) => h.refresh()), close: () => run((h) => h.close()) };
  },
  palette: (host: HTMLElement, hooks: PaletteHooks) => {
    const run = handled<PaletteHandle>(host, (register) => <Palette register={register} hooks={hooks} />);
    return { open: () => run((h) => h.open()) };
  },
  imageStage: (host: HTMLElement, hooks: ImageHooks) => {
    const run = handled<ImageHandle>(host, (register) => <ImageStage register={register} hooks={hooks} />);
    return { open: (picture?: string, title?: string) => run((h) => h.open(picture, title)), apply: (ops: Parameters<ImageHandle["apply"]>[0]) => run((h) => { h.apply(ops); }), close: () => run((h) => h.close()) };
  },
  projects: (host: HTMLElement, hooks: ProjectsHooks) => {
    const run = handled<ProjectsHandle>(host, (register) => <Projects register={register} hooks={hooks} />);
    return { open: () => run((h) => h.open()) };
  },
  scheduled: (host: HTMLElement, onOpenChat: (id: string) => void) => {
    const run = handled<ScheduledHandle>(host, (register) => <Scheduled register={register} onOpenChat={onOpenChat} />);
    return { open: () => run((h) => h.open()) };
  },
  memory: (host: HTMLElement) => {
    const run = handled<MemoryHandle>(host, (register) => <Memory register={register} />);
    return { open: () => run((h) => h.open()) };
  },
  createImage: (host: HTMLElement) => {
    const run = handled<CreateImageHandle>(host, (register) => <CreateImage register={register} />);
    return { open: (prompt?: string) => run((h) => h.open(prompt)) };
  },
  analysis: (host: HTMLElement, report: (error: unknown) => void) => {
    const run = handled<AnalysisHandle>(host, (register) => <Analysis register={register} report={report} />);
    return { open: (id?: string) => run((h) => h.open(id)) };
  },
  chips: (host: HTMLElement, onRemove: (index: number) => void, onDismiss: () => void) => {
    const run = handled<ChipsHandle>(host, (register) => <AttachChips register={register} onRemove={onRemove} onDismiss={onDismiss} />);
    return { set: (files: Attachment[], warnings: string[]) => run((h) => h.set(files, warnings)) };
  },
  slash: (host: HTMLElement, onPick: (index: number) => void) => {
    const run = handled<SlashHandle>(host, (register) => <SlashMenu register={register} onPick={onPick} />);
    return { set: (items: SlashItem[], active: number) => run((h) => h.set(items, active)) };
  },
  thread: (host: HTMLElement, hooks: ThreadHooks) => {
    const run = handled<ThreadHandle>(host, (register) => <Thread register={register} hooks={hooks} />);
    return {
      set: (messages: Message[], streaming: boolean, context?: ContextInfo, tasks?: DesktopTask[]) => run((h) => h.set(messages, streaming, context, tasks)),
      stream: (blocks: Block[]) => run((h) => h.stream(blocks)),
      find: (needle: string) => run((h) => h.find(needle)),
      undo: (task: number, undoable: boolean) => run((h) => h.undo(task, undoable)),
    };
  },
  dialogs: (host: HTMLElement, rail: HTMLElement | null, hooks: DialogHooks) => {
    const run = handled<DialogHandle>(host, (register) => <Dialogs register={register} hooks={hooks} rail={rail} />);
    return {
      openRename: (title: string) => run((h) => h.openRename(title)),
      openProject: (current: string, suggestions: string[]) => run((h) => h.openProject(current, suggestions)),
      openModel: () => run((h) => h.openModel()),
      openPersona: (create?: boolean) => run((h) => h.openPersona(create)),
      openMenu: (kind: "persona" | "style" | "model" | "mode", anchor: { left: number; top: number; width: number }, mode?: ModeState) => run((h) => h.openMenu(kind, anchor, mode)),
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
