import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@helix/ui";

/** What Rust offers for an application: display-safe text and a `data:` image (or none). Mirrors `AppHit` in the shell's main.rs. */
export interface AppHit {
  id: string;
  name: string;
  generic: string;
  icon: string;
}

export interface LauncherHandle {
  /** The panel was opened: start from an empty search and focus it. */
  open: () => void;
}

/** A coloured tile with the application's first letter, the colour taken from its name, for an application with no icon. */
function LetterTile({ name }: { name: string }) {
  let hash = 0;
  for (const ch of name) hash = (hash * 31 + (ch.codePointAt(0) ?? 0)) >>> 0;
  return (
    <div className="dock-tile icon" style={{ background: `hsl(${hash % 360} 45% 42%)` }}>
      {Array.from(name)[0] || "•"}
    </div>
  );
}

/** The name with the letters the person typed picked out (display only; what is launched is the id Rust listed). */
function Highlighted({ text, query }: { text: string; query: string }) {
  const needle = query.trim().toLowerCase();
  if (!needle) return <>{text}</>;
  const at = text.toLowerCase().indexOf(needle);
  if (at < 0) return <>{text}</>;
  return (
    <>
      {text.slice(0, at)}
      <mark>{text.slice(at, at + needle.length)}</mark>
      {text.slice(at + needle.length)}
    </>
  );
}

export function Launcher({ register }: { register: (handle: LauncherHandle) => void }) {
  const [query, setQuery] = useState("");
  const [hits, setHits] = useState<AppHit[]>([]);
  const [selected, setSelected] = useState(0);
  const [asked, setAsked] = useState(false);
  const sequence = useRef(0);
  const input = useRef<HTMLInputElement>(null);
  const list = useRef<HTMLDivElement>(null);

  const search = useCallback((text: string) => {
    const mine = ++sequence.current;
    invoke<AppHit[]>("launcher_query", { query: text }).then((found) => {
      if (mine !== sequence.current) return; // a newer keystroke already asked
      setHits(found);
      setSelected(0);
      setAsked(true);
    });
  }, []);

  useEffect(() => {
    register({
      open: () => {
        setQuery("");
        setAsked(false);
        search("");
        input.current?.focus();
      },
    });
  }, [register, search]);

  // The selected row stays in view while the arrow keys move through a long list.
  useEffect(() => {
    list.current?.children[selected]?.scrollIntoView({ block: "nearest" });
  }, [selected]);

  const launch = (index: number) => {
    const hit = hits[index];
    if (hit) invoke("launcher_launch", { id: hit.id });
  };

  return (
    <>
      <input
        id="lq"
        ref={input}
        placeholder="Search applications"
        autoComplete="off"
        spellCheck={false}
        value={query}
        data-test="launcher-query"
        onChange={(event) => {
          setQuery(event.target.value);
          search(event.target.value);
        }}
        onKeyDown={(event) => {
          if (event.key === "ArrowDown" || event.key === "ArrowUp") {
            event.preventDefault();
            if (!hits.length) return;
            setSelected((current) => (current + (event.key === "ArrowDown" ? 1 : hits.length - 1)) % hits.length);
          } else if (event.key === "Home" && hits.length) {
            event.preventDefault();
            setSelected(0);
          } else if (event.key === "End" && hits.length) {
            event.preventDefault();
            setSelected(hits.length - 1);
          } else if (event.key === "Enter") {
            event.preventDefault();
            launch(selected);
          }
        }}
      />
      {asked && !query.trim() && hits.length ? <div className="launcher-label">Suggested</div> : null}
      <div id="results" ref={list}>
        {hits.map((hit, index) => (
          <div
            key={hit.id}
            className={`row${index === selected ? " selected" : ""}`}
            data-test={`launcher-row:${hit.id}`}
            // The pointer only moves the selection when it really moves (a list that redraws under a still pointer must not steal it).
            onMouseMove={() => { if (selected !== index) setSelected(index); }}
            onClick={() => launch(index)}
          >
            {hit.icon ? <img className="icon" alt="" src={hit.icon} /> : <LetterTile name={hit.name} />}
            <span className="name">
              <Highlighted text={hit.name} query={query} />
            </span>
            {hit.generic ? <span className="generic">{hit.generic}</span> : null}
            {index === selected ? <span className="enter-hint" aria-hidden="true">↵</span> : null}
          </div>
        ))}
      </div>
      <div id="empty">{asked && !hits.length && query.trim() ? "No matching applications" : ""}</div>
    </>
  );
}
