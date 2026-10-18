import assert from "node:assert/strict";
import { test } from "node:test";
import { ageLabel, contextLabel, matches, priceLine, sorted, validId, type CloudModel } from "../src/cloudlist.ts";

const model = (id: string, over: Partial<CloudModel> = {}): CloudModel => ({
  id, name: id, blurb: "", context: 32_000, prompt_per_m: 1, completion_per_m: 2, free: false, vision: false, makes_images: false,
  tools: false, reasoning: false, created: 0, ...over,
});
const list = [
  model("a/old", { created: 1, prompt_per_m: 5, completion_per_m: 15 }),
  model("b/new-vision", { created: 9, vision: true, context: 1_000_000, prompt_per_m: 2, completion_per_m: 8 }),
  model("c/free", { created: 5, free: true, prompt_per_m: 0, completion_per_m: 0, reasoning: true }),
  model("d/varies", { created: 7, prompt_per_m: null, completion_per_m: null, tools: true }),
];

test("search reads the id, name and description; filters narrow by what a model does", () => {
  assert.equal(matches(model("x/y", { name: "Llama Big", blurb: "writes code" }), "code", "all", []), true);
  assert.equal(matches(model("x/y"), "zzz", "all", []), false);
  assert.deepEqual(list.filter((m) => matches(m, "", "vision", [])).map((m) => m.id), ["b/new-vision"]);
  assert.deepEqual(list.filter((m) => matches(m, "", "free", [])).map((m) => m.id), ["c/free"]);
  assert.deepEqual(list.filter((m) => matches(m, "", "long", [])).map((m) => m.id), ["b/new-vision"]);
  assert.deepEqual(list.filter((m) => matches(m, "", "pinned", ["a/old"])).map((m) => m.id), ["a/old"]);
});

test("sorting: newest first, cheapest first with unknown prices last, largest context", () => {
  assert.deepEqual(sorted(list, "newest").map((m) => m.id), ["b/new-vision", "d/varies", "c/free", "a/old"]);
  assert.deepEqual(sorted(list, "cheapest").map((m) => m.id), ["c/free", "b/new-vision", "a/old", "d/varies"]);
  assert.equal(sorted(list, "context")[0].id, "b/new-vision");
});

test("price, size and age are said plainly", () => {
  assert.equal(priceLine(list[0]), "$5.00 in · $15.00 out");
  assert.equal(priceLine(list[2]), "Free");
  assert.equal(priceLine(list[3]), "Price varies");
  assert.equal(priceLine(model("m/n", { prompt_per_m: 0.15, completion_per_m: 0.6 })), "$0.15 in · $0.60 out");
  assert.equal(priceLine(model("m/n", { prompt_per_m: 0.012, completion_per_m: 0.05 })), "$0.012 in · $0.050 out");
  assert.equal(contextLabel(1_048_576), "1M");
  assert.equal(contextLabel(2_000_000), "2M");
  assert.equal(contextLabel(262_144), "262K");
  assert.equal(ageLabel(0, 1000), "Not fetched yet");
  assert.equal(ageLabel(1000, 1000 + 3600), "Updated today");
  assert.equal(ageLabel(1000, 1000 + 86400 * 3), "Updated 3 days ago");
});

test("only vendor/name ids can be added by hand", () => {
  assert.equal(validId("openai/gpt-4o"), true);
  assert.equal(validId("meta-llama/llama-3.3-70b-instruct:free"), true);
  for (const bad of ["gpt-4o", "/x", "x/", "a b/c", "../x/y", "a/b;c"]) assert.equal(validId(bad), false, bad);
});
