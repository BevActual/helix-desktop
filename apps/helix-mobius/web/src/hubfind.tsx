import { useState } from "react";
import { invoke } from "@helix/ui";

interface Repo { id: string; downloads: number; likes: number; license: string; sees_pictures: boolean; known_maker: boolean; }
interface VFile { name: string; bytes: number; sha256: string; }
interface Variant { label: string; files: VFile[]; bytes: number; fit: string; key: string; }
interface RepoView { repo: Repo; variants: Variant[]; note: string; }

const FIT: Record<string, string> = { gpu: "Fits your graphics card", tight: "Tight on your graphics card", cpu: "Runs on the processor (slower)", no: "Too big for this computer" };

function size(bytes: number): string {
  return bytes >= 1e9 ? `${(bytes / 1e9).toFixed(1)} GB` : `${Math.max(1, Math.round(bytes / 1e6))} MB`;
}
function count(n: number): string {
  return n >= 1e6 ? `${(n / 1e6).toFixed(1)}M` : n >= 1e3 ? `${Math.round(n / 1e3)}K` : String(n);
}
/** A pasted link or an owner/name goes straight to that repository; anything else is a search. */
const looksLikeRepo = (text: string) => /^https:\/\/(www\.)?huggingface\.co\/[^/\s]+\/[^/\s]+/.test(text.trim()) || /^[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+$/.test(text.trim());

/** Models beyond the curated list: search Hugging Face, or paste a link, then choose a file. Each download is confirmed, checked against
 * the checksum Hugging Face publishes, and only ever started by a button. */
export function HubFind({ report }: { report: (error: unknown) => void }) {
  const [open, setOpen] = useState(false);
  const [text, setText] = useState("");
  const [busy, setBusy] = useState(false);
  const [results, setResults] = useState<Repo[] | null>(null);
  const [view, setView] = useState<RepoView | null>(null);
  const [error, setError] = useState("");

  const fail = (e: unknown) => setError(String(e));
  const go = () => {
    const query = text.trim();
    if (!query) return;
    setBusy(true); setError(""); setView(null); setResults(null);
    const work = looksLikeRepo(query)
      ? invoke<RepoView>("hub_repo", { input: query }).then(setView)
      : invoke<Repo[]>("hub_search", { query }).then((list) => setResults(list));
    work.catch(fail).finally(() => setBusy(false));
  };
  const choose = (id: string) => {
    setBusy(true); setError("");
    invoke<RepoView>("hub_repo", { input: id }).then(setView).catch(fail).finally(() => setBusy(false));
  };
  const get = (v: RepoView, variant: Variant) => {
    const maker = v.repo.known_maker ? "a maker Mobius recognises" : "a maker Mobius does not recognise, so check that you trust it";
    const message = `Get ${variant.key.split("/").pop()} (${size(variant.bytes)}) from huggingface.co/${v.repo.id}?\n\n`
      + `Published by ${v.repo.id.split("/")[0]}, ${maker}.\nLicense: ${v.repo.license || "not stated"}.\n\n`
      + "It is checked against the checksum Hugging Face publishes, and thrown away if it does not match.";
    if (!window.confirm(message)) return;
    invoke("hub_download", { repo: v.repo.id, key: variant.key }).catch(report);
  };

  return (
    <section className="hub" data-test="hub">
      <button type="button" className="hub-toggle" aria-expanded={open} data-test="hub-toggle" onClick={() => setOpen(!open)}>
        {open ? "Hide" : "Find more models on Hugging Face"}
      </button>
      {open ? (
        <div className="hub-body">
          <div className="hub-bar">
            <input data-test="hub-input" type="text" spellCheck={false} placeholder="Search, or paste a huggingface.co link" autoComplete="off" value={text}
              onChange={(e) => setText(e.target.value)} onKeyDown={(e) => { if (e.key === "Enter") go(); }} />
            {text || results || view || error ? <button type="button" className="hub-clear" data-test="hub-clear" aria-label="Clear" title="Clear" onClick={() => { setText(""); setResults(null); setView(null); setError(""); }}>Clear</button> : null}
            <button type="button" className="primary" data-test="hub-go" disabled={busy || !text.trim()} onClick={go}>{busy ? "Looking…" : looksLikeRepo(text) ? "Open" : "Search"}</button>
          </div>
          <p className="set-hint">Searching sends the words you type to huggingface.co, and nothing else. Only GGUF files that Hugging Face lists with a checksum are offered, and nothing downloads until you confirm.</p>
          {error ? <p className="lib-error" data-test="hub-error">{error}</p> : null}
          {results && !view ? (
            <div className="hub-results" data-test="hub-results">
              {results.length ? results.map((r) => (
                <button key={r.id} type="button" className="hub-repo" data-test={`hub-repo-${r.id}`} onClick={() => choose(r.id)}>
                  <span className="hub-repo-name">{r.id}</span>
                  <span className="hub-repo-meta">
                    {count(r.downloads)} downloads · {count(r.likes)} likes{r.license ? ` · ${r.license}` : ""}
                  </span>
                  <span className="lib-tags">
                    {r.known_maker ? <span className="lib-tag fit-gpu">Known maker</span> : null}
                    {r.sees_pictures ? <span className="lib-tag vision">Sees pictures</span> : null}
                  </span>
                </button>
              )) : <p className="route-model-empty" data-test="hub-none">Nothing found. Try other words.</p>}
            </div>
          ) : null}
          {view ? (
            <div className="hub-view" data-test="hub-view">
              <div className="hub-view-head">
                <div>
                  <div className="lib-title">{view.repo.id}</div>
                  <div className="lib-sub">{count(view.repo.downloads)} downloads · {view.repo.license || "license not stated"}{view.repo.known_maker ? " · known maker" : ""}</div>
                </div>
                <button type="button" className="set-link" data-test="hub-back" onClick={() => setView(null)}>Back to results</button>
              </div>
              {view.note ? <p className="route-model-empty" data-test="hub-note">{view.note}</p> : null}
              {view.variants.length ? <p className="set-hint">Smaller numbers (Q2, Q3) are smaller and less accurate; Q4_K_M is a good balance; Q8 and F16 are close to the original and large.</p> : null}
              {view.variants.map((v) => (
                <div key={v.key} className="hub-variant" data-test={`hub-variant-${v.key}`}>
                  <div className="hub-variant-text">
                    <strong>{v.label}</strong> · {size(v.bytes)}{v.files.length > 1 ? ` · ${v.files.length} parts` : ""}
                    <span className={`lib-tag fit-${v.fit}`}>{FIT[v.fit] ?? ""}</span>
                  </div>
                  <button type="button" disabled={v.fit === "no"} data-test={`hub-get-${v.key}`} onClick={() => get(view, v)}>Get · {size(v.bytes)}</button>
                </div>
              ))}
            </div>
          ) : null}
        </div>
      ) : null}
    </section>
  );
}
