import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { invoke, listen, type BluetoothView, type NetworkView, type NotificationsView } from "@helix/ui";
import { Icon } from "./icon";
import type { AudioView, BatteryView, BrightnessView, MediaView, QuickView } from "./types";

export interface ControlsHandle {
  /** The panel was opened: back to the main page, views read again, clock set. */
  open: () => void;
  /** One minute passed. */
  tick: () => void;
  /** Escape: leaves a detail page and says so (returns false when it was already on the main page). */
  back: () => boolean;
}

type Detail = "sound" | "wifi" | "bluetooth" | null;

/** A view Rust sends as events, read once at the start and kept current. */
function useView<T>(event: string, command: string | null, initial: T | null = null): [T | null, () => void] {
  const [view, setView] = useState<T | null>(initial);
  const read = useCallback(() => {
    if (command) invoke<T>(command).then(setView, () => undefined);
  }, [command]);
  useEffect(() => {
    const stop = listen<T>(event, setView);
    read();
    return () => { stop.then((fn) => fn()); };
  }, [event, read]);
  return [view, read];
}

/** A slider that sends at most one write every 50 ms (always the last value) and ignores incoming values while it is being dragged. */
function SliderRow({ id, sliderId, fillId, test, percent, onWrite, children, trailing, rowId, title, muted }: { id: string; sliderId: string; fillId: string; test: string; percent: number; onWrite: (percent: number) => void; children?: ReactNode; trailing: ReactNode; rowId: string; title?: string; muted?: boolean }) {
  const [draft, setDraft] = useState(percent);
  const dragging = useRef(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const latest = useRef(percent);
  useEffect(() => {
    if (!dragging.current) setDraft(Math.min(percent, 100));
  }, [percent]);
  useEffect(() => {
    const up = () => { dragging.current = false; };
    window.addEventListener("pointerup", up);
    return () => window.removeEventListener("pointerup", up);
  }, []);
  const shown = Math.min(draft, 100);
  return (
    <div className="sliderrow" id={rowId} title={title} data-muted={muted ? "1" : "0"}>
      {children}
      <div id={sliderId} style={{ ["--pct" as string]: `${shown}%` }}>
        <div id={fillId} />
        <input
          id={id}
          data-test={test}
          type="range"
          min={0}
          max={100}
          step={1}
          value={shown}
          onPointerDown={() => { dragging.current = true; }}
          onChange={(event) => {
            const value = Number(event.target.value);
            setDraft(value);
            latest.current = value;
            if (!timer.current) {
              timer.current = setTimeout(() => {
                timer.current = null;
                onWrite(latest.current);
              }, 50);
            }
          }}
        />
      </div>
      {trailing}
    </div>
  );
}

function Tile({ id, title, sub, icon, on, onClick, open, blocked }: { id: string; title: string; sub: string; icon: string; on?: boolean; onClick: () => void; open?: { test: string; label: string; go: () => void }; blocked?: boolean }) {
  return (
    <div
      className={`tile${on ? " on" : ""}`}
      id={id}
      data-test={id}
      data-on={on ? "1" : "0"}
      data-blocked={blocked ? "1" : "0"}
      onClick={(event) => {
        if ((event.target as HTMLElement).closest(".tile-open") || blocked) return;
        onClick();
      }}
    >
      <span className="tile-ico"><Icon name={icon} /></span>
      <div className="tile-main">
        <div className="tile-title">{title}</div>
        <div className="tile-sub" id={`${id}-sub`}>{sub}</div>
      </div>
      {open ? (
        <button className="tile-open" id={`${id}-open`} data-test={`${id}-open`} title={open.label} onClick={open.go}>
          <Icon name="chevron-right" />
        </button>
      ) : null}
    </div>
  );
}

const bars = (strength: number) => (strength >= 75 ? "▂▄▆█" : strength >= 50 ? "▂▄▆" : strength >= 25 ? "▂▄" : "▂");

export function ControlCenter({ register }: { register: (handle: ControlsHandle) => void }) {
  const [detail, setDetail] = useState<Detail>(null);
  const detailRef = useRef<Detail>(null);
  detailRef.current = detail;
  const [now, setNow] = useState(() => new Date());
  const [powerSheet, setPowerSheet] = useState(false);
  const [password, setPassword] = useState<{ path: string; ssid: string } | null>(null);
  const [typed, setTyped] = useState("");
  const [passwordError, setPasswordError] = useState("");
  const passwordInput = useRef<HTMLInputElement>(null);

  const [audio, readAudio] = useView<AudioView>("audio-view", "audio_view");
  const [network, readNetwork] = useView<NetworkView>("network-view", "network_view");
  const [bluetooth, readBluetooth] = useView<BluetoothView>("bluetooth-view", "bluetooth_view");
  const [quick, readQuick] = useView<QuickView>("quick-view", "quick_view");
  const [brightness, readBrightness] = useView<BrightnessView>("brightness-view", "brightness_view");
  const [battery, readBattery] = useView<BatteryView>("battery-view", "battery_view");
  const [media] = useView<MediaView | null>("media-view", "media_view");
  const [notifications] = useView<NotificationsView>("notifications-view", "notifications_view");

  useEffect(() => {
    register({
      open: () => {
        setDetail(null);
        setPowerSheet(false);
        setPassword(null);
        setNow(new Date());
        [readAudio, readNetwork, readBluetooth, readQuick, readBrightness, readBattery].forEach((read) => read());
      },
      tick: () => setNow(new Date()),
      back: () => {
        if (!detailRef.current) return false;
        setPassword(null);
        setDetail(null);
        return true;
      },
    });
  }, [register, readAudio, readNetwork, readBluetooth, readQuick, readBrightness, readBattery]);

  useEffect(() => {
    if (password) passwordInput.current?.focus();
  }, [password]);

  const join = () => {
    if (!password) return;
    setPasswordError("");
    invoke("network_connect_password", { path: password.path, password: typed }).then(
      () => { setPassword(null); setTyped(""); },
      (error) => setPasswordError(String(error || "Could not join")),
    );
  };

  const usable = !!audio && audio.available && !!audio.output;
  const dnd = notifications?.dnd ?? false;
  const wifiSub = !network ? "" : network.wifi_enabled ? network.status.replace(/^Connected to /, "") : "Off";
  const btSub = !bluetooth ? "" : bluetooth.blocked ? "Blocked" : bluetooth.enabled ? bluetooth.status.replace(/^Connected to /, "") : "Off";

  const main = (
    <div id="qs-main" style={{ display: detail ? "none" : "" }}>
      <div id="qs-head">
        <div>
          <div id="qs-time" data-test="time">{(window as unknown as { helixClockTime?: (d: Date) => string }).helixClockTime?.(now) ?? now.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })}</div>
          <div id="qs-date">{now.toLocaleDateString([], { weekday: "long", month: "long", day: "numeric" })}</div>
        </div>
        <div id="qs-battery" data-test="battery" className={battery?.low ? "low" : ""} style={{ display: battery?.present ? "" : "none" }}>
          {battery ? battery.label + (battery.state ? ` · ${battery.state}` : "") : ""}
        </div>
      </div>
      <div id="tiles">
        {network?.available ? (
          <Tile id="tile-wifi" title="Internet" sub={wifiSub} icon="wifi" on={network.wifi_enabled} onClick={() => invoke("network_set_wifi", { enabled: !network.wifi_enabled })} open={{ test: "tile-wifi-open", label: "Wi-Fi details", go: () => setDetail("wifi") }} />
        ) : null}
        {bluetooth?.available ? (
          <Tile id="tile-bt" title="Bluetooth" sub={btSub} icon="bluetooth" on={bluetooth.enabled} blocked={bluetooth.blocked} onClick={() => invoke("bluetooth_set_power", { enabled: !bluetooth.enabled })} open={{ test: "tile-bt-open", label: "Bluetooth details", go: () => setDetail("bluetooth") }} />
        ) : null}
        {quick && quick.dark !== null && quick.dark !== undefined ? (
          <Tile id="tile-dark" title="Dark theme" sub={quick.dark_label ?? ""} icon="contrast" on={quick.dark} onClick={() => invoke("quick_set_dark", { on: !quick.dark })} />
        ) : null}
        <Tile id="tile-dnd" title="Do not disturb" sub={dnd ? "On" : "Off"} icon="moon" on={dnd} onClick={() => invoke("notification_set_dnd", { on: !dnd })} />
        {quick && quick.night_light !== null && quick.night_light !== undefined ? (
          <Tile id="tile-night" title="Night Light" sub={quick.night_light ? "On" : "Off"} icon="sun" on={quick.night_light} onClick={() => invoke("quick_set_night_light", { on: !quick.night_light })} />
        ) : null}
      </div>
      {brightness?.available ? (
        <SliderRow rowId="brightrow" id="brightness" sliderId="brightslider" fillId="brightfill" test="brightness" percent={brightness.percent} onWrite={(percent) => invoke("brightness_set", { percent })}
          trailing={<><span id="bright-percent">{brightness.percent}%</span><span className="end-slot" /></>}>
          <span id="bright-ico"><Icon name="sun" /></span>
        </SliderRow>
      ) : null}
      <div id="audio-note">{!audio ? "" : audio.available ? (audio.output ? "" : "No output device") : "Sound is unavailable"}</div>
      {usable && audio ? (
        <SliderRow rowId="volrow" id="volume" sliderId="volslider" fillId="volfill" test="volume" percent={audio.percent} onWrite={(percent) => invoke("audio_set_volume", { percent })}
          trailing={<>
            <span id="percent">{audio.percent}%</span>
            <button className="tile-open" id="sound-open" data-test="sound-open" title="Sound settings" onClick={() => setDetail("sound")}><Icon name="chevron-right" /></button>
          </>}>
          <button id="mute" data-test="mute" title={audio.muted ? "Unmute" : "Mute"} data-muted={audio.muted ? "1" : "0"} onClick={() => invoke("audio_set_mute", { muted: !audio.muted })}>
            <Icon name={audio.muted ? "volume-mute" : "volume"} />
          </button>
        </SliderRow>
      ) : null}
      {usable && audio?.input ? (
        <SliderRow rowId="microw" id="micvolume" sliderId="micslider" fillId="micfill" test="micvolume" percent={audio.input_percent} onWrite={(percent) => invoke("audio_set_input_volume", { percent })}
          trailing={<><span id="micpercent">{audio.input_percent}%</span><span className="end-slot" /></>}>
          <button id="micmute" data-test="micmute" title={audio.input_muted ? "Unmute the microphone" : "Mute the microphone"} data-muted={audio.input_muted ? "1" : "0"} onClick={() => invoke("audio_set_input_mute", { muted: !audio.input_muted })}>
            <Icon name={audio.input_muted ? "mic-off" : "mic"} />
          </button>
        </SliderRow>
      ) : null}
      {media && media.title ? (
        <div id="qs-media" className="qs-media" data-test="qs-media">
          <span className="qs-media-ico"><Icon name="music" /></span>
          <div className="qs-media-main">
            <div id="qs-media-title" className="qs-media-title">{media.title}</div>
            <div id="qs-media-artist" className="qs-media-sub">{media.artist}</div>
          </div>
          <button id="qs-media-prev" data-test="qs-media-prev" title="Previous" onClick={() => invoke("media_previous")}><Icon name="skip-back" /></button>
          <button id="qs-media-toggle" data-test="qs-media-toggle" title="Play or pause" onClick={() => invoke("media_toggle")}><Icon name={media.playing ? "pause" : "play"} /></button>
          <button id="qs-media-next" data-test="qs-media-next" title="Next" onClick={() => invoke("media_next")}><Icon name="skip-forward" /></button>
        </div>
      ) : null}
      <div id="qs-system" className="qs-system" data-test="qs-system">
        <button id="qs-settings" data-test="qs-settings" title="Settings" onClick={() => invoke("open_settings", { page: null })}><Icon name="settings" /></button>
        <button id="qs-lock" data-test="qs-lock" title="Lock" onClick={() => invoke("session_lock")}><Icon name="lock" /></button>
        <button id="qs-suspend" data-test="qs-suspend" title="Suspend" onClick={() => invoke("session_suspend")}><Icon name="moon" /></button>
        <button id="qs-power" data-test="qs-power" title="Power" onClick={() => setPowerSheet((on) => !on)}><Icon name="power" /></button>
      </div>
      {powerSheet ? (
        <div id="qs-power-sheet" className="qs-sheet" data-test="qs-power-sheet">
          <button id="qs-logout" data-test="qs-logout" onClick={() => invoke("session_logout")}>Log out</button>
          <button id="qs-reboot" data-test="qs-reboot" onClick={() => invoke("session_reboot")}>Restart</button>
          <button id="qs-shutdown" data-test="qs-shutdown" className="danger" onClick={() => invoke("session_poweroff")}>Shut down</button>
          <button id="qs-power-cancel" data-test="qs-power-cancel" onClick={() => setPowerSheet(false)}>Cancel</button>
        </div>
      ) : null}
    </div>
  );

  const titles: Record<string, string> = { sound: "Sound", wifi: "Internet", bluetooth: "Bluetooth" };
  const page = detail && (
    <div id="qs-detail">
      <div id="detail-head">
        <button id="back" data-test="back" title="Back" onClick={() => { setPassword(null); setDetail(null); }}><Icon name="chevron-left" /></button>
        <h3 id="detail-title">{titles[detail]}</h3>
      </div>
      {detail === "sound" && audio ? (
        <div className="page current" id="detail-sound">
          <h4>Output</h4>
          <div id="outputs">
            {audio.outputs.map((output) => (
              <div key={output.id} className={`row${audio.output && output.id === audio.output.id ? " current" : ""}`} data-test={`out:${output.name}`} onClick={() => invoke("audio_set_default", { id: output.id })}>
                {(audio.output && output.id === audio.output.id ? "● " : "○ ") + output.name}
              </div>
            ))}
          </div>
          {audio.inputs.length > 1 ? (
            <div id="input-section">
              <h4>Input</h4>
              <div id="inputs">
                {audio.inputs.map((input) => (
                  <div key={input.id} className={`row${audio.input && input.id === audio.input.id ? " current" : ""}`} data-test={`in:${input.name}`} onClick={() => invoke("audio_set_default_input", { id: input.id })}>
                    {(audio.input && input.id === audio.input.id ? "● " : "○ ") + input.name}
                  </div>
                ))}
              </div>
            </div>
          ) : null}
        </div>
      ) : null}
      {detail === "wifi" && network ? (
        <div className="page current" id="wifi">
          <div id="wifi-head">
            {network.wifi_enabled ? <button id="scan" data-test="wifi-scan" disabled={network.scanning} onClick={() => invoke("network_scan")}>Scan</button> : null}
            {network.connected ? <button id="disconnect" data-test="wifi-disconnect" onClick={() => invoke("network_disconnect")}>Disconnect</button> : null}
            <button id="wifitoggle" data-test="wifi-toggle" data-on={network.wifi_enabled ? "1" : "0"} onClick={() => invoke("network_set_wifi", { enabled: !network.wifi_enabled })}>
              {network.wifi_enabled ? "Turn off" : "Turn on"}
            </button>
          </div>
          <div id="wifi-status">{network.status}</div>
          <div id="networks">
            {network.networks.map((net) => {
              const joinable = net.action === "connect";
              const asks = net.action === "password";
              const classes = ["net", net.active ? "active" : "", joinable || asks ? "clickable" : "", joinable || asks || net.active ? "" : "unavailable"].filter(Boolean).join(" ");
              return (
                <div
                  key={net.path}
                  className={classes}
                  data-test={`net:${net.ssid}`}
                  onClick={() => {
                    if (joinable) invoke("network_connect", { path: net.path });
                    else if (asks) { setPassword({ path: net.path, ssid: net.ssid }); setTyped(""); setPasswordError(""); }
                  }}
                >
                  <span className="bars">{bars(net.strength)}</span>
                  <span className="ssid">{net.ssid}</span>
                  <span className="tag">
                    {net.active ? "Connected" : net.action === "password" ? "Needs a password" : net.action === "enterprise" ? "Enterprise" : net.saved ? "Saved" : net.secured ? "Secured" : "Open"}
                  </span>
                  {net.saved ? (
                    <button data-test={`net-forget:${net.ssid}`} onClick={(event) => { event.stopPropagation(); invoke("network_forget", { path: net.path }); }}>Forget</button>
                  ) : null}
                </div>
              );
            })}
          </div>
          <div id="vpns">
            {network.vpns.map((vpn) => (
              <div key={vpn.path} className={`net clickable${vpn.active ? " active" : ""}`} data-test={`vpn:${vpn.name}`} onClick={() => invoke("network_vpn", { path: vpn.path, active: !vpn.active })}>
                <span className="ssid">VPN: {vpn.name}</span>
                <span className="tag">{vpn.active ? "On" : "Off"}</span>
              </div>
            ))}
          </div>
          {password ? (
            <div id="wifi-password" className="qs-sheet" data-test="wifi-password">
              <div id="wifi-password-ssid" className="qs-sheet-title">Join “{password.ssid}”</div>
              <input
                id="wifi-password-input"
                ref={passwordInput}
                data-test="wifi-password-input"
                type="password"
                autoComplete="current-password"
                placeholder="Password"
                value={typed}
                onChange={(event) => setTyped(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === "Enter") { event.preventDefault(); join(); }
                  if (event.key === "Escape") { event.preventDefault(); setPassword(null); setTyped(""); }
                }}
              />
              <div id="wifi-password-error" className="qs-sheet-error" data-test="wifi-password-error">{passwordError}</div>
              <div className="qs-sheet-actions">
                <button id="wifi-password-cancel" data-test="wifi-password-cancel" onClick={() => { setPassword(null); setTyped(""); }}>Cancel</button>
                <button id="wifi-password-join" data-test="wifi-password-join" className="primary" onClick={join}>Join</button>
              </div>
            </div>
          ) : null}
        </div>
      ) : null}
      {detail === "bluetooth" && bluetooth ? (
        <div className="page current" id="bluetooth">
          <div id="bt-head">
            {bluetooth.enabled ? (
              <button id="btscan" data-test="bt-scan" data-scanning={bluetooth.scanning ? "1" : "0"} onClick={() => invoke("bluetooth_scan", { on: !bluetooth.scanning })}>
                {bluetooth.scanning ? "Stop" : "Scan"}
              </button>
            ) : null}
            <button id="bttoggle" data-test="bt-toggle" data-on={bluetooth.enabled ? "1" : "0"} disabled={bluetooth.blocked} onClick={() => invoke("bluetooth_set_power", { enabled: !bluetooth.enabled })}>
              {bluetooth.enabled ? "Turn off" : "Turn on"}
            </button>
          </div>
          <div id="bt-status">{bluetooth.status}</div>
          <div id="btdevices">
            {bluetooth.devices.map((device) => {
              const usable = device.action === "connect" || device.action === "disconnect";
              const pair = device.action === "pair";
              const classes = ["net", device.connected ? "active" : "", usable || pair ? "clickable" : "", usable || pair || device.connected ? "" : "unavailable"].filter(Boolean).join(" ");
              const battery = device.battery === null || device.battery === undefined ? "" : ` · ${device.battery}%`;
              return (
                <div
                  key={device.path}
                  className={classes}
                  data-test={`bt:${device.name}`}
                  onClick={() => {
                    if (pair) invoke("bluetooth_pair", { path: device.path });
                    else if (device.action === "connect") invoke("bluetooth_connect", { path: device.path });
                    else if (device.action === "disconnect") invoke("bluetooth_disconnect", { path: device.path });
                  }}
                >
                  <span className="ssid">{device.name}</span>
                  <span className="tag">{(pair ? "Pair" : device.action === "busy" ? "Connecting…" : device.connected ? "Connected" : "Paired") + battery}</span>
                  {device.paired ? (
                    <button data-test={`bt-forget:${device.name}`} onClick={(event) => { event.stopPropagation(); invoke("bluetooth_forget", { path: device.path }); }}>Forget</button>
                  ) : null}
                </div>
              );
            })}
          </div>
        </div>
      ) : null}
    </div>
  );

  return (
    <>
      {main}
      {page}
    </>
  );
}
