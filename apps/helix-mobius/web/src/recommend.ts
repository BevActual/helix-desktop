// Which model should do which job on this computer. Pure, so it is tested without the page (npm test).

export type Job = "chat" | "code" | "vision" | "summarize" | "act" | "speech" | "image";

export interface ModelCard {
  id: string;
  file: string;
  title: string;
  tasks: string[];
  vision: boolean;
  bytes: number;
  memory_gb: number;
  installed: boolean;
  in_catalog: boolean;
  fit: "gpu" | "tight" | "cpu" | "no" | "";
  speed: number | null;
}

const FIT_RANK: Record<string, number> = { gpu: 0, tight: 1, cpu: 2, "": 2, no: 3 };

/** Can this model do the job at all? Speech and image models do only theirs. */
export function eligible(job: Job, card: ModelCard): boolean {
  const speech = card.tasks.includes("speech");
  const image = card.tasks.includes("image");
  if (job === "speech") return speech;
  if (job === "image") return image;
  if (speech || image) return false;
  if (job === "vision") return card.vision;
  return card.tasks.includes("chat") || card.tasks.includes("code");
}

/** Lower is better when listing models for a computer: installed first, then how well it fits, then the larger (better) one. */
export function listOrder(a: ModelCard, b: ModelCard): number {
  if (a.installed !== b.installed) return a.installed ? -1 : 1;
  const fit = FIT_RANK[a.fit] - FIT_RANK[b.fit];
  if (fit !== 0) return fit;
  return b.memory_gb - a.memory_gb || a.title.localeCompare(b.title);
}

/** Why a model was suggested, in a few words. */
export function reason(job: Job, card: ModelCard): string {
  if (job === "summarize") return "Small and quick, which is what a summary needs";
  if (job === "code" && card.tasks.includes("code")) return card.fit === "gpu" ? "Built for code, and it fits your graphics card" : "Built for code";
  if (job === "vision") return card.fit === "gpu" ? "Reads pictures, and fits your graphics card" : "Reads pictures";
  if (job === "speech") return "Recognises speech on this computer, in about 99 languages";
  if (job === "image") return "Draws on your graphics card, so nothing is uploaded";
  if (card.fit === "gpu") return "The most capable model that fits your graphics card";
  if (card.fit === "tight") return "The most capable model that will run here, a tight fit";
  return "Runs on the processor, so replies are slower";
}

/** The model to suggest for a job: one that fits this computer, preferring what is already installed when it is as good a choice. */
export function recommend(job: Job, cards: ModelCard[]): ModelCard | null {
  const pool = cards.filter((c) => c.in_catalog && eligible(job, c) && c.fit !== "no");
  if (!pool.length) return null;
  const fits = pool.filter((c) => c.fit === "gpu");
  const usable = fits.length ? fits : pool.filter((c) => c.fit === "tight").length ? pool.filter((c) => c.fit === "tight") : pool;
  if (job === "speech" || job === "image") return usable[0];
  // A summary wants speed, so the smallest. Anything else wants the most capable that fits; code prefers a code model.
  const ranked = [...usable].sort((a, b) => {
    if (job === "summarize") return a.memory_gb - b.memory_gb || a.title.localeCompare(b.title);
    if (job === "code") {
      const code = Number(b.tasks.includes("code")) - Number(a.tasks.includes("code"));
      if (code !== 0) return code;
    }
    return b.memory_gb - a.memory_gb || a.title.localeCompare(b.title);
  });
  return ranked[0];
}
