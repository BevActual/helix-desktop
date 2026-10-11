import { Fragment, useEffect, useState } from "react";
import { flushSync } from "react-dom";
import { invoke } from "@helix/ui";

export type Span = { kind: "text" | "code" | "bold" | "italic"; text: string };
export type Block =
  | { kind: "paragraph"; spans: Span[] }
  | { kind: "heading"; level: number; spans: Span[] }
  | { kind: "quote"; spans: Span[] }
  | { kind: "rule" }
  | { kind: "list"; ordered: boolean; items: Span[][] }
  | { kind: "code"; language: string; text: string };
export interface Message { role: string; text: string; blocks: Block[]; }

export interface ThreadHooks {
  edit: (index: number, text: string) => void;
  act: (text: string) => void;
  quote: (text: string) => void;
  fork: (index: number) => void;
  report: (error: unknown) => void;
}
export interface ThreadHandle {
  /** The whole chat, drawn now (synchronously) so the page can scroll and find in it straight away. */
  set: (messages: Message[], streaming: boolean) => void;
  /** The live reply's blocks while it streams. */
  stream: (blocks: Block[]) => void;
  /** The text to highlight (empty clears it). */
  find: (needle: string) => void;
}

/** Splits text around case-insensitive matches of the find needle; matches become `<mark class="find-hit">`. */
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

function Blocks({ blocks, needle }: { blocks: Block[]; needle: string }) {
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
          case "code":
            return (
              <div key={index} className="code-block">
                <div className="code-bar">
                  <span>{block.language || "code"}</span>
                  <button className="mini" onClick={() => invoke("chat_copy", { text: block.text })}>Copy</button>
                </div>
                <pre><code>{highlight(block.text, needle)}</code></pre>
              </div>
            );
        }
      })}
    </>
  );
}

/** The message thread. Rust owns the conversation and its markdown blocks; this draws them as elements (never markup) and sends back the
 * index of the message acted on. Find highlighting is done here, so nothing else touches these nodes. */
export function Thread({ register, hooks }: { register: (handle: ThreadHandle) => void; hooks: ThreadHooks }) {
  const [messages, setMessages] = useState<Message[]>([]);
  const [streaming, setStreaming] = useState(false);
  const [live, setLive] = useState<Block[] | null>(null);
  const [needle, setNeedle] = useState("");

  useEffect(() => {
    register({
      set: (next, isStreaming) => flushSync(() => { setMessages(next); setStreaming(isStreaming); setLive(null); }),
      stream: (blocks) => flushSync(() => setLive(blocks)),
      find: (value) => flushSync(() => setNeedle(value)),
    });
  }, [register]);

  return (
    <>
      {messages.map((message, index) => {
        const last = index === messages.length - 1;
        if (message.role === "user") {
          return (
            <div key={index} className="msg user" data-test={`msg-${index}`}>
              <div className="msg-body">{highlight(message.text, needle)}</div>
              <div className="msg-actions">
                <button className="mini" data-test={`edit-${index}`} onClick={() => hooks.edit(index, message.text)}>Edit</button>
                <button className="mini" data-test={`act-${index}`} title="Have Mobius change the desktop using this" onClick={() => hooks.act(message.text)}>Act</button>
                <button className="mini" data-test={`quote-${index}`} title="Insert into the composer" onClick={() => hooks.quote(message.text)}>Quote</button>
                {!streaming ? <button className="mini" data-test={`fork-user-${index}`} title="Continue from here in a new chat" onClick={() => hooks.fork(index)}>Fork</button> : null}
              </div>
            </div>
          );
        }
        const blocks = last && live ? live : message.blocks;
        return (
          <div key={index} className={`msg ${message.role}${last && streaming ? " live" : ""}`} data-test={`msg-${index}`}>
            <div className="msg-body">
              <Blocks blocks={blocks} needle={needle} />
              {!blocks.length ? (
                <span className="typing-dots" data-test="typing" aria-label="Thinking">
                  <span className="dot" /><span className="dot" /><span className="dot" />
                </span>
              ) : null}
            </div>
            <div className="msg-actions">
              <button className="mini" data-test={`copy-${index}`} onClick={() => invoke("chat_copy", { text: message.text })}>Copy</button>
              {!streaming && message.text.trim() ? (
                <button className="mini" data-test={`act-assistant-${index}`} title="Have Mobius act from this reply" onClick={() => hooks.act(message.text)}>Act</button>
              ) : null}
              {!streaming ? <button className="mini" data-test={`fork-${index}`} title="Continue from here in a new chat" onClick={() => hooks.fork(index)}>Fork</button> : null}
              {last && !streaming ? (
                <button className="mini" data-test="regenerate" onClick={() => invoke("chat_regenerate").catch(hooks.report)}>Regenerate</button>
              ) : null}
            </div>
          </div>
        );
      })}
    </>
  );
}
