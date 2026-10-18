// Searching, narrowing and sorting OpenRouter's catalog, and saying a model's price, size and age in plain words. Pure, tested with npm test.

export interface CloudModel {
  id: string;
  name: string;
  blurb: string;
  context: number;
  prompt_per_m: number | null;
  completion_per_m: number | null;
  free: boolean;
  vision: boolean;
  makes_images: boolean;
  tools: boolean;
  reasoning: boolean;
  created: number;
}

export type CloudFilter = "all" | "pinned" | "free" | "vision" | "reasoning" | "tools" | "long";
export type CloudSort = "newest" | "cheapest" | "context" | "name";

export const LONG_CONTEXT = 128_000;

export function matches(model: CloudModel, needle: string, filter: CloudFilter, pins: string[]): boolean {
  const text = needle.trim().toLowerCase();
  if (text && !`${model.id} ${model.name} ${model.blurb}`.toLowerCase().includes(text)) return false;
  switch (filter) {
    case "pinned": return pins.includes(model.id);
    case "free": return model.free;
    case "vision": return model.vision;
    case "reasoning": return model.reasoning;
    case "tools": return model.tools;
    case "long": return model.context >= LONG_CONTEXT;
    default: return true;
  }
}

/** Cost of a typical exchange: the prompt and the reply priced together. Unknown prices sort last. */
function cost(model: CloudModel): number {
  if (model.prompt_per_m === null || model.completion_per_m === null) return Number.POSITIVE_INFINITY;
  return model.prompt_per_m + model.completion_per_m;
}

export function sorted(models: CloudModel[], sort: CloudSort): CloudModel[] {
  const list = [...models];
  switch (sort) {
    case "cheapest": return list.sort((a, b) => cost(a) - cost(b) || a.name.localeCompare(b.name));
    case "context": return list.sort((a, b) => b.context - a.context || a.name.localeCompare(b.name));
    case "name": return list.sort((a, b) => a.name.localeCompare(b.name));
    default: return list.sort((a, b) => b.created - a.created || a.id.localeCompare(b.id));
  }
}

function dollars(value: number): string {
  if (value >= 100) return `$${Math.round(value)}`;
  if (value >= 0.1) return `$${value.toFixed(2)}`;
  return `$${value.toFixed(3)}`;
}

/** "$2.50 in · $10.00 out", "Free", or "Price varies", per million tokens. */
export function priceLine(model: CloudModel): string {
  if (model.free) return "Free";
  if (model.prompt_per_m === null || model.completion_per_m === null) return "Price varies";
  return `${dollars(model.prompt_per_m)} in · ${dollars(model.completion_per_m)} out`;
}

/** "1M", "262K", "33K": how much it can read at once. */
export function contextLabel(tokens: number): string {
  if (!tokens) return "";
  if (tokens >= 1_000_000) return `${(tokens / 1_000_000).toFixed(1).replace(/\.0$/, "")}M`;
  return `${Math.round(tokens / 1000)}K`;
}

export function ageLabel(fetched: number, now: number): string {
  if (!fetched) return "Not fetched yet";
  const days = Math.floor((now - fetched) / 86400);
  if (days <= 0) return "Updated today";
  if (days === 1) return "Updated yesterday";
  return `Updated ${days} days ago`;
}

/** A model id as OpenRouter writes it: vendor, a slash, then the name. */
export function validId(id: string): boolean {
  return /^[A-Za-z0-9._:-]+\/[A-Za-z0-9._:/-]+$/.test(id) && id.length <= 120 && !id.includes("..") && !id.endsWith("/");
}
