import { useCallback, useEffect, useState } from "react";
import { invoke } from "@helix/ui";

export interface ChatGPTStatus { connected: boolean; email: string; name: string; usage_url: string; }

/** Use a ChatGPT Plus or Pro plan in Mobius. The person signs in on OpenAI's own page; Mobius never sees a password. */
export function ChatGPTPlan({ status, onStatus, report }: { status: ChatGPTStatus | null; onStatus: (status: ChatGPTStatus) => void; report: (error: unknown) => void }) {
  const [waiting, setWaiting] = useState(false);
  const [error, setError] = useState("");
  const [welcome, setWelcome] = useState(false);

  useEffect(() => {
    const tauri = (window as unknown as { __TAURI__?: { event?: { listen: (name: string, cb: (e: { payload: ChatGPTStatus }) => void) => Promise<() => void> } } }).__TAURI__;
    let stop: (() => void) | undefined;
    tauri?.event?.listen("chatgpt-changed", (event) => onStatus(event.payload)).then((off) => { stop = off; });
    return () => stop?.();
  }, [onStatus]);

  const signIn = useCallback(() => {
    setWaiting(true);
    setError("");
    invoke<ChatGPTStatus>("chatgpt_sign_in")
      .then((next) => {
        onStatus(next);
        try {
          if (!window.localStorage.getItem("chatgpt-welcome-seen")) setWelcome(true);
        } catch { setWelcome(true); }
      })
      .catch((e) => setError(String(e)))
      .finally(() => setWaiting(false));
  }, [onStatus]);
  const dismiss = () => {
    setWelcome(false);
    try { window.localStorage.setItem("chatgpt-welcome-seen", "1"); } catch { /* the notice may come back */ }
  };
  const who = status?.email || status?.name || "your ChatGPT account";

  return (
    <div className="models-form-card chatgpt-card" data-test="chatgpt-card">
      {status?.connected ? (
        <>
          <div className="chatgpt-line" data-test="chatgpt-account"><strong>Connected as {who}</strong><span className="lib-tag fit-gpu">Using ChatGPT plan</span></div>
          <p className="set-hint">Replies from jobs you point at ChatGPT count against your plan's usage and leave this computer. Mobius cannot see your ChatGPT conversations.</p>
          {welcome ? (
            <div className="set-offer" data-test="chatgpt-welcome">
              <div className="set-offer-text"><div><strong>Eligible requests in Mobius now use your ChatGPT plan</strong></div><span>You can manage this in your ChatGPT settings at any time.</span></div>
              <button type="button" data-test="chatgpt-welcome-dismiss" onClick={dismiss}>Got it</button>
            </div>
          ) : null}
          <div className="models-form-row">
            <button type="button" className="primary" data-test="chatgpt-usage" onClick={() => invoke("chatgpt_open_usage").catch(report)}>Manage usage</button>
            <button type="button" data-test="chatgpt-disconnect" onClick={() => { if (window.confirm("Disconnect ChatGPT? Jobs that use it go back to a model on this computer.")) invoke("chatgpt_disconnect").catch(report); }}>Disconnect</button>
          </div>
        </>
      ) : (
        <>
          <p className="set-hint">Use your ChatGPT Plus or Pro plan instead of an API key. You sign in on OpenAI's own page in your browser, so Mobius never sees your password. Requests count against your plan's usage and leave this computer.</p>
          {waiting ? (
            <div className="models-form-row">
              <span className="models-inline-status" data-test="chatgpt-waiting">Waiting for you to finish in the browser…</span>
              <button type="button" data-test="chatgpt-cancel" onClick={() => invoke("chatgpt_cancel")}>Cancel</button>
            </div>
          ) : (
            <div className="models-form-row">
              <button type="button" className="primary chatgpt-signin" data-test="chatgpt-signin" onClick={signIn}>Continue with ChatGPT</button>
            </div>
          )}
          {error ? <p className="lib-error" data-test="chatgpt-error">{error}</p> : null}
        </>
      )}
    </div>
  );
}
