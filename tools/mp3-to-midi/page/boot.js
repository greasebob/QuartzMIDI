// The canvas surface (docs/design/youtube-window.md §1.3, step 2). When the served page does not mount, browse.py
// opens /robots.txt and evaluates QMBoot (the page as data), parse.js, app.js and then this file. It builds the
// page with no HTML sinks: the document with createElement, the style sheet adopted, the fonts from base64 and
// the icon sprite from JSON. Then app.js starts; on this surface its router uses the hash (#/?q=…).
(() => {
  "use strict";
  const B = globalThis.QMBoot;
  const app = globalThis.QMApp;
  if (!B || B.surface !== "canvas" || !app || !B.body || document.getElementById("rows")) return;
  const NS = "http://www.w3.org/2000/svg";
  const ICON = "/quartzmidi/QuartzMIDI.ico";

  const bytes = (base64) => {
    const raw = atob(base64);
    const out = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
    return out;
  };
  const icon = B.icon ? URL.createObjectURL(new Blob([bytes(B.icon)], { type: "image/x-icon" })) : "";
  // A node of browse.py's JSON: {tag, attrs, children} (svg: true inside an <svg>), or a string for text.
  const build = (node, svg) => {
    if (typeof node === "string") return document.createTextNode(node);
    const inside = !!(svg || node.svg || node.tag === "svg");
    const made = inside ? document.createElementNS(NS, node.tag) : document.createElement(node.tag);
    for (const [name, value] of Object.entries(node.attrs || {})) {
      made.setAttribute(name, name === "src" && value === ICON ? icon : value);
    }
    for (const kid of node.children || []) made.append(build(kid, inside));
    return made;
  };

  // The document: robots.txt's own nodes go, and the page's head and body take their place.
  const root = document.documentElement;
  root.replaceChildren();
  root.lang = "en";
  const head = document.createElement("head");
  const viewport = document.createElement("meta");
  viewport.name = "viewport";
  viewport.content = "width=device-width, initial-scale=1";
  const title = document.createElement("title");
  title.textContent = B.title || "";
  head.append(viewport, title);
  if (icon) {
    const link = document.createElement("link");
    link.rel = "icon";
    link.href = icon;
    head.append(link);
  }
  const body = document.createElement("body");
  for (const [name, value] of Object.entries(B.body.attrs || {})) body.setAttribute(name, value);
  if (B.icons) body.append(build(B.icons, true));
  for (const kid of B.body.children || []) body.append(build(kid, false));
  root.append(head, body);

  // Styles: app.css adopted, without its @font-face rules, whose /quartzmidi/fonts URLs only the served page has.
  const sheet = new CSSStyleSheet();
  sheet.replaceSync(B.css || "");
  for (let i = sheet.cssRules.length - 1; i >= 0; i--) if (sheet.cssRules[i] instanceof CSSFontFaceRule) sheet.deleteRule(i);
  document.adoptedStyleSheets = [sheet];
  for (const font of B.fonts || []) {
    try {
      document.fonts.add(new FontFace("IBM Plex Sans", bytes(font.data).buffer, { weight: String(font.weight), style: "normal" }));
    } catch { /* a font that does not load falls back to Segoe UI */ }
  }

  // app.js reads rules.json, converted.json and artists.json while it starts; here they come from the bundle.
  const data = new Map([
    ["/quartzmidi/rules.json", B.rules],
    ["/quartzmidi/converted.json", B.converted],
    ["/quartzmidi/artists.json", B.artists],
  ]);
  const fetchPage = globalThis.fetch;
  globalThis.fetch = (input, init) => {
    const json = typeof input === "string" ? data.get(input) : undefined;
    if (json == null) return fetchPage(input, init);
    return Promise.resolve(new Response(JSON.stringify(json), { headers: { "Content-Type": "application/json" } }));
  };
  try {
    app.start();
  } finally {
    globalThis.fetch = fetchPage;
  }
})();
