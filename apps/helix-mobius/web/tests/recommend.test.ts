import assert from "node:assert/strict";
import { test } from "node:test";
import { eligible, listOrder, recommend, type ModelCard } from "../src/recommend.ts";

const card = (id: string, over: Partial<ModelCard>): ModelCard => ({
  id, file: `${id}.gguf`, title: id, tasks: ["chat"], vision: false, bytes: 1e9, memory_gb: 2, installed: false, in_catalog: true, fit: "gpu", speed: null, ...over,
});
const library = [
  card("small", { memory_gb: 2 }),
  card("medium", { memory_gb: 4 }),
  card("big", { memory_gb: 9, fit: "cpu" }),
  card("coder", { memory_gb: 5, tasks: ["code", "chat"] }),
  card("looker", { memory_gb: 6, tasks: ["vision", "chat"], vision: true }),
  card("whisper", { memory_gb: 0.7, tasks: ["speech"] }),
  card("sd", { memory_gb: 5, tasks: ["image"] }),
];

test("chat gets the most capable model that fits the graphics card, not one that only runs on the processor", () => {
  assert.equal(recommend("chat", library)?.id, "looker");
  assert.equal(recommend("chat", library.filter((c) => c.id !== "looker" && c.id !== "coder"))?.id, "medium");
});

test("code prefers a code model, vision needs one that sees, summaries want the smallest", () => {
  assert.equal(recommend("code", library)?.id, "coder");
  assert.equal(recommend("vision", library)?.id, "looker");
  assert.equal(recommend("summarize", library)?.id, "small");
});

test("speech and image models are never offered for chat, and chat models never for them", () => {
  assert.equal(eligible("chat", library[5]), false);
  assert.equal(eligible("chat", library[6]), false);
  assert.equal(eligible("speech", library[0]), false);
  assert.equal(recommend("speech", library)?.id, "whisper");
  assert.equal(recommend("image", library)?.id, "sd");
});

test("when nothing fits the graphics card it falls back to tight, then to the processor, and never to too big", () => {
  const tight = [card("a", { fit: "tight", memory_gb: 7 }), card("b", { fit: "cpu", memory_gb: 3 })];
  assert.equal(recommend("chat", tight)?.id, "a");
  assert.equal(recommend("chat", [card("c", { fit: "cpu" })])?.id, "c");
  assert.equal(recommend("chat", [card("d", { fit: "no" })]), null);
});

test("a model that is not in the catalog is never suggested", () => {
  assert.equal(recommend("chat", [card("mine", { in_catalog: false })]), null);
});

test("the list shows installed models first, then by fit, then the larger", () => {
  const sorted = [card("x", { fit: "cpu", installed: false }), card("y", { installed: true, fit: "cpu" }), card("z", { fit: "gpu", memory_gb: 3 }), card("w", { fit: "gpu", memory_gb: 8 })].sort(listOrder);
  assert.deepEqual(sorted.map((c) => c.id), ["y", "w", "z", "x"]);
});
