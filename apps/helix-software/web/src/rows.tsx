import { useEffect, useState } from "react";
import { Button, Chip, HxRow, IconTile, Meter, hueOf, invoke } from "@helix/ui";
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

/** The circular progress control of a running job: the arc is the percent, the square in the middle cancels. */
export function Ring({ job }: { job: JobView }) {
  const radius = 11;
  const circumference = 2 * Math.PI * radius;
  const percent = job.percent;
  return (
    <button
      className={`hx-ring${percent == null ? " indeterminate" : ""}`}
      data-test={`progress:${job.package_id}`}
      title={`${PROGRESS[job.action]}${percent != null ? ` ${percent}%` : "…"} — click to cancel`}
      aria-label={`Cancel ${job.title}`}
      onClick={(event) => { event.stopPropagation(); invoke("software_cancel", { job: job.id }); }}
    >
      <svg viewBox="0 0 28 28" width="28" height="28" fill="none" strokeWidth="2.5">
        <circle className="track" cx="14" cy="14" r={radius} />
        <circle className="arc" cx="14" cy="14" r={radius} strokeDasharray={circumference} strokeDashoffset={circumference * (1 - (percent ?? 25) / 100)} />
      </svg>
      <span className="stop" />
    </button>
  );
}

/** The one control a package shows in a tile or card: Get / Update / Open, or the ring while it works. */
export function GetControl({ pkg, job, blocked, onOpen, onRequest }: {
  pkg: PackageView;
  job: JobView | undefined;
  blocked: boolean;
  onOpen: () => void;
  onRequest: (action: "install" | "update") => void;
}) {
  if (job && job.state === "running") return <Ring job={job} />;
  const action = actionFor(pkg);
  return action === "open" ? (
    <Button small data-test={`action:${pkg.id}`} onClick={(event) => { event.stopPropagation(); onOpen(); }}>Open</Button>
  ) : (
    <Button kind="primary" small data-test={`action:${pkg.id}`} disabled={blocked} onClick={(event) => { event.stopPropagation(); onRequest(action); }}>
      {VERB[action]}
    </Button>
  );
}

/** A tile on a shelf: icon, name, one line of what it is, and the control. */
export function AppTile({ pkg, job, blocked, onDetail, onOpen, onRequest, test }: {
  test?: string;
  pkg: PackageView;
  job: JobView | undefined;
  blocked: boolean;
  onDetail: () => void;
  onOpen: () => void;
  onRequest: (action: "install" | "update") => void;
}) {
  return (
    <div
      className="hx-tile-card"
      tabIndex={0}
      data-test={test ?? `pkg:${pkg.id}`}
      aria-label={pkg.title}
      onClick={onDetail}
      onKeyDown={(event) => { if (event.key === "Enter" && event.target === event.currentTarget) onDetail(); }}
    >
      <AppIcon pkg={pkg} large />
      <div className="name">{pkg.title || pkg.id}</div>
      <div className="sum">{pkg.summary}</div>
      <div className="foot">
        <GetControl pkg={pkg} job={job} blocked={blocked} onOpen={onOpen} onRequest={onRequest} />
      </div>
    </div>
  );
}

/** A compact row for category pages and results: two columns on a wide window. */
export function CardRow({ pkg, job, blocked, onDetail, onOpen, onRequest }: {
  pkg: PackageView;
  job: JobView | undefined;
  blocked: boolean;
  onDetail: () => void;
  onOpen: () => void;
  onRequest: (action: "install" | "update") => void;
}) {
  return (
    <div
      className="hx-card-row"
      tabIndex={0}
      data-test={`pkg:${pkg.id}`}
      aria-label={pkg.title}
      onClick={onDetail}
      onKeyDown={(event) => { if (event.key === "Enter" && event.target === event.currentTarget) onDetail(); }}
    >
      <AppIcon pkg={pkg} />
      <div className="text">
        <div className="name">{pkg.title || pkg.id}</div>
        <div className="sum">{pkg.summary}</div>
      </div>
      <GetControl pkg={pkg} job={job} blocked={blocked} onOpen={onOpen} onRequest={onRequest} />
    </div>
  );
}

/** The average colour of an icon (for the hero's gradient); null until the image has loaded or when there is none. */
function useAverageColor(uri: string | null): string | null {
  const [color, setColor] = useState<string | null>(null);
  useEffect(() => {
    setColor(null);
    if (!uri) return;
    const image = new Image();
    image.onload = () => {
      try {
        const canvas = document.createElement("canvas");
        canvas.width = 8;
        canvas.height = 8;
        const context = canvas.getContext("2d");
        if (!context) return;
        context.drawImage(image, 0, 0, 8, 8);
        const { data } = context.getImageData(0, 0, 8, 8);
        let r = 0, g = 0, b = 0, n = 0;
        for (let i = 0; i < data.length; i += 4) {
          if (data[i + 3] < 128) continue;
          r += data[i]; g += data[i + 1]; b += data[i + 2]; n += 1;
        }
        if (n) {
          // Pulled toward a mid tone so white text always reads on it.
          const mix = (v: number) => Math.round((v / n) * 0.75 + 20);
          setColor(`rgb(${mix(r)}, ${mix(g)}, ${mix(b)})`);
        }
      } catch {
        /* a tainted canvas: keep the neutral gradient */
      }
    };
    image.src = uri;
  }, [uri]);
  return color;
}

/** The featured app: a wide card tinted from its icon. */
export function Hero({ pkg, job, blocked, onDetail, onOpen, onRequest }: {
  pkg: PackageView;
  job: JobView | undefined;
  blocked: boolean;
  onDetail: () => void;
  onOpen: () => void;
  onRequest: (action: "install" | "update") => void;
}) {
  const uri = useIcon(pkg.id);
  const color = useAverageColor(uri);
  return (
    <div
      className="hx-hero-card"
      tabIndex={0}
      data-test="hero"
      style={color ? ({ ["--hero" as string]: color }) : undefined}
      onClick={onDetail}
      onKeyDown={(event) => { if (event.key === "Enter" && event.target === event.currentTarget) onDetail(); }}
    >
      <AppIcon pkg={pkg} large />
      <div>
        <div className="over">Featured</div>
        <h2>{pkg.title || pkg.id}</h2>
        <p>{pkg.summary}</p>
        {job && job.state === "running" ? <div style={{ marginTop: 14 }}><Ring job={job} /></div> : (
          actionFor(pkg) === "open"
            ? <Button data-test="hero-action" onClick={(event) => { event.stopPropagation(); onOpen(); }}>Open</Button>
            : <Button kind="primary" data-test="hero-action" disabled={blocked} onClick={(event) => { event.stopPropagation(); onRequest(actionFor(pkg) as "install" | "update"); }}>{VERB[actionFor(pkg) as "install" | "update"]}</Button>
        )}
      </div>
    </div>
  );
}
