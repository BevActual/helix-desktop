/** The text edits the Write stage's toolbar and shortcuts make: pure functions from (text, selection) to (text, selection), so the editor can
 * apply one and keep the caret where a person expects it. Markdown only; nothing here touches the page. */
export interface Edit { text: string; start: number; end: number }

const lineStart = (text: string, at: number) => text.lastIndexOf("\n", at - 1) + 1;
const lineEnd = (text: string, at: number) => { const i = text.indexOf("\n", at); return i < 0 ? text.length : i; };

/** Wraps the selection in a marker (`**`, `*`, `` ` ``), or takes it off when it is already wrapped. With no selection it places the caret between a new pair. */
export function toggleWrap(text: string, start: number, end: number, marker: string): Edit {
  const m = marker.length;
  const before = text.slice(Math.max(0, start - m), start), after = text.slice(end, end + m);
  if (before === marker && after === marker) {
    return { text: text.slice(0, start - m) + text.slice(start, end) + text.slice(end + m), start: start - m, end: end - m };
  }
  const chosen = text.slice(start, end);
  if (chosen.length >= 2 * m && chosen.startsWith(marker) && chosen.endsWith(marker)) {
    const inner = chosen.slice(m, chosen.length - m);
    return { text: text.slice(0, start) + inner + text.slice(end), start, end: start + inner.length };
  }
  return { text: text.slice(0, start) + marker + chosen + marker + text.slice(end), start: start + m, end: end + m };
}

/** Sets the heading level of the lines the selection touches (0 removes it). Pressing the same level again removes it. */
export function setHeading(text: string, start: number, end: number, level: number): Edit {
  const from = lineStart(text, start), to = lineEnd(text, end);
  const lines = text.slice(from, to).split("\n");
  const same = lines.every((l) => l.startsWith("#".repeat(level) + " "));
  const out = lines.map((l) => {
    const bare = l.replace(/^#{1,6}\s+/, "");
    return same || level === 0 ? bare : "#".repeat(level) + " " + bare;
  }).join("\n");
  return { text: text.slice(0, from) + out + text.slice(to), start: from, end: from + out.length };
}

/** Prefixes (or un-prefixes) every line the selection touches: bullets, numbers, quotes. */
export function toggleLines(text: string, start: number, end: number, kind: "bullet" | "number" | "quote"): Edit {
  const from = lineStart(text, start), to = lineEnd(text, end);
  const lines = text.slice(from, to).split("\n");
  const has = (l: string) => kind === "bullet" ? /^[-*]\s/.test(l) : kind === "number" ? /^\d+\.\s/.test(l) : /^>\s?/.test(l);
  const strip = (l: string) => l.replace(kind === "bullet" ? /^[-*]\s/ : kind === "number" ? /^\d+\.\s/ : /^>\s?/, "");
  const off = lines.every((l) => l.trim() === "" || has(l));
  let n = 0;
  const out = lines.map((l) => {
    if (off) return strip(l);
    if (l.trim() === "") return l;
    n += 1;
    return (kind === "bullet" ? "- " : kind === "number" ? `${n}. ` : "> ") + strip(l);
  }).join("\n");
  return { text: text.slice(0, from) + out + text.slice(to), start: from, end: from + out.length };
}

/** A link around the selection: `[text](url)`, with the address selected so it can be typed over. */
export function makeLink(text: string, start: number, end: number): Edit {
  const chosen = text.slice(start, end) || "link";
  const url = "https://";
  const next = text.slice(0, start) + `[${chosen}](${url})` + text.slice(end);
  const at = start + chosen.length + 3;
  return { text: next, start: at, end: at + url.length };
}

/** Enter at the end of a list item starts the next item (an empty item ends the list). Returns null when the line is not a list item. */
export function continueList(text: string, caret: number): Edit | null {
  const from = lineStart(text, caret), line = text.slice(from, caret);
  const m = /^(\s*)([-*]|\d+\.)\s(.*)$/.exec(line);
  if (!m || caret !== lineEnd(text, caret)) return null;
  const [, indent, marker, rest] = m;
  if (rest.trim() === "") {
    // An empty item: the list ends here.
    return { text: text.slice(0, from) + text.slice(caret), start: from, end: from };
  }
  const next = /^\d+\./.test(marker) ? `${parseInt(marker, 10) + 1}.` : marker;
  const insert = `\n${indent}${next} `;
  return { text: text.slice(0, caret) + insert + text.slice(caret), start: caret + insert.length, end: caret + insert.length };
}

/** Tab indents the lines touched; Shift+Tab takes two spaces off. */
export function indent(text: string, start: number, end: number, back: boolean): Edit {
  const from = lineStart(text, start), to = lineEnd(text, end);
  const lines = text.slice(from, to).split("\n");
  const out = lines.map((l) => back ? l.replace(/^( {1,2}|\t)/, "") : "  " + l).join("\n");
  const delta = out.length - (to - from);
  return { text: text.slice(0, from) + out + text.slice(to), start: Math.max(from, start + (back ? Math.min(0, delta) : 2)), end: end + delta };
}

export interface Heading { level: number; title: string; offset: number; line: number }

/** The document's headings, for the outline. Lines inside code fences are not headings. */
export function outline(text: string): Heading[] {
  const out: Heading[] = [];
  let offset = 0, fenced = false;
  text.split("\n").forEach((line, i) => {
    if (/^\s*```/.test(line)) fenced = !fenced;
    const m = !fenced && /^(#{1,6})\s+(.+?)\s*#*\s*$/.exec(line);
    if (m) out.push({ level: m[1].length, title: m[2], offset, line: i });
    offset += line.length + 1;
  });
  return out;
}

export const readingMinutes = (words: number) => Math.max(1, Math.round(words / 230));

/** Every place `needle` occurs (case-insensitive), as start offsets. */
export function findAll(text: string, needle: string): number[] {
  if (!needle) return [];
  const hay = text.toLowerCase(), n = needle.toLowerCase(), out: number[] = [];
  for (let i = hay.indexOf(n); i >= 0; i = hay.indexOf(n, i + Math.max(1, n.length))) out.push(i);
  return out;
}
