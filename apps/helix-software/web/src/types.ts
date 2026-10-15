export type Page = "discover" | "category" | "library" | "updates" | "sources";

/** The store categories: appstream name, name on screen, sidebar icon (matches `CATEGORIES` in helix-software-model). */
export const CATEGORIES: { id: string; title: string; icon: string }[] = [
  { id: "Office", title: "Productivity", icon: "clipboard" },
  { id: "Graphics", title: "Graphics & Design", icon: "palette" },
  { id: "AudioVideo", title: "Audio & Video", icon: "music" },
  { id: "Development", title: "Developer Tools", icon: "fork" },
  { id: "Game", title: "Games", icon: "play" },
  { id: "Network", title: "Internet", icon: "wifi" },
  { id: "Education", title: "Education", icon: "sparkle" },
  { id: "Science", title: "Science", icon: "sun" },
  { id: "Utility", title: "Utilities", icon: "settings" },
];

export interface PackageView {
  id: string;
  title: string;
  summary: string;
  kind: "flatpak" | "rpm" | "firmware";
  installed: boolean;
  update: boolean;
  remote: string;
  version: string;
  size: number;
  installation: string;
  verified: boolean;
}
export interface Shelf { title: string; category: string; rows: PackageView[]; }
export interface Discover { hero: PackageView | null; shelves: Shelf[]; }
export interface PendingView { id: string; title: string; action: "install" | "remove" | "update"; kind: string; }
export interface Remote { name: string; title: string; url: string; installation: string; enabled: boolean; }
export interface JobView {
  id: number;
  package_id: string;
  title: string;
  action: "install" | "remove" | "update";
  state: "running" | "done" | "failed" | "cancelled";
  percent: number | null;
  message: string;
}
export interface PageView {
  rows: PackageView[];
  total: number;
  pending: PendingView | null;
  sources: Remote[];
  update_count: number;
  installed_count: number;
  loading: boolean;
  checking: boolean;
  error: string;
  jobs: JobView[];
  discover: Discover | null;
  indexing: boolean;
}
export interface Screen { caption: string; uri: string; }
export interface InfoView {
  found: boolean;
  developer: string;
  license: string;
  homepage: string;
  description: string[];
  categories: string[];
  release: string;
}

export const VERB = { install: "Get", remove: "Remove", update: "Update" } as const;
export const PROGRESS = { install: "Installing", remove: "Removing", update: "Updating" } as const;

export function formatSize(bytes: number): string {
  if (!bytes) return "";
  if (bytes >= 1e9) return `${(bytes / 1e9).toFixed(1)} GB`;
  if (bytes >= 1e6) return `${(bytes / 1e6).toFixed(bytes >= 1e8 ? 0 : 1)} MB`;
  return `${Math.max(1, Math.round(bytes / 1e3))} KB`;
}
