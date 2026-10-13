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
  const rows: [string, string][] = [
    ["Helix Settings", info.version],
    ["Desktop service", info.service],
    ["Agent access", info.agent_access],
    ["Settings on these pages", String(info.settings)],
    ["Wi-Fi", info.network ? "Available" : "Not available"],
    ["Bluetooth", info.bluetooth ? "Available" : "Not available"],
  ];
  return (
    <div className="rows-card" data-test="react-page">
      {rows.map(([name, value]) => (
        <div className="setting-row" key={name}>
          <div className="setting-title">{name}</div>
          <div className="slider-value">{value}</div>
        </div>
      ))}
    </div>
  );
}
