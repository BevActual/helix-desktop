import { useEffect, useState } from "react";
import { invoke } from "@helix/ui";
import type { AboutView } from "../types";
import type { PageHandle } from "./RowsPage";

export function AboutPage({ register }: { register: (handle: PageHandle) => void }) {
  const [info, setInfo] = useState<AboutView | null>(null);
  useEffect(() => {
    const refresh = () => {
      invoke<AboutView>("about").then(setInfo, () => undefined);
    };
    register({ setRows: () => undefined, refresh });
    refresh();
  }, [register]);
  if (!info) return null;
  const computer: [string, string][] = info.system;
  const helix: [string, string][] = [
    ["Helix Settings", info.version],
    ["Desktop service", info.service],
    ["Agent access", info.agent_access],
    ["Wi-Fi", info.network ? "Available" : "Not available"],
    ["Bluetooth", info.bluetooth ? "Available" : "Not available"],
  ];
  const card = (rows: [string, string][]) => (
    <div className="rows-card">
      {rows.map(([name, value]) => (
        <div className="setting-row" key={name}>
          <div className="setting-title">{name}</div>
          <div className="slider-value" data-test={`about-${name}`}>{value}</div>
        </div>
      ))}
    </div>
  );
  return (
    <div data-test="react-page">
      {computer.length ? <><h4>This computer</h4>{card(computer)}</> : null}
      <h4>Helix</h4>
      {card(helix)}
    </div>
  );
}
