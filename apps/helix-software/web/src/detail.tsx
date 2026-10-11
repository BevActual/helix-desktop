import { useEffect, useState } from "react";
import { Banner, Button, Chip, Group, HxRow, invoke } from "@helix/ui";
import { AppIcon, JobProgress, actionFor } from "./rows";
import { VERB, formatSize, type InfoView, type JobView, type PackageView } from "./types";

function Fact({ label, value }: { label: string; value: string }) {
  return (
    <div className="hx-fact">
      <div className="hx-fact-label">{label}</div>
      <div className="hx-fact-value" title={value}>{value || "—"}</div>
    </div>
  );
}

/** One application: what it is, what it will do to the computer, and the one control that matters. */
export function Detail({ pkg, job, blocked, onOpen, onRequest }: {
  pkg: PackageView;
  job: JobView | undefined;
  blocked: boolean;
  onOpen: () => void;
  onRequest: (action: "install" | "remove" | "update") => void;
}) {
  const [info, setInfo] = useState<InfoView | null>(null);
  useEffect(() => {
    let live = true;
    setInfo(null);
    invoke<InfoView>("software_info", { id: pkg.id }).then((value) => { if (live) setInfo(value); }, () => { if (live) setInfo({ found: false, developer: "", license: "", homepage: "", description: [], categories: [], release: "" }); });
    return () => { live = false; };
  }, [pkg.id]);

  const action = actionFor(pkg);
  const name = pkg.title || pkg.id;
  const source = [pkg.remote, pkg.installation && `${pkg.installation} installation`].filter(Boolean).join(" · ");
  return (
    <>
      <div className="hx-hero" data-test="detail">
        <AppIcon pkg={pkg} large />
        <div className="hx-hero-text">
          <h2>{name}</h2>
          {info?.developer ? <p data-test="detail-developer">{info.developer}</p> : null}
          {pkg.summary ? <p>{pkg.summary}</p> : null}
          <div className="hx-hero-actions">
            {job && job.state === "running" ? (
              <JobProgress job={job} />
            ) : (
              <>
                {action === "open" ? (
                  <Button kind="primary" data-test="detail-open" onClick={onOpen}>Open</Button>
                ) : (
                  <Button kind="primary" data-test="detail-action" disabled={blocked} onClick={() => onRequest(action)}>{VERB[action]}</Button>
                )}
                {pkg.installed && pkg.kind === "flatpak" ? (
                  <Button kind="destructive" data-test="detail-remove" disabled={blocked} onClick={() => onRequest("remove")}>Remove</Button>
                ) : null}
                {pkg.update ? <Chip tone="accent">Update available</Chip> : pkg.installed ? <Chip tone="ok">Installed</Chip> : null}
              </>
            )}
          </div>
        </div>
      </div>

      <div className="hx-facts" data-test="facts">
        <Fact label="Version" value={info?.release || pkg.version} />
        <Fact label={pkg.installed ? "Size on disk" : "Download"} value={formatSize(pkg.size)} />
        <Fact label="Source" value={source} />
        <Fact label="License" value={info?.license ?? ""} />
      </div>

      {info === null ? (
        <div className="hx-skeleton" aria-hidden="true"><i style={{ width: "70%", height: 12 }} /></div>
      ) : info.description.length ? (
        <div className="hx-prose" data-test="description">
          {info.description.map((paragraph, index) => <p key={index}>{paragraph}</p>)}
        </div>
      ) : (
        <Banner>No description is published for this application.</Banner>
      )}

      {info?.categories.length ? (
        <div className="hx-chips" data-test="categories">
          {info.categories.map((category) => <Chip key={category}>{category}</Chip>)}
        </div>
      ) : null}
      {info?.homepage ? (
        <Group title="Website">
          <HxRow title={info.homepage} />
        </Group>
      ) : null}
    </>
  );
}
