import { useEffect, useRef, useState } from "react";
import { invoke, listen, type BluetoothView, type PairingView } from "@helix/ui";
import { ActionButton, ListRow, SwitchRow } from "../components";
import type { PageHandle } from "./RowsPage";

/** The question a device asks while it is being paired. What is typed goes to Rust once and the field is emptied; nothing is kept. */
function Pairing({ view }: { view: PairingView }) {
  const [text, setText] = useState("");
  const [message, setMessage] = useState("");
  const input = useRef<HTMLInputElement>(null);
  useEffect(() => {
    setText("");
    input.current?.focus();
  }, [view.stage]);
  if (!view.active) return null;
  const typed = (label: string, test: string, numeric: boolean) => (
    <form
      className="password-form"
      style={{ padding: 0 }}
      onSubmit={(event) => {
        event.preventDefault();
        const answer = text;
        setText("");
        invoke("bluetooth_pair_answer", { text: answer }).catch(() => {
          setMessage(numeric ? "A passkey is up to six digits." : "A PIN is 1 to 16 characters.");
          input.current?.focus();
        });
      }}
    >
      <input
        ref={input}
        type="text"
        placeholder={label}
        autoComplete="off"
        spellCheck={false}
        inputMode={numeric ? "numeric" : undefined}
        maxLength={numeric ? 6 : undefined}
        value={text}
        data-test={test}
        onChange={(event) => setText(event.target.value)}
      />
      <button type="submit" className="primary" data-test="pair-send">Pair</button>
    </form>
  );
  let title = `Pairing with ${view.device}…`;
  let body: React.ReactNode = null;
  let accept: React.ReactNode = null;
  switch (view.stage) {
    case "pin":
      title = `Enter the PIN for ${view.device}`;
      body = typed("PIN", "pair-pin", false);
      break;
    case "passkey":
      title = `Enter the passkey for ${view.device}`;
      body = typed("Passkey", "pair-passkey", true);
      break;
    case "display":
      title = `Type this code on ${view.device}`;
      body = (
        <div className="pair-code" data-test="pair-code">
          {[...view.code].map((digit, index) => (
            <span key={index} className={index < view.entered ? "done" : ""}>{digit}</span>
          ))}
        </div>
      );
      break;
    case "confirm":
      title = `Does ${view.device} show this code?`;
      body = <div className="pair-code" data-test="pair-code">{view.code}</div>;
      accept = <ActionButton label="They match" test="pair-accept" kind="primary" onClick={() => invoke("bluetooth_pair_accept")} />;
      break;
    case "authorize":
      title = `${view.device} wants to pair`;
      accept = <ActionButton label="Allow" test="pair-accept" kind="primary" onClick={() => invoke("bluetooth_pair_accept")} />;
      break;
    default:
      break;
  }
  return (
    <div className="rows-card pairing">
      <div className="setting-title">{title}</div>
      {body}
      {message ? <div className="setting-about warn">{message}</div> : null}
      <div className="pairing-actions">
        {accept}
        <ActionButton label="Cancel" test="pair-cancel" onClick={() => invoke("bluetooth_pair_cancel")} />
      </div>
    </div>
  );
}

export function BluetoothPage({ register }: { register: (handle: PageHandle) => void }) {
  const [view, setView] = useState<BluetoothView | null>(null);
  const [pairing, setPairing] = useState<PairingView | null>(null);

  useEffect(() => {
    const load = () => {
      invoke<BluetoothView>("bluetooth_view").then(setView);
      invoke<PairingView>("pairing_view").then(setPairing);
    };
    register({ setRows: () => undefined, refresh: load });
    const stops = [listen<BluetoothView>("bluetooth-view", setView), listen<PairingView>("pairing-view", setPairing)];
    load();
    return () => { stops.forEach((stop) => stop.then((fn) => fn())); };
  }, [register]);

  if (!view) return null;
  return (
    <div data-test="react-page">
      {pairing ? <Pairing view={pairing} /> : null}
      {!view.available ? (
        <div className="empty">Bluetooth is not available on this computer.</div>
      ) : (
        <>
          <div className="rows-card">
            <SwitchRow
              title="Bluetooth"
              about={view.blocked ? "Blocked by the system" : view.status}
              on={view.enabled}
              test="bt-switch"
              disabled={view.blocked}
              onChange={(on) => invoke("bluetooth_set_power", { enabled: on })}
            />
          </div>
          {view.enabled ? (
            <>
              <div className="section-head">
                <h4>Devices</h4>
                <div className="section-tools">
                  <ActionButton label={view.scanning ? "Stop" : "Scan"} test="bt-scan" onClick={() => invoke("bluetooth_scan", { on: !view.scanning })} />
                </div>
              </div>
              <div className="rows-card">
                {view.devices.map((device) => {
                  const usable = device.action === "connect" || device.action === "disconnect";
                  const pairable = device.action === "pair";
                  const battery = device.battery === null || device.battery === undefined ? "" : ` · ${device.battery}% battery`;
                  const tag = (pairable ? "Not paired" : device.action === "busy" ? "Connecting…" : device.connected ? "Connected" : "Paired") + battery;
                  const pair = () => { invoke("bluetooth_pair", { path: device.path }).catch((error) => console.error(error)); };
                  const click = () => {
                    if (device.action === "connect") invoke("bluetooth_connect", { path: device.path });
                    else if (device.action === "disconnect") invoke("bluetooth_disconnect", { path: device.path });
                    else if (pairable) pair();
                  };
                  return (
                    <ListRow
                      key={device.path}
                      title={device.name}
                      tag={tag}
                      test={`bt:${device.name}`}
                      icon="bluetooth"
                      active={device.connected}
                      clickable={usable || pairable}
                      dim={!usable && !pairable && !device.connected}
                      onClick={click}
                    >
                      {pairable ? <ActionButton label="Pair" test={`bt-pair:${device.name}`} onClick={pair} /> : null}
                      {device.paired ? <ActionButton label="Forget" test={`bt-forget:${device.name}`} onClick={() => invoke("bluetooth_forget", { path: device.path })} /> : null}
                    </ListRow>
                  );
                })}
                {view.devices.length === 0 ? <div className="empty">{view.scanning ? "Looking for devices…" : "No devices yet. Choose Scan."}</div> : null}
              </div>
            </>
          ) : null}
        </>
      )}
    </div>
  );
}
