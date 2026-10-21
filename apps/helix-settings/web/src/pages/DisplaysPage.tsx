import { useCallback, useEffect, useRef, useState } from "react";
import { invoke, type RowView } from "@helix/ui";
import { RowsPage, type PageHandle, type PageSpec } from "./RowsPage";

interface Screen {
  name: string;
  label: string;
  x: number;
  y: number;
  w: number;
  h: number;
}

const WIDTH = 496;
const HEIGHT = 190;
const PAD = 14;

/** Where the screens are drawn: the desktop's bounds, scaled to fit the card, with the same scale on both axes. */
function frame(screens: Screen[]) {
  const left = Math.min(...screens.map((s) => s.x));
  const top = Math.min(...screens.map((s) => s.y));
  const right = Math.max(...screens.map((s) => s.x + s.w));
  const bottom = Math.max(...screens.map((s) => s.y + s.h));
  // Room beyond the desktop on every side, so a screen can be dragged past the edge to the far side of another.
  const spanW = right - left;
  const spanH = bottom - top;
  const scale = Math.min((WIDTH - PAD * 2) / (spanW * 1.4), (HEIGHT - PAD * 2) / (spanH * 1.4));
  const originX = (WIDTH - spanW * scale) / 2 - left * scale;
  const originY = (HEIGHT - spanH * scale) / 2 - top * scale;
  return { scale, originX, originY };
}

/** The arrangement picture: each screen in use is a box; drag one beside another. The compositor snaps it against a neighbour. */
function Arrangement({ screens, onMove, busy }: { screens: Screen[]; onMove: (name: string, x: number, y: number) => void; busy: boolean }) {
  const [drag, setDrag] = useState<{ name: string; dx: number; dy: number } | null>(null);
  // The latest offset lives in a ref too: a quick drag can end before React has drawn the last move.
  const offset = useRef({ dx: 0, dy: 0 });
  const view = useRef(frame(screens));
  // The scale holds still while a screen is being dragged, so the picture does not shift under the pointer.
  if (!drag) view.current = frame(screens);
  const { scale, originX, originY } = view.current;

  // Mouse events on the window rather than pointer capture: the drag keeps following the pointer when it leaves the box, and it also works under the
  // compositor's synthetic pointer.
  const down = (event: React.MouseEvent, screen: Screen) => {
    if (busy || event.button !== 0) return;
    event.preventDefault();
    const from = { x: event.clientX, y: event.clientY };
    offset.current = { dx: 0, dy: 0 };
    setDrag({ name: screen.name, dx: 0, dy: 0 });
    const move = (e: MouseEvent) => {
      offset.current = { dx: e.clientX - from.x, dy: e.clientY - from.y };
      setDrag({ name: screen.name, ...offset.current });
    };
    const up = () => {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
      const { dx, dy } = offset.current;
      setDrag(null);
      if (Math.abs(dx) < 3 && Math.abs(dy) < 3) return;
      onMove(screen.name, Math.round(screen.x + dx / scale), Math.round(screen.y + dy / scale));
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
  };

  return (
    <div className="arrangement" style={{ width: WIDTH, height: HEIGHT }} data-test="arrangement" role="group" aria-label="Arrangement of your displays">
      {screens.map((screen, index) => {
        const dragging = drag?.name === screen.name;
        const left = originX + screen.x * scale + (dragging ? drag.dx : 0);
        const top = originY + screen.y * scale + (dragging ? drag.dy : 0);
        return (
          <div
            key={screen.name}
            className={`arrangement-screen${dragging ? " dragging" : ""}`}
            data-test={`arrange-${screen.name}`}
            style={{ left, top, width: screen.w * scale, height: screen.h * scale }}
            onMouseDown={(event) => down(event, screen)}
          >
            <span className="arrangement-number">{index + 1}</span>
            <span className="arrangement-name">{screen.name}</span>
          </div>
        );
      })}
    </div>
  );
}

/** Displays: the arrangement picture above the rows for each screen. */
export function DisplaysPage({ spec, register, initialRows, onRows, report }: { spec: PageSpec; register: (handle: PageHandle) => void; initialRows: RowView[] | null; onRows: (rows: RowView[]) => void; report: (text: string) => void }) {
  const [screens, setScreens] = useState<Screen[]>([]);
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState("");
  const [serial, setSerial] = useState(0);

  const refreshScreens = useCallback(() => {
    invoke<Screen[]>("display_layout").then(setScreens, () => undefined);
  }, []);
  useEffect(refreshScreens, [refreshScreens, serial]);

  // The rows reload on every change made in them (a scale or resolution moves the neighbours), so the picture follows.
  const rowsSeen = useCallback((rows: RowView[]) => { onRows(rows); setSerial((n) => n + 1); }, [onRows]);
  const handle = useCallback((ready: PageHandle) => {
    register({ setRows: ready.setRows, refresh: () => { ready.refresh(); setSerial((n) => n + 1); } });
  }, [register]);

  const moved = (name: string, x: number, y: number) => {
    setBusy(true);
    setNote("");
    invoke<Screen[]>("display_move", { name, x, y }).then(
      (next) => { setScreens(next); setBusy(false); setSerial((n) => n + 1); },
      (error) => { setBusy(false); setNote(String(error)); report(`arrange: ${String(error)}`); refreshScreens(); },
    );
  };

  return (
    <>
      {screens.length > 1 ? (
        <>
          <h4>Arrangement</h4>
          <div className="rows-card arrangement-card">
            <Arrangement screens={screens} onMove={moved} busy={busy} />
            <div className="setting-about" data-test="arrangement-note">{note || "Drag a screen to where it sits on your desk. It snaps against its neighbour so the pointer can cross."}</div>
          </div>
        </>
      ) : null}
      <RowsPage spec={spec} initialRows={initialRows} register={handle} onRows={rowsSeen} report={report} />
    </>
  );
}
