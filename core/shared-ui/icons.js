// Shared by the shell page and the approval dialog.
// Icons. One small stroke set drawn with the SVG DOM (no markup is ever parsed): 24-unit grid, round caps, currentColor.
const SVG = "http://www.w3.org/2000/svg";
const ICONS = {
  wifi: [["path", "M2.5 9.2a14 14 0 0 1 19 0"], ["path", "M5.6 12.6a9.4 9.4 0 0 1 12.8 0"], ["path", "M8.9 16a4.8 4.8 0 0 1 6.2 0"], ["dot", 12, 19.3]],
  "wifi-off": [["path", "M2.5 9.2a14 14 0 0 1 4.6-2.9M21.5 9.2a14 14 0 0 0-8-3.9"], ["path", "M5.6 12.6a9.4 9.4 0 0 1 3-2M18.4 12.6a9.4 9.4 0 0 0-3-2"], ["path", "M8.9 16a4.8 4.8 0 0 1 6.2 0"], ["dot", 12, 19.3], ["path", "M4 4l16 16"]],
  code: [["path", "M8 7l-5 5 5 5M16 7l5 5-5 5M14 4l-4 16"]],
  chat: [["path", "M5 5h14a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-8l-4 3.5V17H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2z"]],
  doc: [["path", "M7 3h7l5 5v13H7zM14 3v5h5M10 13h6M10 17h6"]],
  plus: [["path", "M12 5v14M5 12h14"]],
  "arrow-up": [["path", "M12 19V5M6 11l6-6 6 6"]],
  stop: [["rect", 6.5, 6.5, 11, 11, 2.2]],
  copy: [["rect", 8.5, 8.5, 11, 11, 2.4], ["path", "M15.5 8.5V6.9a2.4 2.4 0 0 0-2.4-2.4H6.9a2.4 2.4 0 0 0-2.4 2.4v6.2a2.4 2.4 0 0 0 2.4 2.4h1.6"]],
  trash: [["path", "M5 7h14M10 7V5h4v2M7 7l.8 12h8.4L17 7M10 11v5M14 11v5"]],
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
  "chevron-left": [["path", "M14.5 6l-6 6 6 6"]],
  mic: [["rect", 9, 3, 6, 11, 3], ["path", "M5.5 11.5a6.5 6.5 0 0 0 13 0M12 18v3"]],
  "mic-off": [["path", "M15 9.5V6a3 3 0 0 0-5.7-1.3M9 9v2a3 3 0 0 0 4.7 2.5M5.5 11.5a6.5 6.5 0 0 0 10.4 5.2M18.5 11.5c0 1-.2 1.9-.6 2.7M12 18v3M4 4l16 16"]],
  shield: [["path", "M12 3l7.5 2.8v5.6c0 4.3-3 8-7.5 9.6-4.5-1.6-7.5-5.3-7.5-9.6V5.8z"], ["path", "M9 12l2.2 2.2L15.4 10"]],
  check: [["path", "M5.5 12.6l4.1 4.1L18.6 7.6"]],
  alert: [["path", "M12 4.2L2.9 19.6h18.2z"], ["path", "M12 10v4.6M12 17.4v.1"]],
  moon: [["path", "M19.6 14.4A8 8 0 1 1 9.6 4.4a6.5 6.5 0 0 0 10 10z"]],
  sun: [["circle", 12, 12, 4], ["path", "M12 2.8v2.4M12 18.8v2.4M2.8 12h2.4M18.8 12h2.4M5.5 5.5l1.7 1.7M16.8 16.8l1.7 1.7M5.5 18.5l1.7-1.7M16.8 7.2l1.7-1.7"]],
  contrast: [["circle", 12, 12, 8.4], ["half", 12, 12, 8.4]],
  "chevron-right": [["path", "M9.5 5.5l6.5 6.5-6.5 6.5"]],
  "chevron-left": [["path", "M14.5 5.5L8 12l6.5 6.5"]],
  "chevron-down": [["path", "M6 9.5l6 6 6-6"]],
  settings: [["circle", 12, 12, 3], ["path", "M12 2.8v2.2M12 19v2.2M2.8 12h2.2M19 12h2.2M5.5 5.5l1.6 1.6M16.9 16.9l1.6 1.6M5.5 18.5l1.6-1.6M16.9 7.1l1.6-1.6"]],
  dots: [["dot", 6, 12], ["dot", 12, 12], ["dot", 18, 12]],
  pin: [["path", "M9 3.5h6l-.8 6.2 2.8 2.8V14H13v6l-1 1-1-1v-6H7v-1.5l2.8-2.8z"]],
  folder: [["path", "M3.5 7.5V18a2 2 0 0 0 2 2h13a2 2 0 0 0 2-2V9.5a2 2 0 0 0-2-2h-6.2L10.5 5.5H5.5a2 2 0 0 0-2 2z"]],
  sidebar: [["rect", 3.5, 4.5, 17, 15, 2.4], ["path", "M9.5 4.5v15"]],
  battery: [["rect", 2.5, 7.5, 16, 9, 2.6], ["path", "M21 10.6v2.8"]],
  bolt: [["path", "M13 3L5.5 13.5H11L10 21l7.5-10.5H12z"]],
  music: [["path", "M9 17.5V6.2l9-2v11.3"], ["circle", 6.8, 17.6, 2.6], ["circle", 15.8, 15.6, 2.6]],
  power: [["path", "M12 3v9"], ["path", "M7.2 6.2a7.2 7.2 0 1 0 9.6 0"]],
  play: [["path", "M8 5.5v13l11-6.5z"]],
  pause: [["rect", 6.5, 5.5, 3.8, 13, 1.2], ["rect", 13.7, 5.5, 3.8, 13, 1.2]],
  "skip-forward": [["path", "M5 5.5v13l9.5-6.5z"], ["path", "M18.5 5.5v13"]],
  "skip-back": [["path", "M19 5.5v13L9.5 12z"], ["path", "M5.5 5.5v13"]],
  x: [["path", "M6.5 6.5l11 11M17.5 6.5l-11 11"]],
  search: [["circle", 10.8, 10.8, 6.3], ["path", "M15.6 15.6l5 5"]],
  clipboard: [["rect", 5.5, 4.5, 13, 16, 2.4], ["path", "M9 4.5V4a1.6 1.6 0 0 1 1.6-1.6h2.8A1.6 1.6 0 0 1 15 4v.5"]],
  fork: [["path", "M7 4v8.5a3 3 0 0 0 3 3h4"], ["path", "M17 20v-4.5a3 3 0 0 0-3-3H10"], ["circle", 7, 4, 2], ["circle", 17, 20, 2], ["circle", 17, 8, 2], ["path", "M17 10V4"]],
  paperclip: [["path", "M8.5 12.5l6.2-6.2a2.8 2.8 0 0 1 4 4L9.8 19.2a4.5 4.5 0 0 1-6.4-6.4l8.3-8.3a2 2 0 0 1 2.8 2.8L7.2 14.6"]],
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
