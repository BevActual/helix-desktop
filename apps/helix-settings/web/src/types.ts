// Views whose structs live in the Settings app itself (the shared ones are generated into @helix/ui bindings). Keep in step with
// apps/helix-settings/src-tauri/src/main.rs and services/helix-wallpaper/src/picker.rs.
export interface AboutView {
  version: string;
  service: string;
  agent_access: string;
  system: [string, string][];
  network: boolean;
  bluetooth: boolean;
}

export interface ImageRow {
  index: number;
  name: string;
  current: boolean;
}

export interface SwatchRow {
  index: number;
  hex: string;
  current: boolean;
}

export interface PickerView {
  folder: string;
  images: ImageRow[];
  swatches: SwatchRow[];
}

/** What a wallhaven search returns to the page (`saved` is added by the Settings app: the picture is already in the folder). */
export interface WallhavenHit {
  id: string;
  resolution: string;
  size_mb: string;
  colors: string[];
  extension: string;
  saved: boolean;
}

export interface WallhavenResults {
  hits: WallhavenHit[];
  page: number;
  last_page: number;
}
