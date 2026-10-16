// Views whose structs live in the shell's own crate or are not exported to TypeScript yet. Keep in step with
// core/helix-models/src/{audio,brightness,power,quick}.rs and shell/helix-shell-tauri/src-tauri/src/island.rs.
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
  /** Whether the desktop draws dark right now (Automatic follows the time of day). */
  resolved_dark: boolean | null;
  night_light: boolean | null;
}

export interface MediaView {
  playing: boolean;
  artist: string;
  title: string;
}

/** One row of the Caps Lock palette: display text only; the choice is sent back as a position in the list Rust drew it from. Mirrors the
 * rows of `quick_insert_query` in the shell's main.rs and `core/helix-shell-model/src/quick_insert.rs`. */
export interface QuickInsertRow {
  glyph: string | null;
  label: string;
  detail: string;
  ask: boolean;
}

export interface QuickInsertResults {
  rows: QuickInsertRow[];
  generation: number;
}

export interface AssistantView {
  phase: string;
  text: string;
  description: string;
  reason: string;
  failed: boolean;
  unverified: boolean;
  armed: boolean;
  task: number;
  before: string;
  after: string;
  undoable: boolean;
}
export interface TalkLine { role: string; text: string; }
export interface TalkView { phase: string; reply: string; error: string; history: TalkLine[]; }
export interface PendingRow { operation: number; who: string; tool: string; tier: string; grant: boolean; tainted: boolean; reason: string; }
export interface GrantRow { id: string; who: string; tiers: string[]; }
export interface ActivityView {
  available: boolean;
  enabled: boolean;
  busy: boolean;
  pending: PendingRow[];
  grants: GrantRow[];
  history: import("@helix/ui").HistoryRow[];
}
