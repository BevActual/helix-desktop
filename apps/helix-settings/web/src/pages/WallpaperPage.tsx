import { useCallback, useEffect, useRef, useState } from "react";
import { invoke, listen } from "@helix/ui";
import { useLazyBackground } from "../hooks";
import type { PageHandle } from "./RowsPage";
import type { ImageRow, PickerView, WallhavenResults } from "../types";

const SORTINGS: [string, string][] = [
  ["toplist", "Top this year"],
  ["latest", "Latest"],
  ["random", "Random"],
  ["views", "Most viewed"],
  ["favorites", "Most favourited"],
];
const SIZES: [string, string][] = [
  ["1080", "Full HD and up"],
  ["1440", "QHD and up"],
  ["2160", "4K and up"],
  ["any", "Any size"],
];

function ImageTile({ image }: { image: ImageRow }) {
  const ref = useLazyBackground<HTMLButtonElement>(() => invoke<string | null>("wallpaper_thumb", { index: image.index }));
  return (
    <button
      ref={ref}
      className={`wp-tile${image.current ? " current" : ""}`}
      title={image.name}
      data-test={`wp-image-${image.index}`}
      onClick={() => invoke("wallpaper_choose_image", { index: image.index })}
    >
      <span className="wp-name">{image.name}</span>
    </button>
  );
}

function HitTile({ id, label, saved, busy, onUse }: { id: string; label: string; saved: boolean; busy: boolean; onUse: () => void }) {
  const ref = useLazyBackground<HTMLButtonElement>(() => invoke<string | null>("wallhaven_thumb", { id }), "300px");
  return (
    <button ref={ref} className={`wp-tile${busy ? " busy" : ""}`} title={label} data-test={`wh-hit-${id}`} onClick={onUse}>
      <span className="wp-name">{label}</span>
      {saved ? <span className="wp-saved">Saved</span> : null}
    </button>
  );
}

/** Searching wallhaven.cc (safe-for-work only). It is one component for the life of the page, so choosing a picture keeps the results. */
function Wallhaven({ report }: { report: (text: string) => void }) {
  const [query, setQuery] = useState("");
  const [sorting, setSorting] = useState("toplist");
  const [minimum, setMinimum] = useState("1080");
  const [results, setResults] = useState<WallhavenResults | null>(null);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");
  const [using, setUsing] = useState<string | null>(null);
  const [saved, setSaved] = useState<Set<string>>(new Set());
  const root = useRef<HTMLDivElement>(null);

  const run = useCallback(
    (page: number, over?: { query?: string; sorting?: string }) => {
      if (busy) return;
      setBusy(true);
      setMessage("");
      setResults(null);
      if (page > 1) root.current?.scrollIntoView({ block: "start" });
      invoke<WallhavenResults>("wallhaven_search", { query: over?.query ?? query, sorting: over?.sorting ?? sorting, minimum, page }).then(
        (found) => {
          setResults(found);
          setSaved(new Set(found.hits.filter((hit) => hit.saved).map((hit) => hit.id)));
          if (!found.hits.length && page === 1) setMessage("Nothing found. Try other words.");
        },
        (error) => setMessage(String(error)),
      ).finally(() => setBusy(false));
    },
    [busy, query, sorting, minimum],
  );

  const use = (id: string) => {
    if (using) return;
    setUsing(id);
    setMessage("");
    invoke("wallhaven_use", { id }).then(
      () => setSaved((current) => new Set(current).add(id)),
      (error) => { setMessage(String(error)); report(`wallhaven_use: ${String(error)}`); },
    ).finally(() => setUsing(null));
  };

  return (
    <div className="wh" ref={root}>
      <form className="wh-bar" onSubmit={(event) => { event.preventDefault(); run(1); }}>
        <input type="search" placeholder="Search wallpapers" autoComplete="off" spellCheck={false} value={query} data-test="wh-query" onChange={(event) => setQuery(event.target.value)} />
        <select value={sorting} data-test="wh-sorting" onChange={(event) => setSorting(event.target.value)}>
          {SORTINGS.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
        </select>
        <select value={minimum} data-test="wh-minimum" onChange={(event) => setMinimum(event.target.value)}>
          {SIZES.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
        </select>
        <button type="submit" className="primary" disabled={busy} data-test="wh-search">Search</button>
        <button type="button" data-test="wh-surprise" onClick={() => { setQuery(""); setSorting("random"); run(1, { query: "", sorting: "random" }); }}>Surprise me</button>
      </form>
      <div className="setting-about note">Searching sends your words to wallhaven.cc. Only safe-for-work pictures are shown.</div>
      {message ? <div className="setting-about warn">{message}</div> : null}
      <div className="wp-grid">
        {busy && !results ? Array.from({ length: 6 }, (_, i) => <div key={i} className="wp-tile skeleton" />) : null}
        {results?.hits.map((hit) => (
          <HitTile key={hit.id} id={hit.id} label={`${hit.resolution} · ${hit.size_mb}`} saved={saved.has(hit.id)} busy={using === hit.id} onUse={() => use(hit.id)} />
        ))}
      </div>
      {results && results.last_page > 1 ? (
        <div className="wh-pager">
          <button disabled={busy || results.page <= 1} data-test="wh-previous" onClick={() => run(results.page - 1)}>‹ Previous</button>
          <span className="wh-page" data-test="wh-page">Page {results.page} of {results.last_page}</span>
          <button disabled={busy || results.page >= results.last_page} data-test="wh-next" onClick={() => run(results.page + 1)}>Next ›</button>
        </div>
      ) : null}
    </div>
  );
}

export function WallpaperPage({ register, report }: { register: (handle: PageHandle) => void; report: (text: string) => void }) {
  const [view, setView] = useState<PickerView | null>(null);

  const load = useCallback(() => {
    invoke<PickerView>("wallpaper_view").then(setView, (error) => report(`wallpaper_view: ${String(error)}`));
  }, [report]);

  useEffect(() => {
    register({ setRows: () => undefined, refresh: load });
    load();
    const unlisten = listen<PickerView>("wallpaper-view", setView);
    return () => {
      unlisten.then((stop) => stop());
    };
  }, [register, load]);

  if (!view) return null;
  return (
    <div data-test="react-page">
      <h4>Colours</h4>
      <div className="swatches">
        {view.swatches.map((swatch) => (
          <button
            key={swatch.index}
            className={`swatch${swatch.current ? " current" : ""}`}
            style={{ background: swatch.hex }}
            title={swatch.hex}
            data-test={`wp-swatch-${swatch.index}`}
            onClick={() => invoke("wallpaper_choose_swatch", { index: swatch.index })}
          />
        ))}
      </div>
      <h4>Find on Wallhaven</h4>
      <Wallhaven report={report} />
      <h4>Pictures</h4>
      <div className="setting-about note">{view.folder}</div>
      <div className="wp-grid">
        {view.images.map((image) => (
          <ImageTile key={`${image.index}:${image.name}`} image={image} />
        ))}
      </div>
      {view.images.length === 0 ? <div className="empty">No pictures in this folder.</div> : null}
    </div>
  );
}
