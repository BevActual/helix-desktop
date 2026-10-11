import { useEffect, useRef, useState } from "react";
import { invoke, listen, type NetworkRow, type NetworkView } from "@helix/ui";
import { ActionButton, ListRow, SwitchRow } from "../components";
import type { PageHandle } from "./RowsPage";

type Asking = { path: string; kind: "password" | "enterprise" } | null;

/** A Wi-Fi password: typed once, sent once, emptied at once. It is never stored or drawn back. */
function PasswordForm({ net, onClose }: { net: NetworkRow; onClose: () => void }) {
  const [password, setPassword] = useState("");
  const [message, setMessage] = useState("");
  const input = useRef<HTMLInputElement>(null);
  useEffect(() => input.current?.focus(), []);
  return (
    <>
      <form
        className="password-form"
        onKeyDown={(event) => { if (event.key === "Escape") { event.stopPropagation(); onClose(); } }}
        onSubmit={(event) => {
          event.preventDefault();
          const typed = password;
          setPassword("");
          invoke("network_connect_password", { path: net.path, password: typed }).then(onClose, (error) => {
            setMessage(String(error).includes("BadPassword") ? "A Wi-Fi password has 8 to 63 characters." : "That did not work.");
            input.current?.focus();
          });
        }}
      >
        <input ref={input} type="password" placeholder="Password" autoComplete="off" spellCheck={false} value={password} data-test="wifi-password" onChange={(event) => setPassword(event.target.value)} />
        <button type="submit" className="primary" data-test="wifi-join">Join</button>
        <button type="button" data-test="wifi-cancel" onClick={onClose}>Cancel</button>
      </form>
      {message ? <div className="setting-about warn">{message}</div> : null}
    </>
  );
}

const ENTERPRISE_ERRORS: [string, string][] = [
  ["MissingIdentity", "Enter your identity."],
  ["MissingServerName", "Enter the server name (ask whoever runs the network), or a CA certificate file under More options."],
  ["BadPassword", "Enter your password."],
  ["UnsupportedPhase2", "PEAP works only with MSCHAPv2."],
  ["BadField", "Check the certificate path (it starts with /) and the server name."],
];

/** Enterprise (802.1X) sign-in. Same rule as the password; a form the person can fix keeps what they typed, the password was never sent. */
function EnterpriseForm({ net, onClose }: { net: NetworkRow; onClose: () => void }) {
  const [identity, setIdentity] = useState("");
  const [password, setPassword] = useState("");
  const [eap, setEap] = useState("peap");
  const [phase2, setPhase2] = useState("mschapv2");
  const [domain, setDomain] = useState("");
  const [anonymous, setAnonymous] = useState("");
  const [certificate, setCertificate] = useState("");
  const [message, setMessage] = useState("");
  const first = useRef<HTMLInputElement>(null);
  useEffect(() => first.current?.focus(), []);
  const inner = eap === "peap" ? "mschapv2" : phase2;
  const field = (label: string, test: string, value: string, set: (v: string) => void, type = "text", placeholder?: string, ref?: React.Ref<HTMLInputElement>) => (
    <label className="field">
      <span>{label}</span>
      <input ref={ref} type={type} autoComplete="off" spellCheck={false} placeholder={placeholder} value={value} data-test={test} onChange={(event) => set(event.target.value)} />
    </label>
  );
  return (
    <form
      className="enterprise-form"
      onKeyDown={(event) => { if (event.key === "Escape") { event.stopPropagation(); onClose(); } }}
      onSubmit={(event) => {
        event.preventDefault();
        const typed = password;
        setPassword("");
        invoke("network_connect_enterprise", { path: net.path, form: { identity, anonymous, password: typed, certificate, domain, eap, phase2: inner } }).then(onClose, (error) => {
          const text = String(error);
          const known = ENTERPRISE_ERRORS.find(([code]) => text.includes(code));
          setMessage(known ? known[1] : "That did not work.");
          if (known) setPassword(typed);
        });
      }}
    >
      {field("Identity", "ent-identity", identity, setIdentity, "text", undefined, first)}
      {field("Password", "ent-password", password, setPassword, "password")}
      <label className="field">
        <span>Method</span>
        <select value={eap} data-test="ent-eap" onChange={(event) => setEap(event.target.value)}>
          <option value="peap">PEAP</option>
          <option value="ttls">TTLS</option>
        </select>
      </label>
      <label className="field">
        <span>Inner authentication</span>
        <select value={inner} data-test="ent-phase2" onChange={(event) => setPhase2(event.target.value)}>
          {[["mschapv2", "MSCHAPv2"], ["pap", "PAP"], ["mschap", "MSCHAP"], ["chap", "CHAP"]].map(([value, label]) => (
            <option key={value} value={value} disabled={eap === "peap" && value !== "mschapv2"}>{label}</option>
          ))}
        </select>
      </label>
      {/* The server name tells the real network from a look-alike, so it is not hidden away. */}
      {field("Server name", "ent-domain", domain, setDomain, "text", "radius.example.com")}
      <details className="more">
        <summary>More options</summary>
        <div>
          {field("Anonymous identity", "ent-anonymous", anonymous, setAnonymous)}
          {field("CA certificate file (full path)", "ent-certificate", certificate, setCertificate)}
        </div>
      </details>
      <div className="pairing-actions">
        <button type="submit" className="primary" data-test="ent-join">Join</button>
        <button type="button" data-test="ent-cancel" onClick={onClose}>Cancel</button>
      </div>
      {message ? <div className="setting-about warn">{message}</div> : null}
    </form>
  );
}

function tagOf(net: NetworkRow): string {
  if (net.active) return "Connected";
  if (net.action === "password") return "Password required";
  if (net.action === "enterprise") return "Enterprise network. Sign in with your account";
  if (net.saved) return "Saved";
  return net.secured ? "Secured" : "Open";
}

export function NetworkPage({ register }: { register: (handle: PageHandle) => void }) {
  const [view, setView] = useState<NetworkView | null>(null);
  const [asking, setAsking] = useState<Asking>(null);
  // A newer view that arrives while a password is being typed waits: the form must not be torn down under the person's hands.
  const deferred = useRef<NetworkView | null>(null);
  const askingRef = useRef<Asking>(null);
  askingRef.current = asking;

  useEffect(() => {
    const receive = (next: NetworkView) => {
      if (askingRef.current) deferred.current = next;
      else setView(next);
    };
    register({ setRows: () => undefined, refresh: () => { invoke<NetworkView>("network_view").then(receive); } });
    const unlisten = listen<NetworkView>("network-view", receive);
    invoke<NetworkView>("network_view").then(receive);
    return () => { unlisten.then((stop) => stop()); };
  }, [register]);

  const close = () => {
    setAsking(null);
    if (deferred.current) {
      setView(deferred.current);
      deferred.current = null;
    }
  };

  if (!view) return null;
  if (!view.available) return <div className="empty" data-test="react-page">Networking is not available on this computer.</div>;
  return (
    <div data-test="react-page">
      <div className="rows-card">
        <SwitchRow title="Wi-Fi" about={view.status} on={view.wifi_enabled} test="wifi-switch" onChange={(on) => invoke("network_set_wifi", { enabled: on })} />
      </div>
      {view.wifi_enabled ? (
        <>
          <div className="section-head">
            <h4>Networks</h4>
            <div className="section-tools">
              {view.connected ? <ActionButton label="Disconnect" test="wifi-disconnect" onClick={() => invoke("network_disconnect")} /> : null}
              <ActionButton label={view.scanning ? "Scanning…" : "Scan"} test="wifi-scan" disabled={view.scanning} onClick={() => invoke("network_scan")} />
            </div>
          </div>
          <div className="rows-card">
            {view.networks.map((net) => {
              const joinable = net.action === "connect";
              const asks = net.action === "password" || net.action === "enterprise";
              const click = () => {
                if (joinable) invoke("network_connect", { path: net.path });
                else if (net.action === "password" && asking?.path !== net.path) setAsking({ path: net.path, kind: "password" });
                else if (net.action === "enterprise" && asking?.path !== net.path) setAsking({ path: net.path, kind: "enterprise" });
              };
              return (
                <div key={net.path}>
                  <ListRow
                    title={net.ssid}
                    tag={tagOf(net)}
                    test={`net:${net.ssid}`}
                    icon={net.secured ? "lock" : "wifi"}
                    active={net.active}
                    clickable={joinable || asks}
                    dim={!joinable && !asks && !net.active}
                    onClick={click}
                  >
                    {net.saved ? <ActionButton label="Forget" test={`net-forget:${net.ssid}`} onClick={() => invoke("network_forget", { path: net.path })} /> : null}
                  </ListRow>
                  {asking?.path === net.path && asking.kind === "password" ? <PasswordForm net={net} onClose={close} /> : null}
                  {asking?.path === net.path && asking.kind === "enterprise" ? <EnterpriseForm net={net} onClose={close} /> : null}
                </div>
              );
            })}
            {view.networks.length === 0 ? <div className="empty">No networks found.</div> : null}
          </div>
        </>
      ) : null}
      {view.vpns.length ? (
        <>
          <h4>VPN</h4>
          <div className="rows-card">
            {view.vpns.map((vpn) => (
              <SwitchRow key={vpn.path} title={vpn.name} on={vpn.active} test={`vpn:${vpn.name}`} onChange={(on) => invoke("network_vpn", { path: vpn.path, active: on })} />
            ))}
          </div>
        </>
      ) : null}
    </div>
  );
}
