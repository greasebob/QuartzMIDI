// frame_test.js: page/frame.js, the dock's /watch frame script, run against a made-up player and clock.
//   deno test --allow-read tools\mp3-to-midi\tests
const SOURCE = Deno.readTextFileSync(new URL("../page/frame.js", import.meta.url));

function check(cond, msg) {
  if (!cond) throw new Error(msg);
}
const same = (a, b, msg) => check(JSON.stringify(a) === JSON.stringify(b), msg + ": " + JSON.stringify(a) + " != " + JSON.stringify(b));

// A /watch frame with one ad showing: `video` is the ad's element, `player` is #movie_player. `skip` is
// YouTube's Skip button, laid out once `skip.shown` is set, with no offsetParent as when it is fixed; `at.hit`
// is what the frame finds at a point.
function frame({ hold = true, data = true } = {}) {
  const log = { plays: 0, playVideo: 0, loads: [], clicks: 0 };
  let now = 1000;
  let timer = null;
  let sheet = "";
  class Media {
    play() { // the browser's own play: an ad paused by the hold does not start on it
      log.plays++;
      if (this.takes) this.paused = false;
      return Promise.resolve();
    }
  }
  // closest() finds Skip only for a selector list that names Skip's class.
  const inSkip = (selector) => selector.split(",").map((s) => s.trim()).includes(".ytp-skip-ad-button");
  const video = Object.assign(Object.create(Media.prototype), {
    paused: true, ended: false, muted: false, playbackRate: 1, takes: false,
    duration: data ? 15 : NaN, readyState: data ? 4 : 0, currentTime: 0,
    pause() { this.paused = true; },
    closest: () => null,
  });
  const classes = new Set(["ad-showing"]);
  const player = {
    classList: { contains: (c) => classes.has(c) },
    querySelector: (s) => (s === "video" ? video : null),
    getVideoData: () => ({ video_id: "" }),
    getPlayerState: () => -1,
    getCurrentTime: () => 0,
    playVideo: () => { log.playVideo++; },
    loadVideoById: (o) => { log.loads.push({ at: now, ...o }); },
  };
  const label = { closest: (s) => (inSkip(s) ? skip : null) };
  const skip = {
    offsetParent: null,
    shown: false,
    rect: { left: 276, top: 109, width: 85, height: 36 },
    getBoundingClientRect() { return this.rect; },
    getClientRects() { return this.shown ? [this.rect] : []; },
    contains: (node) => node === skip || node === label,
    closest: (s) => (inSkip(s) ? skip : null),
    click: () => { log.clicks++; },
  };
  const at = { hit: label, points: [] };
  const listeners = {};
  const window = {
    __qmWant: { id: "abcdefghijk", start: 0 }, innerWidth: 383, innerHeight: 215, dispatchEvent() {},
    frameElement: { told: [], dispatchEvent(e) { this.told.push(e.type); } }, // the page's iframe
    addEventListener: (type, fn, capture) => { (listeners[type] = listeners[type] || []).push([fn, capture]); },
  };
  window.top = {};
  const document = {
    adoptedStyleSheets: [],
    getElementById: (id) => (id === "movie_player" ? player : null),
    querySelector: (s) => (s.includes(".ytp-skip-ad-button") ? skip : null),
    elementFromPoint: (x, y) => { at.points.push([x, y]); return at.hit; },
    addEventListener() {},
  };
  class Sheet { replaceSync(text) { sheet = text; } }
  const location = { hostname: "www.youtube.com", pathname: "/watch", search: "?v=abcdefghijk" + (hold ? "&qmhold=1" : "") };
  const timers = {
    setInterval: (fn) => { timer = fn; return 1; },
    clearInterval: () => { timer = null; },
  };
  // Elements and the window take focus as a browser's do, until frame.js has its say.
  class Html { focus() { log.focused.push(this.name); } }
  class Svg { focus() { log.focused.push(this.name); } }
  log.focused = [];
  window.focus = () => { log.focused.push("window"); };
  new Function("window", "location", "document", "CSSStyleSheet", "HTMLMediaElement", "HTMLElement", "SVGElement", "performance",
    "setInterval", "clearInterval", SOURCE)(window, location, document, Sheet, Media, Html, Svg, { now: () => now }, timers.setInterval,
    timers.clearInterval);
  // Time moves in the loop's own 200 ms steps.
  const run = (ms) => {
    for (let t = 0; t < ms; t += 200) {
      now += 200;
      if (timer) timer();
    }
  };
  // A pointer or mouse event of `type` on `target`, through the frame's capture listeners: what was blocked.
  // Trusted unless said otherwise, as real input and DevTools input are; a script's own events are not.
  const fire = (type, target, isTrusted = true) => {
    const e = { type, target, isTrusted, prevented: false, stopped: false };
    e.preventDefault = () => { e.prevented = true; };
    e.stopImmediatePropagation = () => { e.stopped = true; };
    for (const [fn, capture] of listeners[type] || []) {
      check(capture === true, type + " is not caught in the capture phase");
      fn(e);
    }
    return [e.prevented, e.stopped];
  };
  return { window, video, player, classes, log, run, skip, label, at, fire, Html, Svg, sheet: () => sheet, now: () => now };
}

// The rule whose selector list holds `selector`.
const ruleFor = (css, selector) =>
  css.split("\n").find((r) => r.slice(0, r.indexOf("{")).split(",").map((s) => s.trim()).includes(selector)) || "";

Deno.test("frame: the player fills the frame on black, and the rest is hidden", () => {
  const css = frame().sheet();
  for (const s of ["#cinematics", "#cinematics-container", ".iv-branding", ".ytp-watermark", ".branding-img-container", "#masthead-container"]) {
    check(/\{display:none!important\}$/.test(ruleFor(css, s)), s + " is not hidden: " + ruleFor(css, s));
  }
  const mp = ruleFor(css, "#movie_player");
  check(mp.includes("position:fixed!important") && mp.includes("background:#000!important"), "#movie_player: " + mp);
  same(ruleFor(css, "#movie_player .html5-video-container"),
    "#movie_player .html5-video-container{position:absolute!important;inset:0!important;width:100%!important;height:100%!important}",
    "video container");
  for (const s of ["ytd-app", "#page-manager"]) {
    check(/background:#000!important/.test(ruleFor(css, s)) && /margin-top:0!important/.test(ruleFor(css, s)), s + ": " + ruleFor(css, s));
  }
  check(/opacity:0!important/.test(ruleFor(css, "#movie_player.ad-showing video")), "the ad still plays unseen");
  same(ruleFor(css, "#movie_player.ad-showing .html5-video-container"), "#movie_player.ad-showing .html5-video-container{z-index:auto!important}",
    "during an ad the video is not stacked over Skip");
  // During an ad only Skip and what is in it take pointer input: both rules are !important, so the more specific
  // wins, and these selectors hold only ids, classes and *.
  const none = "#movie_player.ad-showing *";
  same(ruleFor(css, none), none + "{pointer-events:none!important}", "during an ad nothing of the player takes pointer input");
  const weight = (s) => (s.match(/#/g) || []).length * 100 + (s.match(/\./g) || []).length;
  for (const skip of [".ytp-skip-ad-button", ".ytp-ad-skip-button", ".ytp-ad-skip-button-modern"]) {
    for (const s of ["#movie_player.ad-showing " + skip, "#movie_player.ad-showing " + skip + " *"]) {
      check(/\{pointer-events:auto!important\}$/.test(ruleFor(css, s)) && weight(s) > weight(none), s + " takes no pointer input");
    }
  }
  const narrow = css.split("\n").find((r) => r.startsWith("@media (max-height:120px){")) || "";
  check(narrow.includes("#movie_player.ad-showing .ytp-skip-ad-button") && narrow.includes("position:fixed!important;left:0!important;top:0!important"),
    "a frame too short for YouTube's layout keeps Skip inside it: " + narrow);
});

Deno.test("frame: a held ad with data, once released, is played again and not loaded again", () => {
  const f = frame();
  f.run(1000);
  same([f.log.plays, f.log.loads.length], [0, 0], "held: nothing starts and nothing reloads");
  f.window.__qmHold = false;
  same([f.log.plays, f.log.playVideo], [1, 1], "the release starts the ad");
  f.run(800);
  same([f.log.plays, f.log.playVideo], [1, 1], "no second try within a second");
  f.run(400);
  same([f.log.plays, f.log.playVideo], [2, 2], "one second try after a second");
  f.run(1600);
  same([f.log.plays, f.log.loads.length], [2, 0], "no reload while it settles, and only one try");
  f.video.paused = false;
  f.run(3000);
  same(f.log.loads.length, 0, "an ad that plays is never loaded again");
});

Deno.test("frame: a released ad that has data but stays paused is a stall after the settling time", () => {
  const f = frame();
  f.window.__qmHold = false;
  f.run(3000);
  same(f.log.loads.length, 0, "settling");
  f.run(1400);
  same(f.log.loads.map((l) => l.videoId), ["abcdefghijk"], "loaded again once the settling time is over");
});

Deno.test("frame: a released ad without data is loaded again after a second's stall", () => {
  const f = frame({ data: false });
  f.window.__qmHold = false;
  const released = f.now();
  f.run(1000);
  same(f.log.loads.length, 0, "not before a second");
  f.run(400);
  same(f.log.loads.length, 1, "loaded again");
  same([f.log.loads[0].videoId, f.log.loads[0].startSeconds], ["abcdefghijk", 0], "the same video, at its time");
  check(f.log.loads[0].at - released <= 1400, "loaded " + (f.log.loads[0].at - released) + " ms after the release");
  f.run(1600);
  same(f.log.loads.length, 2, "at most twice");
  f.run(5000);
  same(f.log.loads.length, 2, "never a third time");
});

Deno.test("frame: an ad that plays runs muted at 16x and is never loaded again", () => {
  const f = frame({ hold: false });
  f.video.takes = true;
  f.video.paused = false;
  f.run(2000);
  same([f.video.muted, f.video.playbackRate, f.log.loads.length], [true, 16, 0], "muted at 16x, no reload");
});

// The ad plays at 16x: its time moves 3.2 s a tick until it reaches `end`, where it stays.
const playTo = (f, end) => {
  while (f.video.currentTime < end) {
    f.video.currentTime = Math.min(end, f.video.currentTime + 3.2);
    f.run(200);
  }
};

Deno.test("frame: an ad parked at its end, playing but not moving, is loaded again after a second", () => {
  const f = frame({ hold: false });
  f.video.paused = false;
  playTo(f, 14.8);
  const parked = f.now();
  f.run(1000);
  same(f.log.loads.length, 0, "not within a second");
  f.run(400);
  same(f.log.loads.map((l) => [l.videoId, l.startSeconds]), [["abcdefghijk", 0]], "loaded again, at the video's time");
  check(f.log.loads[0].at - parked <= 1400, "loaded " + (f.log.loads[0].at - parked) + " ms after it stopped");
  f.run(1600);
  same(f.log.loads.length, 2, "parked again: loaded a second time");
  f.run(5000);
  same(f.log.loads.length, 2, "never a third time");
});

Deno.test("frame: an ad that parks at its end right after Play is not waited out as settling", () => {
  const f = frame();
  f.video.takes = true;
  f.window.__qmHold = false;
  same(f.video.paused, false, "the release starts the ad");
  playTo(f, 15);
  f.run(1400);
  same(f.log.loads.length, 1, "loaded again within the 3 s after the release");
  same(f.log.plays, 1, "and not played again as a slow start");
});

Deno.test("frame: an ad is not parked while it moves, while it is short of its end, or while the user paused", () => {
  const f = frame({ hold: false });
  f.video.paused = false;
  for (let i = 0; i < 4; i++) {
    f.video.currentTime = 0;
    playTo(f, 14.9); // the next ad of the pod starts as this one ends
  }
  same(f.log.loads.length, 0, "an ad pod that keeps moving");
  f.video.currentTime = 14.5;
  f.run(3000);
  same(f.log.loads.length, 0, "0.5 s short of its end: buffering, not parked");
  f.window.__qmUserPaused = true;
  f.video.currentTime = 15;
  f.run(3000);
  same(f.log.loads.length, 0, "the user's pause holds it");
});

// The ad plays out and YouTube swaps in the video, which stays unstarted with no data until `ready`.
const adEnds = (f, ready = false) => {
  f.video.paused = false;
  playTo(f, 15);
  f.classes.delete("ad-showing");
  Object.assign(f.video, { duration: 213, currentTime: 0, readyState: ready ? 4 : 0 });
  f.player.getVideoData = () => ({ video_id: "abcdefghijk" });
  return f.now();
};

Deno.test("frame: a video held back after an ad, unstarted with no data, is loaded again after a second", () => {
  const f = frame({ hold: false });
  const ended = adEnds(f);
  f.run(1000);
  same(f.log.loads.length, 0, "not within a second");
  same([f.video.muted, f.video.playbackRate], [false, 1], "the ad's muting and rate are put back");
  f.run(400);
  same(f.log.loads.map((l) => [l.videoId, l.startSeconds]), [["abcdefghijk", 0]], "loaded again, at the video's time");
  check(f.log.loads[0].at - ended <= 1400, "loaded " + (f.log.loads[0].at - ended) + " ms after the ad");
  f.run(1600);
  same(f.log.loads.length, 2, "still held back: loaded a second time");
  f.run(5000);
  same(f.log.loads.length, 2, "never a third time");
});

Deno.test("frame: a video after an ad is not loaded again once it has data or starts, or while the user paused", () => {
  let f = frame({ hold: false });
  adEnds(f);
  f.run(600);
  f.video.readyState = 4;
  f.run(3000);
  same(f.log.loads.length, 0, "its data came within a second");
  f = frame({ hold: false });
  adEnds(f);
  f.player.getPlayerState = () => 3;
  f.run(3000);
  same(f.log.loads.length, 0, "buffering, not held back");
  f = frame({ hold: false });
  adEnds(f);
  f.window.__qmUserPaused = true;
  f.run(3000);
  same(f.log.loads.length, 0, "the user's pause holds it");
  f = frame({ hold: false });
  f.classes.delete("ad-showing");
  Object.assign(f.video, { paused: false, readyState: 0 });
  f.run(3000);
  same(f.log.loads.length, 0, "no ad before it: a slow start is left alone");
});

Deno.test("frame: __qmSkip says where a shown Skip is, in frame px, with its frame's size and its ad", () => {
  const f = frame({ hold: false });
  same(f.window.__qmSkip(), null, "Skip not shown yet");
  f.skip.shown = true;
  f.player.getVideoData = () => ({ video_id: "adadadadada" });
  f.video.currentSrc = "blob:https://www.youtube.com/1";
  same(f.window.__qmSkip(), { ad: "adadadadada|blob:https://www.youtube.com/1", x: 318.5, y: 127, width: 383, height: 215 }, "where, with no offsetParent as a fixed Skip has");
  same(f.at.points, [[318.5, 127]], "hit-tested at its centre");
  f.video.duration = NaN;
  same(f.window.__qmSkip().ad, "adadadadada|blob:https://www.youtube.com/1", "the same ad while its duration reads NaN");
  f.video.currentSrc = "blob:https://www.youtube.com/2";
  same(f.window.__qmSkip().ad, "adadadadada|blob:https://www.youtube.com/2", "the next ad of a pod is another ad");
  f.run(400);
  check(f.log.clicks > 0, "the scripted click stays");
});

Deno.test("frame: __qmSkip is null when the ad is held, paused by the user, gone, covered or off the frame", () => {
  const cases = {
    "held for prewarm": (f) => { f.window.__qmHold = true; },
    "paused by the user": (f) => { f.window.__qmUserPaused = true; },
    "no ad": (f) => { f.classes.delete("ad-showing"); },
    "hidden": (f) => { f.skip.shown = false; },
    "covered": (f) => { f.at.hit = f.video; },
    "nothing there": (f) => { f.at.hit = null; },
    "too small": (f) => { f.skip.rect = { left: 276, top: 109, width: 2, height: 36 }; },
    "off the frame": (f) => { f.skip.rect = { left: 380, top: 109, width: 85, height: 36 }; },
  };
  for (const [name, change] of Object.entries(cases)) {
    const f = frame({ hold: false });
    f.skip.shown = true;
    check(f.window.__qmSkip() !== null, name + ": shown before the change");
    change(f);
    same(f.window.__qmSkip(), null, name);
  }
  const held = frame();
  held.skip.shown = true;
  same(held.window.__qmSkip(), null, "a prewarmed frame starts held");
});

Deno.test("frame: pointer input reaches Skip during an ad and nothing else, and never moves focus", () => {
  const f = frame({ hold: false });
  for (const type of ["pointerdown", "mousedown", "pointerup", "mouseup", "click"]) {
    same(f.fire(type, f.label), [type === "mousedown", false], type + " on Skip during an ad");
    same(f.fire(type, f.video), [true, true], type + " on the ad's video");
  }
  for (const type of ["dblclick", "auxclick", "contextmenu"]) same(f.fire(type, f.video), [true, true], type + " on the ad's video");
  same(f.fire("click", {}), [true, true], "a click on something with no closest()");
  same(f.fire("click", null), [true, true], "a click with no target");
  same(f.window.__qmClicks, 1, "the clicks that reached Skip: only the trusted click on it during the ad");
  f.fire("click", f.label, false);
  same(f.window.__qmClicks, 1, "not a script's click on Skip");
  f.classes.delete("ad-showing");
  same(f.fire("click", f.label), [true, true], "a press that comes as the ad ends");
  same(f.fire("click", f.video), [true, true], "a press on the video");
  same(f.window.__qmClicks, 1, "nor a click after the ad");
});

Deno.test("frame: the frame never takes focus, and tells the page when it gets it anyway", () => {
  const f = frame({ hold: false });
  Object.assign(new f.Html(), { name: "movie_player" }).focus();
  Object.assign(new f.Svg(), { name: "an icon" }).focus();
  f.window.focus();
  same(f.log.focused, [], "YouTube's player, an SVG of it, and the frame's window ask for focus in vain");
  same(f.window.frameElement.told, [], "nothing to tell");
  f.fire("focus", f.player); // YouTube's player focuses itself, or the window comes forward with it focused
  f.fire("focus", f.window);
  same(f.window.frameElement.told, ["qm-focus", "qm-focus"], "each time, from the capture phase");
  f.window.frameElement = null; // a frame of no page, or of another origin: nothing to tell, and no error
  f.fire("focus", f.player);
});

Deno.test("frame: YouTube's own scripted events pass the pointer guard untouched", () => {
  const f = frame({ hold: false });
  for (const type of ["pointerdown", "mousedown", "pointerup", "mouseup", "click", "dblclick", "auxclick", "contextmenu"]) {
    same(f.fire(type, f.video, false), [false, false], type + " from a script on the ad's video");
    same(f.fire(type, f.label, false), [false, false], type + " from a script on Skip");
  }
  f.classes.delete("ad-showing");
  same(f.fire("click", f.video, false), [false, false], "a script's click on the video after the ad");
  same(f.fire("click", f.video), [true, true], "while a trusted one is still swallowed");
});

Deno.test("browse.py: its Skip check finds Skip from Skip, a text node in it, and its ::before and ::after", () => {
  const py = Deno.readTextFileSync(new URL("../browse.py", import.meta.url));
  const skip = py.match(/^SKIP = "([^"]+)"/m)[1];
  const parts = py.match(/^ON_SKIP = \(((?:\s*"[^"]*")+)\)/m)[1];
  const onSkip = new Function("return " + [...parts.matchAll(/"([^"]*)"/g)].map((m) => m[1]).join(""))();
  // closest() finds Skip from Skip or from inside it, for browse.py's selector list only. A CSSPseudoElement is
  // no node: it has no nodeType and gives its element.
  const element = (inside) => ({ nodeType: 1, closest: (s) => (inside && s === skip ? {} : null) });
  const button = element(true);
  const other = element(false);
  const cases = [
    ["Skip", button, true],
    ["a text node in Skip", { nodeType: 3, parentElement: button }, true],
    ["Skip's ::before", { element: button }, true],
    ["another element", other, false],
    ["a text node elsewhere", { nodeType: 3, parentElement: other }, false],
    ["another element's ::after", { element: other }, false],
    ["the document", { nodeType: 9, parentElement: null }, false],
    ["a text node out of the document", { nodeType: 3, parentElement: null }, false],
    ["a ::before whose element has gone", { element: null }, false],
    ["an element field that is no element", { element: { closest: () => ({}) } }, false],
    ["something that is no node", {}, false],
  ];
  for (const [name, node, want] of cases) same(onSkip.call(node, skip), want, name);
  same(onSkip.call(button, ".ytp-play-button"), false, "Skip, asked with another selector list");
});
