// Shared by the shell page and the approval dialog.
// Icons. One small stroke set drawn with the SVG DOM (no markup is ever parsed): 24-unit grid, round caps, currentColor.
const SVG = "http://www.w3.org/2000/svg";
const ICONS = {
  wifi: [["path", "M2.5 9.2a14 14 0 0 1 19 0"], ["path", "M5.6 12.6a9.4 9.4 0 0 1 12.8 0"], ["path", "M8.9 16a4.8 4.8 0 0 1 6.2 0"], ["dot", 12, 19.3]],
  "wifi-off": [["path", "M2.5 9.2a14 14 0 0 1 4.6-2.9M21.5 9.2a14 14 0 0 0-8-3.9"], ["path", "M5.6 12.6a9.4 9.4 0 0 1 3-2M18.4 12.6a9.4 9.4 0 0 0-3-2"], ["path", "M8.9 16a4.8 4.8 0 0 1 6.2 0"], ["dot", 12, 19.3], ["path", "M4 4l16 16"]],
  bluetooth: [["path", "M7 7.5l10 9-5 4.5V3l5 4.5-10 9"]],
  volume: [["path", "M4 9.6h3.4L12 5.6v12.8l-4.6-4H4z"], ["path", "M15.4 9a4.2 4.2 0 0 1 0 6"], ["path", "M18.2 6.4a8 8 0 0 1 0 11.2"]],
  "volume-mute": [["path", "M4 9.6h3.4L12 5.6v12.8l-4.6-4H4z"], ["path", "M16 9.5l5 5M21 9.5l-5 5"]],
  bell: [["path", "M6 16.8V11a6 6 0 0 1 12 0v5.8l1.6 2.2H4.4z"], ["path", "M10 21h4"]],
  grid: [["rect", 4, 4, 6.5, 6.5, 1.8], ["rect", 13.5, 4, 6.5, 6.5, 1.8], ["rect", 4, 13.5, 6.5, 6.5, 1.8], ["rect", 13.5, 13.5, 6.5, 6.5, 1.8]],
  sparkle: [["path", "M12 3.2l1.9 5.4 5.4 1.9-5.4 1.9L12 17.8l-1.9-5.4-5.4-1.9 5.4-1.9z"], ["path", "M19 16.5l.7 1.8 1.8.7-1.8.7-.7 1.8-.7-1.8-1.8-.7 1.8-.7z"]],
  palette: [["path", "M12 3.5a8.5 8.5 0 1 0 0 17c1.4 0 2-.9 2-1.8 0-1.2-1-1.6-1-2.7 0-1 .8-1.6 1.8-1.6H17a3.5 3.5 0 0 0 3.5-3.5C20.5 7 16.7 3.5 12 3.5z"], ["dot", 7.8, 11.4], ["dot", 10.4, 7.6], ["dot", 15, 7.8]],
  eye: [["path", "M2.5 12s3.4-6.2 9.5-6.2S21.5 12 21.5 12s-3.4 6.2-9.5 6.2S2.5 12 2.5 12z"], ["circle", 12, 12, 2.8]],
  lock: [["rect", 5, 10.5, 14, 10, 2.4], ["path", "M8 10.5V8a4 4 0 0 1 8 0v2.5"]],
  keyboard: [["rect", 2.5, 6.5, 19, 11, 2.4], ["path", "M6.5 10h.01M10 10h.01M13.5 10h.01M17 10h.01M7 14h10"]],
  image: [["rect", 3.5, 5, 17, 14, 2.6], ["circle", 9, 10, 1.8], ["path", "M4 17l5-4.5 3.5 3 3-2.5 4.5 4"]],
  shield: [["path", "M12 3l7.5 2.8v5.6c0 4.3-3 8-7.5 9.6-4.5-1.6-7.5-5.3-7.5-9.6V5.8z"], ["path", "M9 12l2.2 2.2L15.4 10"]],
  moon: [["path", "M19.6 14.4A8 8 0 1 1 9.6 4.4a6.5 6.5 0 0 0 10 10z"]],
  sun: [["circle", 12, 12, 4], ["path", "M12 2.8v2.4M12 18.8v2.4M2.8 12h2.4M18.8 12h2.4M5.5 5.5l1.7 1.7M16.8 16.8l1.7 1.7M5.5 18.5l1.7-1.7M16.8 7.2l1.7-1.7"]],
  contrast: [["circle", 12, 12, 8.4], ["half", 12, 12, 8.4]],
  "chevron-right": [["path", "M9.5 5.5l6.5 6.5-6.5 6.5"]],
  "chevron-left": [["path", "M14.5 5.5L8 12l6.5 6.5"]],
  battery: [["rect", 2.5, 7.5, 16, 9, 2.6], ["path", "M21 10.6v2.8"]],
  bolt: [["path", "M13 3L5.5 13.5H11L10 21l7.5-10.5H12z"]],
  music: [["path", "M9 17.5V6.2l9-2v11.3"], ["circle", 6.8, 17.6, 2.6], ["circle", 15.8, 15.6, 2.6]],
  x: [["path", "M6.5 6.5l11 11M17.5 6.5l-11 11"]],
  search: [["circle", 10.8, 10.8, 6.3], ["path", "M15.6 15.6l5 5"]],
  clipboard: [["rect", 5.5, 4.5, 13, 16, 2.4], ["path", "M9 4.5V4a1.6 1.6 0 0 1 1.6-1.6h2.8A1.6 1.6 0 0 1 15 4v.5"]],
};
function icon(name) {
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("class", "i");
  svg.dataset.name = name;
  for (const [kind, ...a] of ICONS[name] || []) {
    const part = document.createElementNS(SVG, kind === "dot" || kind === "half" ? (kind === "dot" ? "circle" : "path") : kind);
    if (kind === "path") part.setAttribute("d", a[0]);
    else if (kind === "circle") { part.setAttribute("cx", a[0]); part.setAttribute("cy", a[1]); part.setAttribute("r", a[2]); }
    else if (kind === "dot") { part.setAttribute("cx", a[0]); part.setAttribute("cy", a[1]); part.setAttribute("r", 1.2); part.setAttribute("class", "solid"); }
    else if (kind === "half") { part.setAttribute("d", "M" + a[0] + " " + (a[1] - a[2]) + "a" + a[2] + " " + a[2] + " 0 0 1 0 " + 2 * a[2] + "z"); part.setAttribute("class", "solid"); }
    else if (kind === "rect") { ["x", "y", "width", "height", "rx"].forEach((attr, i) => part.setAttribute(attr, a[i])); }
    svg.append(part);
  }
  return svg;
}
// Swaps the icon element for another by name, keeping its slot, so a status glyph can change without being found again.
function swapIcon(svg, name) {
  if (!svg || svg.dataset.name === name) return svg;
  const next = icon(name);
  if (svg.dataset.slot) next.dataset.slot = svg.dataset.slot;
  svg.replaceWith(next);
  return next;
}
const slot = (name) => document.querySelector('[data-slot="' + name + '"]');
document.querySelectorAll("i[data-icon]").forEach((placeholder) => {
  const svg = icon(placeholder.dataset.icon);
  if (placeholder.dataset.slot) svg.dataset.slot = placeholder.dataset.slot;
  placeholder.replaceWith(svg);
});
