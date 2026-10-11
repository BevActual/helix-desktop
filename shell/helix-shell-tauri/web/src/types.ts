// Views whose structs live in the shell's own crate or are not exported to TypeScript yet. Keep in step with
// core/helix-shell-core/src/desktop/{audio,brightness,power,quick}.rs and shell/helix-shell-tauri/src-tauri/src/island.rs.
export interface OutputView {
  id: number;
  name: string;
  is_default: boolean;
}

export interface AudioView {
  available: boolean;
  output: OutputView | null;
  percent: number;
  muted: boolean;
  outputs: OutputView[];
  input: OutputView | null;
  input_percent: number;
  input_muted: boolean;
  inputs: OutputView[];
}

export interface BrightnessView {
  available: boolean;
  percent: number;
}

export interface BatteryView {
  present: boolean;
  percent: number;
  label: string;
  state: string;
  charging: boolean;
  low: boolean;
}

export interface QuickView {
  dark: boolean | null;
  dark_label: string | null;
  night_light: boolean | null;
}

export interface MediaView {
  playing: boolean;
  artist: string;
  title: string;
}
