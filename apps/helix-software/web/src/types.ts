export type Page = "explore" | "library" | "updates" | "sources";

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
}
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
}
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
