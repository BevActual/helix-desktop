import { Chart, parseChart } from "./chart";
import { ArtifactCard } from "./workpanel";
import { Regenerate } from "./regenerate";
import { CompareBar } from "./compare";
import { EditsCard } from "./imagestage";
import { FlowDiagram, parseDiagram } from "./diagram";
import { Ephemeral, parseUi } from "./ephemeral";
import { Fragment, useEffect, useRef, useState } from "react";
import { flushSync } from "react-dom";
import { Icon, invoke, listen } from "@helix/ui";
import { afterDone } from "./acttask";
import type { ContextInfo } from "./context";

export type Span = { kind: "text" | "code" | "bold" | "italic"; text: string };
export type Block =
  | { kind: "paragraph"; spans: Span[] }
  | { kind: "heading"; level: number; spans: Span[] }
  | { kind: "quote"; spans: Span[] }
  | { kind: "rule" }
  | { kind: "table"; headers: Span[][]; rows: Span[][][]; alignments: ("left" | "center" | "right")[] }
  | { kind: "list"; ordered: boolean; items: Span[][] }
  | { kind: "code"; language: string; text: string }
  | { kind: "thinking"; text: string; done: boolean };
interface Source { provider?: string; title: string; url: string; kind: string; fetched: number }
/** A desktop task handed over from this chat (Act) and how it ended. Shown after `after` messages; never part of what a model is sent. */
export interface DesktopTask { id: number; after: number; request: string; outcome: string; summary: string; detail: string; before?: string; after_text?: string; undone?: boolean; at: number }
interface Activity { id: string; tool: string; input: string; state: string; output: string }
interface FileRef { title: string; heading: string; excerpt: string }

/** The passages from the person's own folders a reply was given, behind a line that says how many. */
function FileSources({ files, index }: { files: FileRef[]; index: number }) {
  const [open, setOpen] = useState(false);
  return (
    <section className="kn-sources" data-test={`answer-files-${index}`} aria-label="Passages from your files that the model was given">
      <button type="button" className="kn-sources-toggle" aria-expanded={open} data-test={`answer-files-toggle-${index}`} onClick={() => setOpen(!open)}>
        {open ? "▾" : "▸"} From your files · {files.length}
      </button>
      {open ? files.map((file, i) => (
        <div key={`${file.title}:${i}`} className="kn-source" data-test={`answer-file-${index}-${i}`}>
          <div className="kn-source-title">[{i + 1}] {file.title}{file.heading ? ` › ${file.heading}` : ""}</div>
          <div className="kn-source-text">{file.excerpt}</div>
        </div>
      )) : null}
    </section>
  );
}
interface ReplyStats { model: string; tokens: number; ms: number; files?: FileRef[] }
export interface Message { activities?: Activity[]; sources?: Source[]; role: string; text: string; blocks: Block[]; images?: string[]; stats?: ReplyStats | null; }
/** What a reply cost, in a few words: the model, its tokens, and how fast they came. */
function replyDetails(stats: ReplyStats | null | undefined): string {
  if (!stats || !stats.tokens) return "";
  const rate = stats.ms >= 500 ? ` · ${Math.round(stats.tokens / (stats.ms / 1000))} tok/s` : "";
  return [stats.model, `${stats.tokens} tokens${rate}`].filter(Boolean).join(" · ");
}

const pictureUrls = new Map<string, Promise<string>>();
/** A stored picture as an object URL, fetched once. */
function pictureUrl(id: string): Promise<string> {
  let url = pictureUrls.get(id);
  if (!url) {
    url = invoke<ArrayBuffer>("image_data", { id }).then((bytes) => URL.createObjectURL(new Blob([bytes])));
    pictureUrls.set(id, url);
  }
  return url;
}

function Picture({ id, generated = false, onEdit }: { id: string; generated?: boolean; onEdit?: (id: string) => void }) {
  const [src, setSrc] = useState("");
  useEffect(() => { let live = true; pictureUrl(id).then((u) => live && setSrc(u)).catch(() => {}); return () => { live = false; }; }, [id]);
  if (!src) return <span className="msg-picture pending" />;
  const image = <img className="msg-picture" data-test="msg-picture" src={src} alt={generated ? "Generated image" : "Attached picture"} />;
  return onEdit ? <span className="msg-picture-wrap">{image}<button type="button" className="mini pic-edit" data-test="pic-edit" onClick={() => onEdit(id)}>Edit</button></span> : image;
}

interface GeneratedInfo { prompt: string; model: string; seed: number; width: number; height: number; seconds: number }
function GeneratedPicture({ id, streaming, hooks }: { id: string; streaming: boolean; hooks: ThreadHooks }) {
  const [info, setInfo] = useState<GeneratedInfo | null>(null);
  useEffect(() => { let live = true; invoke<GeneratedInfo | null>("image_info", { id }).then((value) => { if (live) setInfo(value); }, hooks.report); return () => { live = false; }; }, [id, hooks.report]);
  return <div className="generated-picture" data-test="generated-image">
    <Picture id={id} generated onEdit={hooks.editPicture} />
    {info && <>
      <p className="sheet-note">{info.model} · {info.width} × {info.height} · seed {info.seed}</p>
      <div className="generated-picture-actions">
        <button className="mini" data-test="generated-image-save" onClick={() => invoke("image_save", { id }).catch(hooks.report)}>Save image</button>
        <button className="mini" data-test="generated-image-copy" onClick={() => invoke("image_copy", { id }).catch(hooks.report)}>Copy image</button>
        <button className="mini" data-test="generated-image-again" disabled={streaming} onClick={() => invoke("image_regenerate", { id }).catch(hooks.report)}>Generate again</button>
        <button className="mini" data-test="generated-image-prompt" disabled={streaming} onClick={() => window.dispatchEvent(new CustomEvent("mobius-image-prompt", { detail: info.prompt }))}>Use as prompt</button>
      </div>
    </>}
  </div>;
}

export interface ThreadHooks {
  /** An edited message was sent: later replies are replaced and a new answer written. */
  edited: (index: number, text: string) => void;
  edit: (index: number, text: string) => void;
  act: (text: string) => void;
  quote: (text: string) => void;
  fork: (index: number) => void;
  report: (error: unknown) => void;
  /** Applies edits a reply suggested to the picture open on the Image stage. */
  applyEdits: (ops: Record<string, unknown>[]) => void;
  /** Opens a picture from the thread on the Image stage. */
  editPicture: (id: string) => void;
  /** Puts a document the reply wrote on the stage. */
  stage: (title: string, body: string) => void;
  /** Sends this text as the next message (what the person chose or filled in on a piece of ephemeral UI). */
  send: (text: string) => void;
  /** Asks the shell to write this task's previous value back. */
  undo: (id: number) => void;
}
export interface ThreadHandle {
  /** The whole chat, drawn now (synchronously) so the page can scroll and find in it straight away. */
  set: (messages: Message[], streaming: boolean, context?: ContextInfo, tasks?: DesktopTask[]) => void;
  /** The live reply's blocks while it streams. */
  stream: (blocks: Block[]) => void;
  /** The text to highlight (empty clears it). */
  find: (needle: string) => void;
  /** The task the shell still says can be undone. 0, or the same id with undoable false, clears it. A different task that is not undoable leaves it. */
  undo: (task: number, undoable: boolean) => void;
}

/** Splits text around case-insensitive matches of the find needle; matches become `<mark class="find-hit">`. */
function snippetFilename(language: string) {
  const lang = language.trim().toLowerCase();
  const ext = ({ rust: "rs", python: "py", py: "py", typescript: "ts", ts: "ts", javascript: "js", js: "js", sh: "sh", bash: "sh", json: "json", yaml: "yaml", yml: "yml", toml: "toml", go: "go", java: "java", kotlin: "kt", c: "c", cpp: "cpp", md: "md", markdown: "md" } as Record<string, string>)[lang] || "txt";
  return `mobius-snippet.${ext}`;
}

function highlight(text: string, needle: string): React.ReactNode {
  if (!needle) return text;
  const lower = text.toLowerCase();
  const low = needle.toLowerCase();
  const parts: React.ReactNode[] = [];
  let start = 0;
  let at = lower.indexOf(low, start);
  if (at === -1) return text;
  while (at !== -1) {
    if (at > start) parts.push(text.slice(start, at));
    parts.push(<mark key={at} className="find-hit" data-test="find-hit">{text.slice(at, at + needle.length)}</mark>);
    start = at + needle.length;
    at = lower.indexOf(low, start);
  }
  if (start < text.length) parts.push(text.slice(start));
  return parts.map((part, i) => <Fragment key={i}>{part}</Fragment>);
}

function Spans({ spans, needle }: { spans: Span[]; needle: string }) {
  return (
    <>
      {spans.map((span, index) => {
        const inner = highlight(span.text, needle);
        if (span.kind === "code") return <code key={index}>{inner}</code>;
        if (span.kind === "bold") return <strong key={index}>{inner}</strong>;
        if (span.kind === "italic") return <em key={index}>{inner}</em>;
        return <Fragment key={index}>{inner}</Fragment>;
      })}
    </>
  );
}

/** Editing a message where it is: the bubble becomes a box with the text in it. Enter sends, Shift+Enter breaks the line, Escape puts it back. */
function InlineEdit({ initial, onSend, onCancel }: { initial: string; onSend: (text: string) => void; onCancel: () => void }) {
  const [value, setValue] = useState(initial);
  const box = useRef<HTMLTextAreaElement>(null);
  const grow = () => { const el = box.current; if (el) { el.style.height = "auto"; el.style.height = `${Math.min(el.scrollHeight, 320)}px`; } };
  useEffect(() => { const el = box.current; if (el) { el.focus(); el.setSelectionRange(el.value.length, el.value.length); grow(); } }, []);
  const send = () => { if (value.trim()) onSend(value.trim()); };
  return <div className="msg-edit" data-test="msg-edit">
    <textarea ref={box} data-test="edit-text" aria-label="Edit your message" maxLength={4000} value={value}
      onChange={(e) => { setValue(e.target.value); grow(); }}
      onKeyDown={(e) => { if (e.key === "Enter" && !e.shiftKey && !e.nativeEvent.isComposing) { e.preventDefault(); send(); } else if (e.key === "Escape") { e.preventDefault(); onCancel(); } }} />
    <div className="msg-edit-actions"><span>Replies after this message are replaced.</span><button type="button" data-test="edit-cancel" onClick={onCancel}>Cancel</button><button type="button" className="primary" data-test="edit-save" disabled={!value.trim()} onClick={send}>Send</button></div>
  </div>;
}

/** The diff of a proposed or applied file edit: the path, then removed lines in red and added lines in green. */
function Diff({ text, testId, unified }: { text: string; testId?: string; unified?: boolean }) {
  const lines = text.replace(/\n$/, "").split("\n");
  const kind = (line: string, i: number) => unified
    ? (line.startsWith("@@") ? "diff-head" : line.startsWith("+") ? "diff-add" : line.startsWith("-") ? "diff-del" : "")
    : (i === 0 ? "diff-head" : line.startsWith("+ ") ? "diff-add" : line.startsWith("- ") ? "diff-del" : "");
  return <pre className="diff" data-test={testId}>{lines.map((line, i) => <span key={i} className={kind(line, i)}>{line || " "}</span>)}</pre>;
}

interface FileChange { path: string; kind: string; diff: string; added: number; removed: number; reverted: boolean }

/** Everything a Tools turn changed in the project folder, file by file with its diff, and a way to put each file back. */
function ChangesCard({ activity, streaming, hooks }: { activity: { id: string; input: string; output: string }; streaming: boolean; hooks: ThreadHooks }) {
  let files: FileChange[] = [];
  try { files = JSON.parse(activity.input); } catch { /* shown as empty */ }
  return <section className="changes-card" data-test="changes-card" aria-label="Changes made to the project folder">
    <div className="changes-head"><strong>Changes</strong><span>{activity.output}</span></div>
    {files.map((file, i) => <details key={file.path} className={file.reverted ? "reverted" : ""} data-test={`change-${i}`}>
      <summary>
        <span className={`change-kind ${file.kind}`}>{file.kind}</span>
        <span className="change-path">{file.path}</span>
        <span className="change-count">{file.reverted ? "reverted" : `+${file.added} −${file.removed}`}</span>
        {!file.reverted && <button type="button" className="mini" data-test={`change-revert-${i}`} disabled={streaming}
          onClick={(event) => { event.preventDefault(); invoke("tools_revert_change", { activityId: activity.id, index: i }).catch(hooks.report); }}>Revert</button>}
      </summary>
      <Diff text={file.diff} unified />
    </details>)}
  </section>;
}

export function Blocks({ blocks, needle, report, onSaveSnippet, live, onSend, onStage, onEdits, writing }: { blocks: Block[]; needle: string; report: ThreadHooks["report"]; onSaveSnippet?: (body: string, defaultFilename: string) => void; live?: boolean; onSend?: (text: string) => void; onStage?: (title: string, body: string) => void; onEdits?: (ops: Record<string, unknown>[]) => void; writing?: boolean }) {
  return (
    <>
      {blocks.map((block, index) => {
        switch (block.kind) {
          case "paragraph":
            return <p key={index}><Spans spans={block.spans} needle={needle} /></p>;
          case "heading": {
            const Tag = `h${Math.min(6, Math.max(3, block.level + 2))}` as "h3";
            return <Tag key={index}><Spans spans={block.spans} needle={needle} /></Tag>;
          }
          case "quote":
            return <blockquote key={index}><Spans spans={block.spans} needle={needle} /></blockquote>;
          case "rule":
            return <hr key={index} />;
          case "list": {
            const Tag = block.ordered ? "ol" : "ul";
            return (
              <Tag key={index}>
                {block.items.map((item, i) => <li key={i}><Spans spans={item} needle={needle} /></li>)}
              </Tag>
            );
          }
          case "table": {
            const tableCellText = (value: Span[]) => value.map((span) => span.text).join("");
            const tableTsv = () => {
              // TSV pastes directly into spreadsheets; quoting preserves tabs and newlines in cells.
              const cell = (value: Span[]) => {
                const text = tableCellText(value);
                return /[\t\n\r"]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
              };
              return [block.headers, ...block.rows].map((row) => row.map(cell).join("\t")).join("\n");
            };
            const copy = () => invoke("chat_copy", { text: tableTsv() }).catch(report);
            const saveCsv = () => {
              const cell = (value: Span[]) => {
                const text = tableCellText(value);
                return /[,\n\r"]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
              };
              const text = [block.headers, ...block.rows].map((row) => row.map(cell).join(",")).join("\n");
              invoke("chat_reply_save", { content: text, defaultFilename: "table.csv" }).catch(report);
            };
            return <div key={index} className="reply-table" data-test="reply-table">
              <div className="code-bar">
                <span>Table</span>
                <button className="mini" data-test="copy-table" onClick={copy}>Copy table</button>
                <button className="mini" data-test="save-table-csv" onClick={saveCsv}>Save as CSV</button>
              </div>
              <div className="reply-table-scroll" tabIndex={0} role="region" aria-label="Reply table">
                <table>
                  <thead><tr>{block.headers.map((cell, i) => <th key={i} scope="col" style={{ textAlign: block.alignments[i] }}><Spans spans={cell} needle={needle} /></th>)}</tr></thead>
                  <tbody>{block.rows.map((row, r) => <tr key={r}>{row.map((cell, i) => <td key={i} style={{ textAlign: block.alignments[i] }}><Spans spans={cell} needle={needle} /></td>)}</tr>)}</tbody>
                </table>
              </div>
            </div>;
          }
          case "thinking":
            return <details key={index} className="thinking" data-test="thinking" open={!block.done}>
              <summary>{block.done ? "Thought it through" : "Thinking…"}</summary>
              <p>{block.text}</p>
            </details>;
          case "code": {
            const chart = block.language === "chart" ? parseChart(block.text) : null;
            if (chart) return <Chart key={index} spec={chart} />;
            const artifactTitle = /^artifact:/i.test(block.language) ? block.language.replace(/^artifact:\s*/i, "").trim() : "";
            if (artifactTitle) return <ArtifactCard key={index} title={artifactTitle} body={block.text} writing={!!writing} onOpen={() => onStage?.(artifactTitle, block.text)} />;
            if (block.language === "imageops") {
              try {
                const ops = JSON.parse(block.text);
                if (Array.isArray(ops) && ops.length > 0 && ops.length <= 10 && ops.every((o) => o && typeof o === "object" && typeof o.op === "string")) return <EditsCard key={index} ops={ops} onApply={() => onEdits?.(ops)} />;
              } catch { /* an ordinary code block */ }
            }
            const flow = block.language === "diagram" ? parseDiagram(block.text) : null;
            if (flow) return <FlowDiagram key={index} diagram={flow} />;
            const ui = block.language === "ui" ? parseUi(block.text) : null;
            if (ui) return <Ephemeral key={index} spec={ui} live={!!live && !!onSend} onSend={onSend ?? (() => undefined)} />;
            return (
              <div key={index} className="code-block">
                <div className="code-bar">
                  <span>{block.language || "code"}</span>
                  <button className="mini" onClick={() => invoke("chat_copy", { text: block.text })}>Copy</button>
                  {onSaveSnippet && block.text.trim() ? (
                    <button className="mini" data-test="save-code-block" onClick={() => onSaveSnippet(block.text, snippetFilename(block.language))}>Save as file</button>
                  ) : null}
                </div>
                <pre><code>{highlight(block.text, needle)}</code></pre>
              </div>
            );
          }
        }
      })}
    </>
  );
}

/** The message thread. Rust owns the conversation and its markdown blocks; this draws them as elements (never markup) and sends back the
 * index of the message acted on. Find highlighting is done here, so nothing else touches these nodes. */
interface ReplySaveState { body: string; defaultFilename: string; busy: boolean; error: string }

export function Thread({ register, hooks }: { register: (handle: ThreadHandle) => void; hooks: ThreadHooks }) {
  const [messages, setMessages] = useState<Message[]>([]);
  const saveDialog = useRef<HTMLDialogElement>(null);
  const [save, setSave] = useState<ReplySaveState | null>(null);
  const [streaming, setStreaming] = useState(false);
  const [context, setContext] = useState<ContextInfo | null>(null);
  const [tasks, setTasks] = useState<DesktopTask[]>([]);
  const [undoOf, setUndoOf] = useState(0);
  const [live, setLive] = useState<Block[] | null>(null);
  const [editing, setEditing] = useState<number | null>(null);
  const [needle, setNeedle] = useState("");
  const [progress, setProgress] = useState({ fraction: 0, stage: "Queued for local image generation" });
  useEffect(() => { const stop = listen<{ fraction: number; stage: string }>("image-progress", setProgress); return () => { stop.then((fn) => fn()); }; }, []);

  useEffect(() => {
    register({
      set: (next, isStreaming, info, noted) => flushSync(() => { setMessages(next); setStreaming(isStreaming); setLive(null); setContext(info || null); setTasks(noted || []); }),
      stream: (blocks) => flushSync(() => setLive(blocks)),
      find: (value) => flushSync(() => setNeedle(value)),
      undo: (task, undoable) => flushSync(() => setUndoOf((current) => (undoable && task ? task : current === task ? 0 : current))),
    });
  }, [register]);

  useEffect(() => {
    if (save) saveDialog.current?.showModal();
    else if (saveDialog.current?.open) saveDialog.current.close();
  }, [save]);

  const beginSave = (body: string, defaultFilename: string) => {
    setSave({ body, defaultFilename, busy: false, error: "" });
  };

  const openSave = async (index: number) => {
    try {
      const draft = await invoke<{ body: string; default_filename: string }>("chat_reply_draft", { index });
      beginSave(draft.body, draft.default_filename);
    } catch (reason) {
      hooks.report(reason);
    }
  };

  const confirmSave = async () => {
    if (!save) return;
    setSave({ ...save, busy: true, error: "" });
    try {
      const path = await invoke<string | null>("chat_reply_save", { content: save.body, defaultFilename: save.defaultFilename });
      if (path) setSave(null);
      else setSave({ ...save, busy: false, error: "" });
    } catch (reason) {
      setSave({ ...save, busy: false, error: String(reason) });
    }
  };

  // What was done on the desktop from this chat, in the place it happened (never past the last message).
  const taskNotes = (at: number) => tasks.map((task, i) => ({ task, i })).filter(({ task }) => Math.min(task.after, messages.length) === at).map(({ task, i }) => {
    const title = task.outcome === "done" ? "Done" : task.outcome === "failed" ? "That didn’t work" : "Stopped";
    const pair = task.before && task.after_text ? `${task.before} to ${task.after_text}` : "";
    const rest = task.undone && task.before
      ? `Restored to ${task.before}.`
      : task.outcome === "done" ? (afterDone(task.detail) || pair) : task.detail;
    const canUndo = undoOf === task.id && !task.undone && !!task.before && !!task.after_text;
    return (
      <div className={`task-note ${task.outcome}`} key={`task-${i}`} data-test={`task-note-${i}`} data-outcome={task.outcome} role="note">
        <span className="task-note-mark" aria-hidden="true"><Icon name={task.outcome === "done" ? "check" : task.outcome === "failed" ? "alert" : "stop"} /></span>
        <div className="task-note-text">
          <div className="task-note-request" data-test={`task-note-request-${i}`}>{task.request}</div>
          <div className="task-note-result" data-test={`task-note-result-${i}`}>{rest ? `${title}: ${rest}` : title}</div>
          {canUndo ? <button type="button" className="mini" data-test={`task-note-undo-${i}`} onClick={() => hooks.undo(task.id)}>Undo</button> : null}
        </div>
      </div>
    );
  });

  return (
    <>
      {messages.map((message, index) => (<Fragment key={`m${index}`}>{taskNotes(index)}{(() => {
        const last = index === messages.length - 1;
        const boundary = context && context.omitted_messages > 0 && index === context.first_message
          ? <div className="context-boundary" data-test="context-boundary" role="note"><strong>Recent context starts here</strong><span>Earlier messages stay saved. Use Chat context to keep important details in a brief.</span></div> : null;
        if (message.role === "user") {
          return (
            <Fragment key={index}>{boundary}<div className="msg user" data-test={`msg-${index}`}>
              {message.images && message.images.length ? (
                <div className="msg-pictures">{message.images.map((id) => <Picture key={id} id={id} onEdit={hooks.editPicture} />)}</div>
              ) : null}
              {editing === index ? <InlineEdit initial={message.text} onCancel={() => setEditing(null)} onSend={(text) => { setEditing(null); hooks.edited(index, text); }} /> : <div className="msg-body">{message.sources?.length ? <><p>{highlight(message.text.split("\n\n--- Online sources")[0], needle)}</p><details className="web-request-evidence"><summary>Question and source excerpts</summary><pre>{highlight(message.text, needle)}</pre></details></> : highlight(message.text, needle)}</div>}
              {editing !== index && <div className="msg-actions">
                  {!message.text.startsWith("/image ") && !message.sources?.length && !messages[index + 1]?.activities?.length && <button className="mini" data-test={`edit-${index}`} onClick={() => setEditing(index)}>Edit</button>}
                  <button className="mini" data-test={`act-${index}`} title="Have Mobius change the desktop using this" onClick={() => hooks.act(message.text)}>Act</button>
                  <button className="mini" data-test={`quote-${index}`} title="Insert into the composer" onClick={() => hooks.quote(message.text)}>Quote</button>
                  {!streaming ? <button className="mini" data-test={`fork-user-${index}`} title="Continue from here in a new chat" onClick={() => hooks.fork(index)}>Fork</button> : null}
                </div>}
            </div></Fragment>
          );
        }
        const blocks = last && live ? live : message.blocks;
        return (
          <Fragment key={index}>{boundary}<div className={`msg ${message.role}${last && streaming ? " live" : ""}`} data-test={`msg-${index}`}>
            <div className="msg-body">
              {message.images?.map((id) => <GeneratedPicture key={id} id={id} streaming={streaming} hooks={hooks} />)}
              {!!message.activities?.length && <section className="tool-activities" aria-label="Task steps">{message.activities.map((activity) => activity.tool === "changes" ? <ChangesCard key={activity.id} activity={activity} streaming={streaming} hooks={hooks} /> : <div className={`tool-activity ${activity.state}`} key={activity.id} data-test={`tool-${activity.tool}`}>
                <div className="tool-heading"><strong>{({ planning: "Choosing next step", web_search: "Search the web", web_read: "Read a page", terminal: "Project command", answer: "Write answer", edit_file: "Edit a file" } as Record<string, string>)[activity.tool] || activity.tool}</strong><span>{activity.state === "waiting" ? "Needs your review" : activity.state}</span></div>
                {activity.state === "waiting" ? <>{activity.tool === "edit_file" ? <Diff text={activity.input} testId="tool-command" /> : <pre data-test="tool-command">{activity.input}</pre>}<p className="sheet-note">{activity.output}</p><div className="tool-review"><button className="primary" data-test="tool-approve" disabled={!streaming} onClick={() => invoke("tools_review", { activityId: activity.id, approved: true }).catch(hooks.report)}>{activity.tool === "terminal" ? "Run command" : activity.tool === "edit_file" ? "Apply edit" : "Allow lookup"}</button><button data-test="tool-decline" disabled={!streaming} onClick={() => invoke("tools_review", { activityId: activity.id, approved: false }).catch(hooks.report)}>Skip</button></div></> : (activity.input || activity.output) && <><details open={activity.tool === "edit_file" || (activity.state === "running" && activity.tool === "terminal")}><summary>Details</summary>{activity.input && (activity.tool === "edit_file" ? <Diff text={activity.input} /> : <pre>{activity.input}</pre>)}{activity.output && <pre>{activity.output}</pre>}</details>{activity.tool === "edit_file" && activity.state === "completed" && <button type="button" className="mini" data-test="tool-undo-edit" disabled={streaming} onClick={() => invoke("tools_undo_edit", { activityId: activity.id }).catch(hooks.report)}>Undo edit</button>}</>}
              </div>)}</section>}
              <Blocks blocks={blocks} needle={needle} report={hooks.report} onSaveSnippet={!streaming ? beginSave : undefined} live={last && !streaming} onSend={hooks.send} onStage={hooks.stage} onEdits={hooks.applyEdits} writing={last && streaming} />
              {last && !streaming && message.stats ? <CompareBar index={index} current={{ text: message.text, model: message.stats.model, tokens: message.stats.tokens, ms: message.stats.ms }} streaming={streaming} report={hooks.report} /> : null}
              {!!message.sources?.length && <section className="answer-web-sources" data-test="answer-web-sources" aria-label="Online sources provided to the model"><strong>Online sources</strong>{message.sources.map((source, i) => <div key={`${i}:${source.url}`}><button type="button" className="web-citation" data-test={`answer-source-${index}-${i}`} title={source.url} onClick={() => invoke("web_open_source", { url: source.url }).catch(hooks.report)}>[{i + 1}] {source.title || source.url}</button><span>{source.kind === "page" ? "Page excerpt" : `Search snippet${source.provider ? ` · ${source.provider}` : ""}`} · retrieved {new Date(source.fetched * 1000).toLocaleDateString()}</span></div>)}</section>}
              {!streaming && message.stats?.files?.length ? <FileSources files={message.stats.files} index={index} /> : null}
              {last && streaming && messages[index - 1]?.text.startsWith("/image ") && !messages[index - 1]?.sources?.length && <div className="image-progress" data-test="image-generation-progress"><p>{progress.stage}</p><progress value={Number.isFinite(progress.fraction) ? Math.max(0, Math.min(1, progress.fraction)) : 0} max={1} /><p className="sheet-note">On this computer · use Stop to cancel</p></div>}
              {!blocks.length && !message.activities?.some((activity) => activity.state === "waiting" || activity.state === "running") ? (
                <span className="typing-dots" data-test="typing" aria-label="Thinking">
                  <span className="dot" /><span className="dot" /><span className="dot" />
                </span>
              ) : null}
            </div>
            <div className="msg-actions">
              <button className="mini" data-test={`copy-${index}`} onClick={() => invoke("chat_copy", { text: message.text })}>Copy</button>
              {!streaming && message.text.trim() ? (
                <button className="mini" data-test={`save-reply-${index}`} title="Preview and save this reply to a file" onClick={() => openSave(index)}>Save as file</button>
              ) : null}
              {!streaming && message.text.trim() ? (
                <button className="mini" data-test={`act-assistant-${index}`} title="Have Mobius act from this reply" onClick={() => hooks.act(message.text)}>Act</button>
              ) : null}
              {!streaming ? <button className="mini" data-test={`fork-${index}`} title="Continue from here in a new chat" onClick={() => hooks.fork(index)}>Fork</button> : null}
              {last && !streaming && !message.images?.length && !message.activities?.length ? (
                <Regenerate disabled={streaming} report={hooks.report} />
              ) : null}
              {!streaming && replyDetails(message.stats) ? <span className="msg-stats" data-test={`stats-${index}`}>{replyDetails(message.stats)}</span> : null}
            </div>
          </div></Fragment>
        );
      })()}</Fragment>))}
      {taskNotes(messages.length)}
      <dialog ref={saveDialog} className="sheet" data-test="save-reply-panel" aria-labelledby="save-reply-title" onClose={() => setSave(null)}>
        <div className="sheet-body">
          <header className="sheet-head">
            <h3 id="save-reply-title">Save reply as a file</h3>
            <p className="sheet-note">Edit the content, then choose where to save. Mobius will not overwrite an existing file.</p>
          </header>
          <div className="sheet-scroll">
            <label className="sheet-field" htmlFor="save-reply-text">Content</label>
            <textarea id="save-reply-text" className="save-reply-text" data-test="save-reply-text" rows={12} value={save?.body || ""} disabled={save?.busy} onChange={(event) => save && setSave({ ...save, body: event.target.value })} />
            <p className="sheet-note">Suggested name: <code data-test="save-reply-filename">{save?.defaultFilename || "mobius-reply.md"}</code></p>
            {save?.error && <p className="sheet-warning" role="alert">{save.error}</p>}
          </div>
          <footer className="sheet-actions">
            <button type="button" disabled={save?.busy} onClick={() => setSave(null)}>Cancel</button>
            <span className="sheet-actions-spacer" />
            <button type="button" className="primary" data-test="save-reply-confirm" disabled={!save?.body.trim() || save?.busy} onClick={confirmSave}>{save?.busy ? "Saving…" : "Choose file and save"}</button>
          </footer>
        </div>
      </dialog>
    </>
  );
}
