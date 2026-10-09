// boot_test.js: page/boot.js, the canvas surface's boot, run against a made-up document. app.js reads its
// /quartzmidi/*.json files while it starts; on the canvas each must come from the bundle browse.py sends.
//   deno test --allow-read tools\mp3-to-midi\tests
const SOURCE = Deno.readTextFileSync(new URL("../page/boot.js", import.meta.url));
const APP = Deno.readTextFileSync(new URL("../page/app.js", import.meta.url));

function check(cond, msg) {
  if (!cond) throw new Error(msg);
}
const same = (a, b, msg) => check(JSON.stringify(a) === JSON.stringify(b), msg + ": " + JSON.stringify(a) + " != " + JSON.stringify(b));

// Every JSON file app.js fetches from /quartzmidi while it starts.
const STARTS = [...APP.matchAll(/fetch\("(\/quartzmidi\/([a-z]+)\.json)"/g)].map((m) => ({ url: m[1], name: m[2] }));

const node = () => ({
  attrs: {},
  kids: [],
  setAttribute(name, value) { this.attrs[name] = value; },
  append(...kids) { this.kids.push(...kids); },
});

// Runs boot.js with QMBoot carrying `bundle`; app.start fetches what app.js fetches, while it starts, and the
// page's own fetch answers anything the bundle does not hold with a 404.
async function boot(bundle) {
  const asked = [];
  const pageFetch = (url) => {
    asked.push(url);
    return Promise.resolve(new Response("", { status: 404 }));
  };
  const started = [];
  const g = {
    fetch: pageFetch,
    QMBoot: { surface: "canvas", body: { attrs: {}, children: [] }, css: "", ...bundle },
    QMApp: {
      start() {
        for (const { url } of STARTS) started.push(g.fetch(url, { credentials: "same-origin" }).then((r) => (r.ok ? r.json() : null)));
      },
    },
  };
  const document = {
    documentElement: { replaceChildren() {}, append() {} },
    getElementById: () => null,
    createElement: node,
    createElementNS: node,
    createTextNode: (text) => ({ text }),
    adoptedStyleSheets: [],
  };
  class CSSStyleSheet {
    cssRules = [];
    replaceSync() {}
    deleteRule() {}
  }
  class CSSFontFaceRule {}
  new Function("globalThis", "document", "CSSStyleSheet", "CSSFontFaceRule", SOURCE)(g, document, CSSStyleSheet, CSSFontFaceRule);
  check(g.fetch === pageFetch, "the page's own fetch is back once app.js has started");
  return { got: await Promise.all(started), asked };
}

Deno.test("the canvas hands app.js every JSON file it reads while it starts, from the bundle", async () => {
  same(STARTS.map((s) => s.name).sort(), ["artists", "converted", "rules"], "what app.js reads while it starts");
  const bundle = {
    rules: { other: [{ term: "tutorial" }] },
    converted: { ids: ["dQw4w9WgXcQ"] },
    artists: { artists: [{ id: "UC" + "a".repeat(22), name: "Ann", n: 2, at: 5 }], seeds: [] },
  };
  const { got, asked } = await boot(bundle);
  STARTS.forEach(({ name }, i) => same(got[i], bundle[name], name + ".json"));
  same(asked, [], "nothing went to the page's own fetch");
});
