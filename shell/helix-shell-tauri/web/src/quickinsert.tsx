import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";
import { Icon } from "./icon";
import type { QuickInsertResults, QuickInsertRow } from "./types";

export interface QuickInsertHandle {
  /** The palette was opened: emoji, empty search, focused. */
  open: () => void;
}

type Source = "emoji" | "clipboard" | "actions";
const ORDER: Source[] = ["emoji", "clipboard", "actions"];
const COLUMNS = 8;
const TABS: [Source, string][] = [["emoji", "Emoji"], ["clipboard", "Clipboard"], ["actions", "Actions"]];

/** The Caps Lock palette. Rust holds the emoji, the clipboard history and what each row means; this draws display rows and sends back
 * the position of the one chosen, so it can neither invent text to insert nor name an action it was not offered. */
export function QuickInsert({ register }: { register: (handle: QuickInsertHandle) => void }) {
  const [source, setSource] = useState<Source>("emoji");
  const [text, setText] = useState("");
  const [rows, setRows] = useState<QuickInsertRow[]>([]);
  const [selected, setSelected] = useState(0);
  const generation = useRef(0);
  const sequence = useRef(0);
  const input = useRef<HTMLInputElement>(null);
  const list = useRef<HTMLDivElement>(null);

  const search = useCallback((nextSource: Source, nextText: string) => {
    const mine = ++sequence.current;
    invoke<QuickInsertResults>("quick_insert_query", { source: nextSource, text: nextText }).then((results) => {
      if (mine !== sequence.current) return; // a newer keystroke already asked
      setRows(results.rows);
      generation.current = results.generation;
      setSelected(0);
    });
  }, []);

  useEffect(() => {
    register({
      open: () => {
        setSource("emoji"); // always opens on emoji, so the same keys do the same thing every time
        setText("");
        search("emoji", "");
        input.current?.focus();
      },
    });
  }, [register, search]);

  // The Rust-side tests and the stylesheet read the active source from the body.
  useEffect(() => {
    document.body.dataset.qi = source;
  }, [source]);

  useEffect(() => {
    list.current?.children[selected]?.scrollIntoView({ block: "nearest" });
  }, [selected, rows]);

  const choose = (index: number) => {
    // Rust only honours the choice for the results it was drawn from; if newer results exist, the press is ignored and can be repeated.
    if (index >= 0 && index < rows.length) {
      invoke("quick_insert_activate", { generation: generation.current, index }).catch(() => search(source, text));
    }
  };
  const switchTab = (next: Source) => {
    setSource(next);
    search(next, text);
  };
  const grid = source === "emoji";

  return (
    <>
      <div id="qi-field">
        <Icon name="search" />
        <input
          id="qi-input"
          ref={input}
          placeholder="Search emoji, clipboard and actions"
          autoComplete="off"
          value={text}
          onChange={(event) => {
            setText(event.target.value);
            search(source, event.target.value);
          }}
          onKeyDown={(event) => {
            const move = (delta: number) => {
              event.preventDefault();
              if (!rows.length) return;
              setSelected((current) => Math.max(0, Math.min(rows.length - 1, current + delta)));
            };
            if (event.key === "ArrowDown") move(grid ? COLUMNS : 1);
            else if (event.key === "ArrowUp") move(grid ? -COLUMNS : -1);
            else if (event.key === "ArrowRight" && grid) move(1);
            else if (event.key === "ArrowLeft" && grid) move(-1);
            else if (event.key === "Tab") {
              event.preventDefault();
              switchTab(ORDER[(ORDER.indexOf(source) + (event.shiftKey ? -1 : 1) + ORDER.length) % ORDER.length]);
            } else if (event.key === "Enter") {
              event.preventDefault();
              choose(selected);
            }
          }}
        />
      </div>
      <div id="qi-tabs" role="tablist">
        {TABS.map(([value, label]) => (
          <button
            key={value}
            className={`qi-tab${value === source ? " current" : ""}`}
            id={`qi-tab-${value}`}
            data-test={`qi-tab-${value}`}
            data-source={value}
            onClick={() => {
              switchTab(value);
              input.current?.focus();
            }}
          >
            {label}
          </button>
        ))}
        <span id="qi-spacer" />
        <button id="qi-forget" data-test="qi-forget" title="Forget everything copied so far" onClick={() => invoke("quick_insert_forget_clipboard").then(() => search(source, text))}>
          Clear history
        </button>
      </div>
      <div id="qi-results" ref={list} className={grid ? "grid" : ""}>
        {rows.map((row, index) => {
          const isSelected = index === selected;
          const props = {
            "data-test": `qi-row-${index}`,
            onClick: () => choose(index),
            onMouseMove: () => { if (selected !== index) setSelected(index); },
          };
          if (grid && row.glyph) {
            return (
              <div key={`${index}:${row.glyph}`} className={`qi-cell${isSelected ? " selected" : ""}`} title={row.label} {...props}>
                {row.glyph}
              </div>
            );
          }
          return (
            <div key={`${index}:${row.label}`} className={`qi-row${isSelected ? " selected" : ""}${row.ask ? " ask" : ""}`} {...props}>
              {row.ask ? <Icon name="sparkle" /> : null}
              {row.glyph ? <span>{row.glyph}</span> : null}
              <div className="qi-main"><div className="qi-label">{row.label}</div></div>
              <div className="qi-detail">{row.detail}</div>
            </div>
          );
        })}
      </div>
      <div id="qi-empty">{rows.length ? "" : source === "clipboard" ? "Nothing copied yet" : "No matches"}</div>
      <div id="qi-hint"><span>↵ insert</span><span>⇥ switch</span><span>esc close</span></div>
    </>
  );
}
