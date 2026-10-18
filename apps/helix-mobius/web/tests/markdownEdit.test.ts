// Run with: npm test  (node's built-in runner, TypeScript types stripped by node)
import assert from "node:assert/strict";
import { test } from "node:test";
import { continueList, findAll, indent, makeLink, outline, readingMinutes, setHeading, toggleLines, toggleWrap } from "../src/markdownEdit.ts";

test("bold wraps the selection, and wrapping again takes it off", () => {
  const a = toggleWrap("make this loud", 5, 9, "**");
  assert.equal(a.text, "make **this** loud");
  assert.deepEqual([a.start, a.end], [7, 11]);
  const b = toggleWrap(a.text, a.start, a.end, "**");
  assert.equal(b.text, "make this loud");
  assert.deepEqual([b.start, b.end], [5, 9]);
});

test("with nothing selected the caret goes between a new pair", () => {
  const a = toggleWrap("ab", 1, 1, "*");
  assert.equal(a.text, "a**b");
  assert.deepEqual([a.start, a.end], [2, 2]);
});

test("a selection that already contains its markers is unwrapped", () => {
  const a = toggleWrap("x **bold** y", 2, 10, "**");
  assert.equal(a.text, "x bold y");
});

test("headings set the level for every line touched, and the same level again removes it", () => {
  const a = setHeading("one\ntwo", 0, 7, 2);
  assert.equal(a.text, "## one\n## two");
  assert.equal(setHeading(a.text, 0, a.text.length, 2).text, "one\ntwo");
  assert.equal(setHeading("# old", 0, 5, 3).text, "### old");
  assert.equal(setHeading("## gone", 0, 7, 0).text, "gone");
});

test("lists and quotes prefix each line, number in order, skip blank lines and toggle off", () => {
  const a = toggleLines("a\nb\n\nc", 0, 6, "number");
  assert.equal(a.text, "1. a\n2. b\n\n3. c");
  assert.equal(toggleLines(a.text, 0, a.text.length, "number").text, "a\nb\n\nc");
  assert.equal(toggleLines("x", 0, 1, "bullet").text, "- x");
  assert.equal(toggleLines("> q", 0, 3, "quote").text, "q");
});

test("a link keeps the selected words and selects its address", () => {
  const a = makeLink("see docs now", 4, 8);
  assert.equal(a.text, "see [docs](https://) now");
  assert.equal(a.text.slice(a.start, a.end), "https://");
});

test("Enter in a list continues it, counts numbers, and an empty item ends the list", () => {
  assert.equal(continueList("- one", 5)?.text, "- one\n- ");
  assert.equal(continueList("3. three", 8)?.text, "3. three\n4. ");
  assert.equal(continueList("  - nested", 10)?.text, "  - nested\n  - ");
  assert.equal(continueList("- one\n- ", 8)?.text, "- one\n");
  assert.equal(continueList("plain", 5), null);
  assert.equal(continueList("- mid line", 4), null, "only at the end of the line");
});

test("Tab indents, Shift+Tab takes it back", () => {
  assert.equal(indent("a\nb", 0, 3, false).text, "  a\n  b");
  assert.equal(indent("  a\n  b", 0, 7, true).text, "a\nb");
  assert.equal(indent("a", 0, 1, true).text, "a");
});

test("the outline lists headings, not lines in code fences", () => {
  const text = "# Title\ntext\n## Part one\n```\n# not a heading\n```\n### Deep ##";
  const o = outline(text);
  assert.deepEqual(o.map((h) => [h.level, h.title]), [[1, "Title"], [2, "Part one"], [3, "Deep"]]);
  assert.equal(text.slice(o[1].offset, o[1].offset + 11), "## Part one");
});

test("find is case-insensitive and finds every place", () => {
  assert.deepEqual(findAll("Cat cat CAT", "cat"), [0, 4, 8]);
  assert.deepEqual(findAll("aaa", "aa"), [0], "matches do not overlap");
  assert.deepEqual(findAll("abc", ""), []);
});

test("reading time is at least a minute", () => {
  assert.equal(readingMinutes(10), 1);
  assert.equal(readingMinutes(2300), 10);
});
