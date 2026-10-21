import { Fragment, useCallback, useEffect, useRef, useState } from "react";
import { invoke, RowGroups, type Change, type KeybindRow, type RowView, type ShortcutGroup } from "@helix/ui";
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

export function KeyboardPage({ register, report }: { register: (handle: PageHandle) => void; report: (text: string) => void }) {
  const [keybinds, setKeybinds] = useState<KeybindRow[] | null>(null);
  const [groups, setGroups] = useState<ShortcutGroup[] | null>(null);
  const [editing, setEditing] = useState<string | null>(null);
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
        These are the key binds in your compositor config. Settings shows them and flags clashes; it does not change them.
      </div>
      {groups.map((group) => (
        <Fragment key={group.name}>
          <h4>{group.name}</h4>
          <div className="rows-card">
            {group.rows.map((row) => (
              <div className="setting-row" key={`${row.title}:${row.keys.join("+")}`} data-test={`shortcut:${row.title}`}>
                <div className="setting-text">
                  <div className="setting-title">{row.title}</div>
                  {row.conflict ? <div className="setting-about warn">Also bound in another place. Only one of them will work.</div> : null}
                </div>
                <div className="keycaps">
                  {row.keys.map((key) => (
                    <kbd key={key}>{key}</kbd>
                  ))}
                </div>
              </div>
            ))}
          </div>
        </Fragment>
      ))}
      {groups.length === 0 ? <div className="empty">No compositor key binds found.</div> : null}
    </div>
  );
}
