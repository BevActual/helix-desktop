import { Button, Chip, HxRow, IconTile, Meter, hueOf } from "@helix/ui";
import { useIcon } from "./hooks";
import { PROGRESS, VERB, formatSize, type JobView, type PackageView } from "./types";

export function AppIcon({ pkg, large }: { pkg: PackageView; large?: boolean }) {
  const uri = useIcon(pkg.id);
  const name = pkg.title || pkg.id;
  if (uri) return <img className="hx-icon-img" src={uri} alt="" draggable={false} width={large ? 72 : 40} height={large ? 72 : 40} />;
  return <IconTile large letter={Array.from(name)[0]?.toUpperCase() || "•"} hue={hueOf(name)} />;
}

/** What the primary control of a package says right now: the running job, an update, Open, or Get. */
export function actionFor(pkg: PackageView): "install" | "update" | "open" {
  if (pkg.update) return "update";
  return pkg.installed ? "open" : "install";
}

export function JobProgress({ job }: { job: JobView }) {
  return (
    <div style={{ width: 120 }} data-test={`progress:${job.package_id}`}>
      <div className="hx-row-sub" style={{ textAlign: "right" }}>
        {PROGRESS[job.action]}{job.percent != null ? ` ${job.percent}%` : "…"}
      </div>
      {job.percent != null ? <Meter percent={job.percent} /> : <div className="hx-meter"><i style={{ width: "40%" }} /></div>}
    </div>
  );
}

export function PackageRow({ pkg, job, blocked, focus, library, onOpen, onDetail, onRequest }: {
  pkg: PackageView;
  job: JobView | undefined;
  blocked: boolean;
  focus: boolean;
  library: boolean;
  onOpen: () => void;
  onDetail: () => void;
  onRequest: (action: "install" | "remove" | "update") => void;
}) {
  const action = actionFor(pkg);
  const name = pkg.title || pkg.id;
  const size = formatSize(pkg.size);
  const stop = (event: React.MouseEvent) => event.stopPropagation();
  return (
    <div
      tabIndex={0}
      className="hx-row-button"
      aria-label={name}
      onClick={onDetail}
      onKeyDown={(event) => { if (event.key === "Enter" && event.target === event.currentTarget) onDetail(); }}
    >
      <HxRow
        test={`pkg:${pkg.id}`}
        focus={focus}
        leading={<AppIcon pkg={pkg} />}
        title={<>{name} {pkg.update ? <Chip tone="accent">Update</Chip> : null}</>}
        sub={pkg.summary || [pkg.version, pkg.remote].filter(Boolean).join(" · ")}
        trailing={
          <span onClick={stop} style={{ display: "contents" }}>
            {job && job.state === "running" ? (
              <JobProgress job={job} />
            ) : (
              <>
                {size && !library ? <span className="hx-row-sub" style={{ marginRight: 4 }}>{size}</span> : null}
                {action === "open" ? (
                  <Button small data-test={`action:${pkg.id}`} onClick={onOpen}>Open</Button>
                ) : (
                  <Button kind="primary" small data-test={`action:${pkg.id}`} disabled={blocked} onClick={() => onRequest(action)}>
                    {VERB[action]}
                  </Button>
                )}
              </>
            )}
          </span>
        }
      />
    </div>
  );
}

export function SkeletonRows({ count = 7 }: { count?: number }) {
  return (
    <div className="hx-group" aria-hidden="true" data-test="loading">
      {Array.from({ length: count }, (_, index) => (
        <div key={index} className="hx-skeleton">
          <i style={{ width: 40, height: 40, borderRadius: 9 }} />
          <div style={{ flex: 1, display: "grid", gap: 6 }}>
            <i style={{ width: `${30 + ((index * 17) % 30)}%`, height: 12 }} />
            <i style={{ width: `${50 + ((index * 23) % 35)}%`, height: 10 }} />
          </div>
          <i style={{ width: 60, height: 24, borderRadius: 8 }} />
        </div>
      ))}
    </div>
  );
}
