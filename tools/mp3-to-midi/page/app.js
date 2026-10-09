// The YouTube window's page (docs/design/youtube-window.md §2, §3): QuartzMIDI's own UI at the YouTube
// origin. Every element is built with createElement; YouTube is read through QMParse; browse.py is reached
// through pywebview.api, and QuartzMIDI's messages arrive through qm.receive. The dock's player (frame.js
// and the /watch frame, §1.7 and §3.6) fills in QMApp.player.
(() => {
  "use strict";
  const P = globalThis.QMParse;
  const SURFACE = globalThis.QMBoot ? "canvas" : "served";
  const ORIGIN = "https://www.youtube.com";
  const WATCH = ORIGIN + "/watch?v=";
  const VIDEO = /^[A-Za-z0-9_-]{11}$/;
  const LIST = /^[A-Za-z0-9_-]{2,64}$/;
  const UCID = /^UC[A-Za-z0-9_-]{22}$/;
  const CAP = 5000; // rows read for a playlist or a channel, and items in one pick
  const MIX_CAP = 100; // unique videos read for a Mix
  const VIEWS = 20; // views kept for Back and Forward
  const ACK = 3000; // ms before an unanswered pick shows Retry (protocol 2)
  const GUARD = 400; // ms in which a second click on the same Convert is ignored
  const AUDITION = 250; // ms of no movement before a focused video plays
  const LISTS = new Set(["playlist", "channel", "mix", "selection"]);
  const QUEUE = new Set(["sent", "queued", "reading", "downloading", "transcribing", "list"]);
  const DURATIONS = [["any", "Any length"], ["short", "Under 3 min"], ["medium", "3–20 min"], ["long", "Over 20 min"]];
  const SORTS = [["relevance", "Relevance"], ["popular", "Popular"]];
  const CHANNEL_SORTS = [["latest", "Latest"], ["popular", "Popular"], ["oldest", "Oldest"]];
  const GENRE_SORTS = [["top", "Top", "trending-up"], ["new", "New", "calendar"]];
  const GENRES = new Map(((P && P.GENRES) || []).map((g) => [g.key, g]));
  // Videos of 3 to 20 minutes: by relevance, uploaded this week, or this month when the week is thin (§2.3).
  const TOP = "EgQQARgF";
  const WEEK = "EgYIAxABGAU=";
  const MONTH = "EgYIBBABGAU=";
  const THIN = 6; // counted rows on a This week page 1 under which This month is asked
  const SHELF = 6; // rows on a Home shelf
  const FLIGHT = 2; // Home's requests at once
  const MEMO = 600000; // ms a discovery page is kept
  const MEMOS = 40;
  const WAIT = 1500; // ms Home waits for artists.json before it starts cold
  const ARTISTS = 300; // qm-artists' size
  const RECENT = 5; // qm-dests' size
  const OWN = 6; // your pianists first in Home's strip
  const HOME_CURATED = 12; // curated pianists after them
  const CARDS = 24; // a strip's cards, all shown: wrapped wide, on one line that scrolls narrow
  const FEW = 12; // a pianist's songs in a genre under which the channel's own search adds more
  const FILL_PAGES = 3; // pages of that search at most
  const UNBIASED = new Set(["home", "genre", "similar"]); // no typed query for the Piano pill to bias

  // ---- helpers

  const root = document.documentElement;
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, ...kids) => {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    for (const kid of kids) if (kid != null && kid !== false) node.append(kid);
    return node;
  };
  const NS = "http://www.w3.org/2000/svg";
  const icon = (name, cls) => {
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("aria-hidden", "true");
    if (cls) svg.setAttribute("class", cls);
    const use = document.createElementNS(NS, "use");
    use.setAttribute("href", "#i-" + name);
    svg.append(use);
    return svg;
  };
  const setIcon = (svg, name) => {
    const use = svg && svg.firstElementChild;
    if (use && use.getAttribute("href") !== "#i-" + name) use.setAttribute("href", "#i-" + name);
  };
  const button = (cls, name, text, title) => {
    const b = el("button", cls);
    b.type = "button";
    if (name) b.append(icon(name));
    if (text != null) b.append(el("span", null, text));
    if (title) named(b, title);
    return b;
  };
  // Icon-only buttons carry their name as the title, as Panels' IconButton does.
  const named = (b, title) => {
    if (b.title !== title) { b.title = title; b.setAttribute("aria-label", title); }
  };
  const read = (key, fallback) => {
    try {
      const value = localStorage.getItem(key);
      return value == null ? fallback : value;
    } catch {
      return fallback;
    }
  };
  const write = (key, value) => {
    try { localStorage.setItem(key, String(value)); } catch { /* storage is off: the setting lasts this session */ }
  };
  const readJson = (key) => {
    try { return JSON.parse(read(key, "null")); } catch { return null; }
  };
  const time = (at) => {
    const t = new Date(at).getTime();
    return Number.isFinite(t) ? t : 0;
  };
  const clock = (seconds) => {
    const s = Math.max(0, Math.floor(seconds || 0));
    const h = Math.floor(s / 3600);
    const m = Math.floor(s / 60) % 60;
    const rest = String(s % 60).padStart(2, "0");
    return h ? h + ":" + String(m).padStart(2, "0") + ":" + rest : m + ":" + rest;
  };
  const mq = (id) => "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg";
  const fileName = (path) => String(path || "").split(/[\\/]/).pop();
  const typing = (node) => !!node && (node.tagName === "TEXTAREA" || node.isContentEditable ||
    (node.tagName === "INPUT" && !["checkbox", "radio", "range", "button"].includes(node.type)));
  const label = (pairs, value) => (pairs.find((p) => p[0] === value) || pairs[0])[1];

  // ---- browse.py

  const call = async (name, ...args) => {
    const api = globalThis.pywebview && globalThis.pywebview.api;
    if (!api || typeof api[name] !== "function") return null;
    try { return await api[name](...args); } catch { return null; }
  };
  const logged = new Set();
  const drift = (texts) => {
    for (const text of texts || []) {
      if (text && !logged.has(text)) { logged.add(text); call("log", "drift", String(text)); }
    }
  };
  window.addEventListener("error", (e) =>
    call("log", "script", (e.message || "error") + (e.filename ? " at " + fileName(e.filename) + ":" + e.lineno : "")));
  window.addEventListener("unhandledrejection", (e) =>
    call("log", "script", "unhandled " + ((e.reason && e.reason.message) || String(e.reason))));

  // ---- theme (§4.2)

  const DARK = {
    canvas: "#191C21", structure: "#20242A", card: "#272B31", elevated: "#2E333A", elevatedHot: "#363C44",
    recessed: "#14171B", inkPrimary: "#E7E9EC", inkSecondary: "#A3AAB5", inkTertiary: "#79818D", accent: "#4EA3EA",
    ok: "#4FB277", okInk: "#8FDCAA", warn: "#D0A259", bad: "#E0685A", accentSoft: "#4EA3EA24", accentLine: "#4EA3EA66",
    okSoft: "#4FB27729", okBorder: "#4FB27766", hairline: "#FFFFFF14", strong: "#FFFFFF24", topHighlight: "#FFFFFF0F",
    contact: "#0000004D", ambient: "#00000038", inner: "#00000073",
  };
  const SHAPE = {
    scale: 1, controlHeight: 32, textBody: 14, textMeta: 12, textHeading: 14, textTitle: 20, radiusWindow: 12,
    radiusCard: 12, radiusControl: 10, radiusElement: 8, windowPad: 16, panelPad: 16, contactOffset: 1, contactBlur: 1,
    ambientOffset: 2, ambientBlur: 6, innerOffset: 1, innerBlur: 2,
  };
  const COLOUR_VARS = [
    ["--canvas", "canvas"], ["--bar", "structure"], ["--card", "card"], ["--raised", "elevated"], ["--hot", "elevatedHot"],
    ["--well", "recessed"], ["--ink", "inkPrimary"], ["--ink2", "inkSecondary"], ["--ink3", "inkTertiary"],
    ["--accent", "accent"], ["--accent-soft", "accentSoft"], ["--accent-line", "accentLine"], ["--ok", "ok"],
    ["--ok-ink", "okInk"], ["--ok-soft", "okSoft"], ["--ok-line", "okBorder"], ["--warn", "warn"], ["--bad", "bad"],
    ["--line", "hairline"], ["--line2", "strong"],
  ];
  const SHAPE_VARS = [
    ["--h", "controlHeight"], ["--r-window", "radiusWindow"], ["--r-card", "radiusCard"], ["--r-control", "radiusControl"],
    ["--r-element", "radiusElement"], ["--pad-window", "windowPad"], ["--pad-panel", "panelPad"], ["--fs-meta", "textMeta"],
    ["--fs-body", "textBody"], ["--fs-heading", "textHeading"], ["--fs-title", "textTitle"],
  ];
  const HEX = /^#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?$/;
  const channels = (hex) => [1, 3, 5].map((at) => parseInt(hex.slice(at, at + 2), 16));
  const alpha = (hex) => (hex.length > 7 ? parseInt(hex.slice(7, 9), 16) / 255 : 1);
  const luminance = (hex) => {
    const [r, g, b] = channels(hex).map((c) => {
      const v = c / 255;
      return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
    });
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  };
  // #FFFFFF or #101218, whichever has the higher WCAG contrast (browse.py ink_on).
  const inkOn = (hex) => {
    const base = luminance(hex);
    const contrast = (other) => (Math.max(base, other) + 0.05) / (Math.min(base, other) + 0.05);
    return contrast(1) >= contrast(luminance("#101218")) ? "#FFFFFF" : "#101218";
  };
  // A translucent colour made opaque over another, as StatePill's fill (Panels.cpp:893-894).
  const over = (top, under) => {
    const a = alpha(top);
    const t = channels(top);
    const u = channels(under);
    return "#" + t.map((c, i) => Math.round(c * a + u[i] * (1 - a)).toString(16).padStart(2, "0")).join("").toUpperCase();
  };
  let scale = 1;
  const applySkin = (skin) => {
    if (!skin || typeof skin !== "object") return;
    const given = skin.colours && typeof skin.colours === "object" ? skin.colours : {};
    const shape = skin.shape && typeof skin.shape === "object" ? skin.shape : {};
    const c = (key) => (typeof given[key] === "string" && HEX.test(given[key]) ? given[key] : DARK[key]);
    const n = (key) => {
      const v = shape[key];
      return typeof v === "number" && Number.isFinite(v) && v >= 0 && v <= 1000 ? v : SHAPE[key];
    };
    const set = (name, value) => root.style.setProperty(name, value);
    for (const [name, key] of COLOUR_VARS) set(name, c(key));
    set("--accent-hot", "color-mix(in srgb, var(--accent) 85%, var(--ink))");
    set("--accent-ink", inkOn(c("accent")));
    set("--warn-ink", inkOn(c("warn")));
    set("--ok-fill", over(c("okSoft"), c("structure")));
    set("--ok-hot", over(c("okSoft"), c("elevatedHot")));
    set("--hi", "inset 0 1px 0 " + c("topHighlight"));
    set("--raise", `0 ${n("contactOffset")}px ${n("contactBlur")}px ${c("contact")}, 0 ${n("ambientOffset")}px ${n("ambientBlur")}px ${c("ambient")}`);
    set("--sunk", `inset 0 ${n("innerOffset")}px ${n("innerBlur")}px ${c("inner")}`);
    for (const [name, key] of SHAPE_VARS) set(name, n(key) + "px");
    root.style.colorScheme = skin.dark === false ? "light" : "dark";
    scale = n("scale") || 1;
    root.style.zoom = String(scale);
    layout();
    measure();
  };
  // CSS media queries ignore zoom, so the breakpoint is a class (§2). Crossing it shrinks the frame, collapses
  // the sheet and repaints every Convert control in its other form.
  const layout = () => {
    const narrow = innerWidth / scale < 900;
    if (root.classList.contains("narrow") === narrow) return;
    root.classList.toggle("narrow", narrow);
    if (!ui) return;
    if (S.enlarged) enlarge(false);
    setSheet(false);
    if (cur) for (const node of cur.nodes.values()) node.sig = null;
    measure();
    schedule();
  };

  // ---- state

  const S = {
    protocol: 0, // 0 until init; then 1 or 2
    signedIn: false,
    account: null,
    client: null, // a promise of the InnerTube client context
    sts: null, // a promise of the player's signature timestamp
    piano: read("qm-piano", "1") !== "0",
    volume: Math.max(0, Math.min(100, Number(read("qm-volume", "100")) || 0)),
    muted: read("qm-muted", "0") === "1",
    converted: new Map(), // video id -> .mid path
    titles: new Map(), // video id -> title, for the Converted list
    items: new Map(), // key -> the app's latest item message
    children: new Map(), // parent + "/" + v:ID -> the app's latest child message
    pending: new Map(), // key -> {failed, timer}: picks sent and not yet answered
    clicked: new Map(), // key -> ms of the last Convert click (the 400 ms guard)
    picks: new Map(), // key -> the pick sent, for Retry and the tray's thumbnails
    cover: new Map(), // video id -> {key, folder, dest} of the list or selection pick that holds it
    sent: new Set(), // protocol 1: keys sent this session
    reading: new Map(), // list key -> rows read so far, while a list row's Convert reads its pages
    notes: new Map(), // list key -> status line ("No piano videos.")
    done: [], // this session's converted files, newest first
    paused: null, // "signin" or "folder"
    transcribing: false,
    dock: null, // the row in the dock
    nowId: null, // the video the user chose to play
    lastSearch: null, // the route of the last search view shown
    artists: new Map(), // channel id -> {name, avatar, dest, at}: your pianists, qm-artists and artists.json (§4.1)
    seeds: [], // artists.json's seeds: your newest conversions (§2.4)
    folders: null, // the MIDI folder's folders as the app sends them; null until it does, and no Save to (§4.2)
    onDisk: new Map(), // a folder's folded path -> its path on disk
    listed: 0, // how many times the folders came
    made: new Set(), // dests picks were sent to that the app has not listed yet
    dests: [], // qm-dests: the folders chosen last, newest first
    dockDest: null, // {id, dest}: the folder chosen for the docked video
    refused: false, // browse.py refused a pick's folder
    pianists: {}, // channel id -> {rec, rows}: what this session read of each channel (§2.2)
    turn: 0, // qm-turn: this start's turn of each genre's curated pianists after its first 9 (§3.19 Turns)
    temp: 0,
    dragging: false,
    enlarged: false,
    sheet: false, // narrow: the bottom sheet is expanded (§2.6)
  };

  // The dock's player (§1.7, §3.6): one same-origin /watch frame, never moved, that takes pointer input only
  // for the trusted press on Skip. The page drives YouTube's player in it through #movie_player; frame.js,
  // inside it, fills the frame, holds a prewarmed video and gets ads out of the way. The page reads id, state,
  // time, duration, overlay and showing, and the player calls changed() whenever one of them moves.
  const POLL = 250; // ms between reads while the frame plays, buffers or shows an ad
  const RUNNING = new Set(["loading", "playing", "buffering", "ad"]);
  const resume = new Map(); // video id -> the last time played, for this session
  const F = {
    held: false, // the frame holds a prewarmed video, muted and paused, never played
    broken: false, // the frame's document has no player: the next load sets src again
    stale: null, // the document a src change is replacing
    tuned: null, // the #movie_player the volume was last set on
    started: false, // the video showed a frame since it was loaded
    since: 0, // ms the load started
    adSince: 0, // ms the ad showed
    endedAt: 0,
    bad: 0, // polls in a row with state -1 and a playability status that is not OK
    timer: 0,
    heard: null, // the frame document whose media events wake the polling
    pressing: false, // a press on Skip is under way
    point: null, // [x, y] in whole viewport px, where browse.py presses
    skipped: null, // [frame document, ad] of the last ad whose Skip was pressed
  };
  const movie = () => {
    try {
      const doc = ui.player.contentDocument;
      if (doc === F.stale) return null;
      F.stale = null;
      const p = doc && doc.getElementById("movie_player");
      return p && typeof p.getPlayerState === "function" ? p : null;
    } catch {
      return null;
    }
  };
  const flags = (values) => {
    try { Object.assign(ui.player.contentWindow, values); } catch { /* the frame is not ours */ }
  };
  const source = (src) => {
    try { F.stale = ui.player.contentDocument; } catch { F.stale = null; }
    Object.assign(F, { broken: false, tuned: null });
    ui.player.src = src;
  };
  // YouTube's own volume and mute; the element's muted too, since the hold sets it directly.
  const tune = (p) => {
    if (F.held || p.classList.contains("ad-showing")) return;
    p.setVolume(S.volume);
    if (S.muted) p.mute(); else p.unMute();
    const video = p.querySelector("video");
    if (video) video.muted = S.muted;
    F.tuned = p;
  };
  const wake = () => {
    if (!F.timer) F.timer = setTimeout(poll, POLL);
  };
  // While paused nothing polls, so the frame's media events wake it: a resume YouTube makes, or autonav
  // loading another video after the end, however late.
  const listen = (doc) => {
    if (F.heard === doc) return;
    F.heard = doc;
    for (const type of ["play", "pause", "loadstart", "ended"]) doc.addEventListener(type, wake, true);
  };
  const poll = () => {
    F.timer = 0;
    const now = Date.now();
    const p = movie();
    if (p) {
      if (!F.held && player.state !== "idle" && player.state !== "unplayable") follow(p, now);
    } else if (player.state === "loading" && now - F.since > 10000) {
      drift(["#movie_player missing after 10 s"]);
      F.broken = true;
      Object.assign(player, { state: "idle", showing: false });
      player.changed();
    }
    if (RUNNING.has(player.state) || (player.state === "ended" && now - F.endedAt < 2000)) wake();
  };
  // §3.6, after loading: the frame's own playabilityStatus, when the state stays -1, or a live or upcoming
  // video the player endpoint did not report.
  const refused = (p, id, code) => {
    const response = p.getPlayerResponse && p.getPlayerResponse();
    const got = response ? P.parsePlayer(response) : null;
    // A private or removed video's response has no videoDetails; another id is the last video's.
    if (!got || (got.id && got.id !== id)) return null;
    if (got.live || got.upcoming) return got;
    if (code === -1 && got.status && got.status !== "OK") return ++F.bad >= 2 ? got : null;
    F.bad = 0;
    return null;
  };
  const unplayable = (p, got) => {
    if (got.overlay && got.playable !== null) mark(player.id, got);
    p.pauseVideo();
    flags({ __qmUserPaused: true });
    Object.assign(player, { state: "unplayable", overlay: got.overlay || "Not available", showing: false });
  };
  // §3.6: YouTube ignores a scripted click on Skip, so browse.py presses it with trusted input, once per ad and
  // only after Play: never while the frame is held, user-paused or cued. frame.js says where Skip is in the
  // frame; the point is that, through the frame's box. The frame takes pointer input only for the press, and the
  // point must hit the frame itself, never the poster, the ad overlay or anything else of ours over it. An ad
  // counts as pressed only when browse.py says it pressed and frame.js saw the click reach Skip; a refused press,
  // or one the layout sent elsewhere, is asked again on a later poll. F.point is where the press goes, so the page
  // can swallow a press the layout moved onto it.
  const skip = async (p) => {
    if (F.pressing || F.held || player.state !== "ad") return;
    let w = null;
    try { w = ui.player.contentWindow; } catch { return; }
    if (!w || w.__qmHold || w.__qmUserPaused || typeof w.__qmSkip !== "function") return;
    const at = w.__qmSkip();
    const doc = p.ownerDocument;
    if (!at || (F.skipped && F.skipped[0] === doc && F.skipped[1] === at.ad)) return;
    F.pressing = true;
    const focused = document.activeElement;
    ui.player.style.pointerEvents = "auto";
    try {
      const box = ui.player.getBoundingClientRect();
      const x = box.left + (at.x * box.width) / at.width;
      const y = box.top + (at.y * box.height) / at.height;
      if (document.elementFromPoint(x, y) === ui.player) {
        F.point = [Math.floor(x), Math.floor(y)];
        const clicks = w.__qmClicks;
        if ((await call("press", x, y)) === true && w.__qmClicks > clicks) F.skipped = [doc, at.ad];
      }
    } finally {
      F.point = null;
      ui.player.style.pointerEvents = "none";
      if (document.activeElement === ui.player) {
        if (focused && focused !== ui.player && typeof focused.focus === "function") focused.focus({ preventScroll: true });
        else ui.player.blur();
      }
      F.pressing = false;
    }
  };
  const follow = (p, now) => {
    const id = player.id;
    const before = [player.state, player.time, player.duration, player.showing].join();
    listen(p.ownerDocument);
    // YouTube may load the frame again on its own, as it does with themeRefresh=1 after a fresh profile's passive
    // sign-in check. The new document reads qmhold=1 from the prewarm's address though Play let the video go,
    // so it is let go again, and played unless the user paused it or it ended.
    let w = null;
    try { w = ui.player.contentWindow; } catch { /* not ours */ }
    if (w && w.__qmHold) {
      const paused = !RUNNING.has(player.state);
      flags({ __qmUserPaused: paused, __qmHold: false });
      if (!paused) p.playVideo();
    }
    let sent = false;
    try { sent = ui.player.contentWindow.__qmSent === "accounts"; } catch { /* not ours */ }
    if (sent) unplayable(p, { overlay: "Age-restricted", playable: false });
    else if (p.classList.contains("ad-showing")) {
      // An ad counts as loading, never as failure; the dock shows it without words.
      if (!F.adSince) F.adSince = now;
      if (now - F.adSince > 30000) drift(["an ad showed for over 30 s"]);
      if (player.state !== "paused") Object.assign(player, { state: "ad", showing: false });
      skip(p);
    } else {
      // A new player, or an ad's end, which puts back the muting it found (the hold's, perhaps).
      const video = p.querySelector("video");
      if (F.adSince || F.tuned !== p || (video && video.muted !== S.muted)) tune(p);
      F.adSince = 0;
      const code = p.getPlayerState();
      const vid = (p.getVideoData() || {}).video_id;
      if (player.state === "ended") {
        // Autonav: the wanted video goes back in, cued at its end and paused.
        if (vid && vid !== id) {
          flags({ __qmUserPaused: true });
          p.cueVideoById({ videoId: id, startSeconds: Math.max(0, player.duration - 0.1) });
          player.showing = false;
        }
      } else if (!vid || vid === id) {
        const got = F.started || code === 1 ? null : refused(p, id, code);
        if (got) unplayable(p, got);
        else {
          if (code === 1) F.started = true;
          if (F.started && code >= 1 && code <= 3) {
            player.time = p.getCurrentTime() || 0;
            resume.set(id, player.time);
          }
          player.duration = p.getDuration() || player.duration;
          if (code === 0) {
            p.pauseVideo();
            flags({ __qmUserPaused: true });
            F.endedAt = now;
            resume.delete(id);
            Object.assign(player, { state: "ended", time: player.duration, showing: true });
          } else if (code === 1) Object.assign(player, { state: "playing", showing: true });
          else if (code === 2) Object.assign(player, { state: "paused", showing: F.started });
          else if (code === 3) Object.assign(player, { state: F.started ? "buffering" : "loading" });
          // The ad is over, though YouTube may keep the video unstarted for a while.
          else if (player.state === "ad") Object.assign(player, { state: F.started ? "buffering" : "loading" });
        }
      }
    }
    if ([player.state, player.time, player.duration, player.showing].join() !== before) player.changed();
  };
  const player = {
    id: null,
    state: "idle", // idle, cued, loading, playing, buffering, paused, ended, ad or unplayable
    time: 0,
    duration: 0,
    overlay: null, // an overlay word the frame found (§2.9)
    showing: false, // the frame shows the video, so the poster hides
    // Prewarm: the video waits in the frame, muted and paused, until Play releases it.
    cue(row, start = 0) {
      // The frame already holds it there, as on a ?v= route, where take()'s prewarm cues it before loadVideo does.
      if (F.held && this.state === "cued" && this.id === row.id && this.time === start) return;
      const p = movie();
      Object.assign(this, { id: row.id, state: "cued", time: start, duration: row.seconds || 0, overlay: null, showing: false });
      if (p && F.held) {
        flags({ __qmWant: { id: row.id, start } });
        p.cueVideoById({ videoId: row.id, startSeconds: start });
      } else source("/watch?v=" + row.id + "&qmhold=1" + (start ? "&t=" + Math.floor(start) + "s" : ""));
      F.held = true;
      if (start) resume.set(row.id, start);
      this.changed();
    },
    play(row) {
      const id = row.id;
      const p = movie();
      const loaded = p && !F.broken && (p.getVideoData() || {}).video_id === id;
      if (loaded && !F.held && this.id === id && this.state !== "unplayable" && this.state !== "idle") {
        if (!RUNNING.has(this.state)) this.toggle();
        return;
      }
      const start = resume.get(id) || 0;
      Object.assign(this, { id, state: "loading", time: start, duration: row.seconds || 0, overlay: null, showing: false });
      Object.assign(F, { started: false, since: Date.now(), adSince: 0, endedAt: 0, bad: 0 });
      if (p && !F.broken) {
        const wasHeld = F.held;
        F.held = false;
        flags({ __qmHold: false, __qmUserPaused: false, __qmWant: { id, start }, __qmSent: null });
        tune(p);
        if (wasHeld && loaded) {
          if (start) p.seekTo(start, true);
          p.playVideo();
        } else p.loadVideoById({ videoId: id, startSeconds: start });
      } else {
        F.held = false;
        source("/watch?v=" + id + "&t=" + Math.floor(start) + "s");
      }
      this.changed();
      wake();
    },
    toggle() {
      const p = movie();
      if (this.state === "unplayable") return;
      if (RUNNING.has(this.state)) {
        if (!p) { this.cue({ id: this.id, seconds: this.duration }, this.time); return; } // the frame is still loading
        flags({ __qmUserPaused: true });
        p.pauseVideo();
        Object.assign(this, { state: "paused", showing: F.started });
      } else if (!p) {
        if (S.dock && S.dock.id === this.id) this.play(S.dock);
        return;
      } else {
        flags({ __qmUserPaused: false });
        // Another video (autonav), or ours cued at its end after autonav, loads again.
        if ((p.getVideoData() || {}).video_id !== this.id || p.getPlayerState() === 5) {
          p.loadVideoById({ videoId: this.id, startSeconds: this.state === "ended" ? 0 : this.time });
          Object.assign(F, { started: false, since: Date.now(), bad: 0 });
        } else {
          if (this.state === "ended") p.seekTo(0, true);
          p.playVideo();
        }
        Object.assign(this, { state: F.started ? "playing" : "loading", showing: F.started });
        wake();
      }
      this.changed();
    },
    seek(seconds) {
      const to = Math.max(0, seconds);
      if (this.state === "cued") { // the held video starts there when played
        this.time = to;
        resume.set(this.id, to);
        this.changed();
        return;
      }
      const p = movie();
      if (!p || this.state === "ad" || this.state === "unplayable" || p.classList.contains("ad-showing")) return;
      p.seekTo(to, true);
      this.time = to;
      resume.set(this.id, to);
      if (this.state === "ended") Object.assign(this, { state: "paused", showing: true });
      this.changed();
    },
    volume() {
      const p = movie();
      if (p) tune(p);
    },
    stop() { // the dock's video cannot play: nothing else plays either
      const p = movie();
      if (p && !F.held && RUNNING.has(this.state)) {
        flags({ __qmUserPaused: true });
        p.pauseVideo();
      }
      Object.assign(this, { state: "idle", showing: false });
      this.changed();
    },
    // The app reports a running entry in phase transcribe: a frame prewarmed but never played lets go.
    busy(on) {
      if (!on || !F.held) return;
      F.held = false;
      source("about:blank");
      if (this.state === "cued") Object.assign(this, { state: "idle", showing: false });
      this.changed();
    },
    changed() { scheduleDock(); },
  };

  // ---- routes (§3.1, §3.14)

  const defaults = () => ({ view: "search", f: "videos", d: "any", s: "relevance", p: S.piano ? "1" : "0" });
  const parseRoute = (text) => {
    const u = new URLSearchParams(String(text || "").replace(/^#?\/?/, ""));
    const g = (key) => (u.get(key) || "").trim();
    if (VIDEO.test(g("mix"))) {
      const list = /^RD[A-Za-z0-9_-]+$/.test(g("list")) && g("list") !== "RD" + g("mix") ? g("list") : "";
      return { view: "mix", mix: g("mix"), list };
    }
    if (LIST.test(g("list")) && !g("list").startsWith("RD")) return { view: "playlist", list: g("list"), v: VIDEO.test(g("v")) ? g("v") : "" };
    if (g("channel")) {
      // g: opened from a genre, whose songs alone show on the "genre" tab, its default (§3.19 Genre scope).
      const scope = GENRES.has(g("g")) ? g("g") : "";
      const tabs = scope ? ["genre", "videos", "playlists", "similar"] : ["videos", "playlists", "similar"];
      const t = tabs.includes(g("t")) ? g("t") : scope ? "genre" : "videos";
      return { view: "channel", channel: g("channel"), ...(scope && { g: scope }), t,
        s: ["popular", "oldest"].includes(g("s")) && t !== "similar" ? g("s") : "latest" };
    }
    if (VIDEO.test(g("v"))) return { view: "video", v: g("v") };
    if (VIDEO.test(g("similar"))) return { view: "similar", similar: g("similar") };
    if (GENRES.has(g("g"))) return { view: "genre", g: g("g"), s: g("s") === "new" ? "new" : "top" };
    if (!g("q")) return { view: "home" };
    const d = defaults();
    return {
      view: "search",
      q: g("q"),
      f: ["videos", "playlists", "channels"].includes(g("f")) ? g("f") : d.f,
      d: DURATIONS.some((x) => x[0] === g("d")) ? g("d") : d.d,
      s: g("s") === "popular" ? "popular" : d.s,
      p: u.has("p") ? (g("p") === "0" ? "0" : "1") : d.p,
    };
  };
  // Home is /quartzmidi itself (§3.4).
  const routeString = (r) => {
    const u = new URLSearchParams();
    if (r.view === "home") return "";
    if (r.view === "search") for (const key of ["q", "f", "d", "s", "p"]) u.set(key, r[key]);
    else if (r.view === "playlist") { u.set("list", r.list); if (r.v) u.set("v", r.v); }
    else if (r.view === "channel") {
      u.set("channel", r.channel);
      if (r.g) u.set("g", r.g);
      if (r.t !== (r.g ? "genre" : "videos")) u.set("t", r.t);
      if (r.s && r.s !== "latest" && r.t !== "similar") u.set("s", r.s);
    } else if (r.view === "mix") { u.set("mix", r.mix); if (r.list) u.set("list", r.list); }
    else if (r.view === "video") u.set("v", r.v);
    else if (r.view === "similar") u.set("similar", r.similar);
    else if (r.view === "genre") { u.set("g", r.g); if (r.s === "new") u.set("s", "new"); }
    return "?" + u;
  };
  const here = () => (SURFACE === "canvas" ? location.hash || (globalThis.QMBoot && globalThis.QMBoot.route) || "" : location.search);
  const urlOf = (route) => (SURFACE === "canvas" ? "#/" + route : "/quartzmidi" + route);
  const youtubeUrl = (r) => {
    if (r.view === "video") return WATCH + r.v;
    if (r.view === "playlist") return r.v ? WATCH + r.v + "&list=" + r.list : ORIGIN + "/playlist?list=" + r.list;
    if (r.view === "mix") return WATCH + r.mix + "&list=" + (r.list || "RD" + r.mix);
    if (r.view === "channel") {
      const base = UCID.test(r.channel) ? ORIGIN + "/channel/" + r.channel : ORIGIN + "/" + encodeURI(r.channel);
      return r.t === "similar" ? base : base + "/" + (r.t === "playlists" ? "playlists" : "videos");
    }
    if (r.view === "similar") return WATCH + r.similar + "&list=RD" + r.similar;
    if (r.view === "genre") return ORIGIN + P.resultsUrl(GENRES.get(r.g).query, r.s === "new" ? WEEK : TOP);
    if (r.view === "home") return ORIGIN + "/";
    return ORIGIN + "/results?search_query=" + encodeURIComponent(r.q || "");
  };
  const seedOf = (row) => (VIDEO.test(row.videoId || "") ? row.videoId : row.id.slice(2));
  const listKey = (row) => (row.kind === "mix" ? "m:" + seedOf(row) : "p:" + row.id);
  const mixTitle = (title) => (!title ? "Mix" : /^mix\b/i.test(title) ? title : "Mix - " + title);
  const linkOf = (row) => {
    if (row.kind === "video") return WATCH + row.id;
    if (row.kind === "playlist") return ORIGIN + "/playlist?list=" + row.id;
    if (row.kind === "mix") return WATCH + seedOf(row) + "&list=" + row.id;
    return ORIGIN + "/channel/" + row.id;
  };

  // ---- YouTube (§1.8)

  const fail = (why) => Object.assign(new Error(why), { why });
  const httpError = (status) => Object.assign(new Error("HTTP " + status), { status });
  const why = (e) => {
    if (e && e.why) return e.why;
    if (e && e.status === 429) return "limit";
    if (typeof navigator !== "undefined" && navigator.onLine === false) return "offline";
    return "read";
  };
  const getClient = () => S.client || (S.client = (async () => {
    let text = "";
    try {
      const response = await fetch("/sw.js_data", { credentials: "same-origin" });
      if (response.ok) text = await response.text();
    } catch { /* the fallbacks below */ }
    const got = P.clientFrom(text, navigator.language);
    drift(got.drift);
    return got.client;
  })());
  const ask = async (endpoint, make, { signed = false } = {}) => {
    const client = await getClient();
    const body = make(client);
    // A request that never reached YouTube is no connection, whatever navigator.onLine says (§2.9).
    const once = (extra) => fetch(P.api(endpoint), {
      method: "POST", credentials: "same-origin", headers: { ...P.headers(client), ...extra }, body: JSON.stringify(body),
    }).catch(() => { throw fail("offline"); });
    let extra = signed && S.signedIn ? await P.signedHeaders(document.cookie) : null;
    let response = await once(extra || {});
    let json = response.ok ? await response.json() : null;
    // A signed-in browse or next that comes back empty is asked once more with the session's hash.
    const retry = !extra && S.signedIn && (endpoint === "browse" || endpoint === "next") && !body.continuation;
    if (retry && (response.status === 401 || response.status === 403 || (json && !json.contents))) {
      extra = await P.signedHeaders(document.cookie);
      if (extra) {
        response = await once(extra);
        json = response.ok ? await response.json() : null;
      }
    }
    if (!response.ok) throw httpError(response.status);
    return json;
  };
  // The HTML page's ytInitialData, when InnerTube rejects a search or a browse.
  const html = async (url) => {
    const response = await fetch(url, { credentials: "same-origin", redirect: "manual" }).catch(() => { throw fail("offline"); });
    if (response.type === "opaqueredirect") {
      // Only the consent page redirects these; it loads at the top level and comes back here.
      if (SURFACE === "served" && !/(?:^|;\s*)(?:SOCS|CONSENT)=/.test(document.cookie)) {
        location.href = "https://consent.youtube.com/m?continue=" + encodeURIComponent(location.href);
      }
      throw fail("read");
    }
    if (!response.ok) throw httpError(response.status);
    const data = P.initialData(await response.text());
    if (!data) throw fail("read");
    return data;
  };
  const first = async (endpoint, make, parse, page) => {
    let got = null;
    try {
      got = parse(await ask(endpoint, make));
    } catch (e) {
      if (e.status !== 400) throw e;
    }
    if (!got || !got.known) got = parse(await html(page));
    drift(got.drift);
    if (!got.known && !got.rows.length) throw fail("read");
    return got;
  };
  const next = async (endpoint, token) => {
    const got = P.parseMore(await ask(endpoint, (c) => P.moreBody(c, token)));
    drift(got.drift);
    return got;
  };
  const mixPage = async (seed, list, last) => {
    const got = P.parseMix(await ask("next", (c) => ({ ...P.mixBody(c, seed, last && last.id, last && last.index), playlistId: list || "RD" + seed })));
    drift(got.drift);
    if (!got.known && !got.rows.length) throw fail("read");
    return got;
  };
  // Discovery's pages, kept 10 minutes: a shelf's list opens without asking again (§3.3).
  const memo = new Map(); // endpoint + query + params, or the Mix seed -> {rows, more, at}
  const kept = async (key, read) => {
    const hit = memo.get(key);
    memo.delete(key);
    if (hit && Date.now() - hit.at < MEMO) { memo.set(key, hit); return hit; }
    const page = await read();
    const entry = { rows: page.rows, more: page.more || null, at: Date.now() };
    memo.set(key, entry);
    while (memo.size > MEMOS) memo.delete(memo.keys().next().value);
    return entry;
  };
  const searched = (q, params) =>
    kept("search " + q + " " + params, () => first("search", (c) => P.searchBody(c, q, params), P.parseSearch, P.resultsUrl(q, params)));
  // A genre's page 1: New is This week, or This month when the week has under 6 counted rows (§1.3).
  const genrePage = async (genre, s) => {
    if (s !== "new") return searched(genre.query, TOP);
    const week = await searched(genre.query, WEEK);
    return P.count(week.rows, genre.query) < THIN ? searched(genre.query, MONTH) : week;
  };
  // Similar (§2.4): the seed's Mix and a search for its piece at once, or one after the other when only the
  // Mix can say what the seed is. Whichever half arrives is used; the other's failure comes back as `error`.
  const similar = async (seed) => {
    const asked = (q) => {
      const p = q ? searched(q, TOP) : null;
      if (p) p.catch(() => {}); // awaited below
      return p;
    };
    let piece = seed.title ? asked(P.pieceQuery(seed.title, seed.channel)) : undefined;
    let mix = null;
    let error = null;
    try { mix = await kept("mix " + seed.id, () => mixPage(seed.id, "")); } catch (e) { error = e; }
    const own = mix && mix.rows.find((r) => r.id === seed.id);
    if (own) {
      if (!seed.channelId && own.channelId) Object.assign(seed, { channelId: own.channelId, channel: seed.channel || own.channel });
      if (!seed.title) seed.title = own.title;
    }
    if (piece === undefined) piece = seed.title ? asked(P.pieceQuery(seed.title, seed.channel)) : null;
    let found = null;
    if (piece) {
      try { found = await piece; } catch (e) { error = error || e; }
    }
    if (!mix && !found) throw error || fail("read");
    return { mix: mix ? mix.rows : [], search: found ? found.rows : [], more: found ? found.more : null, error };
  };
  // The player script's signature timestamp: without it the player endpoint calls every video unplayable.
  // It is read once per player release (the hash in /iframe_api) and kept.
  const signature = () => S.sts || (S.sts = (async () => {
    try {
      const api = await (await fetch("/iframe_api", { credentials: "same-origin" })).text();
      const hash = (/\/s\/player\/([\w-]+)\//.exec(api.replace(/\\\//g, "/")) || [])[1];
      if (!hash) throw fail("read");
      const [keptHash, keptSts] = read("qm.sts", "").split(":");
      if (keptHash === hash && Number(keptSts) > 0) return Number(keptSts);
      const response = await fetch(`/s/player/${hash}/player_ias.vflset/en_US/base.js`, { credentials: "same-origin" });
      const sts = response.ok ? P.stsFrom(await response.text()) : null;
      if (!sts) throw fail("read");
      write("qm.sts", hash + ":" + sts);
      return sts;
    } catch {
      drift(["player signature timestamp not found"]);
      return null;
    }
  })());
  const checks = new Map();
  // §3.6: the player endpoint, asked in English, says whether a video plays before the frame loads it.
  // A members-only video is asked signed (§1.8): only the account says whether it is a member.
  const playability = (id, signed = false) => {
    const key = (signed ? "s:" : "") + id;
    if (!checks.has(key)) {
      checks.set(key, (async () => {
        try {
          const sts = await signature();
          const got = P.parsePlayer(await ask("player", (c) => P.playerBody(c, id, sts), { signed }));
          drift(got.drift);
          if (got.stale) {
            // A stale timestamp says nothing about playing, but the length and title still hold.
            if (sts) { write("qm.sts", ""); S.sts = null; }
            return { title: got.title, channel: got.channel, channelId: got.channelId, seconds: got.seconds };
          }
          if (got.overlay) mark(id, got, signed);
          return got;
        } catch {
          checks.delete(key);
          return null;
        }
      })());
    }
    return checks.get(key);
  };
  // An unsigned answer says nothing about a members-only video: the frame and the signed call decide.
  const mark = (id, got, signed = true) => {
    const fix = (r) => {
      if (r && r.kind === "video" && r.id === id && (signed || !r.members)) {
        Object.assign(r, { playable: got.playable === false ? false : r.playable, overlay: got.overlay, live: got.live, upcoming: got.upcoming });
        if (!r.seconds && got.seconds) r.seconds = got.seconds;
      }
    };
    for (const v of views.values()) v.rows.forEach(fix);
    fix(S.dock);
    for (const v of views.values()) for (const node of v.nodes.values()) if (node.qmRow.id === id) refreshThumb(node);
    schedule();
  };
  // Rows read for a list row's own Convert (§3.9, §3.11), without opening its view.
  const readList = async (row, progress) => {
    const rows = [];
    const seen = new Set();
    const keep = (list, cap) => {
      for (const r of list) {
        if (r.kind === "video" && !seen.has(r.id) && rows.length < cap) { seen.add(r.id); rows.push(r); remember(r); }
      }
      progress(rows.length);
    };
    if (row.kind === "mix") {
      const seed = seedOf(row);
      const list = row.id === "RD" + seed ? "" : row.id;
      let page = await mixPage(seed, list);
      keep(page.rows, MIX_CAP);
      let last = page.last;
      while (last && rows.length < MIX_CAP) {
        const before = rows.length;
        page = await mixPage(seed, list, last);
        keep(page.rows, MIX_CAP);
        if (rows.length === before || !page.last || page.last.id === last.id) break;
        last = page.last;
      }
      return rows;
    }
    const page = await first("browse", (c) => P.playlistBody(c, row.id), P.parsePlaylist, "/playlist?list=" + encodeURIComponent(row.id));
    keep(page.rows, CAP);
    let more = page.more;
    while (more && rows.length < CAP) {
      const got = await next("browse", more);
      keep(got.rows, CAP);
      more = got.known ? got.more : null;
    }
    return rows;
  };

  // ---- states (§2.8)

  const fromItem = (it) => {
    const list = LISTS.has(it.kind) || (it.total != null && !String(it.key).startsWith("v:"));
    if (it.state === "queued") return { s: "queued", key: it.key };
    if (it.state === "converting") {
      if (it.phase === "reading") return { s: "reading", n: it.total || it.done || 0 };
      if (list) return it.total ? { s: "list", done: it.done || 0, total: it.total } : { s: "reading", n: 0 };
      if (it.phase === "transcribe") return { s: "transcribing", percent: Math.max(0, Math.min(100, it.percent || 0)) };
      return { s: "downloading" };
    }
    if (it.state === "done") {
      if (list) return { s: "listdone", n: it.done != null ? it.done : Math.max(0, (it.total || 0) - (it.failed || 0)), failed: it.failed || 0 };
      return { s: "open", path: it.path || S.converted.get(String(it.key).slice(2)) || "" };
    }
    if (it.state === "failed") return { s: "failed", reason: it.reason || "" };
    return { s: "idle" };
  };
  const fromChild = (c) => {
    if (c.state === "converting") {
      return c.phase === "transcribe" ? { s: "transcribing", percent: Math.max(0, Math.min(100, c.percent || 0)) } : { s: "downloading" };
    }
    if (c.state === "done") return { s: "open", path: c.path || "" };
    if (c.state === "failed") return { s: "failed", reason: c.reason || "", child: true };
    return null;
  };
  const pendingState = (key) => {
    const p = S.pending.get(key);
    if (p) return p.failed ? { s: "retry", key } : { s: "sent" };
    return S.sent.has(key) ? { s: "sent" } : null;
  };
  // A video that cannot play here (§2.9): unplayable, live, upcoming, or members only while signed out.
  const blocked = (row) => row.playable === false || row.live || row.upcoming || (!!row.members && !S.signedIn);
  // Its thumbnail's word; a members-only row signed out has none of its own.
  const blockedWord = (row) => (blocked(row) ? row.overlay || (row.members && !S.signedIn ? "Not available" : "") : "");
  const rowState = (row) => {
    if (!row) return { s: "none" };
    if (row.kind === "channel") return { s: "none" };
    if (row.kind !== "video") {
      const key = listKey(row);
      if (S.reading.has(key)) return { s: "reading", n: S.reading.get(key) };
      const p = pendingState(key);
      if (p) return p;
      const it = S.items.get(key);
      if (it) return fromItem(it);
      return S.notes.has(key) ? { s: "idle", note: S.notes.get(key) } : { s: "idle" };
    }
    const key = "v:" + row.id;
    const own = pendingState(key);
    if (own) return own;
    const it = S.items.get(key);
    if (it && it.state !== "removed") return fromItem(it);
    const cover = S.cover.get(row.id);
    if (cover) {
      const c = S.children.get(cover.key + "/" + key);
      const st = c && fromChild(c);
      if (st) return { ...st, parent: cover.key };
      const p = pendingState(cover.key);
      if (p) return p;
      const parent = S.items.get(cover.key);
      if (!c && parent && (parent.state === "queued" || parent.state === "converting")) return { s: "queued", key, parent: cover.key };
    }
    const path = S.converted.get(row.id);
    if (path) return { s: "open", path };
    if (row.overlay === "Age-restricted") return S.signedIn ? { s: "idle" } : { s: "signin" };
    if (blocked(row)) return { s: "none" };
    return { s: "idle" };
  };
  const canTick = (row, st) => row.kind === "video" && (st.s === "idle" || st.s === "failed" || st.s === "retry");
  const isCounted = (query, row, st) =>
    P.counted(row, query, { converted: st.s === "open", queue: QUEUE.has(st.s) });
  const entryState = (key) => {
    const p = pendingState(key);
    if (p) return p.s === "retry" ? null : p;
    const it = S.items.get(key);
    return it && it.state !== "failed" ? fromItem(it) : null;
  };
  const active = (key) => {
    const p = S.pending.get(key);
    const it = S.items.get(key);
    return (p && !p.failed) || S.sent.has(key) || (it && (it.state === "queued" || it.state === "converting"));
  };
  const stateText = (st) => {
    if (st.s === "reading") return st.n ? "Reading " + st.n : "Reading";
    if (st.s === "downloading") return "Downloading";
    if (st.s === "transcribing") return "Transcribing " + Math.round(st.percent) + "%";
    if (st.s === "list") return st.done + " of " + st.total;
    return "Queued";
  };

  // The Convert control (§2.8). Inert states keep their place, never look disabled and take no focus.
  // Narrow (§2.6), it is an icon button, and a stateful label keeps only its number; the expanded sheet has room.
  const setConvert = (b, st, { primary = false, n = null } = {}) => {
    const row = b.closest(".row");
    const short = root.classList.contains("narrow") && !(b === ui.dockConvert && S.sheet);
    const sig = JSON.stringify([st, primary, n, row ? row.classList.contains("current") : null, short]);
    if (b.sig === sig) return;
    b.sig = sig;
    let name = null;
    let text = "";
    let look = "";
    let fill = null;
    let spin = false;
    let inert = false;
    switch (st.s) {
      case "idle": name = "audio-lines"; text = n != null ? "Convert " + n : "Convert"; look = primary ? "primary" : ""; break;
      case "sent": name = "check"; text = "Sent"; look = "ghost"; inert = true; break;
      case "retry": name = "rotate-ccw"; text = "Retry"; break;
      case "queued": name = "clock"; text = "Queued"; look = "ghost"; inert = true; break;
      case "reading": name = "loader-circle"; spin = true; text = st.n ? "Reading " + st.n : "Reading"; look = "ghost work"; inert = true; break;
      case "downloading": name = "loader-circle"; spin = true; text = "Downloading"; look = "ghost work"; inert = true; break;
      case "transcribing": text = Math.round(st.percent) + "%"; look = "fill"; fill = st.percent; inert = true; break;
      case "list": text = st.done + " of " + st.total; look = "fill"; fill = st.total ? (100 * st.done) / st.total : 0; inert = true; break;
      case "open": name = "file-music"; text = "Open"; look = "ok"; break;
      case "listdone": name = "check"; text = "Converted " + st.n; look = "ok inert"; inert = true; break;
      case "failed": name = "triangle-alert"; text = "Retry"; look = "failed"; break;
      case "signin": name = "log-in"; text = "Sign in"; break;
      default: break;
    }
    let shown = text;
    if (short) {
      const number = { idle: n, reading: st.n || null, transcribing: text, list: st.done + "/" + st.total, listdone: st.n }[st.s];
      shown = number != null ? String(number) : "";
    }
    if (!shown) named(b, text);
    else if (shown !== text) { b.removeAttribute("title"); b.setAttribute("aria-label", text); }
    else if (b.hasAttribute("aria-label")) { b.removeAttribute("title"); b.removeAttribute("aria-label"); }
    b.className = "convert" + (look ? " " + look : "") + (shown ? "" : " bare");
    b.hidden = st.s === "none";
    b.replaceChildren(...(name ? [icon(name, spin ? "spin" : null)] : []), ...(shown ? [el("span", null, shown)] : []));
    b.tabIndex = inert || (row && !row.classList.contains("current")) ? -1 : 0;
    if (inert) b.setAttribute("aria-disabled", "true");
    else b.removeAttribute("aria-disabled");
    if (fill == null) b.style.removeProperty("--fill");
    else b.style.setProperty("--fill", fill.toFixed(1) + "%");
  };

  // Every Convert label at once, so the action column is as wide as the widest (Panels' reserve).
  const measure = () => {
    if (!ui) return;
    const probe = el("div", "row probe");
    const act = el("div", "act");
    act.style.minWidth = "0";
    act.style.width = "max-content";
    probe.append(act);
    document.body.append(probe);
    let widest = 0;
    const samples = [{ s: "idle" }, { s: "sent" }, { s: "retry" }, { s: "queued" }, { s: "reading", n: 888 }, { s: "downloading" },
      { s: "transcribing", percent: 100 }, { s: "list", done: 88, total: 888 }, { s: "open" }, { s: "listdone", n: 888 },
      { s: "failed" }, { s: "signin" }];
    for (const st of samples) {
      const b = el("button");
      b.type = "button";
      act.replaceChildren(b);
      setConvert(b, st);
      if (st.s === "queued") act.append(button("icon remove", "x", null, "Remove"));
      widest = Math.max(widest, act.offsetWidth);
    }
    probe.remove();
    root.style.setProperty("--act", Math.ceil(widest) + "px");
  };

  // ---- Save to (discovery-design §5)

  // A dest is its folders' names joined by "/", "" being the MIDI folder (§4.2). Names compare without case and
  // with spaces collapsed, as convert.py finds an existing folder, and a folder that exists keeps its case on disk.
  const fold = (s) => String(s).replace(/\s+/g, " ").toLowerCase();
  const namesOf = (dest) => (dest ? dest.split("/") : []);
  const lastName = (dest) => namesOf(dest).pop() || "";
  const destText = (dest) => (dest ? namesOf(dest).join(" / ") : "MIDI folder");
  const onDisk = (dest) => (dest ? S.onDisk.get(fold(dest)) ?? null : "");
  const destIcon = (dest) => (dest === "" ? "folder-root" : onDisk(dest) != null ? "folder" : "folder-plus");
  const byName = (a, b) => (fold(a) < fold(b) ? -1 : fold(a) > fold(b) ? 1 : 0);
  // A file's place: its folder and its name ("Chopin / Nocturne.mid").
  const where = (path) => String(path || "").split(/[\\/]/).filter(Boolean).slice(-2).join(" / ");
  const setFolders = (list) => {
    S.folders = list.filter((f) => typeof f === "string" && f && P.validDest(f));
    S.onDisk = new Map(S.folders.map((f) => [fold(f), f]));
    for (const d of S.made) if (S.onDisk.has(fold(d))) S.made.delete(d);
    S.listed++;
  };
  // Names typed, or a name from YouTube, as a dest (§5.2): each through folderName, the folders that exist in their
  // case on disk, four deep at most.
  const destOf = (names) => {
    let out = [];
    for (const name of names.slice(0, 4)) {
      out.push(P.folderName(name));
      const known = onDisk(out.join("/"));
      if (known != null) out = namesOf(known);
    }
    return out.join("/");
  };
  // The folder named like an artist, shallowest first, or null. An artist's name is one folder: a "/" in it never nests.
  const artistFolder = (name) => {
    const want = fold(P.folderName(name));
    let best = null;
    for (const f of S.folders || []) {
      if (fold(lastName(f)) === want && (best == null || namesOf(f).length < namesOf(best).length)) best = f;
    }
    return best;
  };
  // The folder an artist keeps (qm-artists or the ledger), or null.
  const remembered = (id) => {
    const a = id && S.artists.get(id);
    return a && typeof a.dest === "string" && P.validDest(a.dest) ? onDisk(a.dest) ?? a.dest : null;
  };
  // The one artist a choice is for (§5.3), as {id, name}: that of rows all from one channel, else null.
  const oneArtist = (rows) => {
    let id = null;
    let name = "";
    for (const r of rows) {
      const c = channelOf(r);
      if (!c || (id && c !== id)) return null;
      id = c;
      name = name || r.channel || r.owner || "";
    }
    return id ? { id, name } : null;
  };
  // A channel's name is its list's title; a genre's songs of a channel keep it as their folder.
  const listArtist = (list) => (list.kind === "channel" ? list.title : list.kind === "genre" ? list.folder || "" : null);
  const viewArtist = (v, rows) => (v.list && listArtist(v.list) != null ? { id: v.list.id, name: listArtist(v.list) } : oneArtist(rows));
  // A queue entry's: its channel's, its video's, or that of its rows.
  const entryArtist = (key) => {
    if (key.startsWith("c:")) {
      const v = [...views.values()].find((x) => x.list && x.list.key === key);
      const a = S.artists.get(key.slice(2));
      return UCID.test(key.slice(2)) ? { id: key.slice(2), name: (v && v.list.title) || (a && a.name) || "" } : null;
    }
    const pick = S.picks.get(key);
    const ids = key.startsWith("v:") ? [key.slice(2)] : ((pick && pick.items) || []).map((it) => it.id);
    const rows = ids.map(rowOf);
    return rows.length && rows.every(Boolean) ? oneArtist(rows) : null;
  };
  // Row Convert and the dock (§5.3): the choice made there, else the artist's folder, else a folder named like the
  // artist, else the MIDI folder. A failed row of a list converts again into the list's folder.
  const rowDest = (row, chosen) => {
    if (chosen != null) return chosen;
    const cover = S.cover.get(row.id);
    const st = rowState(row);
    if (st.s === "failed" && st.parent && cover && cover.dest != null) return cover.dest;
    const name = row.channel || row.owner || "";
    return remembered(channelOf(row)) ?? (name ? artistFolder(name) : null) ?? "";
  };
  const dockDest = (row) => rowDest(row, S.dockDest && S.dockDest.id === row.id ? S.dockDest.dest : null);
  // Convert N (§5.3): the view's choice, else a list's own folder (a channel's is its artist's), else null for each
  // row's own.
  const listDest = (v) => {
    if (v.dest != null) return v.dest;
    const list = v.list;
    if (!list) return null;
    const name = listArtist(list) ?? list.title;
    if (listArtist(list) != null) {
      const own = remembered(list.id) ?? (name ? artistFolder(name) : null);
      if (own != null) return own;
    }
    return name ? destOf([name]) : "";
  };
  const tickedRows = (v) => v.rows.filter((r) => v.ticks.has(r.id) && canTick(r, rowState(r))).slice(0, CAP);
  // The ticked rows by folder, in first-row order.
  const byDest = (v, rows) => {
    const shared = listDest(v);
    const groups = new Map();
    for (const r of rows) {
      const dest = shared ?? rowDest(r, null);
      if (!groups.has(fold(dest))) groups.set(fold(dest), { dest, rows: [] });
      groups.get(fold(dest)).rows.push(r);
    }
    return [...groups.values()];
  };
  // A pick's folder (§4.2): its dest, the artist whose folder it is, and the last name as the legacy folder.
  const destFields = (dest, artist) => {
    const own = artist ? remembered(artist.id) : null;
    return { dest, artist: dest != null && own != null && fold(own) === fold(dest) ? artist.id : null, folder: lastName(dest) || null };
  };
  // A choice becomes a recent folder, and a choice for one artist's things that artist's folder (§5.3).
  const choose = (dest, artist) => {
    S.refused = false;
    if (dest) {
      S.dests = [dest, ...S.dests.filter((d) => fold(d) !== fold(dest))].slice(0, RECENT);
      write("qm-dests", JSON.stringify(S.dests));
    }
    if (artist) {
      const was = S.artists.get(artist.id) || {};
      S.artists.set(artist.id, { ...was, name: was.name || artist.name || "", avatar: was.avatar || "", dest, at: new Date().toISOString() });
      saveArtists();
    }
    schedule();
  };
  // The Save to control (§5.1): what the folder is as its icon, its last name, a chevron, and the whole path as its
  // name. Rows going to several folders show how many.
  const setSave = (b, dests) => {
    const one = dests.length === 1 ? dests[0] : null;
    const symbol = one == null ? "folders" : destIcon(one);
    const text = one == null ? String(dests.length) : one ? lastName(one) : "MIDI folder";
    const sig = JSON.stringify([symbol, text, dests]);
    if (b.sig === sig) return;
    b.sig = sig;
    b.replaceChildren(icon(symbol), el("span", null, text), icon("chevron-down", "chevron"));
    named(b, dests.map(destText).join(", "));
  };
  // The same as plain text, for an entry that runs or is done.
  const folderTag = (symbol, text, name) => {
    const tag = el("span", "save chip", icon(symbol), el("span", null, text));
    tag.title = name;
    return tag;
  };
  // The Save to menu (§5.2): a field, empty whenever it opens, over the artist's one folder, the MIDI folder, the
  // current one still to be made, the recent ones and every folder A to Z. Typing filters them, and a name that is no
  // folder yet comes last, to be made.
  const folderMenu = (owner, current, artist, take) => {
    if (menuOwner === owner) { closeMenu(true); return; }
    const known = () => {
      const all = new Map();
      for (const d of [...(S.folders || []), ...S.made]) if (!all.has(fold(d))) all.set(fold(d), d);
      return [...all.values()].sort(byName);
    };
    const items = (text) => {
      // A pianist has one folder: the one they keep, else the one named like them, made or to be made.
      const own = artist ? remembered(artist.id) : null;
      const mine = own ?? (artist && artist.name ? artistFolder(artist.name) ?? destOf([artist.name]) : null);
      const out = [];
      const seen = new Set();
      const add = (dest) => {
        if (seen.has(fold(dest))) return;
        seen.add(fold(dest));
        out.push({ icon: destIcon(dest), text: destText(dest), checked: current != null && fold(current) === fold(dest), run: () => take(dest) });
      };
      const names = text.split(/[\\/]/).map((s) => s.trim()).filter(Boolean);
      if (!names.length) {
        if (mine != null) add(mine);
        add("");
        // The current folder comes next when neither recent nor made: still to be made, it is checked here like
        // any other.
        const later = [...S.dests, ...known()];
        if (current != null && !later.some((x) => fold(x) === fold(current))) add(current);
        later.forEach(add);
        return out;
      }
      // The exact folder, then those whose last name starts so, then the rest, the pianist's and the current one
      // among them though not made yet, each as the empty field shows it; then the typed one, to be made.
      const typed = destOf(names);
      const needle = fold(typed);
      const rank = (d) => (fold(d) === needle ? 0 : fold(lastName(d)).startsWith(fold(lastName(typed))) ? 1 : 2);
      [mine, ...S.dests, ...known(), current].filter((d) => d && fold(d).includes(needle))
        .sort((a, b) => rank(a) - rank(b) || byName(a, b)).forEach(add);
      add(typed);
      return out;
    };
    openMenu(owner, items(""), null, { icon: "folder", placeholder: "Folder", items });
  };
  // A waiting or failed entry's new folder (§5.3): the app moves it, and its Retry sends it there.
  const retarget = async (key, dest, artist) => {
    choose(dest, artist);
    const it = S.items.get(key);
    const pick = S.picks.get(key);
    const was = [it && it.dest, pick && { dest: pick.dest, artist: pick.artist, folder: pick.folder }];
    const fields = destFields(dest, artist);
    if (it) it.dest = dest;
    if (pick) Object.assign(pick, fields);
    ui.tray.sig = null;
    schedule();
    if ((await call("dest", key, dest, fields.artist)) === false) {
      // Refused (§5.4): the entry keeps its folder.
      if (it) it.dest = was[0];
      if (pick) Object.assign(pick, was[1]);
      S.refused = true;
    } else if (onDisk(dest) == null) S.made.add(dest);
    ui.tray.sig = null;
    schedule();
  };

  // ---- converting (§3.7, §3.8)

  const item = (r) => ({ id: r.id, title: String(r.title || "").slice(0, 300) });
  const remember = (r) => { if (r.kind === "video" && r.title) S.titles.set(r.id, r.title); };
  // A pick's rows wait under its key; a list's or a selection's are covered by it, in its folder.
  const send = async (key, pick, ids) => {
    const selection = !key;
    const now = Date.now();
    if (key && now - (S.clicked.get(key) || 0) < GUARD) return;
    if (!key) key = "s:?" + ++S.temp;
    S.clicked.set(key, now);
    const was = S.pending.get(key);
    if (was && !was.failed) return;
    if (was) clearTimeout(was.timer);
    const p = { failed: false, timer: 0 };
    S.pending.set(key, p);
    const old = S.items.get(key);
    if (old && (old.state === "failed" || old.state === "done")) S.items.delete(key);
    S.picks.set(key, pick);
    const cover = (k) => ({ key: k, folder: pick.folder, dest: pick.dest });
    for (const id of ids) S.cover.set(id, cover(key));
    schedule();
    const answer = await call("pick", pick);
    if (S.pending.get(key) !== p) return;
    if (answer && answer.ok === false && answer.reason === "dest" && typeof pick.dest === "string") {
      // browse.py refused its folder (§5.4): nothing was sent, and the rows go back to Convert. A pick refused
      // for anything else is the plain failure below.
      S.pending.delete(key);
      S.picks.delete(key);
      for (const [id, c] of S.cover) if (c.key === key) S.cover.delete(id);
      S.refused = true;
      schedule();
      return;
    }
    if (!answer || !answer.ok) { p.failed = true; schedule(); return; }
    S.refused = false;
    if (typeof pick.dest === "string" && onDisk(pick.dest) == null) S.made.add(pick.dest);
    if (selection && answer.seq != null) {
      // A selection's key is known only now.
      S.pending.delete(key);
      S.picks.delete(key);
      key = "s:" + answer.seq;
      S.pending.set(key, p);
      S.picks.set(key, pick);
      for (const id of ids) S.cover.set(id, cover(key));
      if (S.items.has(key)) S.pending.delete(key);
    }
    // Things all from one artist, taken, make that artist yours at once (§1.4, §5.3), with no folder of their own.
    const artist = entryArtist(key);
    if (artist) noteArtist(artist.id, artist.name, faceOf(artist.id), new Date().toISOString());
    if (S.protocol === 1) {
      S.pending.delete(key);
      S.sent.add(key);
    } else if (S.pending.get(key) === p) {
      p.timer = setTimeout(() => {
        if (S.pending.get(key) === p) { p.failed = true; schedule(); }
      }, ACK);
    }
    schedule();
  };
  const resend = (key) => {
    const pick = S.picks.get(key);
    if (!pick) return;
    const ids = [...S.cover].filter(([, c]) => c.key === key).map(([id]) => id);
    // The same pick again, in the same folder (§5.3).
    if (key.startsWith("s:")) {
      S.pending.delete(key);
      send(null, pick, ids);
    } else send(key, pick, ids);
  };
  const openFile = (path) => { if (path) call("open_file", path); };
  // A pick's folder fields: none but the legacy folder until the app lists its folders (§4.2).
  const pickFolder = (dest, artist, legacy) => (S.folders ? destFields(dest, artist) : { folder: legacy });
  const convertRow = (v, row, again = false, dock = false) => {
    if (!row) return;
    if (row.kind === "playlist" || row.kind === "mix") { convertList(v, row); return; }
    if (row.kind !== "video") return;
    const st = rowState(row);
    if (!again) {
      if (st.s === "open") { openFile(st.path); return; }
      if (st.s === "signin") { call("sign_in"); return; }
      if (st.s === "retry") { resend(st.key); return; }
      if (st.s !== "idle" && st.s !== "failed") return;
    }
    // A failed child converts again into its list's folder.
    const cover = S.cover.get(row.id);
    const legacy = st.s === "failed" && st.parent && cover ? cover.folder : null;
    const dest = dock ? dockDest(row) : rowDest(row, v && v.dest);
    send("v:" + row.id, { kind: "video", id: row.id, title: item(row).title, again, ...pickFolder(dest, oneArtist([row]), legacy) }, []);
  };
  const convertList = async (v, row) => {
    const key = listKey(row);
    const st = rowState(row);
    if (st.s === "retry") { resend(key); return; }
    if ((st.s !== "idle" && st.s !== "failed") || S.reading.has(key)) return;
    const query = v ? v.query : "";
    S.notes.delete(key);
    S.reading.set(key, 0);
    schedule();
    try {
      const rows = await readList(row, (count) => { S.reading.set(key, count); schedule(); });
      const counted = rows.filter((r) => isCounted(query, r, rowState(r))).slice(0, CAP);
      S.reading.delete(key);
      if (!counted.length) { S.notes.set(key, "No piano videos."); schedule(); return; }
      const title = (row.kind === "mix" ? mixTitle(row.title) : row.title).slice(0, 300);
      const id = row.kind === "mix" ? seedOf(row) : row.id;
      // A list converts into the view's choice, else a folder named for it (§5.3).
      const dest = v && v.dest != null ? v.dest : title ? destOf([title]) : "";
      send(key, { kind: row.kind, id, title, again: false, ...pickFolder(dest, null, title || null), items: counted.map(item) }, counted.map((r) => r.id));
    } catch (e) {
      S.reading.delete(key);
      if (cur) cur.error = why(e);
      schedule();
    }
  };
  const convertTicked = () => {
    const v = cur;
    if (!v) return;
    const rows = tickedRows(v);
    if (!rows.length) return;
    const list = v.list;
    v.ticks.clear();
    v.anchor = null;
    // One pick per folder, in first-row order (§5.3).
    const groups = S.folders ? byDest(v, rows) : [{ dest: null, rows }];
    const title = list ? list.title : v.kind === "search" ? v.route.q : (v.head && v.head.title) || (v.rows[0] && v.rows[0].title) || "";
    for (const g of groups) {
      const ids = g.rows.map((r) => r.id);
      const fields = pickFolder(g.dest, viewArtist(v, g.rows), list ? list.folder : null);
      // A genre's songs of a channel go as a selection: a channel pick converts every video it has.
      if (list && list.kind !== "genre" && groups.length === 1 && !active(list.key)) {
        send(list.key, { kind: list.kind, id: list.id, title: list.title.slice(0, 300), again: false, ...fields, items: g.rows.map(item) }, ids);
      } else send(null, { kind: "selection", title: title.slice(0, 300), again: false, ...fields, items: g.rows.map(item) }, ids);
    }
  };
  const removeRow = (row) => {
    const st = rowState(row);
    if (st.s !== "queued") return;
    if (st.parent) call("drop", "v:" + row.id, st.parent);
    else call("drop", st.key);
  };
  const converted = (id, path, title, dest) => {
    S.converted.set(id, path);
    const d = { id, path, title: title || S.titles.get(id) || fileName(path), dest: typeof dest === "string" ? dest : null };
    S.done = [d, ...S.done.filter((x) => x.path !== path)].slice(0, 50);
  };
  // The folder a pick goes to: as the app tells it, else as it was sent; null when neither knows.
  const destFor = (key) => {
    const it = S.items.get(key);
    const pick = S.picks.get(key);
    return it && typeof it.dest === "string" ? it.dest : pick && typeof pick.dest === "string" ? pick.dest : null;
  };

  // ---- QuartzMIDI's messages (§5.3)

  const settle = (key) => {
    const p = S.pending.get(key);
    if (p) { clearTimeout(p.timer); S.pending.delete(key); }
  };
  const busy = () => {
    const on = [...S.items.values()].some((it) => it.state === "converting" && it.phase === "transcribe");
    if (on !== S.transcribing) { S.transcribing = on; player.busy(on); }
  };
  const receive = (m) => {
    if (!m || typeof m !== "object" || !ui) return;
    if (m.type === "init") {
      S.protocol = m.protocol === 2 ? 2 : 1;
      if (m.skin) applySkin(m.skin);
      if (typeof m.signedIn === "boolean") signed(m.signedIn, false);
      if (Array.isArray(m.folders)) setFolders(m.folders);
    } else if (m.type === "folders" && Array.isArray(m.folders)) setFolders(m.folders);
    else if (m.type === "theme") applySkin(m.skin);
    else if (m.type === "item" && typeof m.key === "string" && m.key) {
      settle(m.key);
      if (m.state === "removed") {
        // A removed pick no longer covers its rows; a failed or done one keeps them for Retry and Convert again.
        S.items.delete(m.key);
        for (const [id, c] of S.cover) if (c.key === m.key) S.cover.delete(id);
      } else S.items.set(m.key, { ...m, at: Date.now() });
      // A video converted before stays where it was, so its folder is its path's, never the one picked.
      if (m.state === "done" && m.key.startsWith("v:") && typeof m.path === "string" && m.path) converted(m.key.slice(2), m.path, m.title, m.already ? null : m.dest);
      busy();
    } else if (m.type === "child" && typeof m.parent === "string" && typeof m.key === "string") {
      settle(m.parent);
      S.children.set(m.parent + "/" + m.key, m);
      if (m.state === "done" && m.key.startsWith("v:") && typeof m.path === "string" && m.path) converted(m.key.slice(2), m.path, null, m.already ? null : destFor(m.parent));
    } else if (m.type === "paused") S.paused = m.reason === "folder" ? "folder" : "signin";
    else if (m.type === "resumed") S.paused = null;
    else if (m.type === "signed") signed(!!m.in, true);
    else if (m.type === "search" && typeof m.q === "string" && m.q.trim()) runSearch(m.q);
    else if (m.type === "signin") call("sign_in");
    schedule();
  };
  const signed = (on, changed) => {
    S.signedIn = on;
    if (changed) {
      S.client = null; // §1.6: the session's own visitorData and context
      checks.clear();
      // Members-only rows wait for the new account's answer.
      const reset = (r) => { if (r && r.members && r.overlay === "Not available") Object.assign(r, { playable: null, overlay: null }); };
      for (const v of views.values()) v.rows.forEach(reset);
      reset(S.dock);
      for (const v of views.values()) for (const node of v.nodes.values()) if (node.qmRow.members) refreshThumb(node);
      if (cur) checkMembers(cur, cur.rows);
    }
    S.account = null;
    if (on) {
      ask("account/account_menu", P.accountBody, { signed: true })
        .then((json) => { S.account = P.parseAccount(json); paintAccount(); })
        .catch(() => paintAccount());
    }
    paintAccount();
  };

  // ---- pianists (§2.2, §4.1)

  // qm-artists and artists.json, per artist: the newer `at` wins, and a folder or avatar either one knows is kept.
  const mergeArtists = (list) => {
    for (const a of list || []) {
      if (!a || !UCID.test(a.id)) continue;
      const was = S.artists.get(a.id);
      const now = { name: String(a.name || ""), avatar: a.avatar || "", dest: a.dest, at: a.at || "" };
      if (!was) { S.artists.set(a.id, now); continue; }
      const [newer, older] = time(now.at) > time(was.at) ? [now, was] : [was, now];
      S.artists.set(a.id, {
        name: newer.name || older.name, avatar: newer.avatar || older.avatar,
        dest: newer.dest !== undefined ? newer.dest : older.dest, at: newer.at,
      });
    }
  };
  const saveArtists = () => {
    const newest = [...S.artists].sort((a, b) => time(b[1].at) - time(a[1].at)).slice(0, ARTISTS);
    const out = {};
    for (const [id, a] of newest) {
      out[id] = { name: a.name, at: a.at };
      if (a.avatar) out[id].avatar = a.avatar;
      if (a.dest !== undefined) out[id].dest = a.dest;
    }
    write("qm-artists", JSON.stringify(out));
  };
  const loadArtists = () => {
    const stored = readJson("qm-artists");
    if (stored && typeof stored === "object") mergeArtists(Object.entries(stored).map(([id, a]) => ({ ...a, id })));
  };
  // Your pianist found for a seed that came without one (§2.4), with `at`, or the avatar a row brought one you have.
  const noteArtist = (id, name, avatar, at) => {
    if (!UCID.test(id || "")) return;
    const was = S.artists.get(id);
    if (was ? !at && (was.avatar || !avatar) : !at) return;
    S.artists.set(id, { ...(was || {}), name: (was && was.name) || name || "", avatar: (was && was.avatar) || avatar || "", at: at || (was && was.at) || "" });
    saveArtists();
  };
  // What this session read of each channel, for the pianist share (§2.2).
  const tally = (v, r) => {
    if (r.kind !== "video" || !r.channelId) return;
    const s = S.pianists[r.channelId] || (S.pianists[r.channelId] = { rec: 0, rows: 0 });
    s.rows++;
    if (P.isRecording(r, queryOf(v, r))) s.rec++;
    if (r.avatar && S.artists.has(r.channelId)) noteArtist(r.channelId, r.channel, r.avatar);
  };
  // Your newest conversions with distinct channels, 3 at most (§2.4). A seed without a channel still seeds.
  const seedsOf = () => {
    const out = [];
    const channels = new Set();
    for (const s of S.seeds) {
      if (out.length >= 3) break;
      if (!s || !VIDEO.test(s.id || "")) continue;
      const channelId = UCID.test(s.channelId || "") ? s.channelId : null;
      if (channelId && channels.has(channelId)) continue;
      if (channelId) channels.add(channelId);
      const a = channelId && S.artists.get(channelId);
      out.push({ id: s.id, title: String(s.title || ""), channelId, channel: (a && a.name) || "" });
    }
    return out;
  };

  // ---- views and history (§3.14)

  const views = new Map(); // history index -> view
  const H = { i: 0, max: 0 };
  let cur = null;
  let used = 0;
  // The words rows are dimmed and counted against: the typed query, a genre's, none on Home and Similar.
  const queryFor = (route) => {
    if (route.view === "search") return route.q;
    if (route.view === "genre") return GENRES.get(route.g).query;
    if (route.view === "home" || route.view === "similar" || route.t === "similar") return "";
    return S.lastSearch ? S.lastSearch.q : "";
  };
  // Home's rows each keep their shelf's query.
  const queryOf = (v, row) => v.queries.get(row.id) || v.query;
  const newView = (route) => {
    const box = el("div", "rows");
    box.setAttribute("role", "list");
    const tail = el("div", "tail", icon("loader-circle", "spin"));
    tail.hidden = true;
    box.append(tail);
    // A channel's videos, whole or a genre's songs alone (scope), keep every video read in read, for the other.
    const videos = route.view === "channel" && (route.t === "videos" || route.t === "genre");
    return {
      route, key: routeString(route), kind: route.view, gen: 0, used: 0,
      scope: route.view === "channel" && route.t === "genre" ? route.g : null, read: videos ? [] : null, done: false,
      // A channel's Similar opened from a genre lists that genre's songs alone (§3.19 Genre scope).
      within: route.view === "channel" && route.t === "similar" && route.g ? route.g : null,
      rows: [], seen: new Set(), nodes: new Map(), more: null, endpoint: "", ondemand: false, pretick: false,
      head: null, list: null, query: queryFor(route), queries: new Map(), flight: 0,
      ticks: new Set(), anchor: null, focus: null, scroll: 0,
      busy: false, reading: false, error: null, box, tail, skeleton: null, empty: null,
    };
  };
  const leave = () => { if (cur) cur.scroll = ui.scroll.scrollTop; };
  const trim = () => {
    while (views.size > VIEWS) {
      let oldest = null;
      for (const [i, v] of views) if (v !== cur && (!oldest || v.used < oldest[1].used)) oldest = [i, v];
      if (!oldest) return;
      views.delete(oldest[0]);
      oldest[1].gen++;
    }
  };
  const go = (route, replace = false) => {
    const key = routeString(route);
    leave();
    if (replace) history.replaceState({ i: H.i }, "", urlOf(key));
    else {
      H.i += 1;
      H.max = H.i;
      for (const i of [...views.keys()]) if (i >= H.i) { views.get(i).gen++; views.delete(i); }
      history.pushState({ i: H.i }, "", urlOf(key));
    }
    enter(route, true);
  };
  const enter = (route, fresh) => {
    let v = views.get(H.i);
    if (fresh || !v || v.key !== routeString(route)) {
      if (v) v.gen++;
      v = newView(route);
      views.set(H.i, v);
      trim();
      show(v);
      load(v);
    } else show(v);
  };
  const popped = () => {
    const state = history.state;
    leave();
    H.i = state && Number.isInteger(state.i) ? state.i : 0;
    H.max = Math.max(H.max, H.i);
    enter(parseRoute(here()), false);
  };
  const runSearch = (text, over = {}) => {
    const q = String(text || "").trim();
    if (!q) { goHome(); return; }
    const base = S.lastSearch || defaults();
    hideSuggest();
    go({ ...base, view: "search", q, p: S.piano ? "1" : "0", ...over });
  };
  const goHome = () => {
    hideSuggest();
    if (!cur || cur.kind !== "home") go({ view: "home" });
  };
  const lastQuery = () => (cur && cur.kind === "search" ? cur.route.q : S.lastSearch ? S.lastSearch.q : "");
  const channelOf = (row) => (row && [row.channelId, row.ownerId].find((id) => UCID.test(id || ""))) || null;
  // A pianist opens on Latest when it is yours, else on Popular (§1.4); found in a genre, on its songs of that genre.
  const openChannel = (id, g = null) =>
    go({ view: "channel", channel: id, ...(g && { g }), t: g ? "genre" : "videos", s: S.artists.has(id) ? "latest" : "popular" });
  // The genre a row was found in: the open genre view's, a channel's songs of a genre or their Similar, or that of
  // its shelf on Home; else null, as on a channel's whole Videos (§3.19 Genre scope).
  const scopeOf = (row) => {
    if (!row || !cur) return null;
    if (cur.kind === "genre" || (cur.kind === "channel" && cur.route.g && (cur.route.t === "genre" || cur.route.t === "similar"))) {
      return cur.rows.some((r) => r.kind === row.kind && r.id === row.id) ? cur.route.g : null;
    }
    if (cur.kind !== "home") return null;
    const query = cur.queries.get(row.id);
    const genre = query && [...GENRES.values()].find((x) => x.query === query);
    return genre ? genre.key : null;
  };
  // Any row this window has read, by video id: the dock, then the open views (Similar's seed, §2.4).
  const rowOf = (id) => {
    if (S.dock && S.dock.id === id) return S.dock;
    for (const v of views.values()) for (const r of v.rows) if (r.kind === "video" && r.id === id) return r;
    return null;
  };
  // A channel's picture: from a row this window has read of it, or its own view's header; else "".
  const faceOf = (id) => {
    if (S.dock && S.dock.avatar && channelOf(S.dock) === id) return S.dock.avatar;
    for (const v of views.values()) {
      if (v.list && listArtist(v.list) != null && v.list.id === id && v.head && v.head.avatar) return v.head.avatar;
      for (const r of v.rows) if (r.kind === "video" && r.avatar && channelOf(r) === id) return r.avatar;
    }
    return "";
  };
  const CURATED = new Map(P.CURATED.map((p) => [p.id, p]));
  const curatedFace = (id) => (CURATED.has(id) ? P.avatarUrl(CURATED.get(id).avatar) : "");

  // ---- loading (§3.1, §3.9-3.11)

  const alive = (v, gen) => v.gen === gen && [...views.values()].includes(v);
  // A genre's songs of a channel stop where its whole Videos would: at the rows read, not those shown.
  const size = (v) => (v.scope ? v.seen.size : v.rows.length);
  // Whether a video row is one of genre g's songs: its title names one, or it is a pianist's who posts nothing else
  // and its title names no other genre (§3.19 Genre scope).
  const inScope = (g, r) => r.kind === "video" && P.inScope(g, r.title, r.channel || r.owner || "", channelOf(r));
  const take = (v, rows, before = v.tail) => {
    const fresh = [];
    const cap = v.kind === "mix" ? MIX_CAP : CAP;
    for (const r of rows || []) {
      const key = r.kind + ":" + r.id;
      // The fill adds to the songs shown, however many videos the read stopped at.
      if (v.seen.has(key) || (v.filling ? v.rows.length : size(v)) >= cap) continue;
      v.seen.add(key);
      // A channel's own videos carry no channel name; the dock still shows it.
      if (v.kind === "channel" && r.kind === "video" && !r.channel && v.head) Object.assign(r, { owner: v.head.title, ownerId: v.route.channel });
      // The Videos read in read, the fill's in found: a later view of the same songs takes both (§3.19).
      if (v.read && r.kind === "video") (v.filling ? (v.found || (v.found = [])) : v.read).push(r);
      // Opened from a genre, only its songs are listed, counted and ticked (§3.19 Genre scope).
      if (v.scope && !inScope(v.scope, r)) continue;
      v.rows.push(r);
      remember(r);
      tally(v, r);
      fresh.push(r);
    }
    if (v.pretick) for (const r of fresh) if (isCounted(queryOf(v, r), r, rowState(r))) v.ticks.add(r.id);
    for (const r of fresh) {
      const node = rowNode(v, r);
      v.nodes.set(r.kind + ":" + r.id, node);
      v.box.insertBefore(node, before);
    }
    if (!v.focus && v.rows.length) v.focus = v.rows[0].kind + ":" + v.rows[0].id;
    if (v.strip === undefined && v.kind !== "home" && v.rows.length) setStrip(v);
    if (v === cur) {
      if (!S.dock && v.kind !== "search") prewarm(v); // a list opened directly fills the empty dock
      observe(v);
      schedule();
    }
    checkMembers(v, fresh);
    return fresh;
  };
  // Signed in, the account says which members-only rows it may play (§1.8): one signed player call at a time,
  // in the background, while the view stays open. mark() turns a non-member's rows into Not available.
  const checkMembers = (v, rows) => {
    const members = rows.filter((r) => r.kind === "video" && r.members);
    if (!S.signedIn || !members.length) return;
    v.memberCheck = (v.memberCheck || Promise.resolve()).then(async () => {
      for (const r of members) {
        if (cur !== v || !S.signedIn) return;
        await playability(r.id, true);
      }
    });
  };
  const load = async (v) => {
    const gen = ++v.gen;
    v.busy = true;
    v.error = null;
    schedule();
    try {
      await rulesReady;
      if (v.kind === "search") await loadSearch(v, gen);
      else if (v.kind === "video") await loadVideo(v, gen);
      else if (v.kind === "playlist") await loadPlaylist(v, gen);
      else if (v.kind === "channel") await loadChannel(v, gen);
      else if (v.kind === "mix") await loadMix(v, gen);
      else if (v.kind === "home") await loadHome(v, gen);
      else if (v.kind === "genre") await loadGenre(v, gen);
      else if (v.kind === "similar") await loadSimilar(v, gen);
    } catch (e) {
      if (v.gen === gen) v.error = why(e);
    } finally {
      if (v.gen === gen) { v.busy = v.flight > 0; v.reading = false; schedule(); }
    }
  };
  const loadSearch = async (v, gen) => {
    const r = v.route;
    const q = P.searchQuery(r.q, r.p === "1");
    const params = P.searchParams({ type: r.f, duration: r.d, popular: r.s === "popular" });
    const page = await first("search", (c) => P.searchBody(c, q, params), P.parseSearch, P.resultsUrl(q, params));
    if (!alive(v, gen)) return;
    Object.assign(v, { endpoint: "search", more: page.more, ondemand: true });
    take(v, channelResults(v) ? P.rankChannels(page.rows) : page.rows);
    prewarm(v);
  };
  // Channels results: verified artists first, Topic and lesson channels last, nothing dropped (§1.5).
  const channelResults = (v) => v.kind === "search" && v.route.f === "channels";
  const loadVideo = async (v, gen) => {
    const id = v.route.v;
    const check = playability(id);
    const page = P.parseNext(await ask("next", (c) => P.nextBody(c, id)));
    drift(page.drift);
    let got = await check;
    if (page.video && page.video.members && S.signedIn) got = await playability(id, true);
    if (!alive(v, gen)) return;
    const own = page.video || (got && got.title ? {
      kind: "video", id, title: got.title, channel: got.channel, channelId: got.channelId, seconds: got.seconds,
      duration: got.seconds ? clock(got.seconds) : "", views: "", age: "", thumb: mq(id), chapters: [], badges: [],
      live: got.live, upcoming: got.upcoming, playable: got.playable, overlay: got.overlay,
    } : null);
    if (own && got) {
      if (!own.seconds && got.seconds) { own.seconds = got.seconds; own.duration = clock(got.seconds); }
      if (got.overlay) Object.assign(own, { playable: got.playable === false ? false : own.playable, overlay: got.overlay, live: got.live, upcoming: got.upcoming });
    }
    if (!own && !page.rows.length) throw fail("read");
    Object.assign(v, { endpoint: "next", more: page.more, ondemand: true });
    take(v, own ? [own, ...page.rows] : page.rows);
    if (own && !S.nowId) { S.dock = own; if (!S.transcribing && !blocked(own)) player.cue(own); }
  };
  const readAll = async (v, gen) => {
    v.reading = true;
    schedule();
    while (v.more && size(v) < CAP && alive(v, gen)) {
      const page = await next(v.endpoint, v.more);
      if (!alive(v, gen)) return;
      v.more = page.known ? page.more : null;
      take(v, page.rows);
    }
    v.reading = false;
    v.done = alive(v, gen);
  };
  const loadPlaylist = async (v, gen) => {
    const id = v.route.list;
    const page = await first("browse", (c) => P.playlistBody(c, id), P.parsePlaylist, "/playlist?list=" + encodeURIComponent(id));
    if (!alive(v, gen)) return;
    v.head = { title: page.title, channel: page.channel, countText: page.countText };
    v.list = { kind: "playlist", id, key: "p:" + id, title: page.title || "", folder: page.title || null };
    Object.assign(v, { endpoint: "browse", more: page.more, pretick: true });
    take(v, page.rows);
    if (v.route.v) {
      const row = v.rows.find((r) => r.id === v.route.v);
      if (row) playRow(row);
    }
    await readAll(v, gen);
  };
  const loadChannel = async (v, gen) => {
    let id = v.route.channel;
    if (!UCID.test(id)) {
      const got = P.parseResolve(await ask("navigation/resolve_url", (c) => P.resolveBody(c, ORIGIN + "/" + id)));
      if (!alive(v, gen)) return;
      if (!got || !UCID.test(got)) throw fail("read");
      id = got;
      v.route = { ...v.route, channel: id };
      v.key = routeString(v.route);
      if (v === cur) history.replaceState({ i: H.i }, "", urlOf(v.key));
    }
    if (v.route.t === "similar") { await loadChannelSimilar(v, gen, id); return; }
    const tab = v.route.t === "genre" ? "videos" : v.route.t;
    // The genre's songs and the whole Videos are the same read: one finished on the same sort serves the other.
    const done = tab === "videos" && [...views.values()].find((w) =>
      w !== v && w.done && w.read && w.head && w.list && w.list.id === id && w.route.s === v.route.s);
    if (done) {
      v.head = { ...done.head };
      v.list = channelList(v, id, v.head.title);
      v.pretick = true;
      take(v, done.read.map((r) => ({ ...r })));
      v.done = true;
      // So is a finished fill of the same genre on the same sort: its songs come along, and nothing is asked again.
      const filled = v.scope && [...views.values()].find((w) => w !== v && w.scope === v.scope && w.done && w.fill &&
        w.fill.done && w.list && w.list.id === id && w.route.s === v.route.s);
      if (filled) {
        v.filling = true;
        take(v, (filled.found || []).map((r) => ({ ...r })));
        v.filling = false;
        v.fill = { ...filled.fill };
      }
      await fill(v, gen, id);
      return;
    }
    const page = await first("browse", (c) => P.channelBody(c, id, tab), P.parseChannel, "/channel/" + id + "/" + tab);
    if (!alive(v, gen)) return;
    v.head = { title: page.title, handle: page.handle, subscribers: page.subscribers, avatar: page.avatar };
    Object.assign(v, { endpoint: "browse", more: page.more });
    if (tab === "playlists") { v.ondemand = true; take(v, page.rows); return; }
    v.list = channelList(v, id, page.title);
    v.pretick = true;
    const index = { latest: 0, popular: 1, oldest: 2 }[v.route.s] || 0;
    const token = index && page.sorts ? page.sorts[index] : null;
    if (token) {
      // Latest, Popular and Oldest are the chips' continuation tokens (§3.10).
      const sorted = await next("browse", token);
      if (!alive(v, gen)) return;
      v.more = sorted.known ? sorted.more : null;
      take(v, sorted.rows);
    } else take(v, page.rows);
    await readAll(v, gen);
    await fill(v, gen, id);
  };
  // A channel's videos convert as the channel; a genre's songs of it as a selection titled "Name · Games", in the
  // artist's folder (§3.19 Genre scope).
  const channelList = (v, id, name) => (v.scope ? {
    kind: "genre", id, g: v.scope, key: "g:" + id + ":" + v.scope,
    title: [name, GENRES.get(v.scope).word].filter(Boolean).join(" · "), folder: name || null,
  } : { kind: "channel", id, key: "c:" + id, title: name || "", folder: name || null });
  // Under FEW of the genre's songs read, the channel's own search for the genre's words adds those it finds, a page
  // at a time, FILL_PAGES at most, until a page brings none. Nothing outside the genre is ever added. A page that
  // fails leaves the fill where it was, for Retry to go on from (v.fill).
  const fill = async (v, gen, id) => {
    if (!v.scope || v.rows.length >= FEW || !alive(v, gen) || (v.fill && v.fill.done)) return;
    const f = v.fill || (v.fill = { n: 0, more: null, done: false });
    v.reading = true;
    v.filling = true;
    schedule();
    try {
      while (alive(v, gen)) {
        const page = f.n ? await next("browse", f.more)
          : P.parseChannel(await ask("browse", (c) => P.channelSearchBody(c, id, GENRES.get(v.scope).words)));
        if (!alive(v, gen)) return;
        if (!f.n) drift(page.drift);
        f.n++;
        f.more = page.known ? page.more : null;
        if (!take(v, page.rows).length || v.rows.length >= FEW || f.n >= FILL_PAGES || !f.more) f.done = true;
        if (f.done) return;
      }
    } finally {
      v.filling = false;
      v.reading = false;
    }
  };
  const loadMix = async (v, gen) => {
    const seed = v.route.mix;
    const list = v.route.list;
    let page = await mixPage(seed, list);
    if (!alive(v, gen)) return;
    const title = mixTitle(page.title);
    v.head = { title };
    v.list = { kind: "mix", id: seed, key: "m:" + seed, title, folder: title };
    v.pretick = true;
    v.reading = true;
    take(v, page.rows);
    let last = page.last;
    while (last && v.rows.length < MIX_CAP) {
      page = await mixPage(seed, list, last);
      if (!alive(v, gen)) return;
      const fresh = take(v, page.rows);
      if (!fresh.length || !page.last || page.last.id === last.id) break;
      last = page.last;
    }
  };
  // Home (§1.2): a shelf per seed, then a New shelf per genre. The first loads on open; the next when the tail shows.
  let artistsReady = Promise.resolve();
  const loadHome = async (v, gen) => {
    await Promise.race([artistsReady, new Promise((done) => setTimeout(done, WAIT))]);
    if (!alive(v, gen)) return;
    v.shelves = [
      ...seedsOf().map((seed) => ({ seed, state: "wait", head: null })),
      ...[...GENRES.values()].map((genre) => ({ genre, state: "wait", head: null })),
    ];
    await Promise.all(shelves(v, v.shelves.length && v.shelves[0].seed ? 1 : 2));
  };
  // Starts up to `count` waiting shelves in order, 2 requests in flight at most; none after a failure until Retry.
  const shelves = (v, count) => {
    const started = [];
    for (const shelf of v.shelves || []) {
      if (started.length >= count || v.error) break;
      if (shelf.state !== "wait") continue;
      const cost = shelf.seed ? 2 : 1;
      if (v.flight + cost > FLIGHT) break;
      started.push(loadShelf(v, shelf, cost));
    }
    schedule();
    return started;
  };
  const loadShelf = async (v, shelf, cost) => {
    const gen = v.gen;
    const { seed, genre } = shelf;
    shelf.state = "busy";
    v.flight += cost;
    v.busy = true;
    const words = el("span", null, seed ? seed.title || S.titles.get(seed.id) || "" : genre.word);
    const head = button("shelf", seed ? "users" : "calendar");
    head.append(words, icon("chevron-right"));
    head.tabIndex = -1;
    head.qmShelf = shelf;
    head.addEventListener("click", () => go(seed ? { view: "similar", similar: seed.id } : { view: "genre", g: genre.key, s: "new" }));
    // Bones under the header while it loads, placed above any later shelf already shown.
    const bones = Array.from({ length: 3 }, skeleton);
    const later = v.shelves.slice(v.shelves.indexOf(shelf) + 1).find((s) => s.head);
    const at = later ? later.head : v.tail;
    for (const node of [head, ...bones]) v.box.insertBefore(node, at);
    shelf.head = head;
    schedule();
    try {
      let rows;
      let query = "";
      if (seed) {
        const had = seed.channelId;
        const got = await similar(seed);
        if (v.gen !== gen) return;
        if (!had && seed.channelId) noteArtist(seed.channelId, seed.channel, "", new Date().toISOString());
        if (got.error) v.error = v.error || why(got.error);
        if (!words.textContent) words.textContent = seed.title || "";
        rows = P.mergeSimilar(seed, got.mix, got.search, v.rows);
      } else {
        query = genre.query;
        rows = (await genrePage(genre, "new")).rows;
        if (v.gen !== gen) return;
      }
      shelf.state = "done";
      const picked = rows.filter((r) => !v.seen.has(r.kind + ":" + r.id) && isCounted(query, r, rowState(r))).slice(0, SHELF);
      if (!picked.length) { head.remove(); shelf.head = null; return; }
      head.title = words.textContent;
      if (query) for (const r of picked) v.queries.set(r.id, query);
      take(v, picked, bones[0]);
      // Rows keep the page's order, whichever shelf answered first; so does the first focus.
      const order = new Map([...v.box.children].map((node, i) => [node.qmRow, i]));
      v.rows.sort((a, b) => order.get(a) - order.get(b));
      const top = v.rows[0].kind + ":" + v.rows[0].id;
      if (v.focus !== top && !v.box.contains(document.activeElement)) {
        const old = nodeOf(v, v.focus);
        if (old) { old.classList.remove("current"); old.tabIndex = -1; old.sig = null; }
        v.focus = top;
      }
    } catch (e) {
      if (v.gen !== gen) return;
      v.error = v.error || why(e);
      shelf.state = "wait";
      head.remove();
      shelf.head = null;
    } finally {
      bones.forEach((node) => node.remove());
      if (v.gen === gen) {
        v.flight -= cost;
        v.busy = v.flight > 0;
        if (!v.flight && !v.error && v.rows.length && v.strip === undefined) setStrip(v);
        schedule();
      }
    }
  };
  const loadGenre = async (v, gen) => {
    const genre = GENRES.get(v.route.g);
    const page = await genrePage(genre, v.route.s);
    if (!alive(v, gen)) return;
    v.head = { title: genre.word };
    Object.assign(v, { endpoint: "search", more: page.more, ondemand: true });
    take(v, page.rows);
    prewarm(v);
  };
  // Similar (§1.5): one recording per other pianist; scrolling pages the piece search.
  const fits = (v, rows) => (v.within ? rows.filter((r) => inScope(v.within, r)) : rows);
  const showSimilar = (v, seed, got) => {
    v.seed = seed;
    Object.assign(v, { endpoint: "search", more: got.more, ondemand: true });
    take(v, P.mergeSimilar(seed, fits(v, got.mix), fits(v, got.search), []));
    // Whichever half arrived stays; Retry asks again for both, the arrived one from the memo.
    if (got.error) { v.error = why(got.error); v.partial = true; }
    prewarm(v);
  };
  const loadSimilar = async (v, gen) => {
    const id = v.route.similar;
    const known = rowOf(id);
    const seed = {
      id, title: (known && known.title) || S.titles.get(id) || "",
      channelId: (known && (known.channelId || known.ownerId)) || null, channel: (known && (known.channel || known.owner)) || "",
    };
    const got = await similar(seed);
    if (!alive(v, gen)) return;
    v.head = { title: seed.title };
    showSimilar(v, seed, got);
  };
  const viewCount = (text) => {
    const m = /([\d.,]+)\s*([KMB])?/i.exec(String(text || ""));
    const n = m ? Number(m[1].replace(/,/g, "")) : 0;
    return (Number.isFinite(n) ? n : 0) * ((m && { K: 1e3, M: 1e6, B: 1e9 }[(m[2] || "").toUpperCase()]) || 1);
  };
  // Channel Similar (§2.4): seeded by the most-viewed counted video read of the channel, or of its first Videos page;
  // opened from a genre, by one of its songs of that genre when there is one (§3.19 Genre scope).
  const loadChannelSimilar = async (v, gen, id) => {
    const g = v.route.g || null;
    // The genre's songs read, with the fill's, else the channel's Videos read, whole or opened from a genre.
    const kept = (t) => [...views.values()].find((w) => w.kind === "channel" && w.route.channel === id && w.route.t === t &&
      (t !== "genre" || w.route.g === g) && w.rows.length);
    const read = (g && kept("genre")) || kept("videos");
    let rows = read ? read.rows : null;
    v.head = read ? read.head : null;
    if (!rows) {
      const page = await first("browse", (c) => P.channelBody(c, id, "videos"), P.parseChannel, "/channel/" + id + "/videos");
      if (!alive(v, gen)) return;
      rows = page.rows;
      v.head = { title: page.title, handle: page.handle, subscribers: page.subscribers, avatar: page.avatar };
    }
    schedule();
    const counted = rows.filter((r) => P.counted(r, ""));
    const songs = g ? counted.filter((r) => P.inScope(g, r.title, (v.head && v.head.title) || "", id)) : [];
    const best = (songs.length ? songs : counted).reduce((a, b) => (a && viewCount(a.views) >= viewCount(b.views) ? a : b), null);
    if (!best) return;
    const seed = { id: best.id, title: best.title, channelId: id, channel: (v.head && v.head.title) || "" };
    const got = await similar(seed);
    if (!alive(v, gen)) return;
    showSimilar(v, seed, got);
  };
  // A genre's songs of a channel whose fill stopped at a failed page, with all its Videos read.
  const refill = (v) => !!(v.scope && v.done && v.fill && !v.fill.done);
  // More results, when the last five rows come into view (§3.1).
  const more = async (v) => {
    if ((!v.more && !refill(v)) || v.busy || v.error) return;
    const gen = v.gen;
    v.busy = true;
    schedule();
    try {
      if (v.ondemand) {
        const page = await next(v.endpoint, v.more);
        if (v.gen !== gen) return;
        v.more = page.known ? page.more : null;
        take(v, v.seed ? P.mergeSimilar(v.seed, [], fits(v, page.rows), v.rows) : channelResults(v) ? P.rankChannels(page.rows) : page.rows);
      } else {
        // A genre's songs of a channel: the rest of its Videos, then the fill they may still need.
        if (v.more) await readAll(v, gen);
        if (v.scope && v.done && v.gen === gen) await fill(v, gen, v.route.channel);
      }
    } catch (e) {
      if (v.gen === gen) v.error = why(e);
    } finally {
      if (v.gen === gen) { v.busy = false; v.reading = false; schedule(); }
    }
  };
  const retry = (v) => {
    if (!v) return;
    v.error = null;
    if (v.kind === "home" && v.shelves) shelves(v, 1);
    else if (v.partial) {
      v.partial = false;
      for (const node of v.nodes.values()) node.remove();
      Object.assign(v, { rows: [], nodes: new Map(), focus: null, more: null });
      v.seen.clear();
      load(v);
    } else if ((size(v) && v.more) || refill(v)) more(v); // a genre's songs go on from the rows read (§3.19)
    else if (v.rows.length && v.kind === "mix") schedule();
    else load(v);
  };
  // §3.1 prewarm: the first playable result waits in the dock, held and muted, unless something is transcribing.
  const prewarm = (v) => {
    if (S.nowId) return;
    const row = v.rows.find((r) => r.kind === "video" && !blocked(r));
    if (!row) return;
    S.dock = row;
    if (!S.transcribing) player.cue(row);
    signature(); // ready before the first play asks the player endpoint
    schedule();
  };
  const playRow = async (row) => {
    if (!row || row.kind !== "video") return;
    S.dock = row;
    S.nowId = row.id;
    schedule();
    // A video that cannot play here (§2.9) keeps its thumbnail and word, and nothing is loaded.
    if (blocked(row)) { player.stop(); return; }
    const got = await playability(row.id, !!row.members);
    if (S.nowId !== row.id) return;
    if (got && (got.playable === false || got.live || got.upcoming)) { player.stop(); return; }
    player.play(row);
  };
  const activate = (row) => {
    if (row.kind === "video") playRow(row);
    else if (row.kind === "playlist") go({ view: "playlist", list: row.id, v: "" });
    else if (row.kind === "mix") {
      const seed = seedOf(row);
      go({ view: "mix", mix: seed, list: row.id === "RD" + seed ? "" : row.id });
    } else if (row.kind === "channel") go({ view: "channel", channel: row.id, t: "videos", s: "latest" });
  };

  // ---- rows (§2.2)

  const metaOf = (row) => {
    if (row.kind === "video") return [row.channel, row.views, row.age].filter(Boolean).join(" · ");
    if (row.kind === "mix") return "Mix";
    if (row.kind === "playlist") return row.channel ? row.channel + " · Playlist" : "Playlist";
    return [row.handle, row.subscribers].filter(Boolean).join(" · ");
  };
  const rowNode = (v, row) => {
    const node = el("div", "row");
    node.setAttribute("role", "listitem");
    node.tabIndex = -1;
    node.qmRow = row;
    const check = el("button", "check");
    check.type = "button";
    check.tabIndex = -1;
    check.setAttribute("role", "checkbox");
    check.setAttribute("aria-checked", "false");
    check.setAttribute("aria-label", row.title);
    check.append(icon("check"));
    const thumb = el("div", "thumb" + (row.kind === "channel" ? " round" : ""));
    if (row.thumb) {
      const img = el("img");
      img.alt = "";
      img.loading = "lazy";
      img.decoding = "async";
      img.src = row.thumb;
      img.addEventListener("error", () => img.remove(), { once: true });
      thumb.append(img);
    }
    const badge = el("span", "badge num");
    if (row.kind === "playlist") badge.append(icon("list-music"), row.count != null ? String(row.count) : "");
    else if (row.kind === "mix") badge.append(icon("radio"), "Mix");
    else if (row.kind === "video") badge.append(row.duration || (row.seconds ? clock(row.seconds) : ""));
    badge.hidden = row.kind === "channel" || (row.kind === "video" && !(row.duration || row.seconds));
    const word = el("span", "word");
    word.hidden = true;
    thumb.append(badge, word);
    const title = el("div", "title", row.title);
    title.title = row.title;
    const status = el("div", "status");
    status.hidden = true;
    const body = el("div", "body", thumb, el("div", "text", title, el("div", "meta", metaOf(row)), status));
    const act = el("div", "act");
    if (row.kind !== "channel") {
      node.convert = el("button", "convert");
      node.convert.type = "button";
      act.append(node.convert);
    }
    node.append(el("span", "tick", check), body, act);
    node.parts = { check, badge, word, status, act, thumb };
    if (row.kind === "video" ? P.isOther(row.title, queryOf(v, row)) : row.kind === "channel" && (row.topic || P.lessonChannel(row.title))) node.classList.add("dim");
    refreshThumb(node);
    return node;
  };
  // The badge warns on rows over 20:00 or whose chapters name two or more pieces (P.long); rows that cannot play here show their word.
  const refreshThumb = (node) => {
    const row = node.qmRow;
    if (row.kind !== "video") return;
    const word = blockedWord(row);
    node.parts.word.hidden = !word;
    node.parts.word.textContent = word;
    node.parts.badge.classList.toggle("warn", P.long(row));
    if (word) node.parts.badge.hidden = true;
    else if (row.duration || row.seconds) {
      node.parts.badge.hidden = false;
      node.parts.badge.textContent = row.duration || clock(row.seconds);
    }
    node.sig = null;
  };
  const paintRow = (v, node, st) => {
    const row = node.qmRow;
    const tickable = canTick(row, st);
    if (!tickable) v.ticks.delete(row.id);
    const ticked = tickable && v.ticks.has(row.id);
    const current = node.classList.contains("current");
    const sig = JSON.stringify([st, tickable, ticked, row.id === S.nowId, current, S.protocol]);
    if (node.sig === sig) return;
    node.sig = sig;
    const parts = node.parts;
    node.classList.toggle("ticked", ticked);
    node.classList.toggle("now", row.kind === "video" && row.id === S.nowId);
    parts.check.style.visibility = tickable ? "" : "hidden";
    parts.check.setAttribute("aria-checked", String(ticked));
    parts.check.tabIndex = current && tickable ? 0 : -1;
    let status = null;
    if (st.s === "failed" && st.reason) status = ["triangle-alert", st.reason, true];
    else if (st.s === "open" && st.path) status = ["file-music", fileName(st.path), false];
    else if (st.s === "listdone" && st.failed) status = ["triangle-alert", st.failed + " failed.", true];
    else if (st.note) status = [null, st.note, false];
    parts.status.hidden = !status;
    parts.status.classList.toggle("bad", !!(status && status[2]));
    parts.status.replaceChildren(...(status ? [...(status[0] ? [icon(status[0])] : []), el("span", null, status[1])] : []));
    if (node.convert) setConvert(node.convert, st);
    const removable = st.s === "queued" && S.protocol !== 1;
    if (removable && !parts.remove) {
      parts.remove = button("icon remove", "x", null, "Remove");
      parts.act.append(parts.remove);
    } else if (!removable && parts.remove) {
      parts.remove.remove();
      parts.remove = null;
    }
    if (parts.remove) parts.remove.tabIndex = current ? 0 : -1;
  };
  const nodeOf = (v, key) => (key ? v.nodes.get(key) : null);
  // Roving focus: the current row is the list's one tab stop, with its own buttons.
  const setCurrent = (v, node, focus) => {
    const old = nodeOf(v, v.focus);
    if (old && old !== node) { old.classList.remove("current"); old.tabIndex = -1; old.sig = null; }
    v.focus = node.qmRow.kind + ":" + node.qmRow.id;
    node.classList.add("current");
    node.tabIndex = 0;
    node.sig = null;
    if (focus) { node.focus(); node.scrollIntoView({ block: "nearest" }); }
    schedule();
  };
  let audition = 0;
  const moveFocus = (node, step) => {
    let target = node;
    do target = step > 0 ? target.nextElementSibling : target.previousElementSibling;
    while (target && !target.qmRow && !target.qmShelf);
    if (!target) return;
    // A shelf header takes focus on the way; the current row stays the list's tab stop.
    if (target.qmShelf) { target.focus(); target.scrollIntoView({ block: "nearest" }); return; }
    setCurrent(cur, target, true);
    clearTimeout(audition);
    if (target.qmRow.kind === "video") {
      audition = setTimeout(() => { if (document.activeElement === target) playRow(target.qmRow); }, AUDITION);
    }
  };
  const tick = (v, row, range) => {
    if (!canTick(row, rowState(row))) return;
    const a = range && v.anchor ? v.rows.findIndex((r) => r.id === v.anchor) : -1;
    const b = v.rows.indexOf(row);
    if (a >= 0 && b >= 0) {
      for (let i = Math.min(a, b); i <= Math.max(a, b); i++) {
        const r = v.rows[i];
        if (canTick(r, rowState(r))) v.ticks.add(r.id);
      }
    } else if (v.ticks.has(row.id)) v.ticks.delete(row.id);
    else v.ticks.add(row.id);
    v.anchor = row.id;
    schedule();
  };
  const skeleton = () => el("div", "row skeleton", el("span", "tick"),
    el("div", "body", el("div", "thumb"), el("div", "text", el("span", "bone"), el("span", "bone"))), el("div", "act"));
  const paintBox = (v) => {
    // Home's shelves show their own bones (§1.6).
    const waiting = !!v.shelves && v.shelves.some((s) => s.state === "wait");
    const loading = v.busy && !v.rows.length && !v.error && !v.shelves;
    if (loading && !v.skeleton) { v.skeleton = Array.from({ length: 6 }, skeleton); v.box.prepend(...v.skeleton); }
    if (!loading && v.skeleton) { v.skeleton.forEach((n) => n.remove()); v.skeleton = null; }
    const empty = !v.busy && !v.rows.length && !v.error && !waiting;
    if (empty && !v.empty) { v.empty = el("div", "empty", "No results."); v.box.prepend(v.empty); }
    if (!empty && v.empty) { v.empty.remove(); v.empty = null; }
    v.tail.hidden = v.shelves ? !(waiting && !v.error && !v.flight) : !(v.rows.length && !v.error && (v.reading || (v.more && v.ondemand)));
  };
  let io = null;
  const observe = (v) => {
    if (!io) {
      io = new IntersectionObserver((entries) => {
        if (!cur || !entries.some((e) => e.isIntersecting)) return;
        if (cur.shelves) shelves(cur, 1);
        else if (cur.ondemand) more(cur);
      }, { root: ui.scroll, rootMargin: "0px 0px 240px 0px" });
    }
    io.disconnect();
    const nodes = [...v.nodes.values()];
    if (nodes.length && !v.shelves) io.observe(nodes[Math.max(0, nodes.length - 5)]);
    io.observe(v.tail);
  };

  // ---- painting

  let ui = null;
  let frame = 0;
  const schedule = () => {
    if (!frame && ui) frame = requestAnimationFrame(() => { frame = 0; paint(); });
  };
  let dockFrame = 0; // the player's changes paint the dock alone
  const scheduleDock = () => {
    if (!dockFrame && !frame && ui) dockFrame = requestAnimationFrame(() => { dockFrame = 0; paintDock(); });
  };
  const paint = () => {
    if (frame) { cancelAnimationFrame(frame); frame = 0; }
    ui.back.disabled = H.i <= 0;
    ui.forward.disabled = H.i >= H.max;
    ui.home.disabled = !!cur && cur.kind === "home";
    ui.piano.disabled = !!cur && (UNBIASED.has(cur.kind) || (cur.kind === "channel" && cur.route.t === "similar"));
    if (cur) {
      let counted = 0;
      let ticked = 0;
      let videos = 0;
      const countedIds = [];
      for (const node of cur.nodes.values()) {
        const row = node.qmRow;
        const st = rowState(row);
        paintRow(cur, node, st);
        if (row.kind !== "video") continue;
        videos++;
        if (isCounted(queryOf(cur, row), row, st)) { counted++; countedIds.push(row.id); }
        if (cur.ticks.has(row.id)) ticked++;
      }
      const focus = nodeOf(cur, cur.focus);
      if (focus && !focus.classList.contains("current")) { focus.classList.add("current"); focus.tabIndex = 0; focus.sig = null; paintRow(cur, focus, rowState(focus.qmRow)); }
      cur.countedIds = countedIds;
      paintHead(cur, counted, ticked, videos);
      paintBox(cur);
      paintBar(cur);
      paintStrip(cur);
    }
    ui.loading.hidden = !(cur && cur.busy);
    paintBanners();
    paintDock();
    paintTray();
    paintNarrow();
  };
  const paintHead = (v, counted, ticked, videos) => {
    const entry = v.list ? entryState(v.list.key) : null;
    const show = videos > 0 && (counted > 0 || ticked > 0 || v.reading || !!entry);
    ui.listhead.hidden = !show;
    if (!show) return;
    ui.count.textContent = String(counted);
    const all = counted > 0 && v.countedIds.every((id) => v.ticks.has(id));
    ui.all.setAttribute("aria-checked", all ? "true" : ticked ? "mixed" : "false");
    setIcon(ui.all.firstElementChild, ticked && !all ? "minus" : "check");
    // While a list reads its pages, "Reading N" holds the slot; "Convert N" replaces it when reading ends (§3.10).
    ui.convertTicked.hidden = !ticked || v.reading;
    ui.untick.hidden = !ticked || v.reading;
    ui.ticked.textContent = String(ticked);
    // Save to shows with Convert N, and where the ticked rows go (§5.1).
    ui.saveTicked.hidden = !S.folders || ui.convertTicked.hidden;
    if (!ui.saveTicked.hidden) {
      const groups = byDest(v, tickedRows(v));
      setSave(ui.saveTicked, groups.length ? groups.map((g) => g.dest) : [listDest(v) ?? ""]);
    }
    const slot = v.reading ? { s: "reading", n: videos } : ticked ? null : entry;
    ui.headState.hidden = !slot;
    if (slot) setConvert(ui.headState, slot);
  };
  const paintBar = (v) => {
    const list = v.kind === "playlist" || v.kind === "channel" || v.kind === "mix" || v.kind === "similar";
    const genres = v.kind === "home" || v.kind === "genre";
    ui.filters.hidden = list || genres;
    ui.listbar.hidden = !list;
    ui.genres.hidden = !genres;
    const bar = list ? ui.listbar : genres ? ui.genres : ui.filters;
    if (ui.loading.parentElement !== bar) {
      if (genres) ui.genreSort.before(ui.loading);
      else bar.append(ui.loading);
    }
    if (genres) {
      for (const b of ui.pills.children) b.setAttribute("aria-checked", String(v.kind === "genre" && b.dataset.genre === v.route.g));
      const on = [...ui.pills.children].find((b) => b.getAttribute("aria-checked") === "true") || ui.pills.firstElementChild;
      for (const b of ui.pills.children) b.tabIndex = b === on ? 0 : -1;
      ui.genreSort.disabled = v.kind !== "genre";
      ui.genreSort.classList.toggle("active", v.kind === "genre" && v.route.s === "new");
      return;
    }
    if (!list) {
      const r = v.kind === "search" ? v.route : S.lastSearch || defaults();
      for (const b of ui.type.children) b.setAttribute("aria-checked", String(b.dataset.type === r.f));
      ui.duration.hidden = r.f !== "videos";
      const text = label(DURATIONS, r.d);
      if (ui.durationLabel.textContent !== text) ui.durationLabel.textContent = text;
      ui.duration.classList.toggle("active", root.classList.contains("narrow") && r.d !== "any");
      ui.sort.classList.toggle("active", r.s === "popular");
      return;
    }
    const sig = JSON.stringify([v.kind, v.head, v.route.g, v.route.t, v.route.s]);
    if (ui.listbar.sig === sig) return;
    ui.listbar.sig = sig;
    const heads = el("div", "heads");
    const h = v.head;
    const name = (text) => { const n = el("span", "name", text); n.title = text; return n; };
    const meta = (parts) => el("span", "meta", parts.filter(Boolean).join(" · "));
    if (!h) heads.append(el("span", "bone"));
    else if (v.kind === "playlist") heads.append(icon("list-music"), name(h.title), meta([h.channel, h.countText]));
    else if (v.kind === "mix") heads.append(icon("radio"), name(h.title));
    else if (v.kind === "similar") heads.append(icon("users"), name("Similar"), meta([h.title]));
    else {
      if (h.avatar) {
        const img = el("img", "avatar");
        img.alt = "";
        img.src = h.avatar;
        img.addEventListener("error", () => img.remove(), { once: true });
        heads.append(img);
      }
      heads.append(name(h.title), meta([h.handle, h.subscribers]));
    }
    const extras = [];
    if (v.kind === "channel") {
      const seg = el("div", "seg");
      seg.setAttribute("role", "radiogroup");
      const tabs = [["videos", "Videos", "list-video"], ["playlists", "Playlists", "list-music"], ["similar", "Similar", "users"]];
      // Opened from a genre: its songs first, under the genre's pill word (§3.19 Genre scope).
      if (v.route.g) tabs.unshift(["genre", GENRES.get(v.route.g).word, "funnel"]);
      const read = (t) => t === "genre" || t === "videos";
      for (const [t, text, symbol] of tabs) {
        const b = button("", symbol, text);
        b.setAttribute("role", "radio");
        b.setAttribute("aria-checked", String(v.route.t === t));
        b.tabIndex = v.route.t === t ? 0 : -1;
        stable(b.lastElementChild);
        // The genre's songs and the whole Videos are one read: the sort stays between them.
        b.addEventListener("click", () => {
          if (cur.route.t !== t) go({ ...cur.route, t, s: read(t) && read(cur.route.t) ? cur.route.s : "latest" });
        });
        seg.append(b);
      }
      seg.addEventListener("keydown", radioKeys);
      extras.push(seg);
      if (v.route.t !== "playlists") {
        const sort = button("icon", "arrow-up-down", null, "Sort");
        sort.setAttribute("aria-haspopup", "menu");
        sort.disabled = v.route.t === "similar";
        sort.classList.toggle("active", v.route.s !== "latest");
        sort.addEventListener("click", () => openMenu(sort, CHANNEL_SORTS.map(([s, text]) => ({
          text, checked: cur.route.s === s, run: () => { if (cur.route.s !== s) go({ ...cur.route, s }); },
        }))));
        extras.push(sort);
      }
    }
    ui.listbar.replaceChildren(heads, ...extras, ui.loading);
  };
  // The pianist strip (§1.4): set from the view's first load and then frozen, so later pages never move the list.
  // None in a channel's own tabs or Channels and Playlists results, nor under 3 cards. A genre's starts with its
  // curated pianists; Home's with yours, then curated ones of the genres you convert (§3.19).
  const setStrip = (v) => {
    v.strip = null;
    if ((v.kind === "search" && v.route.f !== "videos") || (v.kind === "channel" && v.route.t !== "similar")) return;
    let own = [];
    let cards;
    if (v.kind === "home") {
      own = [...S.artists].filter(([id, a]) => a.name && !P.BLOCK.includes(id)).sort((a, b) => time(b[1].at) - time(a[1].at))
        .slice(0, OWN).map(([id, a]) => ({ id, name: a.name, avatar: a.avatar }));
      const mine = [...S.artists.keys()];
      const curated = P.homeCurated(S.seeds.map((s) => s && s.title).filter(Boolean), mine, HOME_CURATED);
      // Each shelf's rows count against its own words.
      const found = P.pianists(v.rows, (row) => queryOf(v, row), S.pianists, [...mine, ...curated.map((p) => p.id)]);
      cards = [...own, ...curated, ...found];
    } else if (v.kind === "genre") cards = P.genrePianists(v.route.g, v.rows, S.pianists, CARDS, S.turn);
    else cards = P.pianists(v.rows, v.query, S.pianists, v.kind === "channel" ? v.route.channel : null);
    // A blocked channel is never a card, yours or found (§3.19 Curated).
    cards = cards.filter((p) => !P.BLOCK.includes(p.id)).slice(0, CARDS);
    if (cards.length >= 3) v.strip = { cards, own: own.length };
  };
  const stripOf = (s) => {
    const box = el("div", "cards");
    s.cards.forEach((p, i) => {
      if (i && i === s.own) box.append(el("span", "divider"));
      const face = el("span", "face");
      const letter = () => face.replaceChildren(([...p.name.trim()][0] || "").toUpperCase());
      // A picture read this session comes first (a curated one can go stale), then the card's own, then the curated
      // one: a Mix's rows carry none.
      const src = faceOf(p.id) || p.avatar || curatedFace(p.id);
      if (src) {
        const img = el("img");
        img.alt = "";
        img.loading = "lazy";
        img.src = src;
        img.addEventListener("error", letter, { once: true });
        face.append(img);
      } else letter();
      const b = el("button", "card", face, el("span", "name", p.name));
      b.type = "button";
      b.tabIndex = i ? -1 : 0;
      b.qmPianist = p;
      named(b, p.name);
      box.append(b);
    });
    return box;
  };
  const paintStrip = (v) => {
    ui.strip.hidden = !v.strip;
    if (v.strip && !v.stripBox) v.stripBox = stripOf(v.strip);
    const box = v.strip ? v.stripBox : null;
    // Wide, the strip wraps onto as many lines as its cards need; narrow, every card is on its one scrolling line
    // (§2.1). Either way each card can be reached.
    if (ui.strip.firstElementChild !== box) ui.strip.replaceChildren(...(box ? [box] : []));
  };
  const paintBanners = () => {
    const list = [];
    const retryButton = ["refresh-cw", "Retry", () => retry(cur)];
    if (S.paused === "signin") {
      list.push(["triangle-alert", true, "YouTube asked to sign in.", [["log-in", "Sign in", () => call("sign_in")], ["refresh-cw", "Retry", () => call("resume")]]]);
    } else if (S.paused === "folder") list.push(["folder", false, "Choose a MIDI folder.", [["folder", "Choose folder", () => call("choose_folder")]]]);
    if (S.refused) list.push(["triangle-alert", true, "Folder not allowed.", []]);
    const e = cur && cur.error;
    if (e === "limit") list.push(["triangle-alert", true, "YouTube is limiting requests.", [retryButton]]);
    else if (e === "offline") list.push(["wifi-off", false, "No connection.", [retryButton]]);
    else if (e === "read") {
      list.push(["triangle-alert", true, "Results could not be read.",
        [retryButton, ["external-link", "Open on YouTube", () => call("open_external", youtubeUrl(cur.route))]]]);
    }
    const sig = list.map((b) => b[2]).join("|");
    if (ui.banners.sig === sig) return;
    ui.banners.sig = sig;
    ui.banners.hidden = !list.length;
    ui.banners.replaceChildren(...list.map(([name, warn, text, buttons]) => el("div", "banner", icon(name, warn ? "warn" : null), el("span", null, text),
      ...buttons.map(([symbol, words, run]) => {
        const b = button("", symbol, words);
        b.addEventListener("click", run);
        return b;
      }))));
  };
  const paintAccount = () => {
    ui.signin.hidden = S.signedIn;
    ui.account.hidden = !S.signedIn;
    const a = S.account;
    named(ui.account, (a && a.name) || "Account");
    const img = ui.account.querySelector("img");
    if (a && a.avatar) {
      if (img && img.src === a.avatar) return;
      const avatar = el("img");
      avatar.alt = "";
      avatar.src = a.avatar;
      avatar.addEventListener("error", () => ui.account.replaceChildren(icon("user")), { once: true });
      ui.account.replaceChildren(avatar);
    } else if (img || !ui.account.firstElementChild) ui.account.replaceChildren(icon("user"));
  };
  const paintDock = () => {
    const row = S.dock;
    ui.dock.classList.toggle("empty", !row);
    if (!row) {
      ui.poster.hidden = true; ui.frameWord.hidden = true; ui.ad.hidden = true; ui.transport.hidden = false; ui.frame.classList.remove("loading");
      return;
    }
    if (ui.dockName.textContent !== row.title) { ui.dockName.textContent = row.title; ui.dockName.title = row.title; }
    const channel = row.channel || row.owner || "";
    const words = ui.dockChannel.lastElementChild;
    if (words.textContent !== channel) { words.textContent = channel; ui.dockChannel.title = channel; }
    ui.dockChannel.hidden = !channel;
    ui.dockChannel.disabled = !channelOf(row);
    const mine = player.id === row.id;
    const poster = mq(row.id);
    if (ui.poster.getAttribute("src") !== poster) ui.poster.src = poster;
    // Loading shows the --well skeleton; an ad and anything not yet shown, the thumbnail.
    const loading = mine && player.state === "loading";
    ui.frame.classList.toggle("loading", loading);
    ui.poster.hidden = mine && (player.showing || loading);
    const word = (mine && player.overlay) || blockedWord(row);
    ui.frameWord.hidden = !word;
    if (ui.frameWord.textContent !== word) ui.frameWord.textContent = word;
    // A video that cannot play here has no transport: nothing in it would act (§2.9).
    const cannot = blocked(row);
    ui.transport.hidden = cannot;
    if (cannot) {
      if (S.enlarged) enlarge(false);
      closePopover();
    }
    const ad = mine && player.state === "ad";
    ui.ad.hidden = !ad;
    const playing = mine && RUNNING.has(player.state);
    setIcon(ui.play.firstElementChild, playing ? "pause" : "play");
    named(ui.play, playing ? "Pause" : "Play");
    const duration = (mine && player.duration) || row.seconds || 0;
    const time = mine ? Math.min(player.time || 0, duration || Infinity) : 0;
    ui.seek.classList.toggle("idle", !duration || !mine || player.state === "idle" || player.state === "unplayable" || !!word);
    ui.seek.classList.toggle("ad", ad);
    ui.seek.style.setProperty("--at", (duration ? Math.min(100, (100 * time) / duration) : 0).toFixed(2) + "%");
    ui.seek.setAttribute("aria-valuemax", String(Math.round(duration)));
    ui.seek.setAttribute("aria-valuenow", String(Math.round(time)));
    ui.seek.setAttribute("aria-valuetext", clock(time));
    const times = duration ? clock(time) + " / " + clock(duration) : "";
    if (ui.time.textContent !== times) ui.time.textContent = times;
    setIcon(ui.volume.firstElementChild, S.muted || S.volume === 0 ? "volume-x" : "volume-2");
    setIcon(ui.enlarge.firstElementChild, S.enlarged ? "minimize-2" : "maximize-2");
    named(ui.enlarge, S.enlarged ? "Shrink" : "Enlarge");
    const st = rowState(row);
    // The dock's folder is for the video it holds.
    if (S.dockDest && S.dockDest.id !== row.id) S.dockDest = null;
    setConvert(ui.dockConvert, st, { primary: true });
    // Done: Open is named for where the file is, and Save to is for Convert again (§5.1).
    if (st.s === "open" && st.path) named(ui.dockConvert, where(st.path));
    ui.again.hidden = st.s !== "open";
    ui.saveDock.hidden = !S.folders || !["idle", "failed", "open", "signin"].includes(st.s);
    if (!ui.saveDock.hidden) setSave(ui.saveDock, [dockDest(row)]);
  };
  const thumbOf = (key) => {
    if (key.startsWith("v:")) return [key.slice(2), false];
    const pick = S.picks.get(key);
    const firstItem = pick && pick.items && pick.items[0];
    return [firstItem ? firstItem.id : null, true];
  };
  const mini = (key) => {
    const [id, list] = thumbOf(key);
    const box = el("div", "mini");
    if (id) {
      const img = el("img");
      img.alt = "";
      img.loading = "lazy";
      img.src = mq(id);
      box.append(img);
    }
    if (list) box.append(el("span", "badge", icon("list-music")));
    return box;
  };
  const paintTray = () => {
    if (S.dragging) return;
    const rank = (it) => (it.state === "converting" ? -1 : it.state === "failed" ? 1e7 : it.position != null ? it.position : 1e6);
    const entries = [...S.items.values()].filter((it) => ["queued", "converting", "failed"].includes(it.state)).sort((a, b) => rank(a) - rank(b));
    const sig = JSON.stringify([entries.map((it) => [it.key, it.state, it.phase, it.percent, it.done, it.total, it.reason, it.title, it.position, destFor(it.key)]),
      S.done.map((d) => [d.path, d.dest]), S.paused, S.protocol, !!S.folders, S.listed, S.made.size]);
    if (ui.tray.sig === sig) return;
    ui.tray.sig = sig;
    ui.tray.hidden = !entries.length && !S.done.length;
    ui.queueHead.hidden = !entries.length;
    let front = true;
    // A chip whose menu is open is redrawn under it: the menu stays, owned by the new chip.
    const owned = menuOwner && menuOwner.classList.contains("chip") && ui.queue.contains(menuOwner) ? menuOwner.closest(".entry").dataset.key : null;
    ui.queue.replaceChildren(...entries.map((it) => {
      const node = entryNode(it, front && it.state === "queued");
      if (it.state === "queued") front = false;
      return node;
    }));
    if (owned) {
      const node = [...ui.queue.children].find((n) => n.dataset.key === owned);
      const chip = node && node.querySelector(".chip");
      if (chip && chip.tagName === "BUTTON") { menuOwner = menuReturn = chip; chip.setAttribute("aria-expanded", "true"); } else closeMenu();
    }
    ui.convertedHead.hidden = !S.done.length;
    ui.converted.replaceChildren(...S.done.map((d) => {
      const name = el("div", "name", d.title);
      name.title = d.title;
      const open = button("ok", "file-music", "Open");
      open.addEventListener("click", () => openFile(d.path));
      const lines = el("div", "lines", name);
      // Its folder (§5.1): the one it is in, named by its place.
      const parts = String(d.path).split(/[\\/]/).filter(Boolean);
      if (S.folders) {
        lines.append(el("div", "state", d.dest === "" ? folderTag("folder-root", "MIDI folder", where(d.path)) : folderTag("folder", parts[parts.length - 2] || "", where(d.path))));
      }
      const node = el("div", "entry", el("span", "grip"), mini("v:" + d.id), lines, open);
      node.setAttribute("role", "listitem");
      return node;
    }));
  };
  const entryNode = (it, front) => {
    const st = fromItem(it);
    const queued = it.state === "queued";
    const running = it.state === "converting";
    const failed = it.state === "failed";
    const node = el("div", "entry" + (queued ? " waiting" : ""));
    node.setAttribute("role", "listitem");
    node.dataset.key = it.key;
    const grip = el("span", "grip");
    if (queued && S.protocol !== 1) {
      grip.append(icon("grip-vertical"));
      grip.addEventListener("pointerdown", (e) => drag(e, node, grip));
    }
    const title = it.title || S.titles.get(String(it.key).slice(2)) || (S.picks.get(it.key) || {}).title || it.key;
    const name = el("div", "name", title);
    name.title = title;
    const text = failed ? it.reason || "Conversion failed." : queued ? (front && S.paused ? "Paused" : "Queued") : stateText(st);
    const state = el("div", "state" + (failed ? " bad" : ""), el("span", null, text));
    // One folder per entry (§5.1): a chip to move it while it waits or failed, plain once it runs.
    const dest = S.folders ? destFor(it.key) : null;
    if (dest != null && running) state.append(folderTag(destIcon(dest), dest ? lastName(dest) : "MIDI folder", destText(dest)));
    else if (dest != null) {
      const chip = el("button", "secondary save chip");
      chip.type = "button";
      chip.setAttribute("aria-haspopup", "menu");
      setSave(chip, [dest]);
      chip.addEventListener("click", () => {
        const artist = entryArtist(it.key);
        folderMenu(chip, dest, artist, (to) => retarget(it.key, to, artist));
      });
      state.append(chip);
    }
    node.append(grip, mini(it.key), el("div", "lines", name, state));
    if (failed && (S.picks.has(it.key) || it.key.startsWith("v:"))) {
      const again = button("icon", "rotate-ccw", null, "Retry");
      again.addEventListener("click", () => {
        S.items.delete(it.key);
        if (S.picks.has(it.key)) resend(it.key);
        else {
          // A video read back after a remount keeps the folder the app gave it (§5.3).
          const fields = pickFolder(dest ?? "", entryArtist(it.key), null);
          send(it.key, { kind: "video", id: it.key.slice(2), title: String(title).slice(0, 300), again: false, ...fields }, []);
        }
      });
      node.append(again);
    }
    if (failed || S.protocol !== 1) {
      const x = button("icon", "x", null, running ? "Stop" : "Remove");
      x.addEventListener("click", () => {
        if (failed) { S.items.delete(it.key); schedule(); } else call("drop", it.key);
      });
      node.append(x);
    }
    if (running) {
      const bar = el("div", "progress");
      if (st.s === "transcribing") bar.style.setProperty("--fill", st.percent + "%");
      else if (st.s === "list") bar.style.setProperty("--fill", (100 * st.done) / st.total + "%");
      else bar.classList.add("busy");
      node.append(bar);
    }
    return node;
  };
  // §3.12: a grip drags a waiting entry; the drop sends the new order.
  const drag = (e, node, grip) => {
    if (e.button !== 0) return;
    e.preventDefault();
    const nodes = [...ui.queue.querySelectorAll(".entry.waiting")];
    const from = nodes.indexOf(node);
    const height = node.offsetHeight;
    const start = e.clientY;
    let to = from;
    S.dragging = true;
    node.classList.add("dragging");
    grip.setPointerCapture(e.pointerId);
    const move = (ev) => {
      const dy = (ev.clientY - start) / scale;
      to = Math.max(0, Math.min(nodes.length - 1, from + Math.round(dy / height)));
      node.style.transform = `translateY(${dy}px)`;
      nodes.forEach((n, i) => {
        if (n === node) return;
        const shift = i > from && i <= to ? -height : i < from && i >= to ? height : 0;
        n.style.transform = shift ? `translateY(${shift}px)` : "";
      });
    };
    const up = () => {
      grip.removeEventListener("pointermove", move);
      grip.removeEventListener("pointerup", up);
      grip.removeEventListener("pointercancel", up);
      nodes.forEach((n) => { n.style.transform = ""; });
      node.classList.remove("dragging");
      S.dragging = false;
      if (to !== from) {
        const keys = nodes.map((n) => n.dataset.key);
        keys.splice(to, 0, keys.splice(from, 1)[0]);
        keys.forEach((key, i) => { const it = S.items.get(key); if (it) it.position = i + 1; });
        call("order", keys);
      }
      ui.tray.sig = null;
      schedule();
    };
    grip.addEventListener("pointermove", move);
    grip.addEventListener("pointerup", up);
    grip.addEventListener("pointercancel", up);
  };
  // Narrow (§2.6): the controls that lose their words carry them as their names, and the dock is a bottom sheet.
  const paintNarrow = () => {
    const narrow = root.classList.contains("narrow");
    const titled = (b, title) => {
      if (narrow) named(b, title);
      else if (b.title) { b.removeAttribute("title"); b.removeAttribute("aria-label"); }
    };
    titled(ui.signin, "Sign in");
    titled(ui.app, "QuartzMIDI");
    for (const b of ui.type.children) titled(b, b.textContent);
    titled(ui.duration, ui.durationLabel.textContent);
    const queued = [...S.items.values()].filter((it) => ["queued", "converting", "failed"].includes(it.state)).length;
    ui.queueButton.hidden = !narrow || !queued;
    if (ui.queueCount.textContent !== String(queued)) ui.queueCount.textContent = String(queued);
    ui.collapse.hidden = !S.sheet;
    if (narrow && !S.sheet) ui.side.tabIndex = 0;
    else ui.side.removeAttribute("tabindex");
  };
  // Expands or collapses the sheet; toTray scrolls the expanded sheet to the queue. Focus inside a sheet that
  // collapses goes to the sheet itself, whose Enter expands it again.
  const setSheet = (on, toTray) => {
    const narrow = root.classList.contains("narrow");
    on = on && narrow;
    if (S.sheet !== on) {
      const inside = ui.side.contains(document.activeElement);
      S.sheet = on;
      if (!on && S.enlarged) enlarge(false);
      root.classList.toggle("sheet", on);
      ui.collapse.hidden = !on;
      if (narrow && !on) ui.side.tabIndex = 0;
      else ui.side.removeAttribute("tabindex");
      ui.side.scrollTop = 0;
      if (inside && !on && narrow) ui.side.focus();
      schedule();
    }
    if (on && toTray && !ui.tray.hidden) ui.side.scrollTop = ui.tray.offsetTop;
  };
  const show = (v) => {
    cur = v;
    v.used = ++used;
    setSheet(false);
    if (v.kind === "search") {
      S.lastSearch = v.route;
      S.piano = v.route.p === "1";
      ui.q.value = v.route.q;
      ui.clear.hidden = !ui.q.value;
    } else if (v.kind === "home") {
      ui.q.value = "";
      ui.clear.hidden = true;
      ui.q.focus();
    }
    setPiano();
    ui.listbar.sig = null;
    ui.banners.sig = null;
    ui.rows.replaceChildren(v.box);
    // A menu open on the last view closes; focus goes back to its owner only when that is still on the page.
    closeMenu();
    for (const node of v.nodes.values()) node.sig = null;
    paint();
    ui.scroll.scrollTop = v.scroll || 0;
    observe(v);
  };

  // ---- menus, popovers and suggestions (§2.7, §2.10, §3.3)

  let menuOwner = null;
  let menuReturn = null;
  let popover = null;
  const place = (box, x, y, right) => {
    box.style.left = "0px";
    box.style.top = "0px";
    const w = box.offsetWidth;
    const h = box.offsetHeight;
    const vw = innerWidth / scale;
    const vh = innerHeight / scale;
    const left = Math.max(8, Math.min(right ? x - w : x, vw - w - 8));
    const top = y + h > vh - 8 ? Math.max(8, vh - h - 8) : y;
    box.style.left = left + "px";
    box.style.top = top + "px";
  };
  // A menu may have a field over its items (§5.2): typing gives new items, the first highlighted, and focus stays in it.
  let menuField = null;
  const menuItems = (items) => items.map((it) => {
    if (it.note) return el("div", "menu-note", it.note);
    const b = button("menu-item", it.icon || null, it.text);
    b.setAttribute("role", it.checked != null ? "menuitemradio" : "menuitem");
    b.tabIndex = -1;
    if (it.checked != null) {
      b.setAttribute("aria-checked", String(!!it.checked));
      if (it.checked) b.append(icon("check", "mark"));
    }
    b.addEventListener("click", () => { closeMenu(true); it.run(); });
    return b;
  });
  const lightItem = (b) => {
    for (const x of ui.menu.querySelectorAll(".menu-item.hl")) x.classList.remove("hl");
    if (!b) return;
    b.classList.add("hl");
    if (b.scrollIntoView) b.scrollIntoView({ block: "nearest" });
  };
  const openMenu = (owner, items, at, field = null) => {
    const back = document.activeElement;
    closeMenu();
    closePopover();
    const m = ui.menu;
    m.classList.toggle("fields", !!field);
    m.replaceChildren(...menuItems(items));
    if (field) {
      const input = el("input");
      input.type = "text";
      input.value = "";
      input.spellcheck = false;
      input.setAttribute("autocomplete", "off");
      input.setAttribute("placeholder", field.placeholder);
      input.setAttribute("aria-label", field.placeholder);
      input.addEventListener("input", () => {
        // The field stays put, so it keeps focus.
        for (const x of [...m.children].slice(1)) x.remove();
        m.append(...menuItems(field.items(input.value)));
        lightItem(m.querySelector(".menu-item"));
      });
      m.prepend(el("label", "menu-field", icon(field.icon), input));
      menuField = input;
    }
    m.hidden = false;
    m.style.maxHeight = m.style.bottom = "";
    if (owner && field) {
      // The field stays against its button while the items under it change: the menu takes the side with more room.
      const r = owner.getBoundingClientRect();
      const vh = innerHeight / scale;
      const below = vh - r.bottom / scale - 12;
      const above = r.top / scale - 12;
      const down = below >= 240 || below >= above;
      m.style.maxHeight = Math.max(0, Math.min(360, down ? below : above)) + "px";
      place(m, r.left / scale, down ? r.bottom / scale + 4 : 0, false);
      if (!down) {
        m.style.top = "auto";
        m.style.bottom = (vh - r.top / scale + 4) + "px";
      }
      owner.setAttribute("aria-expanded", "true");
    } else if (owner) {
      const r = owner.getBoundingClientRect();
      const right = owner === ui.account;
      place(m, (right ? r.right : r.left) / scale, r.bottom / scale + 4, right);
      owner.setAttribute("aria-expanded", "true");
    } else place(m, at.x / scale, at.y / scale, false);
    menuOwner = owner;
    menuReturn = owner || (back && back !== document.body ? back : null);
    const items2 = [...m.querySelectorAll(".menu-item")];
    if (menuField) {
      lightItem(items2[0]);
      menuField.focus();
      return;
    }
    const target = items2.find((b) => b.getAttribute("aria-checked") === "true") || items2[0];
    if (target) target.focus();
  };
  const closeMenu = (restore = false) => {
    if (ui.menu.hidden) return false;
    const inside = ui.menu.contains(document.activeElement);
    ui.menu.hidden = true;
    ui.menu.replaceChildren();
    menuField = null;
    if (menuOwner) menuOwner.setAttribute("aria-expanded", "false");
    // Focus goes back to the button or row that opened the menu.
    if (menuReturn && menuReturn.isConnected && (restore || inside || document.activeElement === document.body)) menuReturn.focus();
    menuOwner = null;
    menuReturn = null;
    return true;
  };
  const menuKeys = (e) => {
    const items = [...ui.menu.querySelectorAll(".menu-item")];
    if (menuField && e.target === menuField) {
      // In the field, Up and Down move the highlight and Enter takes it.
      const at = items.findIndex((b) => b.classList.contains("hl"));
      if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        e.preventDefault();
        const step = e.key === "ArrowDown" ? 1 : -1;
        if (items.length) lightItem(items[at < 0 ? (step > 0 ? 0 : items.length - 1) : (at + step + items.length) % items.length]);
      } else if (e.key === "Enter") {
        e.preventDefault();
        if (at >= 0) items[at].click();
      } else if (e.key === "Tab") closeMenu(true);
      return;
    }
    const at = items.indexOf(document.activeElement);
    if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      const step = e.key === "ArrowDown" ? 1 : -1;
      if (items.length) items[(at + step + items.length) % items.length].focus();
    } else if (e.key === "Home" || e.key === "End") {
      e.preventDefault();
      if (items.length) items[e.key === "Home" ? 0 : items.length - 1].focus();
    } else if (e.key === "Tab") closeMenu(true);
  };
  const rowMenu = (row, at) => {
    const items = [
      { icon: "copy", text: "Copy link", run: () => copy(linkOf(row)) },
      { icon: "external-link", text: "Open on YouTube", run: () => call("open_external", linkOf(row)) },
    ];
    const st = rowState(row);
    if (row.kind === "video" && st.s === "open") {
      items.push({ icon: "file-music", text: "Open", run: () => openFile(st.path) },
        { icon: "rotate-ccw", text: "Convert again", run: () => convertRow(cur, row, true) });
    }
    if (row.kind === "video") {
      const id = channelOf(row);
      if (id && (row.channel || row.owner)) items.push({ icon: "user", text: row.channel || row.owner, run: () => openChannel(id, scopeOf(row)) });
      items.push({ icon: "users", text: "Similar", run: () => go({ view: "similar", similar: row.id }) });
    }
    openMenu(null, items, at);
  };
  const pianistMenu = (p, at) => {
    const url = ORIGIN + "/channel/" + p.id;
    openMenu(null, [
      { icon: "copy", text: "Copy link", run: () => copy(url) },
      { icon: "external-link", text: "Open on YouTube", run: () => call("open_external", url) },
    ], at);
  };
  const closePopover = () => {
    if (!popover) return false;
    popover.remove();
    popover = null;
    ui.volume.setAttribute("aria-expanded", "false");
    return true;
  };
  const openVolume = () => {
    if (closePopover()) return;
    closeMenu();
    const mute = button("icon", S.muted ? "volume-x" : "volume-2", null, "Volume");
    mute.setAttribute("aria-pressed", String(S.muted));
    const range = el("input", "range");
    range.type = "range";
    range.min = "0";
    range.max = "100";
    range.step = "1";
    range.value = String(S.volume);
    range.setAttribute("aria-label", "Volume");
    const sync = () => {
      range.style.setProperty("--v", (S.muted ? 0 : S.volume) + "%");
      setIcon(mute.firstElementChild, S.muted || S.volume === 0 ? "volume-x" : "volume-2");
      mute.setAttribute("aria-pressed", String(S.muted));
      write("qm-volume", S.volume);
      write("qm-muted", S.muted ? "1" : "0");
      player.volume(S.volume, S.muted);
      schedule();
    };
    mute.addEventListener("click", () => { S.muted = !S.muted; sync(); });
    range.addEventListener("input", () => { S.volume = Number(range.value); if (S.volume > 0) S.muted = false; sync(); });
    popover = el("div", "popover", mute, range);
    popover.setAttribute("role", "dialog");
    popover.setAttribute("aria-label", "Volume");
    document.body.append(popover);
    sync();
    const r = ui.volume.getBoundingClientRect();
    place(popover, r.right / scale, r.bottom / scale + 4, true);
    ui.volume.setAttribute("aria-expanded", "true");
    range.focus();
  };
  // A link in the field: its route, or the link to convert as it is (§3.15). A bare word is never a link.
  const asLink = (text) => {
    const t = String(text || "").trim();
    if (!t || /\s/.test(t)) return null;
    if (/^https?:\/\/\S+\.\S+/i.test(t)) return t;
    return /^((www|m|music)\.)?(youtube\.com|youtu\.be)\//i.test(t) ? "https://" + t : null;
  };
  const linkItem = (text) => {
    const url = asLink(text);
    if (!url) return null;
    const route = P.routeFor(url);
    if (route) {
      const r = parseRoute(route);
      const words = { video: "Open this video", playlist: "Open this playlist", channel: "Open this channel" }[r.view];
      return { url, route: r, text: words || null };
    }
    return { url, route: null, text: "Convert this link" };
  };
  const runLink = (link) => {
    hideSuggest();
    if (link.route) { if (link.route.view === "search") runSearch(link.route.q); else go(link.route); return; }
    send("link:" + link.url, { kind: "link", url: link.url, title: "", again: false, ...pickFolder("", null, null) }, []);
    ui.q.select();
  };
  let suggestTimer = 0;
  let suggestSeq = 0;
  let options = [];
  let highlight = -1;
  const hideSuggest = () => {
    clearTimeout(suggestTimer);
    suggestSeq++;
    options = [];
    highlight = -1;
    ui.suggest.hidden = true;
    ui.suggest.replaceChildren();
    ui.q.setAttribute("aria-expanded", "false");
    return true;
  };
  const showSuggest = (list, typed) => {
    options = list;
    highlight = list.length === 1 && list[0].link ? 0 : -1;
    ui.suggest.replaceChildren(...list.map((o, i) => {
      const b = el("button", "menu-item");
      b.type = "button";
      b.tabIndex = -1;
      b.setAttribute("role", "option");
      if (o.link) b.append(icon("link"), el("span", null, o.text));
      else {
        // The typed prefix stays regular; the rest of the suggestion is semibold.
        const text = o.text;
        const same = text.toLowerCase().startsWith(typed.toLowerCase()) ? typed.length : 0;
        b.append(icon("search"), el("span", null, text.slice(0, same), el("b", null, text.slice(same))));
      }
      b.addEventListener("mousedown", (e) => e.preventDefault());
      b.addEventListener("mousemove", () => { if (highlight !== i) { highlight = i; markSuggest(); } });
      b.addEventListener("click", () => pickSuggest(i));
      return b;
    }));
    ui.suggest.hidden = !list.length;
    ui.q.setAttribute("aria-expanded", String(!!list.length));
    markSuggest();
  };
  const markSuggest = () => {
    [...ui.suggest.children].forEach((b, i) => b.classList.toggle("hl", i === highlight));
  };
  const pickSuggest = (i) => {
    const o = options[i];
    if (!o) return;
    if (o.link) { runLink(o.link); return; }
    ui.q.value = o.text;
    runSearch(o.text);
  };
  const suggest = async () => {
    const text = ui.q.value;
    const seq = ++suggestSeq;
    const link = linkItem(text);
    if (link) {
      if (link.text) showSuggest([{ link, text: link.text }], text);
      else { options = []; ui.suggest.hidden = true; }
      return;
    }
    if (!text.trim()) { hideSuggest(); return; }
    try {
      const client = await getClient();
      const response = await fetch(P.suggestUrl(client, text), { credentials: "omit" });
      const list = P.parseSuggest(await response.text()).slice(0, 8);
      if (seq !== suggestSeq || ui.q.value !== text || document.activeElement !== ui.q) return;
      showSuggest(list.map((s) => ({ text: s })), text);
    } catch {
      if (seq === suggestSeq) { options = []; ui.suggest.hidden = true; }
    }
  };
  const copy = async (text) => {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch { /* the older way below */ }
    const back = document.activeElement;
    const area = el("textarea");
    area.value = text;
    area.style.cssText = "position:fixed;left:-9999px;top:0";
    document.body.append(area);
    area.select();
    let ok = false;
    try { ok = document.execCommand("copy"); } catch { ok = false; }
    area.remove();
    if (back && back.focus) back.focus();
    return ok;
  };
  const stable = (span) => { if (span) { span.classList.add("words"); span.dataset.text = span.textContent; } };
  const setPiano = () => {
    ui.piano.setAttribute("aria-pressed", String(S.piano));
    named(ui.piano, S.piano ? "Piano: On" : "Piano: Off");
    ui.piano.removeAttribute("aria-label");
  };
  function radioKeys(e) {
    if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(e.key)) return;
    const radios = [...e.currentTarget.querySelectorAll("[role=radio]")];
    const at = radios.indexOf(document.activeElement);
    if (at < 0) return;
    e.preventDefault();
    const to = e.key === "Home" ? 0 : e.key === "End" ? radios.length - 1 : (at + (e.key === "ArrowRight" ? 1 : -1) + radios.length) % radios.length;
    radios[to].focus();
    radios[to].click();
  }

  // ---- keys (§3.13)

  const escape = () => {
    if (!ui.suggest.hidden) return hideSuggest();
    if (closeMenu(true) || closePopover()) return true;
    if (S.enlarged) { enlarge(false); return true; }
    if (S.sheet) { setSheet(false); return true; }
    if (cur && cur.ticks.size) { cur.ticks.clear(); schedule(); return true; }
    return false;
  };
  const enlarge = (on) => {
    S.enlarged = on;
    root.classList.toggle("enlarged", on);
    schedule();
  };
  const seekBy = (seconds) => { if (S.dock && player.id === S.dock.id) player.seek(Math.max(0, (player.time || 0) + seconds)); };
  const playPause = () => {
    const row = S.dock;
    if (!row) return;
    if (player.id !== row.id || player.state === "idle" || player.state === "cued") playRow(row);
    else player.toggle();
  };
  let pointed = null; // the control a pointer press focused, until focus moves
  const onKey = (e) => {
    const t = e.target;
    if (e.key === "Escape") { if (escape()) e.preventDefault(); return; }
    if (!ui.menu.hidden && ui.menu.contains(t)) { menuKeys(e); return; }
    const key = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (key === "Home" && e.altKey && !e.ctrlKey && !e.shiftKey && !e.metaKey) { e.preventDefault(); goHome(); return; }
    if (t.qmShelf && (key === "ArrowDown" || key === "ArrowUp")) { e.preventDefault(); moveFocus(t, key === "ArrowDown" ? 1 : -1); return; }
    if ((key === "/" && !typing(t) && !e.ctrlKey && !e.altKey) || (key === "l" && e.ctrlKey && !e.altKey && !e.shiftKey)) {
      e.preventDefault();
      ui.q.focus();
      ui.q.select();
      return;
    }
    if (t === ui.q) {
      if ((key === "ArrowDown" || key === "ArrowUp") && options.length && !ui.suggest.hidden) {
        e.preventDefault();
        highlight = key === "ArrowDown" ? (highlight + 1) % options.length : highlight <= 0 ? options.length - 1 : highlight - 1;
        markSuggest();
      } else if (key === "ArrowDown" && cur) {
        const node = nodeOf(cur, cur.focus) || [...cur.nodes.values()][0];
        if (node) { e.preventDefault(); hideSuggest(); setCurrent(cur, node, true); }
      }
      return;
    }
    const node = t.closest && t.closest(".row");
    if (node && node.qmRow && cur) {
      const row = node.qmRow;
      if (key === "ArrowDown" || key === "ArrowUp") { e.preventDefault(); moveFocus(node, key === "ArrowDown" ? 1 : -1); return; }
      if (t === node && key === "Enter") {
        e.preventDefault();
        if (e.ctrlKey) convertRow(cur, row);
        else activate(row);
        return;
      }
      if (key === "c" && e.ctrlKey && !e.shiftKey && !String(getSelection())) { e.preventDefault(); copy(linkOf(row)); return; }
      if (key === "ContextMenu" || (key === "F10" && e.shiftKey)) {
        e.preventDefault();
        const r = node.getBoundingClientRect();
        rowMenu(row, { x: r.left + 48 * scale, y: r.top + r.height / 2 });
        return;
      }
    }
    if (typing(t) || e.ctrlKey || e.altKey || e.metaKey) return;
    if (key === " ") {
      // Space presses a control the keyboard reached; one a click focused leaves Space to play and pause.
      const role = t.getAttribute && t.getAttribute("role");
      if ((t.tagName === "BUTTON" || role === "checkbox" || role === "radio") && t !== pointed) return;
      e.preventDefault();
      playPause();
    } else if (key === "j" || key === "l") {
      e.preventDefault();
      seekBy(key === "j" ? -10 : 10);
    } else if (/^[0-9]$/.test(key) && t.getAttribute("role") !== "slider") {
      const duration = (S.dock && player.id === S.dock.id && player.duration) || 0;
      if (duration) { e.preventDefault(); player.seek((duration * Number(key)) / 10); }
    }
  };

  // ---- start

  let rulesReady = Promise.resolve();
  let started = false;
  const start = () => {
    if (started || !P || !$("rows")) return;
    started = true;
    ui = {};
    for (const id of ["back", "forward", "search", "q", "clear", "suggest", "piano", "signin", "account", "app", "filters", "type",
      "duration", "sort", "loading", "listbar", "list", "banners", "scroll", "listhead", "all", "count", "ticked", "untick",
      "rows", "side", "dock", "slot", "frame", "player", "poster", "ad", "copy", "youtube", "transport", "play", "seek", "time",
      "volume", "enlarge", "actions", "mix", "tray", "queue", "converted", "menu", "collapse", "home", "genres", "pills", "strip"]) ui[id] = $(id);
    Object.assign(ui, {
      convertTicked: $("convert-ticked"), headState: $("head-state"), frameWord: $("frame-word"), dockName: $("dock-name"),
      dockChannel: $("dock-channel"), dockConvert: $("dock-convert"), queueHead: $("queue-head"), convertedHead: $("converted-head"),
      durationLabel: $("duration").querySelector("span"), queueButton: $("queue-button"), queueCount: $("queue-count"),
      genreSort: $("genre-sort"), saveDock: $("dock-save"), saveTicked: $("save-ticked"),
    });
    // Convert again is an icon beside Open, so Done fits Save to and Mix (§5.1).
    ui.again = button("icon", "rotate-ccw", null, "Convert again");
    ui.again.hidden = true;
    ui.dockConvert.after(ui.again);
    ui.q.setAttribute("aria-autocomplete", "list");
    ui.q.setAttribute("aria-controls", "suggest");
    for (const b of ui.type.children) stable(b.querySelector("span"));
    stable(ui.piano.querySelector("span"));
    // Narrow, "Convert N" shows its icon and N; the word stays in its name.
    const word = ui.ticked.previousSibling;
    if (word && word.nodeType === Node.TEXT_NODE) word.replaceWith(el("span", "clip", word.textContent));

    // Top bar
    ui.back.addEventListener("click", () => history.back());
    ui.forward.addEventListener("click", () => history.forward());
    ui.home.addEventListener("click", goHome);
    ui.search.addEventListener("submit", (e) => {
      e.preventDefault();
      if (highlight >= 0 && options[highlight]) { pickSuggest(highlight); return; }
      const link = linkItem(ui.q.value);
      if (link && (link.route || link.text)) { runLink(link); return; }
      runSearch(ui.q.value);
    });
    ui.q.addEventListener("input", () => {
      ui.clear.hidden = !ui.q.value;
      clearTimeout(suggestTimer);
      suggestTimer = setTimeout(suggest, 120);
    });
    ui.q.addEventListener("blur", () => hideSuggest());
    ui.clear.addEventListener("click", () => {
      ui.q.value = "";
      ui.clear.hidden = true;
      hideSuggest();
      ui.q.focus();
    });
    ui.piano.addEventListener("click", () => {
      S.piano = !S.piano;
      write("qm-piano", S.piano ? "1" : "0");
      setPiano();
      if (cur && cur.kind === "search") go({ ...cur.route, p: S.piano ? "1" : "0" });
    });
    ui.signin.addEventListener("click", () => call("sign_in"));
    ui.account.addEventListener("click", () => {
      if (menuOwner === ui.account) { closeMenu(true); return; }
      openMenu(ui.account, [{ note: (S.account && S.account.name) || "Account" }, { icon: "log-out", text: "Sign out", run: () => call("sign_out") }]);
    });
    ui.app.addEventListener("click", () => call("raise_app"));

    // Filter row
    for (const b of ui.type.children) {
      b.addEventListener("click", () => {
        const f = b.dataset.type;
        const r = cur && cur.kind === "search" ? cur.route : S.lastSearch || defaults();
        if (cur && cur.kind === "search" && r.f === f) return;
        runSearch(lastQuery(), { f });
      });
    }
    ui.type.addEventListener("keydown", radioKeys);
    const filter = (owner, pairs, key) => {
      if (menuOwner === owner) { closeMenu(true); return; }
      const r = cur && cur.kind === "search" ? cur.route : S.lastSearch || defaults();
      openMenu(owner, pairs.map(([value, text]) => ({
        text, checked: r[key] === value,
        run: () => { if (r[key] !== value || !cur || cur.kind !== "search") runSearch(lastQuery(), { [key]: value }); },
      })));
    };
    ui.duration.addEventListener("click", () => filter(ui.duration, DURATIONS, "d"));
    ui.sort.addEventListener("click", () => filter(ui.sort, SORTS, "s"));

    // Genre row (§1.1): a pill opens its genre on the Sort in use, Top at first.
    for (const g of GENRES.values()) {
      const b = button("pill", null, g.word);
      b.setAttribute("role", "radio");
      b.setAttribute("aria-checked", "false");
      b.dataset.genre = g.key;
      stable(b.lastElementChild);
      b.addEventListener("click", () => {
        if (cur && cur.kind === "genre" && cur.route.g === g.key) return;
        go({ view: "genre", g: g.key, s: cur && cur.kind === "genre" ? cur.route.s : "top" });
      });
      ui.pills.append(b);
    }
    ui.pills.addEventListener("keydown", radioKeys);
    ui.genreSort.addEventListener("click", () => {
      if (menuOwner === ui.genreSort) { closeMenu(true); return; }
      if (!cur || cur.kind !== "genre") return;
      openMenu(ui.genreSort, GENRE_SORTS.map(([s, text, symbol]) => ({
        icon: symbol, text, checked: cur.route.s === s, run: () => { if (cur.route.s !== s) go({ ...cur.route, s }); },
      })));
    });

    // Pianist strip (§1.4): one tab stop, arrows move, Enter or a click opens the pianist.
    const cardOf = (t) => (t.closest ? t.closest(".card") : null);
    const rove = (b) => { for (const c of ui.strip.querySelectorAll(".card")) c.tabIndex = c === b ? 0 : -1; };
    ui.strip.addEventListener("click", (e) => {
      const b = cardOf(e.target);
      if (!b) return;
      rove(b);
      // On a genre's page, or a channel's Similar opened from one, its pianists open on their songs of that genre
      // (§3.19 Genre scope).
      openChannel(b.qmPianist.id, cur && (cur.kind === "genre" || cur.kind === "channel") ? cur.route.g || null : null);
    });
    ui.strip.addEventListener("keydown", (e) => {
      const b = cardOf(e.target);
      if (!b || e.altKey || e.ctrlKey || e.metaKey) return;
      if (e.key === "ContextMenu" || (e.key === "F10" && e.shiftKey)) {
        e.preventDefault();
        const r = b.getBoundingClientRect();
        pianistMenu(b.qmPianist, { x: r.left + r.width / 2, y: r.bottom });
        return;
      }
      const cards = [...ui.strip.querySelectorAll(".card")];
      const at = cards.indexOf(b);
      const to = { ArrowLeft: Math.max(0, at - 1), ArrowRight: Math.min(cards.length - 1, at + 1), Home: 0, End: cards.length - 1 }[e.key];
      if (to === undefined) return;
      e.preventDefault();
      rove(cards[to]);
      cards[to].focus();
      cards[to].scrollIntoView({ block: "nearest", inline: "nearest" });
    });
    ui.strip.addEventListener("contextmenu", (e) => {
      const b = cardOf(e.target);
      if (!b) return;
      e.preventDefault();
      rove(b);
      pianistMenu(b.qmPianist, { x: e.clientX, y: e.clientY });
    });

    // List
    ui.all.addEventListener("click", () => {
      const v = cur;
      if (!v) return;
      const ids = v.countedIds || [];
      if (!ids.length || ids.every((id) => v.ticks.has(id))) v.ticks.clear();
      else ids.forEach((id) => v.ticks.add(id));
      schedule();
    });
    ui.convertTicked.addEventListener("click", convertTicked);
    ui.saveTicked.addEventListener("click", () => {
      const v = cur;
      if (!v) return;
      const groups = byDest(v, tickedRows(v));
      const artist = viewArtist(v, tickedRows(v));
      folderMenu(ui.saveTicked, groups.length === 1 ? groups[0].dest : null, artist, (dest) => { v.dest = dest; choose(dest, artist); });
    });
    ui.untick.addEventListener("click", () => { if (cur) { cur.ticks.clear(); schedule(); } });
    ui.rows.addEventListener("click", (e) => {
      const node = e.target.closest(".row");
      if (!node || !node.qmRow || !cur) return;
      const row = node.qmRow;
      if (!node.classList.contains("current")) setCurrent(cur, node, false);
      if (e.target.closest(".check")) tick(cur, row, e.shiftKey);
      else if (e.target.closest(".convert")) convertRow(cur, row);
      else if (e.target.closest(".remove")) removeRow(row);
      else if (e.target.closest(".body")) {
        if (row.kind === "video" && (e.ctrlKey || e.metaKey || e.shiftKey)) tick(cur, row, e.shiftKey);
        else activate(row);
      }
    });
    ui.rows.addEventListener("mousedown", (e) => { if (e.shiftKey && e.target.closest(".row")) e.preventDefault(); });
    ui.rows.addEventListener("contextmenu", (e) => {
      const node = e.target.closest(".row");
      if (!node || !node.qmRow) return;
      e.preventDefault();
      rowMenu(node.qmRow, { x: e.clientX, y: e.clientY });
    });
    ui.scroll.addEventListener("scroll", () => closeMenu(), { passive: true });

    // Dock
    ui.copy.addEventListener("click", async () => {
      if (!S.dock || !(await copy(WATCH + S.dock.id))) return;
      setIcon(ui.copy.firstElementChild, "check");
      clearTimeout(ui.copy.timer);
      ui.copy.timer = setTimeout(() => setIcon(ui.copy.firstElementChild, "copy"), 1500);
    });
    ui.youtube.addEventListener("click", () => { if (S.dock) call("open_external", WATCH + S.dock.id); });
    ui.play.addEventListener("click", playPause);
    ui.dockConvert.addEventListener("click", () => convertRow(cur, S.dock, false, true));
    ui.again.addEventListener("click", () => convertRow(cur, S.dock, true, true));
    ui.saveDock.addEventListener("click", () => {
      const row = S.dock;
      if (!row) return;
      const artist = oneArtist([row]);
      folderMenu(ui.saveDock, dockDest(row), artist, (dest) => { S.dockDest = { id: row.id, dest }; choose(dest, artist); });
    });
    ui.mix.addEventListener("click", () => { if (S.dock) go({ view: "mix", mix: S.dock.id, list: "" }); });
    ui.dockChannel.addEventListener("click", () => { const id = channelOf(S.dock); if (id) openChannel(id, scopeOf(S.dock)); });
    ui.volume.addEventListener("click", openVolume);
    ui.enlarge.addEventListener("click", () => enlarge(!S.enlarged));
    // The narrow sheet (§2.6): its background or the queue button expands it, Collapse or Esc collapses it.
    ui.side.addEventListener("click", (e) => {
      if (root.classList.contains("narrow") && !S.sheet && !e.target.closest("button, .seek")) setSheet(true);
    });
    ui.side.addEventListener("keydown", (e) => {
      if (e.target !== ui.side || e.key !== "Enter" || S.sheet) return;
      e.preventDefault();
      setSheet(true);
      ui.collapse.focus();
    });
    ui.queueButton.addEventListener("click", () => setSheet(true, true));
    ui.collapse.addEventListener("click", () => setSheet(false));
    ui.dock.addEventListener("contextmenu", (e) => {
      if (!S.dock || e.target.closest(".popover")) return;
      e.preventDefault();
      rowMenu(S.dock, { x: e.clientX, y: e.clientY });
    });
    const seekAt = (e) => {
      const r = ui.seek.getBoundingClientRect();
      return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
    };
    const seekable = () => S.dock && player.id === S.dock.id && !ui.seek.classList.contains("idle") && !ui.seek.classList.contains("ad");
    const length = () => (S.dock && player.duration) || (S.dock && S.dock.seconds) || 0;
    const tip = el("span", "tip num");
    tip.hidden = true;
    ui.seek.append(tip);
    ui.seek.addEventListener("pointermove", (e) => {
      if (!seekable() || !length()) { tip.hidden = true; return; }
      const at = seekAt(e);
      tip.hidden = false;
      tip.textContent = clock(at * length());
      tip.style.left = (at * 100).toFixed(2) + "%";
    });
    ui.seek.addEventListener("pointerleave", () => { tip.hidden = true; });
    ui.seek.addEventListener("pointerdown", (e) => {
      if (e.button !== 0 || !seekable()) return;
      ui.seek.setPointerCapture(e.pointerId);
      player.seek(seekAt(e) * length());
      const move = (ev) => player.seek(seekAt(ev) * length());
      const up = () => { ui.seek.removeEventListener("pointermove", move); ui.seek.removeEventListener("pointerup", up); ui.seek.removeEventListener("pointercancel", up); };
      ui.seek.addEventListener("pointermove", move);
      ui.seek.addEventListener("pointerup", up);
      ui.seek.addEventListener("pointercancel", up);
    });
    ui.seek.addEventListener("keydown", (e) => {
      if (!seekable()) return;
      const steps = { ArrowLeft: -5, ArrowRight: 5, PageDown: -30, PageUp: 30 };
      if (e.key in steps) { e.preventDefault(); seekBy(steps[e.key]); }
      else if (e.key === "Home") { e.preventDefault(); player.seek(0); }
      else if (e.key === "End") { e.preventDefault(); player.seek(Math.max(0, length() - 1)); }
    });

    // Page
    // A press on Skip that lands on the page instead, as when the layout moves after browse.py's last look,
    // does nothing (§3.6): while one is under way, trusted press events at its point are swallowed.
    for (const type of ["pointerdown", "mousedown", "pointerup", "mouseup", "click", "dblclick", "auxclick", "contextmenu"]) {
      document.addEventListener(type, (e) => {
        const at = F.point;
        if (!at || !e.isTrusted || Math.abs(e.clientX - at[0]) > 1 || Math.abs(e.clientY - at[1]) > 1) return;
        e.preventDefault();
        e.stopImmediatePropagation();
      }, true);
    }
    document.addEventListener("keydown", onKey);
    document.addEventListener("pointerdown", (e) => {
      pointed = (e.target.closest && e.target.closest("button,[role=checkbox],[role=radio]")) || null;
    }, true);
    document.addEventListener("focusin", (e) => { if (e.target !== pointed) pointed = null; }, true);
    document.addEventListener("mousedown", (e) => {
      if (!ui.menu.hidden && !ui.menu.contains(e.target) && !(menuOwner && menuOwner.contains(e.target))) closeMenu();
      if (popover && !popover.contains(e.target) && !ui.volume.contains(e.target)) closePopover();
    });
    document.addEventListener("paste", (e) => {
      if (typing(e.target)) return;
      const text = (e.clipboardData && e.clipboardData.getData("text")) || "";
      const link = linkItem(text);
      if (!link) return;
      e.preventDefault();
      if (link.route) runLink(link);
      else {
        ui.q.value = text.trim();
        ui.clear.hidden = false;
        ui.q.focus();
        suggest();
      }
    });
    // The frame never keeps focus (§1.7): frame.js keeps YouTube's scripts from focusing it, and focus that still
    // gets in goes back to the element that had it, or out of the frame when none had it. Focus left for the page's
    // background forgets that element; the window's own blur keeps it.
    let lastFocus = null;
    document.addEventListener("focusin", (e) => { if (e.target !== ui.player) lastFocus = e.target; });
    document.addEventListener("focusout", () => setTimeout(() => {
      if (document.activeElement === document.body) lastFocus = null;
    }, 0));
    const giveBack = () => setTimeout(() => {
      if (document.activeElement !== ui.player) return;
      if (lastFocus && lastFocus.isConnected) lastFocus.focus();
      else ui.player.blur();
    }, 0);
    window.addEventListener("blur", giveBack);
    // With the window in the background the page gets no blur, and frame.js says when the frame gets focus.
    ui.player.addEventListener("qm-focus", giveBack);
    window.addEventListener("resize", () => { layout(); closeMenu(); closePopover(); });
    window.addEventListener("popstate", popped);
    window.addEventListener("hashchange", () => {
      if (SURFACE === "canvas" && cur && routeString(parseRoute(here())) !== cur.key) popped();
    });

    // Data
    rulesReady = fetch("/quartzmidi/rules.json", { credentials: "same-origin" })
      .then((r) => (r.ok ? r.json() : null))
      .then((json) => { if (json) P.loadRules(json); })
      .catch(() => {});
    const converted = fetch("/quartzmidi/converted.json", { credentials: "same-origin" })
      .then((r) => (r.ok ? r.json() : {}))
      .then((json) => {
        for (const [id, path] of Object.entries(json || {})) if (VIDEO.test(id) && typeof path === "string") S.converted.set(id, path);
        schedule();
      })
      .catch(() => {});
    // Your pianists and seeds (§4.1); Home waits for them and the ledger at most 1.5 s, then starts cold.
    loadArtists();
    // This start's turn of the curated pianists, and the next start's (§3.19 Turns).
    const turn = Math.floor(Number(read("qm-turn", "0")));
    S.turn = turn >= 0 && turn < 1e6 ? turn : 0;
    write("qm-turn", S.turn + 1);
    const dests = readJson("qm-dests");
    if (Array.isArray(dests)) S.dests = dests.filter((d) => typeof d === "string" && d && P.validDest(d)).slice(0, RECENT);
    const artists = fetch("/quartzmidi/artists.json", { credentials: "same-origin" })
      .then((r) => (r.ok ? r.json() : null))
      .then((json) => {
        if (!json || typeof json !== "object") return;
        if (Array.isArray(json.artists)) mergeArtists(json.artists);
        if (Array.isArray(json.seeds)) S.seeds = json.seeds;
      })
      .catch(() => {});
    artistsReady = Promise.all([artists, converted]);
    if (document.fonts) document.fonts.ready.then(measure);

    layout();
    measure();
    setPiano();
    paintAccount();
    const state = history.state;
    H.i = state && Number.isInteger(state.i) ? state.i : 0;
    H.max = H.i;
    const route = parseRoute(here());
    history.replaceState({ i: H.i }, "", SURFACE === "canvas" ? urlOf(here().replace(/^#?\/?/, "") || routeString(route)) : location.href);
    enter(route, true);
    mount();
  };
  // §1.3 self-check: browse.py moves to the next surface unless the page says it mounted.
  const mount = () => {
    const go = () => call("mounted", SURFACE);
    if (globalThis.pywebview && globalThis.pywebview.api && globalThis.pywebview.api.mounted) go();
    else window.addEventListener("pywebviewready", go, { once: true });
  };

  globalThis.qm = { receive };
  globalThis.QMApp = { start, player, receive };
  start();
  window.__quartzPage = 1;
})();
