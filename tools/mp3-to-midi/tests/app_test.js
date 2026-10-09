// app_test.js: page/app.js on page/index.html in a made-up document (fake_dom.js), with YouTube and browse.py
// answered by the test.
//   deno test --allow-read tools\mp3-to-midi\tests
import "../page/parse.js";
import { FakeEvent, fakeHistory, input, parseHtml } from "./fake_dom.js";

const P = globalThis.QMParse;
const HTML = Deno.readTextFileSync(new URL("../page/index.html", import.meta.url));
const SOURCE = Deno.readTextFileSync(new URL("../page/app.js", import.meta.url));

function check(cond, msg) {
  if (!cond) throw new Error(msg);
}
const same = (a, b, msg) => check(JSON.stringify(a) === JSON.stringify(b), msg + ": " + JSON.stringify(a) + " != " + JSON.stringify(b));

// ---- made-up YouTube

const searchOf = (items) => ({
  contents: { twoColumnSearchResultsRenderer: { primaryContents: { sectionListRenderer: { contents: [
    { itemSectionRenderer: { contents: items } },
  ] } } } },
});
const video = (videoId, title, extra = {}) =>
  ({ videoRenderer: { videoId, title: { runs: [{ text: title }] }, lengthText: { simpleText: "3:20" }, ...extra } });
const badged = (style, label) => ({ badges: [{ metadataBadgeRenderer: { style, label } }] });
const membersOnly = (videoId, title) => video(videoId, title, badged("BADGE_STYLE_TYPE_MEMBERS_ONLY", "Members only"));
const live = (videoId, title) => ({ videoRenderer: { videoId, title: { runs: [{ text: title }] }, ...badged("BADGE_STYLE_TYPE_LIVE_NOW", "LIVE") } });
// A watch page's next response: the video's own title and badges, and no related videos.
const nextOf = (id, title, badges = []) => ({
  currentVideoEndpoint: { watchEndpoint: { videoId: id } },
  contents: { twoColumnWatchNextResults: {
    results: { results: { contents: [{ videoPrimaryInfoRenderer: { title: { runs: [{ text: title }] }, badges } }] } },
    secondaryResults: { secondaryResults: { results: [] } },
  } },
});
const playerOf = (id, status, reason = "") =>
  ({ playabilityStatus: { status, ...(reason ? { reason } : {}) }, videoDetails: { videoId: id, title: "Video " + id, lengthSeconds: "200", author: "Ann" } });

const A = "aaaaaaaaaaa";
const B = "bbbbbbbbbbb";
const C = "ccccccccccc";

// The discovery pages (§1): channels, bylines, a Mix and a continuation.
const UC = (n) => ("UC" + n + "_".repeat(22)).slice(0, 24);
const byline = (channelId, name) => ({ runs: [{ text: name, navigationEndpoint: { browseEndpoint: { browseId: channelId } } }] });
// A search result by that channel; `views` counts its plays ("1.2M views").
const by = (videoId, title, channelId, name, views = "") =>
  video(videoId, title, { ownerText: byline(channelId, name), ...(views ? { viewCountText: { simpleText: views } } : {}) });
const panel = (videoId, title, channelId, name) =>
  ({ playlistPanelVideoRenderer: { videoId, title: { simpleText: title }, shortBylineText: byline(channelId, name), lengthText: { simpleText: "3:20" } } });
const mixOf = (items) => ({ contents: { twoColumnWatchNextResults: { playlist: { playlist: { contents: items } } } } });
const moreItem = (token) => ({ continuationItemRenderer: { continuationEndpoint: { continuationCommand: { token } } } });
const moreOf = (items) => ({ onResponseReceivedCommands: [{ appendContinuationItemsAction: { continuationItems: items } }] });
// The search filters: Top is Videos by views, New is this week and then this month.
const TOP = "EgQQARgF";
const WEEK = "EgYIAxABGAU=";
const MONTH = "EgYIBBABGAU=";
// Eleven-character video ids, distinct by `n`.
const vid = (n) => (n + "-".repeat(11)).slice(0, 11);

// One page, started on `route`: what it fetches is answered from `net`, what it calls in browse.py is logged.
// Frames run on settle(); the page's own timers fire on fire(ms); the list's end comes into view on reach().
// `search`, `browse` and `mix` answer those requests by body (an object, or a status number); `artists` is
// artists.json, missing when null; `storage` is the page's localStorage. With `resize`, the page has a
// ResizeObserver, whose callbacks run on resized().
function page({
  route = "?q=nocturne&f=videos&d=any&s=relevance&p=1", width = 1200, cookie = "", rows = [], player = null, next = null,
  search = null, browse = null, mix = null, artists = null, storage = new Map(), resize = false,
} = {}) {
  const doc = parseHtml(HTML);
  doc.cookie = cookie;
  const net = {
    offline: false, rows, player: player || ((id) => playerOf(id, "OK")), next: next || (() => ({})),
    search: search || (() => searchOf(net.rows)), browse: browse || (() => ({})), mix: mix || (() => mixOf([])), artists,
  };
  const calls = [];
  const requests = [];
  const replies = {}; // name -> (...args) => what that call answers; null otherwise, and a pick is taken
  let seq = 0;
  const api = new Proxy({}, {
    get: (_, name) => (...args) => {
      calls.push([name, ...args]);
      if (name === "pick" && !replies.pick) return Promise.resolve({ ok: true, seq: ++seq });
      return Promise.resolve(replies[name] ? replies[name](...args) : null);
    },
  });
  const json = (data) => new Response(JSON.stringify(data), { status: 200, headers: { "content-type": "application/json" } });
  const reply = (x) => (typeof x === "number" ? new Response(null, { status: x }) : json(x));
  const answer = (url, body, headers) => {
    if (url === P.api("search")) return reply(net.search(body));
    if (url === P.api("browse")) return reply(net.browse(body));
    if (url === P.api("player")) return json(net.player(body.videoId, headers));
    if (url === P.api("next")) return body.playlistId ? reply(net.mix(body)) : json(net.next(body.videoId));
    if (url === "/quartzmidi/converted.json") return json({});
    if (url === "/quartzmidi/artists.json") return net.artists ? json(net.artists) : new Response(null, { status: 404 });
    return new Response(null, { status: 404 });
  };
  const fetch = (url, opts = {}) => {
    const body = typeof opts.body === "string" ? JSON.parse(opts.body) : null;
    requests.push({ url, headers: opts.headers || {}, body });
    if (net.offline) return Promise.reject(new TypeError("Failed to fetch"));
    return Promise.resolve(answer(url, body, opts.headers || {}));
  };
  const frames = new Map();
  let frameId = 0;
  const timers = new Map();
  let timerId = 0;
  const store = storage;
  const g = { QMParse: P, pywebview: { api } };
  const resizes = [];
  if (resize) g.ResizeObserver = class { constructor(fn) { resizes.push(fn); } observe() {} };
  const listeners = {}; // the window's, by event type
  const window = { addEventListener: (type, fn) => (listeners[type] ||= []).push(fn), removeEventListener() {} };
  const win = (type) => (listeners[type] || []).forEach((fn) => fn({ type }));
  const { location, history } = fakeHistory(route, win);
  const localStorage = { getItem: (k) => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)) };
  const navigator = { language: "en-US", onLine: true, clipboard: { writeText: async () => {} } };
  const watched = { fn: null, nodes: [] }; // the page's IntersectionObserver
  class IntersectionObserver {
    constructor(fn) { watched.fn = fn; }
    observe(node) { watched.nodes.push(node); }
    disconnect() { watched.nodes = []; }
  }
  new Function("globalThis", "window", "document", "location", "history", "localStorage", "navigator", "innerWidth",
    "innerHeight", "requestAnimationFrame", "cancelAnimationFrame", "setTimeout", "clearTimeout", "IntersectionObserver",
    "getSelection", "Node", "fetch", SOURCE)(
    g, window, doc, location, history, localStorage, navigator, width, 800,
    (fn) => { frames.set(++frameId, fn); return frameId; }, (id) => frames.delete(id),
    (fn, ms) => { timers.set(++timerId, { fn, ms }); return timerId; }, (id) => timers.delete(id),
    IntersectionObserver, () => "", { TEXT_NODE: 3 }, fetch);
  const $ = (id) => doc.getElementById(id);
  return {
    doc, net, calls, requests, replies, $, storage, location, history,
    root: doc.documentElement,
    receive: (m) => g.qm.receive(m),
    // The window's listeners for an event of that type, as the browser fires it.
    win,
    resized: () => resizes.forEach((fn) => fn([])),
    // The list scrolls to its end: what the page watches comes into view when it is shown.
    reach() {
      if (watched.fn) watched.fn(watched.nodes.map((n) => ({ target: n, isIntersecting: shown(n) })));
    },
    // The search requests sent, as [query, params].
    searches: () => requests.filter((r) => r.url === P.api("search") && r.body && r.body.query).map((r) => [r.body.query, r.body.params || ""]),
    mixes: () => requests.filter((r) => r.url === P.api("next") && r.body && r.body.playlistId).map((r) => r.body.playlistId),
    // Promises settle, then the frames they asked for paint.
    async settle() {
      for (let i = 0; i < 12; i++) {
        await new Promise((r) => setTimeout(r, 0));
        for (const [id, fn] of [...frames]) { frames.delete(id); fn(); }
      }
    },
    // Fires, once, the page's timers that were set for `ms`, and says how many there were.
    fire(ms) {
      let fired = 0;
      for (const [id, t] of [...timers]) if (t.ms === ms) { timers.delete(id); t.fn(); fired++; }
      return fired;
    },
    row: (id) => $("rows").querySelectorAll(".row").find((n) => n.qmRow && n.qmRow.id === id),
    logged: (kind) => calls.filter((c) => c[0] === "log" && c[1] === kind).map((c) => c[2]),
    players: () => requests.filter((r) => r.url === P.api("player")),
  };
}

// A control as the player reads it: its words, or its name when it shows only an icon.
const words = (b) => b.textContent.trim() || b.getAttribute("aria-label") || "";
// A row's Convert and whether it has Remove.
const convertOf = (node) => [node.convert.hidden ? "" : words(node.convert), !!node.querySelector(".remove")];
const wordOf = (node) => (node.parts.word.hidden ? "" : node.parts.word.textContent);
const shown = (n) => { for (; n && n.nodeType === 1; n = n.parentNode) if (n.hidden) return false; return true; };

// ---- the queue (§3.8)

Deno.test("app: a removed selection no longer covers its rows, even when its key comes back", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano"), video(B, "Waltz piano"), video(C, "Etude piano")] });
  pg.receive({ type: "init", protocol: 2 });
  await pg.settle();
  input.press(pg.row(A).parts.check);
  input.press(pg.row(B).parts.check);
  input.press(pg.$("convert-ticked"));
  await pg.settle();
  const picks = pg.calls.filter((c) => c[0] === "pick");
  same(picks.map((c) => c[1].items.map((it) => it.id)), [[A, B]], "one selection of the ticked rows");
  pg.receive({ type: "item", key: "s:1", kind: "selection", state: "queued", total: 2, done: 0 });
  await pg.settle();
  same([A, B, C].map((id) => convertOf(pg.row(id))), [["Queued", true], ["Queued", true], ["Convert", false]], "queued");
  input.press(pg.row(A).querySelector(".remove"));
  same(pg.calls.filter((c) => c[0] === "drop"), [["drop", "v:" + A, "s:1"]], "Remove drops the row from its pick");
  pg.receive({ type: "item", key: "s:1", state: "removed" });
  await pg.settle();
  same([A, B].map((id) => convertOf(pg.row(id))), [["Convert", false], ["Convert", false]], "removed");
  pg.receive({ type: "item", key: "s:1", kind: "selection", state: "queued", total: 2, done: 0 });
  await pg.settle();
  same([A, B].map((id) => convertOf(pg.row(id))), [["Convert", false], ["Convert", false]], "queued again under that key");
});

Deno.test("app: a failed selection keeps its rows, so Retry sends them again", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano"), video(B, "Waltz piano")] });
  pg.receive({ type: "init", protocol: 2 });
  await pg.settle();
  input.press(pg.row(A).parts.check);
  input.press(pg.row(B).parts.check);
  input.press(pg.$("convert-ticked"));
  await pg.settle();
  pg.receive({ type: "item", key: "s:1", kind: "selection", state: "failed", reason: "No space left.", total: 2 });
  await pg.settle();
  input.press(pg.$("queue").querySelector("[aria-label=Retry]"));
  await pg.settle();
  const picks = pg.calls.filter((c) => c[0] === "pick");
  same(picks.map((c) => c[1].items.map((it) => it.id)), [[A, B], [A, B]], "sent again");
  same([A, B].map((id) => convertOf(pg.row(id))), [["Sent", false], ["Sent", false]], "the rows wait for the new pick");
});

// ---- members only (§1.8)

Deno.test("app: signed out, a members-only row is Not available, never prewarmed and never asked signed", async () => {
  const M = "mmmmmmmmmmm";
  const pg = page({ rows: [membersOnly(M, "Members nocturne piano"), video(A, "Nocturne piano")] });
  await pg.settle();
  const m = pg.row(M);
  same([wordOf(m), convertOf(m)[0], m.parts.badge.hidden], ["Not available", "", true], "the members row");
  same([wordOf(pg.row(A)), convertOf(pg.row(A))[0]], ["", "Convert"], "the public row");
  same(pg.$("count").textContent, "1", "only the public row counts");
  same(pg.$("dock-name").textContent, "Nocturne piano", "the dock prewarms the public row");
  // Played anyway: its word, no transport, and nothing loads.
  input.press(m.querySelector(".body"));
  await pg.settle();
  same([pg.$("dock-name").textContent, pg.$("frame-word").textContent, pg.$("transport").hidden], ["Members nocturne piano", "Not available", true], "in the dock");
  check(!pg.players().some((r) => r.body.videoId === M), "the player endpoint was asked about it");
  check(!pg.$("player").src.includes(M), "the frame loaded it: " + pg.$("player").src);
});

Deno.test("app: signed in, a signed player call says which members-only rows play", async () => {
  const [M1, M2] = ["mmmmmmmmmm1", "mmmmmmmmmm2"];
  const pg = page({
    cookie: "SID=x; SAPISID=abc/def",
    rows: [membersOnly(M1, "Lesson 1 piano"), membersOnly(M2, "Lesson 2 piano"), video(A, "Nocturne piano")],
    player: (id, headers) => (id === M1 && headers.Authorization ? playerOf(id, "UNPLAYABLE", "Join this channel to get access") : playerOf(id, "OK")),
  });
  pg.receive({ type: "init", protocol: 2, signedIn: true });
  await pg.settle();
  await pg.settle();
  const signed = pg.players().filter((r) => /^SAPISIDHASH \d+_[0-9a-f]{40}$/.test(r.headers.Authorization || ""));
  same(signed.map((r) => r.body.videoId), [M1, M2], "each members-only row asked signed, in turn");
  same([wordOf(pg.row(M1)), convertOf(pg.row(M1))[0]], ["Not available", ""], "not a member of the first");
  same([wordOf(pg.row(M2)), convertOf(pg.row(M2))[0]], ["", "Convert"], "a member of the second");
  same(pg.$("count").textContent, "1", "members-only rows never count");
  pg.receive({ type: "signed", in: false });
  await pg.settle();
  same(wordOf(pg.row(M2)), "Not available", "signed out again");
});

Deno.test("app: each sign-in asks again, and another account's answer replaces the last one's", async () => {
  const [M1, M2] = ["mmmmmmmmmm1", "mmmmmmmmmm2"];
  let member = M2;
  const pg = page({
    cookie: "SID=x; SAPISID=abc/def",
    rows: [membersOnly(M1, "Lesson 1 piano"), membersOnly(M2, "Lesson 2 piano"), video(A, "Nocturne piano")],
    player: (id, headers) => (id !== member && headers.Authorization ? playerOf(id, "UNPLAYABLE", "Join this channel to get access") : playerOf(id, "OK")),
  });
  await pg.settle();
  same([M1, M2].map((id) => wordOf(pg.row(id))), ["Not available", "Not available"], "signed out");
  pg.receive({ type: "signed", in: true });
  await pg.settle();
  await pg.settle();
  same([M1, M2].map((id) => wordOf(pg.row(id))), ["Not available", ""], "a member of the second");
  pg.receive({ type: "signed", in: false });
  member = M1;
  pg.receive({ type: "signed", in: true });
  await pg.settle();
  await pg.settle();
  same([M1, M2].map((id) => wordOf(pg.row(id))), ["", "Not available"], "another account, a member of the first");
});

Deno.test("app: signed in, an unsigned answer about a members-only video changes none of its rows", async () => {
  const M = "mmmmmmmmmmm";
  const pg = page({
    cookie: "SID=x; SAPISID=abc/def",
    rows: [membersOnly(M, "Lesson piano"), video(A, "Nocturne piano")],
    player: (id, headers) => (headers.Authorization ? playerOf(id, "OK") : playerOf(id, "UNPLAYABLE", "Join this channel to get access")),
    next: (id) => nextOf(id, "Lesson piano", badged("BADGE_STYLE_TYPE_MEMBERS_ONLY", "Members only").badges),
  });
  pg.receive({ type: "init", protocol: 2, signedIn: true });
  await pg.settle();
  await pg.settle();
  const listed = pg.row(M);
  same(wordOf(listed), "", "a member");
  // Its own page asks the player endpoint unsigned first, as for any video.
  pg.$("q").value = "https://www.youtube.com/watch?v=" + M;
  pg.$("search").dispatchEvent(new FakeEvent("submit"));
  await pg.settle();
  await pg.settle();
  check(pg.players().some((r) => r.body.videoId === M && !r.headers.Authorization), "never asked unsigned");
  same([wordOf(listed), wordOf(pg.row(M))], ["", ""], "the listed row and the page's own");
  same([pg.$("frame-word").hidden, pg.$("transport").hidden], [true, false], "the dock plays it");
  check(pg.$("player").src.includes(M), "the frame holds " + pg.$("player").src);
});

// ---- the dock (§2.9)

Deno.test("app: an age-restricted video has no transport, wide and narrow, and keeps Sign in and Mix", async () => {
  for (const width of [1200, 640]) {
    const id = "HtVdAasjOgU";
    const pg = page({ route: "?v=" + id, width, player: (v) => playerOf(v, "LOGIN_REQUIRED", "Sign in to confirm your age.") });
    await pg.settle();
    const where = width < 900 ? "narrow" : "wide";
    same(pg.root.classList.contains("narrow"), width < 900, where);
    same([pg.$("dock-name").textContent, pg.$("frame-word").textContent], ["Video " + id, "Age-restricted"], where + " dock");
    same(pg.$("transport").hidden, true, where + ": the transport shows");
    same([shown(pg.$("dock-convert")), words(pg.$("dock-convert"))], [true, "Sign in"], where + " Convert");
    same([shown(pg.$("mix")), words(pg.$("mix"))], [true, "Mix"], where + " Mix");
    check(!pg.$("player").src, where + ": the frame loaded " + pg.$("player").src);
  }
});

Deno.test("app: a dock that turns to a video that cannot play shrinks, closes Volume and hides the transport", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano"), live(B, "Live piano")] });
  await pg.settle();
  same([pg.$("dock-name").textContent, pg.$("transport").hidden], ["Nocturne piano", false], "prewarmed");
  input.press(pg.$("enlarge"));
  input.press(pg.$("volume"));
  await pg.settle();
  same([pg.root.classList.contains("enlarged"), !!pg.doc.body.querySelector(".popover")], [true, true], "enlarged, Volume open");
  // The keyboard brings the live row in, so no press outside closes anything first.
  input.tabTo(pg.row(B));
  input.key(pg.doc, "Enter");
  await pg.settle();
  same([pg.$("dock-name").textContent, pg.$("frame-word").textContent], ["Live piano", "Live"], "the live row");
  same([pg.$("transport").hidden, pg.root.classList.contains("enlarged"), !!pg.doc.body.querySelector(".popover")],
    [true, false, false], "transport hidden, shrunk, Volume closed");
  same([shown(pg.$("mix")), pg.$("dock-convert").hidden], [true, true], "Mix stays; a live video has no Convert");
  input.tabTo(pg.row(A));
  input.key(pg.doc, "Enter");
  await pg.settle();
  same([pg.$("dock-name").textContent, pg.$("transport").hidden], ["Nocturne piano", false], "a playable row brings it back");
});

Deno.test("app: a ?v= route cues its video in the frame once", async () => {
  const pg = page({ route: "?v=" + A, next: (id) => nextOf(id, "Nocturne piano") });
  const frame = pg.$("player");
  const sources = [];
  Object.defineProperty(frame, "src", {
    get() { return this.getAttribute("src") || ""; },
    set(value) { sources.push(value); this.setAttribute("src", value); },
  });
  await pg.settle();
  same(pg.$("dock-name").textContent, "Nocturne piano", "the dock holds the route's video");
  same(sources, ["/watch?v=" + A + "&qmhold=1"], "the frame's sources");
  input.press(pg.$("play"));
  await pg.settle();
  same(sources.at(-1), "/watch?v=" + A + "&t=0s", "Play loads it");
});

Deno.test("app: once the ad is over the dock shows loading, while YouTube keeps the video unstarted", async () => {
  const pg = page({ route: "?v=" + A, next: (id) => nextOf(id, "Nocturne piano") });
  await pg.settle();
  // The frame's #movie_player, an ad showing over the held video.
  const classes = new Set(["ad-showing"]);
  const yt = {
    state: -1,
    classList: { contains: (c) => classes.has(c) },
    ownerDocument: { addEventListener() {} },
    querySelector: () => null,
    getPlayerState() { return this.state; },
    getVideoData: () => ({ video_id: A }),
    getPlayerResponse: () => null,
    getCurrentTime: () => 0,
    getDuration: () => 213,
    setVolume() {}, mute() {}, unMute() {}, playVideo() {}, pauseVideo() {}, seekTo() {}, loadVideoById() {},
  };
  Object.assign(pg.$("player"), { contentWindow: {}, contentDocument: { getElementById: (id) => (id === "movie_player" ? yt : null) } });
  input.press(pg.$("play"));
  await pg.settle();
  const poll = async () => {
    pg.fire(250);
    await pg.settle();
  };
  await poll();
  same(pg.$("ad").hidden, false, "the ad shows");
  classes.delete("ad-showing");
  await poll();
  same([pg.$("ad").hidden, words(pg.$("play"))], [true, "Pause"], "the ad is over and the video is loading");
  yt.state = 1;
  await poll();
  same([pg.$("ad").hidden, words(pg.$("play"))], [true, "Pause"], "playing");
});

Deno.test("app: a frame YouTube loads again on its own is let go again, paused only if the user paused it", async () => {
  const pg = page({ route: "?v=" + A, next: (id) => nextOf(id, "Nocturne piano") });
  await pg.settle();
  const frame = pg.$("player");
  // Each load is a new window and #movie_player; frame.js holds it, since the prewarm's address has qmhold=1.
  const load = () => {
    const yt = {
      state: -1,
      plays: 0,
      classList: { contains: () => false },
      ownerDocument: { addEventListener() {} },
      querySelector: () => null,
      getPlayerState() { return this.state; },
      getVideoData: () => ({ video_id: A }),
      getPlayerResponse: () => null,
      getCurrentTime: () => 0,
      getDuration: () => 213,
      setVolume() {}, mute() {}, unMute() {}, pauseVideo() {}, seekTo() {}, loadVideoById() {}, cueVideoById() {},
      playVideo() { this.plays++; },
    };
    const win = { __qmHold: true, __qmUserPaused: false };
    Object.assign(frame, { contentWindow: win, contentDocument: { getElementById: (id) => (id === "movie_player" ? yt : null) } });
    return { yt, win };
  };
  const poll = async () => {
    pg.fire(250);
    await pg.settle();
  };
  let { yt, win } = load();
  input.press(pg.$("play"));
  await pg.settle();
  same([win.__qmHold, yt.plays], [false, 1], "Play lets the prewarmed video go");
  ({ yt, win } = load());
  await poll();
  same([win.__qmHold, win.__qmUserPaused, yt.plays], [false, false, 1], "loaded again while loading: let go and played");
  yt.state = 1;
  await poll();
  same(words(pg.$("play")), "Pause", "playing");

  input.press(pg.$("play"));
  await pg.settle();
  ({ yt, win } = load());
  input.press(pg.$("play"));
  await pg.settle();
  await poll();
  same([win.__qmHold, win.__qmUserPaused, yt.plays], [false, false, 2], "loaded again while paused, then Play: let go and played");
  yt.state = 1;
  await poll();

  yt.state = 0;
  await poll();
  same(words(pg.$("play")), "Play", "ended");
  ({ yt, win } = load());
  await poll();
  same([win.__qmHold, win.__qmUserPaused, yt.plays], [false, true, 0], "loaded again once ended: let go, paused");
});

Deno.test("app: Skip is pressed once per ad, after Play, at its point through the frame's box", async () => {
  const pg = page({ route: "?v=" + A, next: (id) => nextOf(id, "Nocturne piano") });
  await pg.settle();
  const classes = new Set(["ad-showing"]);
  const yt = {
    state: -1,
    classList: { contains: (c) => classes.has(c) },
    ownerDocument: { addEventListener() {} },
    querySelector: () => null,
    getPlayerState() { return this.state; },
    getVideoData: () => ({ video_id: A }),
    getPlayerResponse: () => null,
    getCurrentTime: () => 0,
    getDuration: () => 213,
    setVolume() {}, mute() {}, unMute() {}, pauseVideo() {}, seekTo() {}, loadVideoById() {},
    plays: 0,
    playVideo() { this.plays++; },
  };
  // The frame is 383 x 215 px inside, shown at twice that size; Skip is at (318.5, 127) in it.
  let skip = { ad: "ad1", x: 318.5, y: 127, width: 383, height: 215 };
  let looked = 0;
  // __qmClicks: the trusted clicks frame.js saw reach Skip.
  const win = { __qmHold: false, __qmUserPaused: false, __qmClicks: 0, __qmSkip: () => { looked++; return skip; } };
  const frame = pg.$("player");
  Object.assign(frame, { contentWindow: win, contentDocument: { getElementById: (id) => (id === "movie_player" ? yt : null) } });
  frame.getBoundingClientRect = () => ({ left: 800, top: 100, width: 766, height: 430, right: 1566, bottom: 530, x: 800, y: 100 });
  let hit = frame;
  const asked = []; // [x, y, the frame's pointer-events then]
  pg.doc.elementFromPoint = (x, y) => {
    asked.push([x, y, frame.style.pointerEvents]);
    if (hit === frame) frame.focus(); // as if the press put focus in the frame
    return hit;
  };
  // browse.py's answer to each press: true when it pressed; false, or null with no such call, when it did not.
  // A press it made reaches Skip unless the layout moved after its last look.
  let answer = true;
  let lands = true;
  pg.replies.press = () => {
    const said = typeof answer === "function" ? answer() : answer;
    if (said === true && lands) win.__qmClicks++;
    return said;
  };
  const presses = () => pg.calls.filter((c) => c[0] === "press").map((c) => c.slice(1));
  const count = () => presses().length;
  const poll = async () => {
    const fired = pg.fire(250);
    await pg.settle();
    return fired;
  };
  // Held for prewarm, before Play, nothing polls the frame, so Skip is not even looked for.
  same([await poll(), presses(), looked], [0, [], 0], "held for prewarm, before Play");
  pg.$("q").focus();
  input.press(pg.$("play"));
  await pg.settle();
  pg.$("q").focus();
  await poll();
  same(presses(), [[1437, 354]], "pressed at Skip's point in the page");
  same(asked, [[1437, 354, "auto"]], "the frame takes pointer input for the press");
  same([frame.style.pointerEvents, pg.doc.activeElement === pg.$("q")], ["none", true], "and only for it; focus stays where it was");
  await poll();
  await poll();
  same(count(), 1, "once per ad");

  skip = { ...skip, ad: "ad2" };
  hit = pg.$("ad");
  await poll();
  same(count(), 1, "never through our own overlay");
  hit = frame;
  await poll();
  same(count(), 2, "the next ad's Skip, once it is the frame's");

  // A refused press is asked again on the next poll, until browse.py presses it.
  skip = { ...skip, ad: "ad3" };
  answer = false;
  await poll();
  await poll();
  same(count(), 4, "refused, then asked again");
  answer = () => { win.__qmClicks++; return null; }; // a click reached Skip, but browse.py's answer is lost
  await poll();
  same(count(), 5, "no answer counts as refused, whatever frame.js saw");
  answer = true;
  await poll();
  await poll();
  same(count(), 6, "pressed at last, and then no more");
  same([frame.style.pointerEvents, pg.doc.activeElement === pg.$("q")], ["none", true], "after every ask");

  // A press the layout sent elsewhere: browse.py pressed, but no click reached Skip, so it is asked again.
  skip = { ...skip, ad: "ad3b" };
  lands = false;
  await poll();
  await poll();
  same(count(), 8, "pressed off Skip, then asked again");
  lands = true;
  await poll();
  await poll();
  same(count(), 9, "the click reached Skip, and then no more");

  // Two skippable ads back to back: each is pressed, on the first poll that shows it.
  skip = { ...skip, ad: "ad4" };
  await poll();
  skip = { ...skip, ad: "ad5" };
  await poll();
  same(count(), 11, "the second ad of the pod right after the first");

  // One press at a time: none is asked while browse.py still works on the last.
  let finish = null;
  answer = () => new Promise((resolve) => { finish = resolve; });
  skip = { ...skip, ad: "ad6" };
  await poll();
  same([count(), frame.style.pointerEvents], [12, "auto"], "a press under way");
  await poll();
  same(count(), 12, "not asked again while it is under way");
  // A press that lands on the page instead, as when the layout moves after browse.py's last look, does nothing
  // there: while it is under way, the trusted press events at its point are swallowed, and only those.
  const point = { clientX: 1437, clientY: 354 };
  const [shown, focused] = [words(pg.$("play")), pg.doc.activeElement];
  input.press(pg.$("play"), point);
  await pg.settle();
  same([words(pg.$("play")), pg.doc.activeElement === focused], [shown, true], "a press at the point does nothing");
  const got = [];
  for (const type of ["pointerdown", "mousedown", "pointerup", "mouseup", "click"]) {
    pg.doc.body.addEventListener(type, () => got.push(type));
  }
  input.press(pg.doc.body, point);
  same(got, [], "none of its events reaches the page");
  input.press(pg.doc.body, { clientX: 1439, clientY: 354 });
  pg.doc.body.dispatchEvent(new FakeEvent("click", point));
  same(got, ["pointerdown", "mousedown", "pointerup", "mouseup", "click", "click"], "a press elsewhere, and a scripted click");
  win.__qmClicks++; // its click reached Skip
  finish(true);
  await pg.settle();
  input.press(pg.doc.body, point);
  same(got.length, 11, "once the press is done");
  await poll();
  same([count(), frame.style.pointerEvents], [12, "none"], "pressed, and the frame takes no pointer input");

  // A frame YouTube loaded again holds, from qmhold=1 in its address, though Play let the video go: the page lets
  // it go again before it looks for Skip.
  answer = true;
  skip = { ...skip, ad: "ad7" };
  win.__qmHold = true;
  const plays = yt.plays;
  await poll();
  same([win.__qmHold, yt.plays - plays, count()], [false, 1, 13], "let go, played, then pressed");

  skip = { ...skip, ad: "ad8" };
  win.__qmUserPaused = true;
  await poll();
  same(count(), 13, "never while the frame holds a pause");
  win.__qmUserPaused = false;
  same(words(pg.$("play")), "Pause", "the ad plays");
  input.press(pg.$("play"));
  await pg.settle();
  win.__qmUserPaused = false; // as if the frame had not seen the pause yet
  await poll();
  same([words(pg.$("play")), count()], ["Play", 13], "never once the user paused");
  input.press(pg.$("play"));
  await pg.settle();
  await poll();
  same(count(), 14, "played again, the ad's Skip");
  skip = null;
  await poll();
  same(count(), 14, "never without a Skip to press");
});

Deno.test("app: the frame never keeps focus: it goes back to what had it, or out of the frame", async () => {
  const pg = page({ route: "?v=" + A, next: (id) => nextOf(id, "Nocturne piano") });
  await pg.settle();
  const frame = pg.$("player");
  const q = pg.$("q");
  const active = () => (pg.doc.activeElement === frame ? "frame" : pg.doc.activeElement === q ? "q" : pg.doc.activeElement === pg.doc.body ? "body" : "other");
  // Focus gets into the frame, past frame.js: the page's window blurs, and its focus is the frame.
  const taken = () => {
    frame.focus();
    pg.win("blur");
    pg.fire(0);
    return active();
  };
  same(taken(), "body", "nothing had focus: it leaves the frame");
  q.focus();
  same(taken(), "q", "back to the search field");
  q.blur(); // a press on the page's background
  pg.fire(0);
  same(taken(), "body", "not to the field the user left");
  q.focus();
  q.dispatchEvent(new FakeEvent("focusout")); // the window loses the system's focus: the field stays the page's focus
  pg.fire(0);
  same(taken(), "q", "the field the window's own blur left");
  const row = pg.doc.createElement("button"); // a row's button, as a new list replaces it
  pg.$("rows").append(row);
  row.focus();
  row.remove();
  same(taken(), "body", "not to an element that has gone");
  q.focus();
  pg.win("blur"); // the window loses the system's focus with the field focused: nothing moves
  pg.fire(0);
  same(active(), "q", "the window's own blur");
  // With the window in the background, the frame takes focus with no blur here; frame.js says when it gets it.
  frame.focus();
  pg.fire(0);
  same(active(), "frame", "no blur in the page");
  frame.dispatchEvent(new FakeEvent("qm-focus", { bubbles: false }));
  pg.fire(0);
  same(active(), "q", "back once the frame says it has focus");
});

// ---- YouTube (§1.8, §2.9)

Deno.test("app: a missing player signature timestamp is logged as drift in those words", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano")] });
  await pg.settle();
  same(pg.logged("drift").filter((t) => /signature/.test(t)), ["player signature timestamp not found"], "drift");
});

Deno.test("app: a request that never reaches YouTube is No connection, though the browser says it is online", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano")] });
  pg.net.offline = true;
  await pg.settle();
  const banners = pg.$("banners");
  same([banners.hidden, banners.querySelector("span").textContent], [false, "No connection."], "the banner");
  const retry = banners.querySelector("button");
  same(words(retry), "Retry", "its button");
  pg.net.offline = false;
  input.press(retry);
  await pg.settle();
  same([banners.hidden, !!pg.row(A)], [true, true], "Retry reads the results");
});

// ---- keys (§3.13)

Deno.test("app: Space plays after a click on a button, and presses a button the keyboard reached", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano")] });
  await pg.settle();
  const piano = pg.$("piano");
  check(pg.$("player").src.includes("qmhold=1"), "prewarmed: " + pg.$("player").src);
  input.press(piano);
  await pg.settle();
  same(piano.getAttribute("aria-pressed"), "false", "the click turns Piano off");
  input.key(pg.doc, " ");
  await pg.settle();
  same(pg.$("player").src, "/watch?v=" + A + "&t=0s", "Space plays the dock");
  same(piano.getAttribute("aria-pressed"), "false", "Space leaves the clicked Piano alone");
  input.tabTo(pg.$("signin"));
  input.tabTo(piano);
  input.key(pg.doc, " ");
  await pg.settle();
  same(piano.getAttribute("aria-pressed"), "true", "Space presses Piano reached by Tab");
  same(pg.$("player").src, "/watch?v=" + A + "&t=0s", "and the playing video is left alone");
});

// ---- discovery (§1, §2.4, §3.3-3.4)

const ANN = UC("a");
const PIANISTS = [[ANN, "Ann"], [UC("b"), "Ben"], [UC("c"), "Cy"], [UC("d"), "Dee"], [UC("e"), "Eve"]];
// `n` recordings by `channels` in turn, their ids from `tag`.
const recordings = (tag, n, channels) => Array.from({ length: n }, (_, i) => {
  const [id, name] = channels[i % channels.length];
  return by(vid(tag + i), "Piece " + tag + i + " piano", id, name);
});
// Search answers by query and filter, and continuations by token; anything else is an empty page.
const answers = (table) => (body) =>
  table[body.continuation ? body.continuation : body.query + " " + (body.params || "")] || searchOf([]);
const channelPage = (id, name, items) => ({
  metadata: { channelMetadataRenderer: { title: name, externalId: id } },
  contents: { twoColumnBrowseResultsRenderer: { tabs: [{ tabRenderer: { selected: true, content: { richGridRenderer: {
    contents: items.map((content) => ({ richItemRenderer: { content } })),
  } } } }] } },
});
const heads = (pg) => pg.$("rows").querySelectorAll(".shelf").filter(shown).map((b) => b.textContent.trim());
const rowsOf = (pg) => pg.$("rows").querySelectorAll(".row").filter(shown).map((n) => n.qmRow.id);
const cardsOf = (pg) => pg.$("strip").querySelectorAll(".card").map((b) => b.querySelector(".name").textContent);
// Home's curated cards (§3.19): in turn from the genres of the conversions' titles, else from every genre.
const curatedNames = (titles = [], skip = []) => P.homeCurated(titles, skip, 12).map((p) => p.name);
const COLD = {
  ["classical piano " + WEEK]: searchOf(recordings("cl", 6, PIANISTS.slice(0, 2))),
  ["jazz piano solo " + WEEK]: searchOf(recordings("jz", 6, PIANISTS.slice(2, 3))),
  ["ragtime piano " + WEEK]: searchOf(recordings("rg", 6, PIANISTS.slice(3, 4))),
  ["jazz piano solo " + TOP]: searchOf(recordings("jt", 6, PIANISTS.slice(1, 4))),
};

Deno.test("app: Home cold starts with two New shelves, and the next when the list's end shows", async () => {
  const pg = page({ route: "", search: answers(COLD) });
  await pg.settle();
  same(pg.searches(), [["classical piano", WEEK], ["jazz piano solo", WEEK]], "two New searches");
  same(heads(pg), ["Classical", "Jazz"], "a header per shelf");
  same(rowsOf(pg), [...recordings("cl", 6, PIANISTS), ...recordings("jz", 6, PIANISTS)].map((r) => r.videoRenderer.videoId), "their rows in order");
  same([pg.$("genres").hidden, pg.$("filters").hidden], [false, true], "the genre row replaces the filters");
  same([pg.$("home").disabled, pg.$("genre-sort").disabled, pg.$("piano").disabled], [true, true, true], "Home, Sort and Piano are off");
  same(pg.$("pills").querySelectorAll("[aria-checked=true]").length, 0, "no genre is picked");
  same(cardsOf(pg), [...curatedNames(), "Cy", "Ann", "Ben"], "the strip");
  pg.reach();
  await pg.settle();
  same(pg.searches().at(-1), ["ragtime piano", WEEK], "the end of the list loads the next shelf");
  same(heads(pg), ["Classical", "Jazz", "Ragtime"], "below the others");
  same(cardsOf(pg), [...curatedNames(), "Cy", "Ann", "Ben"], "the strip stays as it was");
});

Deno.test("app: Home's strip counts each shelf's rows against that shelf's own query", async () => {
  const pianists = P.pianists;
  const asked = [];
  P.pianists = (rows, query, ...rest) => {
    asked.push(rows.map((r) => [r.id.slice(0, 2), typeof query === "function" ? query(r) : "all: " + query]));
    return pianists(rows, query, ...rest);
  };
  try {
    const pg = page({ route: "", search: answers(COLD) });
    await pg.settle();
    same(cardsOf(pg), [...curatedNames(), "Cy", "Ann", "Ben"], "the strip");
    const each = new Map(asked.at(-1));
    same([...each], [["cl", "classical piano"], ["jz", "jazz piano solo"]], "each row with its shelf's words");
  } finally {
    P.pianists = pianists;
  }
});

Deno.test("app: a limit stops Home's shelves until Retry, which loads the failed one in its place", async () => {
  let limited = true;
  const pg = page({ route: "", search: (body) => (limited && body.query === "classical piano" ? 429 : answers(COLD)(body)) });
  await pg.settle();
  same(heads(pg), ["Jazz"], "the shelf that answered");
  const banners = pg.$("banners");
  same(banners.querySelector("span").textContent, "YouTube is limiting requests.", "the banner");
  const asked = pg.searches().length;
  pg.reach();
  await pg.settle();
  same(pg.searches().length, asked, "the end of the list asks nothing more");
  limited = false;
  input.press(banners.querySelector("button"));
  await pg.settle();
  same(heads(pg), ["Classical", "Jazz"], "Retry loads Classical above Jazz");
  same(rowsOf(pg)[0], vid("cl0"), "and the list starts with it");
  same(cardsOf(pg), [...curatedNames(), "Cy", "Ann", "Ben"], "the strip waits for both");
});

Deno.test("app: Home with a conversion asks its Mix and its piece once, a row per other pianist, yours first in the strip", async () => {
  const [ben, cy, dee] = [UC("b"), UC("c"), UC("d")];
  const artists = { artists: [{ id: ANN, name: "Ann", n: 1, at: "2026-10-01T10:00:00Z" }], seeds: [{ id: A, title: "Clair de Lune (Debussy) piano", channelId: ANN }] };
  const pg = page({
    route: "", artists,
    mix: () => mixOf([panel(A, "Clair de Lune", ANN, "Ann"), panel(vid("m1"), "Clair de Lune", ben, "Ben"), panel(vid("m2"), "Reverie", cy, "Cy")]),
    search: answers({ ["Clair de Lune piano " + TOP]: searchOf([by(vid("s1"), "Clair de Lune piano", dee, "Dee"), by(vid("s2"), "Clair de Lune", ANN, "Ann")]) }),
  });
  await pg.settle();
  same(pg.mixes(), ["RD" + A], "one Mix");
  same(pg.searches(), [["Clair de Lune piano", TOP]], "one search for the piece");
  same(heads(pg), ["Clair de Lune (Debussy) piano"], "the shelf");
  same(rowsOf(pg), [vid("s1"), vid("m1"), vid("m2")], "a row per other pianist");
  same(cardsOf(pg), ["Ann", ...curatedNames(["Clair de Lune (Debussy) piano"], [ANN]), "Dee", "Ben", "Cy"], "yours, the curated pianists of its genre, then the ones found");
  same(pg.$("strip").querySelectorAll(".divider").length, 1, "set apart");
  pg.reach();
  await pg.settle();
  same(pg.searches()[1], ["classical piano", WEEK], "the genres follow");
});

Deno.test("app: a conversion without a channel seeds Home, and the channel its Mix names becomes yours", async () => {
  const pg = page({
    route: "", artists: { artists: [], seeds: [{ id: A, title: "Clair de Lune", channelId: null }] },
    mix: () => mixOf([panel(A, "Clair de Lune", ANN, "Ann"), panel(vid("m1"), "Clair de Lune", UC("b"), "Ben")]),
  });
  await pg.settle();
  same(rowsOf(pg), [vid("m1")], "the Mix's other pianist");
  const saved = JSON.parse(pg.storage.get("qm-artists") || "{}");
  same([Object.keys(saved), saved[ANN] && saved[ANN].name], [[ANN], "Ann"], "Ann is saved as yours");
});

Deno.test("app: a pill opens its genre on Top, Back returns Home as it was, and Enter on an empty field or Alt+Home goes Home", async () => {
  const pg = page({ route: "", search: answers(COLD) });
  await pg.settle();
  const jazz = pg.$("pills").querySelectorAll(".pill").find((b) => b.dataset.genre === "jazz");
  same(words(jazz), "Jazz", "a word-only pill");
  input.press(jazz);
  await pg.settle();
  same(pg.location.search, "?g=jazz", "the genre's route");
  same(pg.searches().at(-1), ["jazz piano solo", TOP], "Top first");
  same([jazz.getAttribute("aria-checked"), pg.$("home").disabled, pg.$("genre-sort").disabled, pg.$("piano").disabled],
    ["true", false, false, true], "the pill is picked, Home and Sort are on, Piano stays off");
  same(rowsOf(pg).length, 6, "its rows");
  const asked = pg.requests.length;
  input.press(pg.$("back"));
  await pg.settle();
  same([pg.location.search, heads(pg), pg.requests.length], ["", ["Classical", "Jazz"], asked], "Back shows Home without asking again");
  input.press(pg.$("forward"));
  await pg.settle();
  pg.$("q").value = "";
  pg.$("search").dispatchEvent(new FakeEvent("submit"));
  await pg.settle();
  same([pg.location.search, heads(pg)], ["", ["Classical", "Jazz"]], "Enter on an empty field opens Home");
  same(pg.requests.length, asked, "from the pages kept");
  input.press(jazz);
  await pg.settle();
  input.tabTo(pg.$("rows").querySelector(".row"));
  input.key(pg.doc, "Home", { altKey: true });
  await pg.settle();
  same(pg.location.search, "", "Alt+Home opens Home");
});

Deno.test("app: New falls back to this month when this week has under six recordings", async () => {
  const pg = page({
    route: "?g=film&s=new",
    search: answers({
      ["film music piano solo " + WEEK]: searchOf(recordings("fw", 3, PIANISTS)),
      ["film music piano solo " + MONTH]: searchOf(recordings("fm", 8, PIANISTS)),
    }),
  });
  await pg.settle();
  same(pg.searches(), [["film music piano solo", WEEK], ["film music piano solo", MONTH]], "the week, then the month");
  same(rowsOf(pg), recordings("fm", 8, PIANISTS).map((r) => r.videoRenderer.videoId), "the month's rows");
  same(pg.$("genre-sort").classList.contains("active"), true, "Sort shows New");
});

Deno.test("app: the strip needs three pianists, keeps its cards as pages arrive, and is one tab stop moved by arrows", async () => {
  const [ann, ben, cy] = PIANISTS;
  // A page of `first`, then on scrolling a page of `then`.
  const paged = (first, then) => (body) => (body.continuation ? moreOf(then) : searchOf([...first, moreItem("more")]));
  const two = page({ search: paged(recordings("p1", 4, [ann, ben]), recordings("p2", 4, PIANISTS.slice(2))) });
  await two.settle();
  same(two.$("strip").hidden, true, "no strip for two");
  two.reach();
  await two.settle();
  same([rowsOf(two).length, two.$("strip").hidden], [8, true], "nor after a page with two more");

  const pg = page({ search: paged(recordings("p1", 3, [ann, ben, cy]), recordings("p2", 6, PIANISTS.slice(3))) });
  await pg.settle();
  same([pg.$("strip").hidden, cardsOf(pg)], [false, ["Ann", "Ben", "Cy"]], "three cards");
  pg.reach();
  await pg.settle();
  same([rowsOf(pg).length, cardsOf(pg)], [9, ["Ann", "Ben", "Cy"]], "the same three after the next page");
  const cards = pg.$("strip").querySelectorAll(".card");
  same(cards.map((b) => b.tabIndex), [0, -1, -1], "one tab stop");
  const steps = [["ArrowRight", 1], ["End", 2], ["ArrowRight", 2], ["Home", 0], ["ArrowLeft", 0]];
  input.tabTo(cards[0]);
  for (const [key, to] of steps) {
    input.key(pg.doc, key);
    same([cards.indexOf(pg.doc.activeElement), cards.map((b) => b.tabIndex).indexOf(0)], [to, to], key);
  }
});

Deno.test("app: a pianist of yours opens on Latest and a new one on Popular, from the strip or the dock", async () => {
  const eve = UC("e");
  const storage = new Map([["qm-artists", JSON.stringify({ [eve]: { name: "Eve", at: "2026-10-01T10:00:00Z" } })]]);
  const pg = page({ storage, rows: [by(A, "Nocturne piano", eve, "Eve"), by(B, "Waltz piano", UC("b"), "Ben"), by(C, "Etude piano", UC("c"), "Cy")] });
  await pg.settle();
  same(cardsOf(pg), ["Eve", "Ben", "Cy"], "the strip");
  const dock = pg.$("dock-channel");
  same([dock.hidden, dock.textContent.trim()], [false, "Eve"], "the dock names the channel");
  input.press(dock);
  await pg.settle();
  same(pg.location.search, "?channel=" + eve, "yours on Latest");
  pg.history.back();
  await pg.settle();
  input.press(pg.$("strip").querySelectorAll(".card")[1]);
  await pg.settle();
  same(pg.location.search, "?channel=" + UC("b") + "&s=popular", "a new one on Popular");
});

Deno.test("app: a pick sent of one pianist's videos makes them yours at once; several pianists' or a refused one does not", async () => {
  const [fay, gus, hal] = [UC("f"), UC("g"), UC("h")];
  const D = vid("d1");
  // Ann's row carries her channel's picture, as YouTube's search rows do.
  const face = "https://yt3.ggpht.com/ann=s68";
  const ann = by(A, "Nocturne piano", ANN, "Ann");
  ann.videoRenderer.channelThumbnailSupportedRenderers = { channelThumbnailWithLinkRenderer: { thumbnail: { thumbnails: [{ url: face }] } } };
  const rows = [ann, by(B, "Waltz piano", fay, "Fay"), by(C, "Etude piano", gus, "Gus"), by(D, "Ballade piano", hal, "Hal")];
  const pg = page({ rows, search: (body) => (body.query.startsWith("nocturne") ? searchOf(rows) : answers(COLD)(body)) });
  pg.receive({ type: "init", protocol: 2, folders: [] });
  await pg.settle();
  const yours = () => JSON.parse(pg.storage.get("qm-artists") || "{}");
  input.press(pg.row(A).convert);
  await pg.settle();
  same([Object.keys(yours()), yours()[ANN].name, yours()[ANN].avatar, !!Date.parse(yours()[ANN].at), "dest" in yours()[ANN]],
    [[ANN], "Ann", face, true, false], "Ann is yours, with the picture her row brought and no folder of her own");
  input.press(pg.row(B).parts.check);
  input.press(pg.row(C).parts.check);
  await pg.settle();
  input.press(pg.$("convert-ticked"));
  await pg.settle();
  same(picksOf(pg).at(-1).items.map((it) => it.id), [B, C], "a selection of two pianists was sent");
  pg.replies.pick = () => ({ ok: false, seq: null });
  input.press(pg.row(D).convert);
  await pg.settle();
  same(Object.keys(yours()), [ANN], "neither of those, nor a pick not taken");
  input.press(pg.$("home"));
  await pg.settle();
  same([cardsOf(pg), pg.$("strip").querySelectorAll(".divider").length], [["Ann", ...curatedNames([], [ANN]), "Cy", "Ben"], 1], "Home shows Ann first, as yours");
  const img = pg.$("strip").querySelectorAll(".card")[0].querySelector("img");
  same(img && img.src, face, "with her picture, not a letter");
});

Deno.test("app: Similar from a row's menu shows one recording per other pianist, with Piano off", async () => {
  const [ben, cy, dee] = [UC("b"), UC("c"), UC("d")];
  const pg = page({
    rows: [by(A, "Clair de Lune piano", ANN, "Ann"), by(B, "Waltz piano", ben, "Ben")],
    mix: () => mixOf([panel(A, "Clair de Lune", ANN, "Ann"), panel(vid("m1"), "Clair de Lune", ben, "Ben"), panel(vid("m2"), "Clair de Lune", ben, "Ben")]),
    search: (body) => (body.query === "Clair de Lune piano"
      ? searchOf([by(vid("s1"), "Clair de Lune piano", cy, "Cy"), by(vid("s2"), "Clair de Lune piano", dee, "Dee"), by(vid("s3"), "Clair de Lune", ANN, "Ann")])
      : searchOf([by(A, "Clair de Lune piano", ANN, "Ann"), by(B, "Waltz piano", ben, "Ben")])),
  });
  await pg.settle();
  pg.row(A).dispatchEvent(new FakeEvent("contextmenu"));
  const items = pg.$("menu").querySelectorAll(".menu-item");
  same(items.map(words), ["Copy link", "Open on YouTube", "Ann", "Similar"], "the row's menu");
  input.press(items[3]);
  await pg.settle();
  same(pg.location.search, "?similar=" + A, "the route");
  same(pg.mixes(), ["RD" + A], "its Mix");
  same(pg.searches().at(-1), ["Clair de Lune piano", TOP], "and its piece");
  same(rowsOf(pg), [vid("m1"), vid("s1"), vid("s2")], "a row per other pianist");
  const bar = pg.$("listbar");
  same([bar.querySelector(".name").textContent, bar.querySelector(".meta").textContent], ["Similar", "Clair de Lune piano"], "the bar");
  same(pg.$("piano").disabled, true, "Piano is off");
  same(cardsOf(pg), ["Ben", "Cy", "Dee"], "the strip");
});

Deno.test("app: an open menu closes when the view changes, by a search, Back or Forward", async () => {
  const pg = page({ rows: [by(A, "Nocturne piano", ANN, "Ann"), by(B, "Waltz piano", UC("b"), "Ben")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Classical"] });
  await pg.settle();
  const menu = pg.$("menu");
  pg.row(A).dispatchEvent(new FakeEvent("contextmenu"));
  same(menu.hidden, false, "the row's menu is open");
  pg.receive({ type: "search", q: "waltz" });
  await pg.settle();
  same([pg.location.search.includes("waltz"), menu.hidden, menu.children.length], [true, true, 0], "a search from the app closes it");
  input.press(pg.$("dock-save"));
  same([menu.hidden, pg.$("dock-save").getAttribute("aria-expanded")], [false, "true"], "Save to's menu is open");
  pg.history.back();
  await pg.settle();
  same([pg.location.search.includes("waltz"), menu.hidden, pg.$("dock-save").getAttribute("aria-expanded")], [false, true, "false"], "Back closes it");
  pg.row(A).dispatchEvent(new FakeEvent("contextmenu"));
  same(menu.hidden, false, "open again");
  pg.history.forward();
  await pg.settle();
  same([pg.location.search.includes("waltz"), menu.hidden], [true, true], "Forward closes it");
});

Deno.test("app: a channel's Similar starts from its most-viewed video and leaves the channel out", async () => {
  const best = vid("v2");
  const pg = page({
    route: "?channel=" + ANN + "&t=similar",
    browse: () => channelPage(ANN, "Ann", [
      video(vid("v1"), "Nocturne", { viewCountText: { simpleText: "1,200 views" } }),
      video(best, "Ballade No. 1", { viewCountText: { simpleText: "2.5M views" } }),
      video(vid("v3"), "Waltz", { viewCountText: { simpleText: "300K views" } }),
    ]),
    mix: () => mixOf([panel(best, "Ballade No. 1", ANN, "Ann"), panel(vid("m1"), "Ballade No. 1", UC("b"), "Ben"), panel(vid("m2"), "Ballade", ANN, "Ann")]),
  });
  await pg.settle();
  same(pg.mixes(), ["RD" + best], "the Mix of the most-viewed");
  same(pg.searches(), [[P.pieceQuery("Ballade No. 1", "Ann"), TOP]], "and a search for its piece");
  same(rowsOf(pg), [vid("m1")], "without the channel's own");
  const seg = pg.$("listbar").querySelectorAll("[role=radio]");
  same(seg.map((b) => [words(b), b.getAttribute("aria-checked")]), [["Videos", "false"], ["Playlists", "false"], ["Similar", "true"]], "the tabs");
  same([pg.$("listbar").querySelector("[aria-label=Sort]").disabled, pg.$("piano").disabled], [true, true], "Sort and Piano are off");
});

// ---- Save to (discovery §5)

const FOLDERS = ["Anime", "Anime/Animenz Piano Sheets", "Classical", "Classical/Chopin", "Jazz"];
const iconsOf = (b) => b.querySelectorAll("use").map((u) => u.getAttribute("href").slice(3));
// Save to as it reads: its icon, its words and its name.
const saveOf = (b) => [iconsOf(b)[0], b.textContent.trim(), b.getAttribute("aria-label")];
// The open menu's items as [icon, words], and the highlighted one's words.
const menuOf = (pg) => pg.$("menu").querySelectorAll(".menu-item").map((b) => [iconsOf(b)[0], words(b)]);
const lit = (pg) => words(pg.$("menu").querySelectorAll(".menu-item").find((b) => b.classList.contains("hl")));
// The picks sent, and their folder fields.
const picksOf = (pg) => pg.calls.filter((c) => c[0] === "pick").map((c) => c[1]);
const destsOf = (pg) => picksOf(pg).map((p) => [p.dest, p.artist, p.folder]);
// Opens Save to's menu on `b`, types `text` and takes the highlighted item.
const chooseIn = async (pg, b, text) => {
  input.press(b);
  input.type(pg.doc, text);
  input.key(pg.doc, "Enter");
  await pg.settle();
};

Deno.test("app: Save to shows only once the app lists its folders; a pick before then has only its legacy folder", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano"), video(B, "Waltz piano")] });
  pg.receive({ type: "init", protocol: 2 });
  await pg.settle();
  input.press(pg.row(B).parts.check);
  await pg.settle();
  same([pg.$("dock-save").hidden, pg.$("save-ticked").hidden, pg.$("convert-ticked").hidden], [true, true, false], "no Save to");
  input.press(pg.row(A).convert);
  await pg.settle();
  const [legacy] = picksOf(pg);
  same(["dest" in legacy, "artist" in legacy, legacy.folder], [false, false, null], "the pick has no dest or artist");
  pg.receive({ type: "folders", folders: ["Classical", "../up", "a/b/c/d/e", 7] });
  await pg.settle();
  same(saveOf(pg.$("save-ticked")), ["folder-root", "MIDI folder", "MIDI folder"], "Save to beside Convert 1");
  same(pg.$("listhead").children.map((n) => n.id).slice(-3), ["save-ticked", "convert-ticked", "untick"], "just before Convert N");
  input.press(pg.$("save-ticked"));
  same(menuOf(pg).map((m) => m[1]), ["MIDI folder", "Classical"], "only valid folders are listed");
});

Deno.test("app: the Save to menu opens empty, lists the artist, the MIDI folder, recents and every folder A to Z, and typing filters", async () => {
  const storage = new Map([["qm-dests", JSON.stringify(["Jazz", "../bad"])]]);
  const pg = page({ storage, rows: [by(A, "Nocturne piano", ANN, "Ann")] });
  pg.receive({ type: "init", protocol: 2, folders: FOLDERS });
  await pg.settle();
  const save = pg.$("dock-save");
  same(saveOf(save), ["folder-root", "MIDI folder", "MIDI folder"], "the dock's Save to");
  same(pg.$("actions").children.map((n) => n.id || words(n)), ["dock-convert", "Convert again", "dock-save", "mix"], "the dock's order");
  input.press(save);
  const field = pg.doc.activeElement;
  same([pg.$("menu").hidden, field.tagName, field.value, field.getAttribute("placeholder"), save.getAttribute("aria-expanded")],
    [false, "INPUT", "", "Folder", "true"], "the field has focus, empty");
  same(menuOf(pg), [["folder-plus", "Ann"], ["folder-root", "MIDI folder"], ["folder", "Jazz"], ["folder", "Anime"],
    ["folder", "Anime / Animenz Piano Sheets"], ["folder", "Classical"], ["folder", "Classical / Chopin"]], "artist, MIDI folder, recents, A to Z");
  same([pg.$("menu").querySelectorAll("[aria-checked=true]").map(words), lit(pg)], [["MIDI folder"], "Ann"], "the current one is marked, the first lit");
  input.type(pg.doc, "cho");
  same(menuOf(pg), [["folder", "Classical / Chopin"], ["folder-plus", "cho"]], "typing filters, a new name last");
  same(lit(pg), "Classical / Chopin", "the first is lit");
  input.key(pg.doc, "ArrowDown");
  same(lit(pg), "cho", "Down");
  input.key(pg.doc, "ArrowUp");
  same(lit(pg), "Classical / Chopin", "Up");
  input.key(pg.doc, "Enter");
  await pg.settle();
  same([pg.$("menu").hidden, pg.doc.activeElement === save], [true, true], "Enter takes it and closes");
  same(saveOf(save), ["folder", "Chopin", "Classical / Chopin"], "its last name, the whole path as its name");
  input.press(save);
  same(pg.doc.activeElement.value, "", "empty when opened again");
  same(menuOf(pg).map((m) => m[1]), ["Classical / Chopin", "MIDI folder", "Jazz", "Anime", "Anime / Animenz Piano Sheets", "Classical"],
    "the choice is Ann's folder, her item in place of a new one, and no item repeats");
  same(pg.$("menu").querySelectorAll("[aria-checked=true]").map(words), ["Classical / Chopin"], "and marked");
  input.key(pg.doc, "Escape");
  same(pg.$("menu").hidden, true, "Esc closes");
  input.press(pg.$("dock-convert"));
  await pg.settle();
  same(destsOf(pg), [["Classical/Chopin", ANN, "Chopin"]], "the pick carries the dest, the artist and the legacy folder");
  same([JSON.parse(pg.storage.get("qm-artists"))[ANN].dest, JSON.parse(pg.storage.get("qm-dests"))], ["Classical/Chopin", ["Classical/Chopin", "Jazz"]],
    "a choice for one artist is theirs, and recent");
});

Deno.test("app: only a typed slash nests, each name a folder name in its case on disk, four deep; an artist's slash never nests", async () => {
  const jacob = UC("j");
  const pg = page({ rows: [by(A, "Nocturne piano", jacob, "Jacob Koller / The Mad Arranger"), video(B, "Waltz piano")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Anime"] });
  await pg.settle();
  const save = pg.$("dock-save");
  input.press(save);
  same(menuOf(pg)[0], ["folder-plus", "Jacob Koller The Mad Arranger"], "the artist is one folder");
  input.type(pg.doc, "anime/Fresh  Name");
  same(menuOf(pg), [["folder-plus", "Anime / Fresh Name"]], "nested in Anime as it is on disk");
  input.key(pg.doc, "Enter");
  await pg.settle();
  same(saveOf(save), ["folder-plus", "Fresh Name", "Anime / Fresh Name"], "to be made");
  input.press(save);
  input.type(pg.doc, "a\\b/c\\d/e");
  same(menuOf(pg), [["folder-plus", "a / b / c / d"]], "four deep at most");
  input.key(pg.doc, "Escape");
  input.press(pg.$("dock-convert"));
  await pg.settle();
  same(destsOf(pg), [["Anime/Fresh Name", jacob, "Fresh Name"]], "the pick");
  input.press(pg.row(B).parts.check);
  await pg.settle();
  input.press(pg.$("save-ticked"));
  same(menuOf(pg), [["folder-root", "MIDI folder"], ["folder-plus", "Anime / Fresh Name"], ["folder", "Anime"]], "a folder sent to shows to be made until listed");
  input.key(pg.doc, "Escape");
  pg.receive({ type: "folders", folders: ["Anime", "Anime/Fresh Name"] });
  await pg.settle();
  input.press(pg.$("save-ticked"));
  same(menuOf(pg), [["folder-root", "MIDI folder"], ["folder", "Anime / Fresh Name"], ["folder", "Anime"]], "then it is a folder");
});

Deno.test("app: an artist's folder is the next pick's, else a folder named like them, shallowest first, else the MIDI folder", async () => {
  const [ben, cy] = [UC("b"), UC("c")];
  const storage = new Map([["qm-artists", JSON.stringify({ [ANN]: { name: "Ann", dest: "classical/ann", at: "2026-10-01T10:00:00Z" } })]]);
  const pg = page({ storage, rows: [by(A, "Nocturne piano", ANN, "Ann"), by(B, "Waltz piano", ben, "Ben"), by(C, "Etude piano", cy, "Cy")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Pop", "Pop/Ben", "Ben", "Classical", "Classical/Ann"] });
  await pg.settle();
  same(saveOf(pg.$("dock-save")), ["folder", "Ann", "Classical / Ann"], "Ann's folder, in its case on disk");
  for (const id of [A, B, C]) input.press(pg.row(id).convert);
  await pg.settle();
  same(destsOf(pg), [["Classical/Ann", ANN, "Ann"], ["Ben", null, "Ben"], ["", null, null]], "kept, named like Ben, the MIDI folder");
});

// Save to's menu on `b` with `text` typed, closed again.
const typedIn = (pg, b, text) => {
  input.press(b);
  input.type(pg.doc, text);
  const out = menuOf(pg);
  input.key(pg.doc, "Escape");
  return out;
};

Deno.test("app: a pianist's remembered folder not made yet is the artist item, checked when current, in the dock, Convert N and a waiting chip", async () => {
  const storage = new Map([
    ["qm-artists", JSON.stringify({ [ANN]: { name: "Ann", dest: "Classical/Rousseau", at: "2026-10-01T10:00:00Z" } })],
    ["qm-dests", JSON.stringify(["Jazz"])],
  ]);
  const pg = page({ storage, rows: [by(A, "Nocturne piano", ANN, "Ann"), by(B, "Waltz piano", ANN, "Ann")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Classical", "Jazz"] });
  await pg.settle();
  const save = pg.$("dock-save");
  same(saveOf(save), ["folder-plus", "Rousseau", "Classical / Rousseau"], "the dock goes to Ann's folder, to be made");
  const checked = () => pg.$("menu").querySelectorAll("[aria-checked=true]").map(words);
  // Ann's one folder comes first and no new folder is named for her; typing finds it by any of its names, ahead
  // of the name to be made, and its own path is no second folder to make.
  const menu = [["folder-plus", "Classical / Rousseau"], ["folder-root", "MIDI folder"], ["folder", "Jazz"], ["folder", "Classical"]];
  const typing = (b, where) => {
    same(typedIn(pg, b, "rou"), [["folder-plus", "Classical / Rousseau"], ["folder-plus", "rou"]], where + ": a name of it typed");
    same(typedIn(pg, b, "class"), [["folder", "Classical"], ["folder-plus", "Classical / Rousseau"], ["folder-plus", "class"]],
      where + ": its first name typed");
    same(typedIn(pg, b, "classical/ROUSSEAU"), [["folder-plus", "Classical / Rousseau"]], where + ": its path typed");
  };
  input.press(save);
  same([menuOf(pg), checked()], [menu, ["Classical / Rousseau"]], "the dock's menu, Ann's folder checked");
  input.key(pg.doc, "Escape");
  typing(save, "the dock");
  input.press(pg.row(A).parts.check);
  await pg.settle();
  input.press(pg.$("save-ticked"));
  same([menuOf(pg), checked()], [menu, ["Classical / Rousseau"]], "Convert N's menu");
  input.key(pg.doc, "Escape");
  typing(pg.$("save-ticked"), "Convert N");
  // An entry going elsewhere: the MIDI folder is checked, and Ann's folder is still hers to go back to.
  pg.receive({ type: "item", key: "v:" + B, kind: "video", state: "queued", position: 0, dest: "" });
  await pg.settle();
  const chip = () => pg.$("queue").querySelector(".chip");
  input.press(chip());
  same([menuOf(pg), checked()], [menu, ["MIDI folder"]], "the chip's menu");
  input.key(pg.doc, "Escape");
  typing(chip(), "the chip");
  await chooseIn(pg, chip(), "rou");
  same(pg.calls.filter((c) => c[0] === "dest"), [["dest", "v:" + B, "Classical/Rousseau", ANN]], "Enter on a typed name of it takes Ann's folder");
});

Deno.test("app: a pianist's folder from the ledger, on disk or the MIDI folder, is the artist item and no folder is named for them", async () => {
  const ledger = (dest) => ({ artists: [{ id: ANN, name: "Ann", n: 2, at: "2026-10-01T10:00:00Z", dest }], seeds: [] });
  const pg = page({
    route: "?channel=" + ANN, artists: ledger("Classical/Rousseau"),
    browse: () => channelPage(ANN, "Ann", [video(vid("c1"), "Nocturne piano"), video(vid("c2"), "Waltz piano")]),
  });
  pg.receive({ type: "init", protocol: 2, folders: ["Classical", "Classical/Rousseau", "Pop", "Pop/Ann"] });
  await pg.settle();
  const save = pg.$("save-ticked");
  same(saveOf(save), ["folder", "Rousseau", "Classical / Rousseau"], "the channel goes to Ann's folder");
  input.press(save);
  same(menuOf(pg), [["folder", "Classical / Rousseau"], ["folder-root", "MIDI folder"], ["folder", "Classical"], ["folder", "Pop"], ["folder", "Pop / Ann"]],
    "her folder, then the MIDI folder and every folder once, the one named like her among them");
  same(pg.$("menu").querySelectorAll("[aria-checked=true]").map(words), ["Classical / Rousseau"], "checked");
  input.key(pg.doc, "Escape");
  same(typedIn(pg, save, "rou"), [["folder", "Classical / Rousseau"], ["folder-plus", "rou"]], "typing finds it");

  const root = page({ artists: ledger(""), rows: [by(A, "Nocturne piano", ANN, "Ann")] });
  root.receive({ type: "init", protocol: 2, folders: ["Classical"] });
  await root.settle();
  input.press(root.$("dock-save"));
  same(menuOf(root), [["folder-root", "MIDI folder"], ["folder", "Classical"]], "a pianist kept in the MIDI folder has no folder named for them");
});

Deno.test("app: typing finds the folders still to be made that the empty field lists, as it shows them", async () => {
  const ben = UC("b");
  const storage = new Map([
    ["qm-artists", JSON.stringify({ [ben]: { name: "Ben", dest: "Classical/Rousseau", at: "2026-10-01T10:00:00Z" } })],
    ["qm-dests", JSON.stringify(["classical/rousseau", "Jazz"])],
  ]);
  const pg = page({ storage, rows: [by(A, "Nocturne piano", ANN, "Ann"), by(B, "Waltz piano", ben, "Ben")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Classical", "Jazz"] });
  await pg.settle();
  const checked = () => pg.$("menu").querySelectorAll("[aria-checked=true]").map(words);
  const dock = pg.$("dock-save");
  input.press(dock);
  same(menuOf(pg)[0], ["folder-plus", "Ann"], "Ann has no folder: one named for her, to be made");
  input.key(pg.doc, "Escape");
  same(typedIn(pg, dock, "an"), [["folder-plus", "Ann"], ["folder-plus", "an"]], "found by part of its name");
  same(typedIn(pg, dock, "ANN"), [["folder-plus", "Ann"]], "and by its name in another case, with no second one");
  // Ben's folder is also recent in another case: typing shows it as the empty field does.
  input.press(pg.row(B).parts.check);
  await pg.settle();
  const list = pg.$("save-ticked");
  input.press(list);
  same(menuOf(pg)[0], ["folder-plus", "Classical / Rousseau"], "Ben's folder");
  input.key(pg.doc, "Escape");
  same(typedIn(pg, list, "rou"), [["folder-plus", "Classical / Rousseau"], ["folder-plus", "rou"]], "typed, the same");
  // An entry going to a folder neither made, recent nor anyone's.
  pg.receive({ type: "item", key: "v:" + A, kind: "video", state: "queued", position: 0, dest: "Film/New Things" });
  await pg.settle();
  const chip = () => pg.$("queue").querySelector(".chip");
  input.press(chip());
  same([menuOf(pg).slice(0, 3), checked()], [[["folder-plus", "Ann"], ["folder-root", "MIDI folder"], ["folder-plus", "Film / New Things"]],
    ["Film / New Things"]], "the chip's current folder");
  input.type(pg.doc, "new");
  same([menuOf(pg), checked()], [[["folder-plus", "Film / New Things"], ["folder-plus", "new"]], ["Film / New Things"]], "found by a name of it, checked");
  input.key(pg.doc, "Escape");
  same(typedIn(pg, chip(), "film/NEW things"), [["folder-plus", "Film / New Things"]], "and by its path, with no second one");
  // A current folder that is a recent one in another case shows as on disk, typed or not.
  pg.receive({ type: "item", key: "v:" + A, kind: "video", state: "queued", position: 0, dest: "jazz" });
  await pg.settle();
  input.press(chip());
  same(checked(), ["Jazz"], "the current one as on disk");
  input.type(pg.doc, "jaz");
  same([menuOf(pg), checked()], [[["folder", "Jazz"], ["folder-plus", "jaz"]], ["Jazz"]], "typed, the same");
  input.key(pg.doc, "Escape");
});

Deno.test("app: Convert N sends one pick per folder in first-row order, and a choice there sends them to one", async () => {
  const D = vid("d1");
  const rows = [by(A, "Nocturne piano", ANN, "Ann"), by(B, "Waltz piano", UC("c"), "Cy"), by(C, "Etude piano", UC("b"), "Ben"), by(D, "Ballade piano", ANN, "Ann")];
  const pg = page({ rows });
  pg.receive({ type: "init", protocol: 2, folders: ["Ann", "Ben"] });
  await pg.settle();
  input.press(pg.$("all"));
  await pg.settle();
  same(saveOf(pg.$("save-ticked")), ["folders", "3", "Ann, MIDI folder, Ben"], "three folders");
  input.press(pg.$("convert-ticked"));
  await pg.settle();
  same(picksOf(pg).map((p) => [p.kind, p.items.map((it) => it.id), p.dest, p.artist, p.folder]),
    [["selection", [A, D], "Ann", null, "Ann"], ["selection", [B], "", null, null], ["selection", [C], "Ben", null, "Ben"]], "one pick per folder");

  const two = page({ rows });
  two.receive({ type: "init", protocol: 2, folders: ["Ann", "Ben"] });
  await two.settle();
  input.press(two.row(A).parts.check);
  input.press(two.row(D).parts.check);
  await two.settle();
  same(saveOf(two.$("save-ticked")), ["folder", "Ann", "Ann"], "one artist's");
  await chooseIn(two, two.$("save-ticked"), "Classical");
  same(saveOf(two.$("save-ticked")), ["folder-plus", "Classical", "Classical"], "the choice");
  input.press(two.$("convert-ticked"));
  await two.settle();
  same(destsOf(two), [["Classical", ANN, "Classical"]], "one pick, Ann's");
  same(JSON.parse(two.storage.get("qm-artists"))[ANN].dest, "Classical", "Ann keeps it");
  input.press(two.row(B).parts.check);
  input.press(two.row(C).parts.check);
  await two.settle();
  same(saveOf(two.$("save-ticked")), ["folder-plus", "Classical", "Classical"], "the view keeps its choice");
  await chooseIn(two, two.$("save-ticked"), "Mixed");
  input.press(two.$("convert-ticked"));
  await two.settle();
  same(destsOf(two).at(-1), ["Mixed", null, "Mixed"], "two artists' rows have no artist");
  const saved = JSON.parse(two.storage.get("qm-artists"));
  same([UC("b") in saved, UC("c") in saved, saved[ANN].dest], [false, false, "Classical"], "and nobody's folder changes");
});

Deno.test("app: a channel converts into its artist's folder, and a choice there becomes theirs", async () => {
  const pg = page({
    route: "?channel=" + ANN,
    browse: () => channelPage(ANN, "Ann", [video(vid("c1"), "Nocturne piano"), video(vid("c2"), "Waltz piano")]),
  });
  pg.receive({ type: "init", protocol: 2, folders: ["Pianists", "Pianists/Ann"] });
  await pg.settle();
  same([words(pg.$("convert-ticked")), shown(pg.$("save-ticked"))], ["Convert 2", true], "a list comes ticked, Save to beside Convert N");
  same(saveOf(pg.$("save-ticked")), ["folder", "Ann", "Pianists / Ann"], "a folder named like the artist");
  input.press(pg.$("save-ticked"));
  same(menuOf(pg)[0], ["folder", "Pianists / Ann"], "the artist item");
  input.key(pg.doc, "Escape");
  input.press(pg.$("convert-ticked"));
  await pg.settle();
  same(picksOf(pg).map((p) => [p.kind, p.id, p.dest, p.artist, p.folder]), [["channel", ANN, "Pianists/Ann", null, "Ann"]], "the channel's pick");

  const two = page({
    route: "?channel=" + ANN,
    browse: () => channelPage(ANN, "Ann", [video(vid("c1"), "Nocturne piano"), video(vid("c2"), "Waltz piano")]),
  });
  two.receive({ type: "init", protocol: 2, folders: [] });
  await two.settle();
  same(saveOf(two.$("save-ticked")), ["folder-plus", "Ann", "Ann"], "else a new folder for the channel");
  await chooseIn(two, two.$("save-ticked"), "Solo");
  input.press(two.$("convert-ticked"));
  await two.settle();
  same(destsOf(two), [["Solo", ANN, "Solo"]], "the choice is Ann's");
  same(JSON.parse(two.storage.get("qm-artists"))[ANN].dest, "Solo", "and kept");
});

Deno.test("app: a waiting entry's chip moves it through the dest call; running or done, its folder is plain", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Classical"] });
  await pg.settle();
  input.press(pg.row(A).convert);
  await pg.settle();
  pg.receive({ type: "item", key: "v:" + A, kind: "video", state: "queued", position: 0, dest: "" });
  await pg.settle();
  const chip = () => pg.$("queue").querySelector(".chip");
  same([chip().tagName, ...saveOf(chip())], ["BUTTON", "folder-root", "MIDI folder", "MIDI folder"], "a chip on the state line");
  await chooseIn(pg, chip(), "classical");
  same(pg.calls.filter((c) => c[0] === "dest"), [["dest", "v:" + A, "Classical", null]], "the dest call");
  same(saveOf(chip()), ["folder", "Classical", "Classical"], "the entry shows it");
  pg.replies.dest = () => false;
  input.press(chip());
  input.key(pg.doc, "Enter");
  await pg.settle();
  same(saveOf(chip()), ["folder", "Classical", "Classical"], "a refused move keeps its folder");
  same(pg.$("banners").querySelector("span").textContent, "Folder not allowed.", "and says so");
  pg.receive({ type: "item", key: "v:" + A, kind: "video", state: "converting", phase: "download", dest: "Classical" });
  await pg.settle();
  same([chip().tagName, chip().textContent.trim(), chip().title], ["SPAN", "Classical", "Classical"], "running, plain");
  const path = "C:\\Music\\MIDI\\Classical\\Nocturne piano.mid";
  pg.receive({ type: "item", key: "v:" + A, kind: "video", state: "done", path, dest: "Classical", title: "Nocturne piano" });
  await pg.settle();
  const tag = pg.$("converted").querySelector(".chip");
  same([tag.tagName, tag.textContent.trim(), tag.title], ["SPAN", "Classical", "Classical / Nocturne piano.mid"], "done, its folder");
  const open = pg.$("dock-convert");
  same([open.textContent.trim(), open.getAttribute("aria-label")], ["Open", "Classical / Nocturne piano.mid"], "Open is named for where it is");
  same([pg.$("dock-save").hidden, words(pg.$("actions").children[1])], [false, "Convert again"], "Save to is for Convert again");
});

Deno.test("app: a video converted before is shown in its own folder, not the one picked", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Chopin"] });
  pg.receive({ type: "item", key: "v:" + A, kind: "video", state: "done", path: "C:\\Music\\MIDI\\Chopin\\Nocturne piano.mid",
    already: true, dest: "", title: "Nocturne piano" });
  await pg.settle();
  const tags = () => [...pg.$("converted").querySelectorAll(".chip")].map((t) => t.textContent.trim());
  same(tags(), ["Chopin"], "a video");
  pg.receive({ type: "item", key: "p:PLx", kind: "playlist", state: "converting", dest: "" });
  pg.receive({ type: "child", parent: "p:PLx", key: "v:" + B, state: "done", path: "C:\\Music\\MIDI\\Chopin\\Waltz piano.mid", already: true });
  await pg.settle();
  same(tags(), ["Chopin", "Chopin"], "a list's video");
});

Deno.test("app: after a remount an entry's folder is its item's, and Retry and resend keep it", async () => {
  const pg = page({ rows: [by(A, "Nocturne piano", ANN, "Ann")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Classical"] });
  pg.receive({ type: "item", key: "v:" + A, kind: "video", state: "failed", reason: "No space left.", dest: "Classical", title: "Nocturne piano" });
  await pg.settle();
  same(saveOf(pg.$("queue").querySelector(".chip")), ["folder", "Classical", "Classical"], "read back");
  input.press(pg.$("queue").querySelector("[aria-label=Retry]"));
  await pg.settle();
  same(destsOf(pg), [["Classical", null, "Classical"]], "Retry sends it there");

  const two = page({ rows: [video(A, "Nocturne piano"), video(B, "Waltz piano")] });
  two.receive({ type: "init", protocol: 2, folders: ["Classical"] });
  await two.settle();
  input.press(two.$("all"));
  await two.settle();
  await chooseIn(two, two.$("save-ticked"), "Classical");
  input.press(two.$("convert-ticked"));
  await two.settle();
  two.receive({ type: "item", key: "s:1", kind: "selection", state: "failed", reason: "No space left.", total: 2 });
  await two.settle();
  input.press(two.$("queue").querySelector("[aria-label=Retry]"));
  await two.settle();
  same(destsOf(two), [["Classical", null, "Classical"], ["Classical", null, "Classical"]], "a selection resent keeps its folder");
});

Deno.test("app: a refused pick says Folder not allowed. and its row returns to Convert", async () => {
  const pg = page({ rows: [video(A, "Nocturne piano"), video(B, "Waltz piano")] });
  pg.receive({ type: "init", protocol: 2, folders: [] });
  await pg.settle();
  pg.replies.pick = () => ({ ok: false, seq: null, reason: "dest" });
  input.press(pg.row(A).convert);
  await pg.settle();
  const banner = pg.$("banners");
  same([banner.hidden, banner.querySelector("span").textContent, iconsOf(banner)[0]], [false, "Folder not allowed.", "triangle-alert"], "the banner");
  same(convertOf(pg.row(A)), ["Convert", false], "the row is back to Convert");
  delete pg.replies.pick;
  input.press(pg.row(B).convert);
  await pg.settle();
  same([banner.hidden, convertOf(pg.row(B))], [true, ["Sent", false]], "a pick taken clears it");
});

Deno.test("app: a pick with a folder refused for anything else is the plain failure, not Folder not allowed.", async () => {
  for (const answer of [{ ok: false, seq: null }, { ok: false, seq: null, reason: "busy" }, null]) {
    const pg = page({ rows: [video(A, "Nocturne piano")] });
    pg.receive({ type: "init", protocol: 2, folders: ["Classical"] });
    await pg.settle();
    pg.replies.pick = () => answer;
    input.press(pg.row(A).convert);
    await pg.settle();
    same(picksOf(pg).at(-1).dest, "", "the pick had a folder, the MIDI folder");
    const banner = pg.$("banners");
    same([banner.hidden || !(banner.textContent || "").includes("Folder not allowed."), convertOf(pg.row(A))],
      [true, ["Retry", false]], JSON.stringify(answer) + ": Retry, and no word about the folder");
  }
});

Deno.test("app: at 624 wide the genre row scrolls, the strip is one line and Save to is its icon", async () => {
  const css = Deno.readTextFileSync(new URL("../page/app.css", import.meta.url));
  const home = page({ route: "", width: 624, search: answers(COLD) });
  await home.settle();
  same([home.root.classList.contains("narrow"), home.$("genres").hidden, cardsOf(home)], [true, false, [...curatedNames(), "Cy", "Ann", "Ben"]], "narrow Home");
  check(/\.pills\{[^}]*overflow-x:auto/.test(css), "the genre row scrolls");
  check(/html\.narrow \.cards\{[^}]*flex-wrap:nowrap[^}]*overflow-x:auto/.test(css), "the strip is one scrolling line");
  check(css.includes("html.narrow :is(.listhead,.actions)>.save>span{display:none}"), "Save to keeps its icon and chevron");
  const pg = page({ width: 624, rows: [by(A, "Nocturne piano", ANN, "Ann"), video(B, "Waltz piano")] });
  pg.receive({ type: "init", protocol: 2, folders: ["Ann"] });
  await pg.settle();
  input.press(pg.row(A).parts.check);
  await pg.settle();
  const save = pg.$("save-ticked");
  same([shown(save), ...saveOf(save), iconsOf(save)], [true, "folder", "Ann", "Ann", ["folder", "chevron-down"]], "icon, chevron and its name");
  await chooseIn(pg, save, "Etudes/Liszt");
  same(saveOf(save), ["folder-plus", "Liszt", "Etudes / Liszt"], "a choice narrow");
});

Deno.test("app: narrow, a channel's name keeps its line and its three labelled tabs go on the next", async () => {
  const css = Deno.readTextFileSync(new URL("../page/app.css", import.meta.url));
  const pg = page({ route: "?channel=" + ANN, width: 626, browse: () => channelPage(ANN, "Ann Example Pianist Long Name", [video(vid("c1"), "Nocturne piano")]) });
  await pg.settle();
  const bar = pg.$("listbar");
  same([pg.root.classList.contains("narrow"), bar.children.map((n) => n.className || n.tagName).slice(0, 3)],
    [true, ["heads", "seg", "icon"]], "narrow: the name, the tabs, Sort");
  same(bar.querySelectorAll("[role=radio]").map(words), ["Videos", "Playlists", "Similar"], "the tabs keep their words");
  const rule = (selector) => (css.match(new RegExp(selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "\\{([^}]*)\\}")) || [])[1] || "";
  const barRule = rule("html.narrow #listbar:has(>.seg)");
  check(/flex-wrap:wrap/.test(barRule) && /height:auto/.test(barRule), "the channel's bar wraps to two lines: " + barRule);
  check(/flex:1 0 100%/.test(rule("html.narrow #listbar:has(>.seg)>.heads")), "the name's line is the whole row");
  check(!/html\.narrow[^{]*\.seg[^{]*\{[^}]*display:none/.test(css), "no narrow rule hides the tabs' words");
});

Deno.test("app: no request asks for solo piano", () => {
  for (const file of ["../page/app.js", "../browse.py"]) {
    check(!Deno.readTextFileSync(new URL(file, import.meta.url)).toLowerCase().includes("solo piano"), file);
  }
});

// ---- a pianist opened from a genre, and the strip's curated pianists (§3.19)

const VIDEOS_TAB = "EgZ2aWRlb3PyBgQKAjoA";
const SEARCH_TAB = "EgZzZWFyY2jyBgQKAloA";
const GAME_SONGS = ["Zelda - Song of Storms", "Undertale - Megalovania", "Super Mario Bros Theme", "Pokemon Center",
  "Final Fantasy VII - Aerith's Theme", "Kirby - Gourmet Race", "Halo Theme", "Tetris Theme A", "Minecraft - Sweden",
  "Celeste - Resurrections", "Hollow Knight - Dirtmouth", "Chrono Trigger - Corridors of Time", "Sonic - Green Hill Zone"];
// A channel's Videos: `games` game songs and `others` that are not, in turn; the others have more views.
const mixed = (games, others) => Array.from({ length: Math.max(games, others) }, (_, i) => [
  ...(i < games ? [video(vid("g" + i), GAME_SONGS[i] + " piano", { viewCountText: { simpleText: (i + 1) + "K views" } })] : []),
  ...(i < others ? [video(vid("o" + i), "Nocturne No. " + (i + 1) + " piano", { viewCountText: { simpleText: (i + 1) + "M views" } })] : []),
]).flat();
const gameIds = (n) => Array.from({ length: n }, (_, i) => vid("g" + i));
// A channel named Ann: its Videos are `videos`, its search tab answers `search`, and a continuation its page in `more`.
const channelAnswers = ({ videos = mixed(13, 3), search = [], more = {} } = {}) => (body) =>
  (body.continuation ? moreOf(more[body.continuation] || []) : channelPage(body.browseId, "Ann", body.params === SEARCH_TAB ? search : videos));
// The browse requests sent: a tab's params (and a search's words), or a continuation's token.
const browses = (pg) => pg.requests.filter((r) => r.url === P.api("browse"))
  .map((r) => r.body.continuation || [r.body.params, r.body.query].filter((x) => x != null).join(" "));
const tabsOf = (pg) => pg.$("listbar").querySelectorAll("[role=radio]").map((b) => [words(b), b.getAttribute("aria-checked")]);
const tabNamed = (pg, text) => pg.$("listbar").querySelectorAll("[role=radio]").find((b) => words(b) === text);
const GAMES_TOP = { ["game ost piano cover " + TOP]: searchOf(recordings("gm", 6, PIANISTS)) };
const GAME_WORDS = P.GENRES.find((g) => g.key === "games").words;

Deno.test("app: a pianist opened from a genre's strip lists only their songs of that genre, ticked, under its radio", async () => {
  check(GAME_SONGS.every((t) => P.inGenre("games", t)) && !P.inGenre("games", "Nocturne No. 1 piano"), "the made-up titles");
  const curated = P.curatedFor("games");
  const pg = page({ route: "?g=games", search: answers(GAMES_TOP), browse: channelAnswers() });
  await pg.settle();
  same(cardsOf(pg), curated.slice(0, 24).map((p) => p.name), "the strip: Games' curated pianists, 24 at most");
  same(pg.$("strip").querySelectorAll(".card")[0].querySelector("img").src, P.avatarUrl(curated[0].avatar), "with their pictures");
  input.press(pg.$("strip").querySelectorAll(".card")[0]);
  await pg.settle();
  same(pg.location.search, "?channel=" + curated[0].id + "&g=games&s=popular", "the route keeps the genre");
  same(rowsOf(pg), gameIds(13), "only the game songs");
  same(tabsOf(pg), [["Games", "true"], ["Videos", "false"], ["Playlists", "false"], ["Similar", "false"]], "the genre's radio first");
  same(iconsOf(tabNamed(pg, "Games")), ["funnel"], "with the funnel");
  same(browses(pg), [VIDEOS_TAB], "one read of Videos and no search");
  same(words(pg.$("convert-ticked")), "Convert 13", "each ticked");
  input.press(pg.$("convert-ticked"));
  await pg.settle();
  const pick = picksOf(pg).at(-1);
  same([pick.kind, pick.title, pick.folder, pick.items.map((it) => it.id)], ["selection", "Ann · Games", "Ann", gameIds(13)],
    "a selection of them in Ann's folder, never the whole channel");
});

Deno.test("app: under 12 songs of the genre, the channel's search for its words adds more, three pages at most, until one adds none", async () => {
  const route = "?channel=" + ANN + "&g=games";
  const search = [video(vid("s0"), GAME_SONGS[5] + " piano"), video(vid("s1"), "Nocturne piano"), moreItem("p2")];
  const pg = page({ route, browse: channelAnswers({ videos: mixed(3, 5), search, more: { p2: [video(vid("s2"), "Waltz piano"), moreItem("p3")] } }) });
  await pg.settle();
  same(rowsOf(pg), [...gameIds(3), vid("s0")], "the read's, then the search's in the genre");
  same(browses(pg), [VIDEOS_TAB, SEARCH_TAB + " " + GAME_WORDS, "p2"], "a page that adds none ends it");
  same(words(pg.$("convert-ticked")), "Convert 4", "each ticked");
  const more = { p2: [video(vid("s2"), GAME_SONGS[6] + " piano"), moreItem("p3")], p3: [video(vid("s3"), GAME_SONGS[7] + " piano"), moreItem("p4")] };
  const three = page({ route, browse: channelAnswers({ videos: mixed(3, 5), search, more }) });
  await three.settle();
  same(browses(three), [VIDEOS_TAB, SEARCH_TAB + " " + GAME_WORDS, "p2", "p3"], "three pages at most");
  same(rowsOf(three), [...gameIds(3), vid("s0"), vid("s2"), vid("s3")], "each adding its songs");
  const many = page({ route, browse: channelAnswers({ videos: mixed(12, 2) }) });
  await many.settle();
  same([rowsOf(many).length, browses(many)], [12, [VIDEOS_TAB]], "12 ask nothing more");
  const none = page({ route, browse: channelAnswers({ videos: mixed(0, 4) }) });
  await none.settle();
  same([rowsOf(none).length, none.$("rows").querySelector(".empty").textContent, tabsOf(none)[1]], [0, "No results.", ["Videos", "false"]],
    "none: No results., with Videos beside");
});

Deno.test("app: Videos leaves the genre and Back returns, from the read already made; Back again is the genre as it was", async () => {
  const curated = P.curatedFor("games");
  const pg = page({ route: "?g=games", search: answers(GAMES_TOP), browse: channelAnswers() });
  await pg.settle();
  input.press(pg.$("strip").querySelectorAll(".card")[0]);
  await pg.settle();
  const scoped = "?channel=" + curated[0].id + "&g=games&s=popular";
  const asked = pg.requests.length;
  input.press(tabNamed(pg, "Videos"));
  await pg.settle();
  same([pg.location.search, rowsOf(pg).length, tabsOf(pg).map((t) => t[1])],
    [scoped.replace("&s=", "&t=videos&s="), 16, ["false", "true", "false", "false"]], "Videos: the whole channel, on the same sort");
  same(pg.requests.length, asked, "without asking again");
  pg.history.back();
  await pg.settle();
  same([pg.location.search, rowsOf(pg)], [scoped, gameIds(13)], "Back: the game songs");
  pg.history.back();
  await pg.settle();
  const pill = pg.$("pills").querySelectorAll(".pill").find((b) => b.dataset.genre === "games");
  same([pg.location.search, pill.getAttribute("aria-checked"), cardsOf(pg)[0], rowsOf(pg).length], ["?g=games", "true", curated[0].name, 6],
    "Back again: Games, its pill, its strip and its rows");
  pg.history.forward();
  await pg.settle();
  same([pg.location.search, rowsOf(pg)], [scoped, gameIds(13)], "Forward");
  same(pg.requests.length, asked, "all from the views kept");
  input.press(tabNamed(pg, "Videos"));
  await pg.settle();
  input.press(tabNamed(pg, "Games"));
  await pg.settle();
  same([pg.location.search, rowsOf(pg), pg.requests.length], [scoped, gameIds(13), asked], "the genre's radio goes back in, asking nothing");
  input.press(tabNamed(pg, "Playlists"));
  await pg.settle();
  same(pg.location.search, "?channel=" + curated[0].id + "&g=games&t=playlists", "Playlists keeps the genre, not the sort");
});

Deno.test("app: a reload reopens a genre's songs of a channel from its route; an unknown genre, or none, is Videos", async () => {
  const three = [["Videos", "true"], ["Playlists", "false"], ["Similar", "false"]];
  for (const [route, tabs, n] of [
    ["?channel=" + ANN + "&g=games", [["Games", "true"], ["Videos", "false"], ["Playlists", "false"], ["Similar", "false"]], 13],
    ["?channel=" + ANN + "&g=games&t=videos&s=oldest", [["Games", "false"], ["Videos", "true"], ["Playlists", "false"], ["Similar", "false"]], 16],
    ["?channel=" + ANN + "&g=nope", three, 16],
    ["?channel=" + ANN + "&t=genre", three, 16],
  ]) {
    const pg = page({ route, browse: channelAnswers() });
    await pg.settle();
    same([tabsOf(pg), rowsOf(pg).length], [tabs, n], route);
  }
  const lost = page({ route: "?channel=" + ANN + "&g=games", browse: () => ({}) });
  await lost.settle();
  const open = lost.$("banners").querySelectorAll("button").find((b) => words(b) === "Open on YouTube");
  input.press(open);
  same(lost.calls.filter((c) => c[0] === "open_external").map((c) => c[1]), ["https://www.youtube.com/channel/" + ANN + "/videos"],
    "on YouTube, its Videos");
});

Deno.test("app: a row's channel opens in the genre it was found in, on a genre view or a Home shelf; elsewhere it is the whole channel", async () => {
  const openFrom = async (pg, id, name) => {
    pg.row(id).dispatchEvent(new FakeEvent("contextmenu"));
    input.press(pg.$("menu").querySelectorAll(".menu-item").find((b) => words(b) === name));
    await pg.settle();
    return pg.location.search;
  };
  const pg = page({ route: "?g=games", search: answers(GAMES_TOP) });
  await pg.settle();
  same(await openFrom(pg, vid("gm0"), "Ann"), "?channel=" + ANN + "&g=games&s=popular", "a genre view's row");
  pg.history.back();
  await pg.settle();
  input.press(pg.$("dock-channel"));
  await pg.settle();
  same(pg.location.search, "?channel=" + ANN + "&g=games&s=popular", "the dock's row, from the genre view");
  const home = page({ route: "", search: answers(COLD) });
  await home.settle();
  same(await openFrom(home, vid("jz0"), "Cy"), "?channel=" + UC("c") + "&g=jazz&s=popular", "a row of Home's Jazz shelf");
  home.history.back();
  await home.settle();
  input.press(home.$("strip").querySelectorAll(".card")[0]);
  await home.settle();
  same(home.location.search, "?channel=" + P.homeCurated()[0].id + "&s=popular", "a card of Home's strip: the whole channel");
  const search = page({ rows: [by(A, "Zelda - Song of Storms piano", ANN, "Ann")] });
  await search.settle();
  same(await openFrom(search, A, "Ann"), "?channel=" + ANN + "&s=popular", "a search's row");
});

Deno.test("app: a genre's Similar starts from its most-viewed song of the genre", async () => {
  const pg = page({ route: "?channel=" + ANN + "&g=games", browse: channelAnswers() });
  await pg.settle();
  input.press(tabNamed(pg, "Similar"));
  await pg.settle();
  same(pg.location.search, "?channel=" + ANN + "&g=games&t=similar", "the route");
  same(pg.mixes(), ["RD" + vid("g12")], "the Mix of the game song most viewed, not of a Nocturne viewed more");
  const cold = page({ route: "?channel=" + ANN + "&g=games&t=similar", browse: channelAnswers() });
  await cold.settle();
  same([browses(cold), cold.mixes()], [[VIDEOS_TAB], ["RD" + vid("g12")]], "opened on it: from the game songs of Videos' first page");
});

Deno.test("app: a genre's strip is its curated pianists, then the channels found posting its songs; never a blocked channel", async () => {
  const [ben, cy] = [UC("b"), UC("c")];
  const [blocked] = P.BLOCK;
  const rags = P.curatedFor("ragtime");
  const pg = page({ route: "?g=ragtime", search: answers({ ["ragtime piano " + TOP]: searchOf([
    by(vid("r1"), "Maple Leaf Rag piano", ben, "Ben"), by(vid("r2"), "The Entertainer - Scott Joplin piano", ben, "Ben"),
    by(vid("r3"), "Maple Leaf Rag piano", blocked, "Blocked"), by(vid("r4"), "Maple Leaf Rag piano", blocked, "Blocked"),
    by(vid("r5"), "Nocturne piano", cy, "Cy"),
  ]) }) });
  await pg.settle();
  same(cardsOf(pg), [...rags.map((p) => p.name), "Ben"].slice(0, 24), "the curated, then Ben; not Cy, nor the blocked channel");
  input.press(pg.$("strip").querySelectorAll(".card")[rags.length]);
  await pg.settle();
  same(pg.location.search, "?channel=" + ben + "&g=ragtime&s=popular", "Ben opens in Ragtime");
});

Deno.test("app: each start takes the next turn of a genre's curated pianists after its first 9, and 6 places stay with those found", async () => {
  const curated = P.curatedFor("games").map((p) => p.name);
  const found = "defghij".split("").map((c, i) => by(vid("f" + i), GAME_SONGS[i] + " piano", UC(c), "Found " + c));
  const search = answers({ ["game ost piano cover " + TOP]: searchOf(found) });
  const first = page({ route: "?g=games", search, storage: new Map() });
  await first.settle();
  same(cardsOf(first), [...curated.slice(0, 18), "Found d", "Found e", "Found f", "Found g", "Found h", "Found i"],
    "a first start: the first 18 of the list, then 6 found");
  same(first.storage.get("qm-turn"), "1", "and the next start is turn 1");
  const next = page({ route: "?g=games", search, storage: new Map([["qm-turn", "1"]]) });
  await next.settle();
  same(cardsOf(next).slice(0, 18), [...curated.slice(0, 9), ...curated.slice(18, 27)], "turn 1: the first 9, then the next 9 of the rest");
  same(next.storage.get("qm-turn"), "2", "then turn 2");
  const odd = page({ route: "?g=games", search, storage: new Map([["qm-turn", "soon"]]) });
  await odd.settle();
  same([cardsOf(odd).slice(0, 18), odd.storage.get("qm-turn")], [curated.slice(0, 18), "1"], "a turn unread is turn 0");
});

Deno.test("app: the strip shows all its cards at any width, wrapped to more lines when wide, and End reaches the last", async () => {
  const css = Deno.readTextFileSync(new URL("../page/app.css", import.meta.url));
  check(/(?:^|[\s}])\.cards\{[^}]*flex-wrap:wrap/m.test(css), "wide, the cards wrap to as many lines as they need");
  check(/html\.narrow \.cards\{[^}]*flex-wrap:nowrap[^}]*overflow-x:auto/.test(css), "narrow, one scrolling line");
  check(!/\.card(?![\w-])[^{]*\{[^}]*display:none/.test(css), "no rule hides a card");
  for (const width of [1600, 1200, 624]) {
    const pg = page({ route: "?g=games", width, resize: true, search: answers(GAMES_TOP) });
    await pg.settle();
    pg.resized();
    const cards = pg.$("strip").querySelectorAll(".card");
    same([cards.length, cards.filter((c) => !shown(c)).length], [24, 0], width + " wide: all 24 shown");
    input.tabTo(cards[0]);
    input.key(pg.doc, "End");
    same(cards.indexOf(pg.doc.activeElement), 23, width + " wide: End is the 24th");
    input.key(pg.doc, "ArrowLeft");
    same(cards.indexOf(pg.doc.activeElement), 22, width + " wide: and the arrows move from it");
  }
  const [eve, fay] = [UC("e"), UC("f")];
  const storage = new Map([["qm-artists", JSON.stringify({ [eve]: { name: "Eve", at: "2026-10-02T10:00:00Z" }, [fay]: { name: "Fay", at: "2026-10-01T10:00:00Z" } })]]);
  const home = page({ route: "", storage, resize: true, search: answers(COLD) });
  await home.settle();
  const items = home.$("strip").firstElementChild.children;
  const named = (n) => (n.classList.contains("card") ? n.querySelector(".name").textContent : "|");
  same(items.map(named), ["Eve", "Fay", "|", ...curatedNames([], [eve, fay]), "Cy", "Ann", "Ben"], "yours, the divider, the curated, the ones found");
  same(items.filter((n) => !shown(n)).length, 0, "each of them shown");
});

Deno.test("app: a blocked channel is never on Home's strip, though it is among yours", async () => {
  const [blocked] = P.BLOCK;
  const eve = UC("e");
  const storage = new Map([["qm-artists", JSON.stringify({
    [blocked]: { name: "Blocked", at: "2026-10-03T10:00:00Z" }, [eve]: { name: "Eve", at: "2026-10-02T10:00:00Z" },
  })]]);
  const home = page({ route: "", storage, search: answers(COLD) });
  await home.settle();
  const items = home.$("strip").firstElementChild.children;
  const named = (n) => (n.classList.contains("card") ? n.querySelector(".name").textContent : "|");
  same(items.map(named).slice(0, 2), ["Eve", "|"], "Eve, then the divider");
  check(!cardsOf(home).includes("Blocked") && home.$("strip").querySelectorAll(".card").every((c) => c.qmPianist.id !== blocked), "not the blocked one");
});

Deno.test("app: a failed fill keeps the songs read, and Retry asks only the channel's search again", async () => {
  let fails = 1;
  const base = channelAnswers({ videos: mixed(3, 5), search: [video(vid("s0"), GAME_SONGS[5] + " piano")] });
  const pg = page({ route: "?channel=" + ANN + "&g=games", browse: (body) => (body.params === SEARCH_TAB && fails-- > 0 ? 500 : base(body)) });
  await pg.settle();
  const banners = pg.$("banners");
  same([rowsOf(pg), banners.hidden, banners.querySelector("span").textContent], [gameIds(3), false, "Results could not be read."],
    "the songs of Videos, and the search's failure");
  same(browses(pg), [VIDEOS_TAB, SEARCH_TAB + " " + GAME_WORDS], "Videos, then the search");
  input.press(banners.querySelectorAll("button").find((b) => words(b) === "Retry"));
  await pg.settle();
  same(browses(pg), [VIDEOS_TAB, SEARCH_TAB + " " + GAME_WORDS, SEARCH_TAB + " " + GAME_WORDS], "Retry: the search again, not Videos");
  same([rowsOf(pg), banners.hidden, words(pg.$("convert-ticked"))], [[...gameIds(3), vid("s0")], true, "Convert 4"], "its song added");
});

Deno.test("app: a Videos page that fails keeps the songs read, and Retry reads on from it, then fills", async () => {
  let fails = 1;
  const base = channelAnswers({
    videos: [...mixed(2, 2), moreItem("v2")], more: { v2: [video(vid("g9"), GAME_SONGS[9] + " piano")] },
    search: [video(vid("s0"), GAME_SONGS[5] + " piano")],
  });
  const pg = page({ route: "?channel=" + ANN + "&g=games", browse: (body) => (body.continuation === "v2" && fails-- > 0 ? 500 : base(body)) });
  await pg.settle();
  const banners = pg.$("banners");
  same([rowsOf(pg), browses(pg), banners.hidden], [gameIds(2), [VIDEOS_TAB, "v2"], false], "the first page's songs, and the failure");
  input.press(banners.querySelectorAll("button").find((b) => words(b) === "Retry"));
  await pg.settle();
  same(browses(pg), [VIDEOS_TAB, "v2", "v2", SEARCH_TAB + " " + GAME_WORDS], "Retry: that page again, then the search");
  same([rowsOf(pg), banners.hidden], [[...gameIds(2), vid("g9"), vid("s0")], true], "the songs of both");
  const end = pg.$("rows").querySelector(".spin, .loading, [aria-busy=true]");
  check(!end || !shown(end), "nothing left reading");
  fails = 1;
  const none = channelAnswers({
    videos: [...mixed(0, 2), moreItem("v2")], more: { v2: [video(vid("g9"), GAME_SONGS[9] + " piano")] },
    search: [video(vid("s0"), GAME_SONGS[5] + " piano")],
  });
  const bare = page({ route: "?channel=" + ANN + "&g=games", browse: (body) => (body.continuation === "v2" && fails-- > 0 ? 500 : none(body)) });
  await bare.settle();
  same([rowsOf(bare), browses(bare), bare.$("banners").hidden], [[], [VIDEOS_TAB, "v2"], false], "a first page with no song of the genre, and the failure");
  input.press(bare.$("banners").querySelectorAll("button").find((b) => words(b) === "Retry"));
  await bare.settle();
  same(browses(bare), [VIDEOS_TAB, "v2", "v2", SEARCH_TAB + " " + GAME_WORDS], "Retry: on from that page, not Videos from the start");
  same(rowsOf(bare), [vid("g9"), vid("s0")], "its song, then the search's");
});

Deno.test("app: the genre's radio after Videos takes the songs its search added, asking nothing", async () => {
  const route = "?channel=" + ANN + "&g=games";
  const pg = page({ route, browse: channelAnswers({ videos: mixed(3, 5), search: [video(vid("s0"), GAME_SONGS[5] + " piano")] }) });
  await pg.settle();
  same([rowsOf(pg), browses(pg)], [[...gameIds(3), vid("s0")], [VIDEOS_TAB, SEARCH_TAB + " " + GAME_WORDS]], "Videos, then the search");
  input.press(tabNamed(pg, "Videos"));
  await pg.settle();
  same(rowsOf(pg).length, 8, "Videos: the whole channel");
  input.press(tabNamed(pg, "Games"));
  await pg.settle();
  same([pg.location.search, rowsOf(pg), words(pg.$("convert-ticked"))], [route, [...gameIds(3), vid("s0")], "Convert 4"],
    "the genre's radio: the read's songs and the search's, each ticked");
  same(browses(pg), [VIDEOS_TAB, SEARCH_TAB + " " + GAME_WORDS], "with no request");
});

Deno.test("app: a channel read to its 5000 videos still takes the songs of the genre its search finds", async () => {
  const nocturnes = Array.from({ length: 4997 }, (_, i) => video(vid("n" + i), "Nocturne piano"));
  const pg = page({
    route: "?channel=" + ANN + "&g=games",
    browse: channelAnswers({ videos: [...mixed(3, 0), ...nocturnes, moreItem("v2")], search: [video(vid("s0"), GAME_SONGS[5] + " piano")] }),
  });
  await pg.settle();
  same(browses(pg), [VIDEOS_TAB, SEARCH_TAB + " " + GAME_WORDS], "Videos stops at 5000, then the search");
  same(rowsOf(pg), [...gameIds(3), vid("s0")], "the search's song is added");
});

Deno.test("app: on a channel's whole Videos opened from a genre, a row's channel and the dock's are the whole channel, and Similar is from its read", async () => {
  const route = "?channel=" + ANN + "&g=games&t=videos";
  const pg = page({ route, browse: channelAnswers() });
  await pg.settle();
  pg.row(vid("o0")).dispatchEvent(new FakeEvent("contextmenu"));
  input.press(pg.$("menu").querySelectorAll(".menu-item").find((b) => words(b) === "Ann"));
  await pg.settle();
  same([pg.location.search, rowsOf(pg).includes(vid("o0"))], ["?channel=" + ANN + "&s=popular", true],
    "a Nocturne's channel: the whole channel, with that Nocturne");
  pg.history.back();
  await pg.settle();
  input.press(pg.$("dock-channel"));
  await pg.settle();
  same(pg.location.search, "?channel=" + ANN + "&s=popular", "the dock's channel");
  const similar = page({ route, browse: channelAnswers() });
  await similar.settle();
  input.press(tabNamed(similar, "Similar"));
  await similar.settle();
  same([similar.location.search, browses(similar), similar.mixes()], ["?channel=" + ANN + "&g=games&t=similar", [VIDEOS_TAB], ["RD" + vid("g12")]],
    "Similar of the genre, from the Videos already read, its game song most viewed");
});

Deno.test("app: a genre's Similar of a channel lists only the genre's songs, and its strip, row menu and dock keep the genre", async () => {
  const [ben, cy, dee, eve] = [UC("b"), UC("c"), UC("d"), UC("e")];
  const chewie = P.curatedFor("games")[0];
  const pg = page({
    route: "?channel=" + ANN + "&g=games&t=similar", browse: channelAnswers(),
    mix: () => mixOf([panel(vid("g12"), GAME_SONGS[12] + " piano", ANN, "Ann"), panel(vid("m1"), GAME_SONGS[12] + " piano", ben, "Ben"),
      panel(vid("m2"), "Nocturne No. 2 piano", cy, "Cy"), panel(vid("m3"), GAME_SONGS[12] + " (Piano)", chewie.id, chewie.name)]),
    search: () => searchOf([by(vid("s1"), GAME_SONGS[12] + " (Piano Cover)", dee, "Dee"), by(vid("s2"), "Moonlight Sonata piano", eve, "Eve")]),
  });
  await pg.settle();
  same(rowsOf(pg).toSorted(), [vid("m1"), vid("m3"), vid("s1")], "the game songs, one per other pianist; not Cy's Nocturne or Eve's Sonata");
  same(cardsOf(pg).toSorted(), ["Ben", chewie.name, "Dee"].toSorted(), "the strip, of theirs");
  const cards = pg.$("strip").querySelectorAll(".card");
  const card = (name) => cards.find((c) => c.querySelector(".name").textContent === name);
  same(card(chewie.name).querySelector("img").src, P.avatarUrl(chewie.avatar), "a curated pianist's own picture, though the Mix gave none");
  input.press(card("Ben"));
  await pg.settle();
  same(pg.location.search, "?channel=" + ben + "&g=games&s=popular", "a card opens in the genre");
  pg.history.back();
  await pg.settle();
  pg.row(vid("s1")).dispatchEvent(new FakeEvent("contextmenu"));
  input.press(pg.$("menu").querySelectorAll(".menu-item").find((b) => words(b) === "Dee"));
  await pg.settle();
  same(pg.location.search, "?channel=" + dee + "&g=games&s=popular", "a row's channel opens in the genre");

  const scoped = page({ route: "?channel=" + ANN + "&g=games", browse: channelAnswers() });
  await scoped.settle();
  same(words(scoped.$("dock-channel")), "Ann", "the dock holds Ann's first song");
  input.press(scoped.$("dock-channel"));
  await scoped.settle();
  same(scoped.location.search, "?channel=" + ANN + "&g=games&s=popular", "the dock's channel, from a genre's songs of a channel");
  scoped.history.back();
  await scoped.settle();
  scoped.row(vid("g0")).dispatchEvent(new FakeEvent("contextmenu"));
  input.press(scoped.$("menu").querySelectorAll(".menu-item").find((b) => words(b) === "Ann"));
  await scoped.settle();
  same(scoped.location.search, "?channel=" + ANN + "&g=games&s=popular", "and a row's");
});
