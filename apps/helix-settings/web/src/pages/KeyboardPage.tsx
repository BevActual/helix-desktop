import { Fragment, useCallback, useEffect, useRef, useState } from "react";
import { invoke, RowGroups, type Change, type KeybindRow, type RowView, type ShortcutGroup, type ShortcutRow } from "@helix/ui";
import type { PageHandle } from "./RowsPage";

function ChordEditor({ row, onDone, report }: { row: KeybindRow; onDone: (saved: boolean) => void; report: (text: string) => void }) {
  const [text, setText] = useState(row.chords.join("; "));
  const input = useRef<HTMLInputElement>(null);
  const entered = useRef(true);
  useEffect(() => {
    input.current?.focus();
    input.current?.select();
  }, []);
  const save = () => {
    invoke("keybinds_set", { id: row.id, chords: text }).then(
      () => onDone(true),
      (error) => report(`${row.id}: ${String(error)}`),
    );
  };
  return (
    <div className="setting-row" data-test={`keybind-form:${row.id}`}>
      <input
        ref={input}
        type="text"
        className="slider-input"
        value={text}
        data-test={`keybind-input:${row.id}`}
        onChange={(event) => setText(event.target.value)}
        // Typing replaces what is there: the whole list is selected when the field opens or is entered again.
        onFocus={(event) => { entered.current = true; event.target.select(); }}
        onMouseUp={(event) => { if (entered.current) { event.preventDefault(); entered.current = false; } }}
        onKeyDown={(event) => {
          if (event.key === "Enter") save();
          else if (event.key === "Escape") {
            event.stopPropagation();
            onDone(false);
          }
        }}
      />
      <button className="segment" data-test={`keybind-save:${row.id}`} onClick={save}>Save</button>
      <button className="segment" data-test={`keybind-cancel:${row.id}`} onClick={() => onDone(false)}>Cancel</button>
    </div>
  );
}


/** The name a compositor config uses for the key that was pressed (`e`, `Return`, `Left`, `F5`, `comma`), or null for a key that is only a modifier. */
function keyName(event: React.KeyboardEvent): string | null {
  const { key, code } = event;
  if (["Control", "Alt", "Shift", "Meta", "AltGraph", "CapsLock", "Dead"].includes(key)) return null;
  const named: Record<string, string> = {
    Enter: "Return", " ": "space", ArrowLeft: "Left", ArrowRight: "Right", ArrowUp: "Up", ArrowDown: "Down", Backspace: "BackSpace",
    Delete: "Delete", Tab: "Tab", Home: "Home", End: "End", PageUp: "Prior", PageDown: "Next", Insert: "Insert", PrintScreen: "Print",
  };
  if (named[key]) return named[key];
  if (/^F([1-9]|1\d|2[0-4])$/.test(key)) return key;
  // The physical key decides punctuation and digits, so Shift+1 is still "1", not "!".
  const punctuation: Record<string, string> = {
    Comma: "comma", Period: "period", Minus: "minus", Equal: "equal", Slash: "slash", Backslash: "backslash", Semicolon: "semicolon",
    Quote: "apostrophe", BracketLeft: "bracketleft", BracketRight: "bracketright", Backquote: "grave",
  };
  if (punctuation[code]) return punctuation[code];
  if (/^Digit\d$/.test(code)) return code.slice(5);
  if (/^Key[A-Z]$/.test(code)) return code.slice(3).toLowerCase();
  return null;
}

interface Mods { mod: boolean; ctrl: boolean; alt: boolean; shift: boolean }
const chordOf = (mods: Mods, key: string | null) =>
  [mods.mod && "Mod", mods.ctrl && "Ctrl", mods.shift && "Shift", mods.alt && "Alt", key ?? ""].filter((part) => part !== false && part !== "").join("+");

/** Asks for new keys for a shortcut. Super is a switch here, not a key to hold: holding it would run the shortcuts it is already used for. */
function ShortcutRecorder({ row, onDone, report }: { row: ShortcutRow; onDone: (changed: boolean) => void; report: (text: string) => void }) {
  const [mods, setMods] = useState<Mods>({ mod: row.chord.startsWith("Mod+"), ctrl: false, alt: false, shift: false });
  const [key, setKey] = useState<string | null>(null);
  const [taken, setTaken] = useState<string | null>(null);
  const [error, setError] = useState("");
  const catcher = useRef<HTMLDivElement>(null);
  useEffect(() => { catcher.current?.focus(); }, []);
  const chord = chordOf(mods, key);
  const apply = (to: string, replace: boolean) => {
    invoke("shortcut_move", { chord: row.chord, to, replace }).then(
      () => onDone(true),
      (failure) => {
        const text = String(failure);
        if (text.startsWith("taken: ")) { setTaken(text.slice(7) || "another shortcut"); return; }
        setError(text);
        report(`shortcut: ${text}`);
      },
    );
  };
  const toggle = (name: keyof Mods) => (setMods((m) => ({ ...m, [name]: !m[name] })), setTaken(null), catcher.current?.focus());
  return (
    <div className="setting-row recorder" data-test={`shortcut-recorder:${row.chord}`}>
      <div className="setting-text">
        <div className="setting-title">{row.title}</div>
        <div className="setting-about">Choose the modifiers, then press the key. Escape cancels.</div>
        {error ? <div className="setting-about warn" role="alert" data-test="shortcut-error">{error}</div> : null}
        {taken ? <div className="setting-about warn" role="alert" data-test="shortcut-taken">{chord} is already used for {taken}. Replacing turns that one off.</div> : null}
      </div>
      <div
        className="keycaps recorder-keys"
        ref={catcher}
        tabIndex={0}
        data-test="shortcut-catcher"
        onKeyDown={(event) => {
          event.preventDefault();
          event.stopPropagation();
          if (event.key === "Escape") { onDone(false); return; }
          const name = keyName(event);
          if (!name) return;
          setKey(name);
          setTaken(null);
          setError("");
          apply(chordOf({ ...mods, ctrl: mods.ctrl || event.ctrlKey, alt: mods.alt || event.altKey, shift: mods.shift || event.shiftKey }, name), false);
        }}
      >
        {(["mod", "ctrl", "shift", "alt"] as const).map((name) => (
          <button key={name} type="button" className={`segment${mods[name] ? " on" : ""}`} aria-pressed={mods[name]} data-test={`shortcut-mod-${name}`}
            onClick={() => toggle(name)}>{name === "mod" ? "Super" : name[0].toUpperCase() + name.slice(1)}</button>
        ))}
        <kbd data-test="shortcut-preview">{key ? chord : "press a key…"}</kbd>
      </div>
      {taken ? <button className="segment" data-test="shortcut-replace" onClick={() => apply(chord, true)}>Replace</button> : null}
      <button className="segment" data-test="shortcut-cancel" onClick={() => onDone(false)}>Cancel</button>
    </div>
  );
}

export function KeyboardPage({ register, report }: { register: (handle: PageHandle) => void; report: (text: string) => void }) {
  const [keybinds, setKeybinds] = useState<KeybindRow[] | null>(null);
  const [groups, setGroups] = useState<ShortcutGroup[] | null>(null);
  const [editing, setEditing] = useState<string | null>(null);
  const [recording, setRecording] = useState<string | null>(null);
  const [typing, setTyping] = useState<RowView[]>([]);

  const load = useCallback(() => {
    invoke<KeybindRow[]>("keybinds_view").then(setKeybinds, () => setKeybinds([]));
    invoke<ShortcutGroup[]>("shortcuts_view").then(setGroups, () => setGroups([]));
    invoke<RowView[]>("typing_rows").then(setTyping, () => setTyping([]));
  }, []);

  useEffect(() => {
    register({ setRows: () => undefined, refresh: load });
    load();
  }, [register, load]);

  if (!keybinds || !groups) return null;
  return (
    <div data-test="react-page">
      <h4>Helix Shell Navigation</h4>
      <div className="setting-about note">
        Shortcuts for moving in Helix surfaces. Use Ctrl, Alt or Shift — Super belongs to the compositor. Separate alternatives with a semicolon.
      </div>
      {keybinds.length ? (
        <div className="rows-card">
          {keybinds.map((row) =>
            editing === row.id ? (
              <ChordEditor
                key={row.id}
                row={row}
                report={report}
                onDone={(saved) => {
                  setEditing(null);
                  if (saved) load();
                }}
              />
            ) : (
              <div className="setting-row" key={row.id} data-test={`keybind:${row.id}`}>
                <div className="setting-text">
                  <div className="setting-title">{row.title}</div>
                  {row.overridden ? <div className="setting-about">Changed from the default.</div> : null}
                </div>
                <div className="keycaps" data-test={`keybind-chords:${row.id}`}>
                  {row.chords.map((chord) => (
                    <kbd key={chord}>{chord}</kbd>
                  ))}
                </div>
                <button className="segment" data-test={`keybind-edit:${row.id}`} onClick={() => setEditing(row.id)}>Edit</button>
              </div>
            ),
          )}
        </div>
      ) : (
        <div className="empty">Navigation shortcuts are unavailable until Settings is connected.</div>
      )}
      {typing.length ? (
        <RowGroups
          rows={typing}
          onChange={(id: string, change: Change) =>
            invoke<RowView[]>("typing_change", { id, change }).then(setTyping, (error) => report(`${id}: ${String(error)}`))
          }
        />
      ) : null}
      <h4>Compositor</h4>
      <div className="setting-about note">
        The shortcuts of your windows and desktop. Changes are kept by the compositor over your config files, which are never edited, and take effect at once.
      </div>
      {groups.map((group) => (
        <Fragment key={group.name}>
          <h4>{group.name}</h4>
          <div className="rows-card">
            {group.rows.map((row) =>
              recording === row.chord ? (
                <ShortcutRecorder
                  key={row.chord}
                  row={row}
                  report={report}
                  onDone={(changed) => {
                    setRecording(null);
                    // The compositor gives a chord back by reloading its configuration, which takes a moment.
                    if (changed) setTimeout(load, 500);
                  }}
                />
              ) : (
                <div className="setting-row" key={`${row.chord}:${row.title}`} data-test={`shortcut:${row.title}`}>
                  <div className="setting-text">
                    <div className="setting-title">{row.title}</div>
                    {row.conflict ? <div className="setting-about warn">Also bound in another place. Only one of them will work.</div> : null}
                    {row.overridden ? <div className="setting-about">{row.off ? "Turned off here." : "Changed here."}</div> : null}
                  </div>
                  <div className="keycaps" data-test={`shortcut-keys:${row.chord}`}>
                    {row.keys.map((key) => (
                      <kbd key={key}>{key}</kbd>
                    ))}
                  </div>
                  {row.movable ? <button className="segment" data-test={`shortcut-change:${row.chord}`} onClick={() => setRecording(row.chord)}>Change</button> : null}
                  {row.movable && !row.overridden ? (
                    <button className="segment" data-test={`shortcut-off:${row.chord}`} onClick={() => invoke("shortcut_off", { chord: row.chord }).then(load, (e) => report(`shortcut: ${String(e)}`))}>Turn off</button>
                  ) : null}
                  {row.overridden ? (
                    <button className="segment" data-test={`shortcut-reset:${row.chord}`} onClick={() => invoke("shortcut_reset", { chord: row.chord }).then(() => setTimeout(load, 500), (e) => report(`shortcut: ${String(e)}`))}>Reset</button>
                  ) : null}
                </div>
              ),
            )}
          </div>
        </Fragment>
      ))}
      {groups.length === 0 ? <div className="empty">No compositor key binds found.</div> : null}
    </div>
  );
}
