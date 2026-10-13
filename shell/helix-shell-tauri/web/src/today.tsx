import { useCallback, useEffect, useRef, useState } from "react";
import { invoke, type MonthView } from "@helix/ui";
import { Icon } from "./icon";

export interface TodayHandle {
  /** The panel was opened: show the current month. */
  open: () => void;
  /** The minute ticked: if the day changed, move to the new current month. */
  tick: () => void;
}

interface Day {
  year: number;
  month: number;
  day: number;
}

const weekStart = () => (/^en-(US|CA)|^ja|^pt-BR|^he/.test(navigator.language) || navigator.language === "en" ? 0 : 1);

function today(): Day {
  const now = new Date();
  return { year: now.getFullYear(), month: now.getMonth() + 1, day: now.getDate() };
}
const keyOf = (d: Day) => d.year * 10000 + d.month * 100 + d.day;
const pad = (n: number) => String(n).padStart(2, "0");

/** The Today panel: the month grid is built in Rust (leap years, week starts, week numbers); this draws it and moves between months. */
export function Today({ register }: { register: (handle: TodayHandle) => void }) {
  const [view, setView] = useState<MonthView | null>(null);
  const [now, setNow] = useState(() => new Date());
  const [picked, setPicked] = useState<string | null>(null);
  const shown = useRef<{ year: number; month: number } | null>(null);
  const drawnDay = useRef<number | null>(null);

  const show = useCallback((delta: number, reset = false) => {
    const current = today();
    const base = reset || !shown.current ? current : shown.current;
    invoke<MonthView>("calendar_view", { year: base.year, month: base.month, delta, today: current, weekStart: weekStart() }).then((next) => {
      shown.current = { year: next.year, month: next.month };
      drawnDay.current = keyOf(current);
      setNow(new Date());
      setView(next);
    });
  }, []);

  useEffect(() => {
    register({
      open: () => {
        setPicked(null);
        show(0, true);
      },
      tick: () => {
        if (drawnDay.current !== null && drawnDay.current !== keyOf(today())) show(0, true);
      },
    });
    show(0, true);
  }, [register, show]);

  // Left and Right move through the months while the panel is open (the panel has no text field to type in).
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (document.body.dataset.panel !== "notifications") return;
      if (event.key === "ArrowLeft") show(-1);
      else if (event.key === "ArrowRight") show(1);
      else if (event.key.toLowerCase() === "t") show(0, true);
    };
    document.addEventListener("keydown", onKey);
    return () => document.removeEventListener("keydown", onKey);
  }, [show]);

  const current = today();
  const first = new Date(2024, 0, 7 + (view?.week_start ?? 1)); // a Sunday, then a Monday
  const onCurrentMonth = view ? view.year === current.year && view.month === current.month : true;
  return (
    <>
      <div id="today-head">
        <div id="today-weekday">{now.toLocaleDateString([], { weekday: "long" })}</div>
        <div id="today-date">{now.toLocaleDateString([], { day: "numeric", month: "long", year: "numeric" })}</div>
      </div>
      <div id="cal-bar">
        <div id="cal-title">{view ? new Date(view.year, view.month - 1, 1).toLocaleDateString([], { month: "long", year: "numeric" }) : ""}</div>
        <button id="cal-prev" className="cal-btn" data-test="cal-prev" title="Previous month" aria-label="Previous month" onClick={() => show(-1)}>
          <Icon name="chevron-left" />
        </button>
        <button id="cal-today" className="cal-btn cal-now" data-test="cal-today" style={{ visibility: onCurrentMonth ? "hidden" : "visible" }} onClick={() => show(0, true)}>
          Today
        </button>
        <button id="cal-next" className="cal-btn" data-test="cal-next" title="Next month" aria-label="Next month" onClick={() => show(1)}>
          <Icon name="chevron-right" />
        </button>
      </div>
      <div id="cal-grid">
        {view ? (
          <>
            <span />
            {Array.from({ length: 7 }, (_, i) => (
              <span key={i} className="cal-dow">
                {new Date(first.getFullYear(), 0, first.getDate() + i).toLocaleDateString([], { weekday: "narrow" })}
              </span>
            ))}
            {view.weeks.map((week) => (
              <FragmentRow key={`${week.iso_week}-${week.days[0]?.day}`} week={week} picked={picked} onPick={setPicked} />
            ))}
          </>
        ) : null}
      </div>
    </>
  );
}

function FragmentRow({ week, picked, onPick }: { week: MonthView["weeks"][number]; picked: string | null; onPick: (key: string) => void }) {
  return (
    <>
      <span className="cal-week">{week.iso_week}</span>
      {week.days.map((cell) => {
        const key = `${cell.year}-${pad(cell.month)}-${pad(cell.day)}`;
        const classes = ["cal-day", cell.in_month ? "" : "out", cell.today ? "today" : "", cell.weekend ? "weekend" : "", picked === key ? "picked" : ""].filter(Boolean).join(" ");
        return (
          <span key={key} className={classes} data-test={`day-${key}`} onClick={() => onPick(picked === key ? "" : key)}>
            {cell.day}
          </span>
        );
      })}
    </>
  );
}
