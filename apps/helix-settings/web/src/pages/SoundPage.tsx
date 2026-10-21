import { useEffect, useState } from "react";
import { invoke, listen, Meter, Slider, type AudioView, type OutputView } from "@helix/ui";
import { ListRow, SwitchRow } from "../components";
import type { PageHandle } from "./RowsPage";

/** Devices of one kind (outputs or inputs): the one in use is marked, and choosing another makes it the default. */
function Devices({ title, devices, test, icon, choose }: { title: string; devices: OutputView[]; test: string; icon: string; choose: (id: number) => void }) {
  return (
    <>
      <div className="section-head">
        <h4>{title}</h4>
      </div>
      <div className="rows-card">
        {devices.map((device) => (
          <ListRow
            key={device.id}
            title={device.name}
            tag={device.is_default ? "In use" : undefined}
            test={`${test}:${device.name}`}
            icon={icon}
            active={device.is_default}
            clickable={!device.is_default}
            onClick={() => { if (!device.is_default) choose(device.id); }}
          />
        ))}
        {devices.length === 0 ? <div className="empty">None found.</div> : null}
      </div>
    </>
  );
}

function Level({ title, percent, muted, test, disabled, setVolume, setMuted }: {
  title: string;
  percent: number;
  muted: boolean;
  test: string;
  disabled: boolean;
  setVolume: (percent: number) => void;
  setMuted: (muted: boolean) => void;
}) {
  return (
    <div className="rows-card">
      <div className="setting-row">
        <div className="setting-text">
          <div className="setting-title">{title}</div>
          <div className="setting-about">{disabled ? "No device" : muted ? "Muted" : `${percent}%`}</div>
        </div>
        {disabled ? null : <Slider id={test} min={0} max={100} integer value={Math.min(percent, 100)} onCommit={setVolume} />}
      </div>
      <SwitchRow title="Mute" on={muted} test={`${test}-mute`} disabled={disabled} onChange={setMuted} />
    </div>
  );
}

/** The microphone is open only while this is on. Leaving the page closes it. */
function MicTest({ disabled }: { disabled: boolean }) {
  const [testing, setTesting] = useState(false);
  const [percent, setPercent] = useState(0);
  const [heard, setHeard] = useState(false);

  useEffect(() => {
    const page = document.getElementById("page-sound");
    if (!page) return;
    const observer = new MutationObserver(() => {
      if (page.hidden) setTesting(false);
    });
    observer.observe(page, { attributes: true, attributeFilter: ["hidden"] });
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    if (!testing) return;
    let gone = false;
    let unlisten = () => {};
    invoke("audio_test_microphone", { on: true }).catch((error) => console.error(error));
    listen<{ source: number; percent: number; available: boolean }>("audio-level", (level) => {
      setPercent(level.percent);
      if (level.available) setHeard(true);
    }).then((stop) => {
      if (gone) stop();
      else unlisten = stop;
    });
    return () => {
      gone = true;
      unlisten();
      setHeard(false);
      setPercent(0);
      invoke("audio_test_microphone", { on: false }).catch((error) => console.error(error));
    };
  }, [testing]);

  return (
    <div className="rows-card">
      <SwitchRow
        title="Test microphone"
        about="Opens the microphone and shows how loud it is. It closes when you leave this page."
        on={testing}
        test="sound-input-test"
        disabled={disabled}
        onChange={setTesting}
      />
      {testing ? (
        <div className="setting-row">
          <div className="setting-text grow">
            {heard ? (
              <div key="heard" className="setting-about" data-test="sound-input-heard">Hearing the microphone</div>
            ) : (
              <div key="listening" className="setting-about">Listening…</div>
            )}
            <Meter percent={percent} live label="Microphone level" />
          </div>
        </div>
      ) : null}
    </div>
  );
}

export function SoundPage({ register }: { register: (handle: PageHandle) => void }) {
  const [view, setView] = useState<AudioView | null>(null);

  useEffect(() => {
    const load = () => { invoke<AudioView>("audio_view").then(setView); };
    register({ setRows: () => undefined, refresh: load });
    const stop = listen<AudioView>("audio-view", setView);
    load();
    return () => { stop.then((fn) => fn()); };
  }, [register]);

  if (!view) return null;
  if (!view.available) {
    return (
      <div data-test="react-page">
        <div className="empty">Sound is not available: no audio server is running.</div>
      </div>
    );
  }
  const call = (command: string, args: Record<string, unknown>) => { invoke(command, args).catch((error) => console.error(error)); };
  return (
    <div data-test="react-page">
      <div className="section-head"><h4>Output</h4></div>
      <Level
        title={view.output ? view.output.name : "Output"}
        percent={view.percent}
        muted={view.muted}
        test="sound-output"
        disabled={!view.output}
        setVolume={(percent) => call("audio_set_volume", { percent })}
        setMuted={(muted) => call("audio_set_muted", { muted })}
      />
      <Devices title="Output devices" devices={view.outputs} test="sound-out" icon="volume" choose={(id) => call("audio_set_default", { id })} />
      <div className="section-head"><h4>Input</h4></div>
      <Level
        title={view.input ? view.input.name : "Microphone"}
        percent={view.input_percent}
        muted={view.input_muted}
        test="sound-input"
        disabled={!view.input}
        setVolume={(percent) => call("audio_set_input_volume", { percent })}
        setMuted={(muted) => call("audio_set_input_muted", { muted })}
      />
      <MicTest disabled={!view.input} />
      <Devices title="Input devices" devices={view.inputs} test="sound-in" icon="mic" choose={(id) => call("audio_set_default_input", { id })} />
    </div>
  );
}
