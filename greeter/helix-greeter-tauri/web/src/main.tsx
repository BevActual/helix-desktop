import { useCallback, useEffect, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { invoke } from "@helix/ui";

interface User { username: string; name: string; }
interface Session { id: string; name: string; }
interface Colors {
  background: string;
  on_background: string;
  surface: string;
  on_surface: string;
  primary: string;
  on_primary: string;
}
interface Offer {
  users: User[];
  sessions: Session[];
  dark: boolean;
  font: string;
  ui_scale: number;
  corner_radius_scale: number;
  transparency_mode: string;
  refraction: boolean;
  high_contrast: boolean;
  reduced_motion: boolean;
  colors: Colors | null;
  last_user: string;
  last_session: string;
  wallpaper_url: string;
}
type Step =
  | { kind: "prompt"; secret: boolean; message: string }
  | { kind: "info"; message: string; error: boolean }
  | { kind: "ready" }
  | { kind: "failed"; message: string };

function initials(name: string): string {
  const words = name.trim().split(/\s+/).filter(Boolean);
  return ((words[0]?.[0] ?? "?") + (words.length > 1 ? words[words.length - 1][0] : "")).toUpperCase();
}

function Clock() {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    const timer = setInterval(() => setNow(new Date()), 10_000);
    return () => clearInterval(timer);
  }, []);
  return (
    <div className="g-clock" data-test="clock">
      <div className="g-time">{now.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })}</div>
      <div className="g-date">{now.toLocaleDateString([], { weekday: "long", month: "long", day: "numeric" })}</div>
    </div>
  );
}

/** The login card. Rust holds the greetd conversation; this shows one step at a time and sends back typed text, a listed account
 * and a listed session, so it can neither name an account nor a command it was not offered. */
function Greeter() {
  const [offer, setOffer] = useState<Offer | null>(null);
  const [user, setUser] = useState<User | null>(null);
  const [step, setStep] = useState<Step | null>(null);
  const [text, setText] = useState("");
  const [session, setSession] = useState("");
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState("");
  const [confirming, setConfirming] = useState<"" | "poweroff" | "reboot">("");
  const field = useRef<HTMLInputElement>(null);
  const power_box = useRef<HTMLDivElement>(null);
  const asked = useRef<"" | "poweroff" | "reboot">("");

  useEffect(() => {
    invoke<Offer>("greeter_offer").then((value) => {
      setOffer(value);
      const root = document.documentElement;
      root.dataset.theme = value.dark ? "dark" : "light";
      root.dataset.glass = value.high_contrast ? "solid" : value.transparency_mode;
      root.dataset.contrast = value.high_contrast ? "1" : "0";
      root.dataset.refract = value.refraction ? "1" : "0";
      root.dataset.motion = value.reduced_motion ? "0" : "1";
      root.style.setProperty("--r-panel", `${Math.round(24 * value.corner_radius_scale)}px`);
      root.style.setProperty("--r-card", `${Math.round(18 * value.corner_radius_scale)}px`);
      if (value.font) root.style.fontFamily = value.font;
      root.style.setProperty("zoom", String(value.ui_scale));
      if (value.colors) {
        root.style.setProperty("--label", value.colors.on_background);
        root.style.setProperty("--label-2", value.colors.on_surface);
        root.style.setProperty("--accent", value.colors.primary);
        root.style.setProperty("--on-accent", value.colors.on_primary);
        root.style.setProperty("--greeter-surface", value.colors.surface);
        document.body.style.backgroundColor = value.colors.background;
      }
      if (value.wallpaper_url && !/["\\()]/.test(value.wallpaper_url)) {
        const dim = value.dark ? "rgba(0,0,0,.35)" : "rgba(255,255,255,.2)";
        document.body.style.backgroundImage = `linear-gradient(${dim}, ${dim}), url("${value.wallpaper_url}")`;
        document.body.style.backgroundSize = "cover";
        document.body.style.backgroundPosition = "center";
      }
      setSession(value.last_session || value.sessions[0]?.id || "");
    });
  }, []);

  useEffect(() => {
    field.current?.focus();
  }, [step, user]);

  const fail = (error: unknown) => {
    setNotice(String(error));
    setStep(null);
    setUser(null);
    setBusy(false);
  };

  const advance = useCallback(
    (next: Step) => {
      setBusy(false);
      if (next.kind === "info") {
        // greetd wants the line acknowledged before the next step.
        setNotice(next.message);
        setBusy(true);
        invoke<Step>("greeter_respond", { response: null }).then(advance, fail);
        return;
      }
      if (next.kind === "failed") setNotice(next.message);
      setStep(next);
    },
    [],
  );

  const choose = (picked: User) => {
    setUser(picked);
    setNotice("");
    setText("");
    setBusy(true);
    invoke<Step>("greeter_begin", { username: picked.username }).then(advance, fail);
  };

  const back = () => {
    invoke("greeter_cancel");
    setUser(null);
    setStep(null);
    setText("");
    setNotice("");
  };

  const submit = () => {
    if (step?.kind !== "prompt" || busy) return;
    const answer = text;
    setText("");
    setBusy(true);
    invoke<Step>("greeter_respond", { response: answer }).then(advance, fail);
  };

  // Authentication is done: start the chosen session.
  useEffect(() => {
    if (step?.kind === "ready" && session) {
      invoke("greeter_start", { session }).catch(fail);
    }
  }, [step, session]);

  // A failed attempt opens a fresh conversation for the same account.
  const retry = () => user && choose(user);

  // One account on the machine: no picker.
  const only = offer?.users.length === 1 ? offer.users[0] : null;
  useEffect(() => {
    if (only && !user && !notice) choose(only);
  }, [only]);

  // Cancelling puts the focus back on the button that was chosen, so the keyboard does not start over from the top.
  useEffect(() => {
    if (confirming) { asked.current = confirming; return; }
    if (asked.current) {
      power_box.current?.querySelector<HTMLButtonElement>(`[data-test="power-${asked.current}"]`)?.focus();
      asked.current = "";
    }
  }, [confirming]);

  const power = (action: "poweroff" | "reboot") => {
    setConfirming("");
    invoke("greeter_power", { action }).catch((error) => setNotice(String(error)));
  };

  const sessions = offer?.sessions ?? [];
  // The account that signed in last comes first.
  const users = [...(offer?.users ?? [])].sort((a, b) => Number(b.username === offer?.last_user) - Number(a.username === offer?.last_user));
  return (
    <main className="g-stage" data-test="app">
      <Clock />
      <section className="g-card glass" data-test="login-card">
        {!offer ? (
          <p className="g-note">Starting…</p>
        ) : !user ? (
          <>
            <h1>Welcome</h1>
            {users.length ? (
              <div className="g-users" data-test="users">
                {users.map((entry) => (
                  <button key={entry.username} className="g-user" data-test={`user-${entry.username}`} onClick={() => choose(entry)}>
                    <span className="g-avatar">{initials(entry.name)}</span>
                    <span className="g-name">{entry.name}</span>
                    {entry.username === offer.last_user && users.length > 1 ? <span className="g-last">Last signed in</span> : null}
                  </button>
                ))}
              </div>
            ) : (
              <p className="g-note">No accounts can sign in here.</p>
            )}
          </>
        ) : (
          <>
            <span className="g-avatar big">{initials(user.name)}</span>
            <h1 data-test="user-name">{user.name}</h1>
            {step?.kind === "prompt" ? (
              <input
                ref={field}
                className="g-input"
                data-test="answer"
                type={step.secret ? "password" : "text"}
                placeholder={step.message.replace(/:\s*$/, "") || "Password"}
                autoComplete="off"
                disabled={busy}
                value={text}
                onChange={(event) => setText(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === "Enter") submit();
                  else if (event.key === "Escape") back();
                }}
              />
            ) : null}
            {step?.kind === "failed" ? (
              <button className="g-primary" data-test="try-again" onClick={retry}>Try again</button>
            ) : null}
            {step?.kind === "ready" ? <p className="g-note">Signing in…</p> : null}
            {!step && busy ? <p className="g-note">One moment…</p> : null}
            <div className={`g-notice${step?.kind === "failed" ? " error" : ""}`} data-test="notice" role="status">{notice}</div>
            <div className="g-actions">
              <button className="g-link" data-test="back" onClick={back}>Not you?</button>
              {sessions.length > 1 ? (
                <select className="g-session" data-test="session" value={session} onChange={(event) => setSession(event.target.value)}>
                  {sessions.map((entry) => <option key={entry.id} value={entry.id}>{entry.name}</option>)}
                </select>
              ) : null}
            </div>
          </>
        )}
        {offer && !user && notice ? <div className="g-notice error" role="status">{notice}</div> : null}
      </section>
      <div className="g-power" data-test="power" ref={power_box}>
        {confirming ? (
          <>
            <span className="g-power-ask">{confirming === "poweroff" ? "Shut down this computer?" : "Restart this computer?"}</span>
            <button className="g-power-btn danger" data-test={`power-${confirming}-yes`} onClick={() => power(confirming)}>{confirming === "poweroff" ? "Shut down" : "Restart"}</button>
            <button className="g-power-btn" data-test="power-cancel" autoFocus onClick={() => setConfirming("")} onKeyDown={(event) => { if (event.key === "Escape") setConfirming(""); }}>Cancel</button>
          </>
        ) : (
          <>
            <button className="g-power-btn" data-test="power-reboot" onClick={() => setConfirming("reboot")}>Restart</button>
            <button className="g-power-btn" data-test="power-poweroff" onClick={() => setConfirming("poweroff")}>Shut down</button>
          </>
        )}
      </div>
    </main>
  );
}

const host = document.getElementById("app");
if (host) createRoot(host).render(<Greeter />);
