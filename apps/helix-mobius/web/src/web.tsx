import { useEffect, useRef, useState } from "react";
import { invoke, listen } from "@helix/ui";

export interface WebSource { id: string; title: string; url: string; text: string; kind: "page" | "snippet"; provider: string; fetched: number; truncated: boolean }
interface Chat { id: string; streaming: boolean; analysis_job?: string }
export interface WebHandle { open: (chat: Chat, query?: string) => void }
function requestId() { return Array.from(crypto.getRandomValues(new Uint8Array(8)), (n) => n.toString(16).padStart(2, "0")).join(""); }
function domain(url: string) { try { return new URL(url).hostname; } catch { return url; } }

export function OnlineResearch({ register, answered }: { register: (handle: WebHandle) => void; answered: () => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const questionField = useRef<HTMLTextAreaElement>(null);
  const request = useRef("");
  const chatId = useRef("");
  const questionEdited = useRef(false);
  const [chat, setChat] = useState<Chat | null>(null);
  const [open, setOpen] = useState(false);
  const [input, setInput] = useState("");
  const [question, setQuestion] = useState("");
  const [sources, setSources] = useState<WebSource[]>([]);
  const [selected, setSelected] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const [answering, setAnswering] = useState(false);
  const [stage, setStage] = useState("");
  const [error, setError] = useState("");
  const [searched, setSearched] = useState(false);

  const cancel = () => {
    const id = request.current; request.current = "";
    if (id) invoke("web_cancel", { requestId: id }).catch(() => {});
    setBusy(false); setStage("Lookup stopped.");
  };
  const lookup = async (value: string, replaceId?: string) => {
    cancel();
    const id = requestId(); request.current = id;
    setBusy(true); setError(""); setStage(replaceId ? `Reading ${domain(value)}…` : "Looking online…");
    if (!replaceId) { setSources([]); setSelected([]); setSearched(true); }
    try {
      const found = await invoke<WebSource[]>("web_lookup", { requestId: id, input: value });
      if (request.current !== id) return;
      if (replaceId && found[0]) {
        setSources((previous) => previous.map((source) => source.id === replaceId ? found[0] : source));
        setSelected((previous) => previous.map((own) => own === replaceId ? found[0].id : own));
      } else {
        setSources(found);
        setSelected(found.filter((source) => source.text.trim()).slice(0, 3).map((source) => source.id));
        if (!questionEdited.current) setQuestion(found.length === 1 && found[0].kind === "page" ? "Summarize the useful information on this page." : value);
      }
      setStage(found.length ? `${found.length} source${found.length === 1 ? "" : "s"} found` : "No results. Try another query or a page address.");
    } catch (reason) { if (request.current === id) { setError(String(reason)); setStage(""); } }
    finally { if (request.current === id) { request.current = ""; setBusy(false); } }
  };
  const lookupRef = useRef(lookup); lookupRef.current = lookup;
  useEffect(() => {
    register({ open: (view, query) => {
      chatId.current = view.id; setChat(view); setError(""); setOpen(true);
      if (query?.trim()) { questionEdited.current = false; setInput(query.trim()); setQuestion(query.trim()); void lookupRef.current(query.trim()); }
    } });
  }, [register]);
  useEffect(() => {
    const stop = listen<Chat>("chat-view", (view) => {
      if (chatId.current && chatId.current !== view.id) setOpen(false);
      setChat(view);
    });
    return () => { stop.then((fn) => fn()); if (request.current) invoke("web_cancel", { requestId: request.current }).catch(() => {}); };
  }, []);
  useEffect(() => {
    if (open && !dialog.current?.open) dialog.current?.showModal();
    if (!open) {
      if (dialog.current?.open) dialog.current.close();
      const id = request.current; request.current = "";
      if (id) invoke("web_cancel", { requestId: id }).catch(() => {});
      setBusy(false);
    }
  }, [open]);
  const answer = async () => {
    if (!chat) return;
    setAnswering(true); setError("");
    try { await invoke("web_answer", { chatId: chat.id, question, sourceIds: selected }); answered(); setOpen(false); }
    catch (reason) { setError(String(reason)); }
    finally { setAnswering(false); }
  };
  const questionCount = Array.from(question).length;
  return <dialog ref={dialog} className="sheet online-sheet" data-test="web-panel" aria-labelledby="web-title" onClose={() => setOpen(false)}>
    <div className="sheet-body">
      <header className="sheet-head"><h3 id="web-title">Look online</h3><p className="sheet-note">Search for sources, read a page, then ask Mobius about the evidence.</p></header>
      <div className="sheet-scroll">
        <form className="web-search-form" onSubmit={(event) => { event.preventDefault(); void lookup(input); }}>
          <label className="sheet-field" htmlFor="web-input">Search or page address<input id="web-input" data-test="web-input" type="search" value={input} onChange={(event) => setInput(event.target.value)} maxLength={1024} placeholder="A search query or https://example.com/article" disabled={busy || answering} /></label>
          <button type="button" className="mini" data-test="web-input-clear" disabled={busy || answering || !input} onClick={() => setInput("")}>Clear</button>
          <button type="submit" className="primary" data-test="web-lookup" disabled={busy || answering || !input.trim()}>Look up</button>
        </form>
        <p className="sheet-note web-privacy">Searches send your query to DuckDuckGo, with Bing as a fallback. Reading a page contacts that website. Answers use your selected chat model.</p>
        {(busy || stage) && <p className="web-status" role="status" data-test="web-status">{stage}</p>}
        {error && <p className="sheet-warning" role="alert" data-test="web-error">{error}</p>}
        {sources.length > 0 && <section className="web-results" aria-label="Online sources">
          <div className="web-results-head"><strong>Select up to three sources</strong><span>{selected.length} selected</span></div>
          {sources.map((source, index) => <article key={source.id} className="web-result" data-test={`web-result-${index}`}>
            <div className="web-result-head"><label><input type="checkbox" data-test={`web-select-${index}`} checked={selected.includes(source.id)} disabled={busy || answering || !source.text.trim() || !selected.includes(source.id) && selected.length >= 3} onChange={(event) => setSelected((previous) => event.target.checked ? [...previous, source.id] : previous.filter((id) => id !== source.id))} /><strong>{source.title || domain(source.url)}</strong></label><button type="button" className="mini" data-test={`web-open-${index}`} title={source.url} onClick={() => invoke("web_open_source", { url: source.url }).catch((reason) => setError(String(reason)))}>Open source</button></div>
            <p className="web-source-address">{source.url}</p>
            <p className="sheet-note">{source.kind === "page" ? "Page text" : `Search snippet · ${source.provider}`} · retrieved {new Date(source.fetched * 1000).toLocaleString()}</p>
            <p className="web-result-excerpt">{source.text.slice(0, 600) || "No excerpt returned. Read the page to get its text."}{source.text.length > 600 ? "…" : ""}</p>
            <div className="web-result-actions">{source.kind === "snippet" && <button type="button" className="mini" data-test={`web-read-${index}`} disabled={busy || answering} onClick={() => lookup(source.url, source.id)}>Read page</button>}<details data-test={`web-excerpt-${index}`}><summary>Preview text for the answer</summary><pre>{Array.from(source.text).slice(0, 1500).join("")}</pre><p className="sheet-note">Answers include up to 1,500 characters per source.{source.truncated ? " The page preview was shortened." : ""}</p></details></div>
          </article>)}
        </section>}
        {searched && !busy && !sources.length && !error && <p className="sheet-note">You can also paste a public page address to read it directly.</p>}
        {sources.length > 0 && <label className="sheet-field" htmlFor="web-question">Question for Mobius<textarea ref={questionField} id="web-question" data-test="web-question" value={question} onChange={(event) => { questionEdited.current = true; setQuestion(event.target.value); }} maxLength={8000} disabled={answering} placeholder="What would you like to know from these sources?" /><span className="sheet-note">{questionCount.toLocaleString()} / 2,000 characters</span></label>}
        {chat?.analysis_job && <p className="sheet-warning">Start a separate chat to answer from online sources. Software-analysis chats keep their evidence local.</p>}
      </div>
      <footer className="sheet-actions">{sources.length > 0 && <div className="web-answer-summary"><span title={question}>{question.trim() ? `Question: ${question}` : "Add a question for Mobius"}</span><button type="button" className="mini" data-test="web-review-question" disabled={answering} onClick={() => { questionField.current?.focus(); questionField.current?.scrollIntoView({ block: "center" }); }}>Review question</button></div>}<button type="button" data-test="web-close" disabled={answering} onClick={() => setOpen(false)}>Back to chat</button>{busy && <button type="button" data-test="web-stop" onClick={cancel}>Stop lookup</button>}<span className="sheet-actions-spacer" /><button type="button" className="primary" data-test="web-answer" disabled={busy || answering || chat?.streaming || !!chat?.analysis_job || !selected.length || !question.trim() || questionCount > 2000} onClick={answer}>{answering ? "Starting answer…" : "Answer with sources"}</button></footer>
    </div>
  </dialog>;
}
