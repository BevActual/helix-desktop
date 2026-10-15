import { useEffect, useState } from "react";
import { Icon } from "@helix/ui";

/** What the desktop assistant reports for the task this window handed over (`org.helix.Shell.Act1`, through the host). */
export interface ActState {
  mine: boolean;
  task: number;
  phase: string;
  request: string;
  text: string;
  description: string;
  reason: string;
  failed: boolean;
}

export interface ActHooks {
  /** Cancels the task. It cannot approve anything. */
  stop: () => void;
  /** Brings the shell's own approval surface back. Approving is only ever done there. */
  review: () => void;
  dismiss: () => void;
}

export interface ActHandle {
  set: (state: ActState | null) => void;
}

/** "Done. The screen is dimmer." under a "Done" title says it twice: keep what follows. */
function afterDone(text: string): string {
  const rest = text.replace(/^done[.!:\s-]*/i, "").trim();
  return rest;
}

/** Plain words for each step, with nothing about how it works. */
function words(state: ActState): { title: string; detail: string; tone: "work" | "ask" | "done" | "fail" } {
  switch (state.phase) {
    case "thinking":
      return { title: "Working on it", detail: state.request, tone: "work" };
    case "confirm":
      return { title: "Waiting for your approval", detail: state.description || state.request, tone: "ask" };
    case "acting":
      return { title: "Doing it", detail: state.description || state.request, tone: "work" };
    case "result":
      return state.failed
        ? { title: "That didn’t work", detail: state.text || state.reason || "Nothing was changed.", tone: "fail" }
        : { title: "Done", detail: afterDone(state.text) || state.description || "", tone: "done" };
    default:
      return { title: "", detail: "", tone: "work" };
  }
}

export function ActTask({ register, hooks }: { register: (handle: ActHandle) => void; hooks: ActHooks }) {
  const [state, setState] = useState<ActState | null>(null);
  useEffect(() => { register({ set: setState }); }, [register]);
  if (!state || state.phase === "idle") return null;
  const { title, detail, tone } = words(state);
  const working = state.phase === "thinking" || state.phase === "acting";
  const open = state.phase === "thinking" || state.phase === "confirm" || state.phase === "acting";
  return (
    <div className={`act-task ${tone}`} data-test="act-task" data-phase={state.phase} role="status" aria-live="polite">
      <span className="act-task-mark" aria-hidden="true">
        {working ? <span className="act-task-spin" /> : <Icon name={tone === "done" ? "check" : tone === "fail" ? "alert" : "shield"} />}
      </span>
      <div className="act-task-text">
        <div className="act-task-title" data-test={tone === "fail" ? "act-task-failed" : "act-task-title"}>{title}</div>
        {detail ? <div className="act-task-detail" data-test="act-task-detail">{detail}</div> : null}
      </div>
      <div className="act-task-actions">
        {state.phase === "confirm" ? <button type="button" className="primary" data-test="act-task-review" onClick={hooks.review}>Review</button> : null}
        {open ? (
          <button type="button" data-test="act-task-stop" onClick={hooks.stop}>{state.phase === "confirm" ? "Cancel" : "Stop"}</button>
        ) : (
          <button type="button" data-test="act-task-dismiss" onClick={hooks.dismiss}>Dismiss</button>
        )}
      </div>
    </div>
  );
}
