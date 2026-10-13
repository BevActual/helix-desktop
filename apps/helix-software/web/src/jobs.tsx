import { useEffect } from "react";
import { Button, Meter, Spinner, invoke } from "@helix/ui";
import { PROGRESS, type JobView } from "./types";

const DONE = { install: "Installed", remove: "Removed", update: "Updated" } as const;

/** What is running or just finished, floating above the content. Successes dismiss themselves; failures wait to be read. */
export function JobsBar({ jobs, onOpen }: { jobs: JobView[]; onOpen: (id: string) => void }) {
  useEffect(() => {
    const timers = jobs
      .filter((job) => job.state === "done" || job.state === "cancelled")
      .map((job) => setTimeout(() => { invoke("software_dismiss", { job: job.id }); }, 6000));
    return () => timers.forEach(clearTimeout);
  }, [jobs]);
  if (!jobs.length) return null;
  return (
    <div className="hx-dock-bar" data-test="jobs">
      {jobs.slice(-3).map((job) => (
        <div key={job.id} className={`hx-job ${job.state}`} data-test={`job:${job.package_id}`}>
          {job.state === "running" ? <Spinner /> : null}
          <div className="hx-job-text">
            <div className="hx-job-title">
              {job.state === "running" ? `${PROGRESS[job.action]} ${job.title}` : job.state === "done" ? `${DONE[job.action]} ${job.title}` : job.state === "cancelled" ? `Cancelled ${job.title}` : `Could not ${job.action} ${job.title}`}
            </div>
            {job.state === "running" ? (
              <>
                <div className="hx-job-sub">{job.percent != null ? `${job.percent}%` : "Working…"}</div>
                {job.percent != null ? <Meter percent={job.percent} /> : null}
              </>
            ) : job.message ? (
              <div className="hx-job-sub">{job.message}</div>
            ) : null}
          </div>
          {job.state === "running" ? (
            <Button small data-test={`cancel:${job.package_id}`} onClick={() => invoke("software_cancel", { job: job.id })}>Cancel</Button>
          ) : (
            <>
              {job.state === "done" && job.action === "install" && job.package_id !== "*" ? (
                <Button small kind="primary" data-test={`open:${job.package_id}`} onClick={() => onOpen(job.package_id)}>Open</Button>
              ) : null}
              <Button small kind="plain" data-test={`dismiss:${job.package_id}`} onClick={() => invoke("software_dismiss", { job: job.id })}>Dismiss</Button>
            </>
          )}
        </div>
      ))}
    </div>
  );
}
