// parse.js: the YouTube window's data layer. Pure (no DOM): it loads in the page before app.js and in
// deno for tests\parse_test.js, and assigns globalThis.QMParse.
//
// Client context
//   clientFrom(swText, navigator.language) -> {client, drift}. swText is GET /sw.js_data. The values are
//   found by shape, not index; a missing field falls back (CLIENT_VERSION, hl and gl from the language,
//   else en and US) and adds "sw.js_data <field> not found" to drift.
//   client = {clientName:"WEB", clientVersion, hl, gl, visitorData}
//
// Requests: fetch(api(endpoint), {method:"POST", credentials:"same-origin", headers:headers(client),
// body:JSON.stringify(<body>)}). Every body is {context:{client}, ...}.
//   api(endpoint)                      "/youtubei/v1/<endpoint>?prettyPrint=false"
//   headers(client)                    Content-Type, X-Youtube-Client-Name 1, X-Youtube-Client-Version, X-Goog-Visitor-Id
//   searchBody(client, query, params)  "search" {query, params}; query = searchQuery(typed, pianoOn)
//   moreBody(client, token)            any endpoint {continuation}: next pages, and the channel sort tokens
//   browseBody(client, browseId, params) "browse" {browseId, params}
//   channelBody(client, UCID, tab)     "browse"; tab "videos" or "playlists"
//   channelSearchBody(client, UCID, q) "browse" {params: the channel's search tab, query}: its videos that match
//                                      q ("|" between words: any of them); read with parseChannel
//   playlistBody(client, listId)       "browse" {browseId:"VL"+listId}
//   nextBody(client, videoId)          "next": the video, its chapters, related videos
//   mixBody(client, seed, last, index) "next" {videoId:last||seed, playlistId:"RD"+seed, playlistIndex:index};
//                                      repeat with parseMix().last until 100 unique ids or none new
//   playerBody(client, videoId, sts)   "player" with hl "en". Without the current signature timestamp
//                                      (stsFrom(the /watch frame's HTML), or its ytcfg.get("STS")) every
//                                      playable video reads UNPLAYABLE; parsePlayer reports that as stale.
//   resolveBody(client, url)           "navigation/resolve_url", for @handle, /c/ and /user/
//   accountBody(client)                "account/account_menu"
//   signedHeaders(document.cookie)     async: the SAPISIDHASH headers for the one signed-in retry, or null
//   searchParams({type, duration, popular, upload})  base64 params. type videos|playlists|channels, duration
//                                      any|short|medium|long (Videos only), popular true for Popular, upload
//                                      any|hour|today|week|month|year
//   suggestUrl(client, q)              the suggestions GET; parseSuggest(text) -> [text]
//   resultsUrl(q, params)              the HTML fallback; initialData(html) -> ytInitialData or null
//   stsFrom(text)                      signature timestamp from page HTML, or null
//
// Parsers take response JSON, never throw and return plain objects. Every list result has
// {known, rows, more, sorts, sort, dropped:{ads, shorts}, drift}: more is a continuation token for
// moreBody or null; sorts holds the Latest, Popular and Oldest tokens in chip order (the chip text is
// localised) and sort the selected index; known is false when no expected section was found; drift lists
// unknown renderer keys and missing fields, for api.log("drift", ...) once each.
//   parseSearch(json)    also {estimated}; it reads the HTML fallback's ytInitialData too
//   parseMore(json)      any continuation: search, browse, related, and a sort chip's reload
//   parseChannel(json)   also {id, title, handle, subscribers, avatar}
//   parsePlaylist(json)  also {title, channel, channelId, count, countText}
//   parseNext(json)      also {video, chapters}; rows are the related videos
//   parseMix(json)       also {id, title, last:{id, index}}; title is YouTube's "Mix - Title"; rows are the
//                        playlist panel's (contents.twoColumnWatchNextResults.playlist.playlist.contents), the seed first
//   parsePlayer(json)    {id, status, playable, overlay, live, upcoming, seconds, title, channel, channelId,
//                        stale, drift}
//   parseAccount(json)   {name, handle, avatar} or null (signed out, or unreadable: show the user icon)
//   parseResolve(json)   browseId or null
// Rows:
//   {kind:"video", id, title, channel, channelId, avatar, artist, seconds, duration, views, age, thumb,
//    chapters:[{title, seconds}], badges:[label], live, upcoming, playable, overlay[, index]}
//   {kind:"playlist"|"mix", id, title, channel, channelId, count, videoId, thumb}   videoId: the first or seed video
//   {kind:"channel", id, title, handle, subscribers, thumb, artist, topic}
//   playable is true, false or null (unknown). overlay is a §2.9 word or null. Ads and Shorts are dropped.
//   avatar is the channel's picture on a search row, else "". artist is true for an Official Artist Channel.
//   topic is true for YouTube's own "- Topic" channel of an artist.
//
// Links, rules and counting
//   routeFor(url)        "?v=ID", "?list=L&v=ID", "?list=L", "?channel=UC…|@handle|c/NAME|user/NAME", "?q=Q" or
//                        null; the same answers as browse.py route_for on tests\fixtures\routes.json
//   loadRules(json)      compiles /quartzmidi/rules.json; isOther is false until it is loaded
//   isOther(title, query), hasPiano(query), searchQuery(query, pianoOn)
//   long(row)            over 20:00 or chapters naming two or more pieces: the warn badge
//   counted(row, query, state)  state {converted, queue}; count(rows, query, stateOf(row) -> state)
//   isRecording(row, query)  counted and a minute or longer: what discovery counts and ranks pianists by
//   lessonChannel(name)  a channel that teaches, never offered as a pianist
//
// Discovery
//   GENRES               [{key, word, query, words}], frozen; Top searches the query with Videos 3-20 min, New
//                        adds This week (This month when it brings too few); words is what a pianist's channel
//                        is searched for when it has few of the genre's songs
//   inGenre(key, title, channel)  whether a title is one of the genre's songs: it names one of GENRE_WORDS[key],
//                        as a whole word in any case, once the channel's own name is taken out. Anime takes OP and
//                        ED too; classical takes a composer, a known piece or an opus, or a form word (FORM_WORDS)
//                        beside a catalogue number, a "No." or a key; games refuses the few songs that share a
//                        game's name. genresOf(title, channel) is every genre it is in, in GENRES order.
//   CURATED              [{id, name, handle, avatar, genres}], frozen: pianists chosen per genre, the genre each
//                        is best known for first; avatar is the picture's path on yt3.googleusercontent.com
//   avatarUrl(path)      that picture's address, 96 px square
//   curatedFor(key)      CURATED best known for the genre, then the others listing it, each in table order
//   BLOCK                channel ids never offered as pianists, whatever their rows: pianists, mergeSimilar,
//                        curatedFor, genrePianists and homeCurated leave them out
//   genrePianists(key, rows, seen, max)  a genre view's strip, 24 at most: curatedFor(key), then the channels
//                        of its rows as pianists finds them, by score, when half or more of their rows are in the
//                        genre and the name is not a label's or a sheet-music channel's (LABEL)
//   homeCurated(titles, skip, n)  12 at most of CURATED for Home: round-robin through the genres the titles (your
//                        newest conversions) are in, most first, or all of GENRES when none is; ids in skip left out
//   pianists(rows, query, seen, own)  [{id, name, avatar, artist, rec, score}], 12 at most: the channels of
//                        the rows, by score (3 a recording, 2 an artist, 1 a piano word in the name), ties in row
//                        order. query is the rows' search words, or row => the words of that row's own search.
//                        Left out: Topic and lesson channels, own (a channel id, or several), channels
//                        with no recording here, and channels seen = {UC…: {rec, rows}} (this session's rows
//                        read, kept by the caller) shows under half recordings of 4 or more.
//   rankChannels(rows)   Channels results: artists, then the rest, then Topic and lesson channels, each in
//                        YouTube's order; nothing dropped
//   pieceQuery(title, channel)  the piece a title names, as words to search for with " piano", or ""
//   mergeSimilar(seed, mix, search, shown)  one row per other channel from a Mix and a piece search, 20 at
//                        most: the best row of each (a recording, then an artist), the channels by score, ties
//                        alternating the two. The seed {id, channelId, channel} and its channel are left out,
//                        and so are Topic channels and the channels of shown, the rows already listed when a
//                        search page is added.
//
// Folders
//   folderName(name)     a name as a folder Windows keeps, as convert.py FolderName (tests\fixtures\foldernames.json)
//   validDest(dest)      "" (the MIDI folder) or 1 to 4 folder names joined by "/", each kept as written
(() => {
  "use strict";

  const CLIENT_VERSION = "2.20261006.01.00";
  const ORIGIN = "https://www.youtube.com";
  const LONG = 20 * 60;
  const VIDEO = /^[A-Za-z0-9_-]{11}$/;
  const LIST = /^[A-Za-z0-9_-]{2,64}$/;
  const UCID = /^UC[A-Za-z0-9_-]{22}$/;
  const HOSTS = new Set(["youtube.com", "www.youtube.com", "m.youtube.com", "music.youtube.com"]);
  const CHANNEL_TABS = { videos: "EgZ2aWRlb3PyBgQKAjoA", playlists: "EglwbGF5bGlzdHPyBgoKCEIGCgIQaCIA" };
  const CHANNEL_SEARCH = "EgZzZWFyY2jyBgQKAloA";
  const TYPES = { videos: 1, channels: 2, playlists: 3 };
  const DURATIONS = { short: 4, medium: 5, long: 2 };
  const UPLOADS = { hour: 1, today: 2, week: 3, month: 4, year: 5 };
  const ARTIST = "BADGE_STYLE_TYPE_VERIFIED_ARTIST";
  const PIANO_WORDS = ["piano", "pianist", "ピアノ", "피아노", "钢琴", "鋼琴"];
  // rules.json's lesson terms, kept equal by rules_test.js; isOther matches the loaded sources.
  const LESSON = ["tutorial", "how to play", "lesson", "lessons", "for beginners", "beginner piano", "easy piano",
    "piano easy", "easy version", "easy ver", "slow version", "learn to play", "play along", "quiz", "interview",
    "evolution of", "easy to impossible", "levels of", "technique", "techniques", "masterclass", "master class",
    "crash course", "explained", "piano 101", "弾き方", "初心者", "簡単", "講座", "강좌", "레슨", "쉬운", "教学", "教程"];
  const LESSON_CHANNEL =
    /\b(?:tutorials?|lessons?|academy|school|coach|teacher|course|learn|beginners?|skills)\b|講座|教室|レッスン|강좌|레슨|教学|教程/i;
  const TOPIC = / - Topic$/;

  // ---- small helpers

  const get = (o, path) => path.split(".").reduce((x, k) => (x == null ? undefined : x[k]), o);

  // Every value under `key`, depth first.
  const all = (node, key, out = []) => {
    if (Array.isArray(node)) for (const x of node) all(x, key, out);
    else if (node && typeof node === "object") {
      for (const [k, v] of Object.entries(node)) {
        if (k === key) out.push(v);
        all(v, key, out);
      }
    }
    return out;
  };
  const first = (node, key) => {
    if (Array.isArray(node)) {
      for (const x of node) { const f = first(x, key); if (f !== undefined) return f; }
    } else if (node && typeof node === "object") {
      if (key in node) return node[key];
      for (const v of Object.values(node)) { const f = first(v, key); if (f !== undefined) return f; }
    }
    return undefined;
  };

  // YouTube text: a string, {simpleText}, {runs:[{text}]} or {content}.
  const text = (x) => {
    if (x == null) return "";
    if (typeof x === "string") return x;
    if (typeof x !== "object") return String(x);
    if (typeof x.simpleText === "string") return x.simpleText;
    if (typeof x.content === "string") return x.content;
    if (Array.isArray(x.runs)) return x.runs.map((r) => (r && r.text) || "").join("");
    return "";
  };

  // "1:02:03" or "54:18" -> seconds, else null.
  const clock = (s) => {
    const m = /^\s*(?:(\d+):)?(\d+):(\d{2})\s*$/.exec(String(s || ""));
    return m ? Number(m[1] || 0) * 3600 + Number(m[2]) * 60 + Number(m[3]) : null;
  };

  // "1,234 videos" -> 1234, "Mix" -> null.
  const countOf = (x) => {
    if (typeof x === "number") return x;
    const m = /\d[\d,.\s  ]*/.exec(String(x || ""));
    return m ? Number(m[0].replace(/\D/g, "")) : null;
  };

  const abs = (url) => (typeof url === "string" && url.startsWith("//") ? "https:" + url : url || "");
  const mq = (id) => "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg";
  const stripXssi = (t) => String(t || "").replace(/^\)\]\}'\s*/, "");
  const decode = (s) => { try { return decodeURIComponent(s); } catch { return s; } };

  // The channel id behind a byline or a metadata part, if it links to a channel.
  const browseIdOf = (x) => {
    const id = (first(x, "browseEndpoint") || {}).browseId;
    return typeof id === "string" && id.startsWith("UC") ? id : "";
  };

  // ---- client context and requests

  // Some responses carry an experiment label after the version: "2.20261006.01.00-canary_control_2.20261007.01.00".
  const CV = /^2\.\d{8}\.\d{2}\.\d{2}(?![\d.])/;
  const HL = /^[a-z]{2,3}(-[A-Za-z]{2,4})?$/;
  const GL = /^[A-Z]{2}$/;

  const arrayWith = (node, test) => {
    if (!Array.isArray(node) && !(node && typeof node === "object")) return null;
    const values = Array.isArray(node) ? node : Object.values(node);
    if (Array.isArray(node) && node.some(test)) return node;
    for (const v of values) { const f = arrayWith(v, test); if (f) return f; }
    return null;
  };

  const clientFrom = (swText, language = "") => {
    const drift = [];
    let arr = null;
    try {
      arr = arrayWith(JSON.parse(stripXssi(swText)), (x) => typeof x === "string" && CV.test(x));
    } catch {
      arr = null;
    }
    const str = (test) => (arr ? arr.find((x) => typeof x === "string" && test(x)) : undefined);
    const lang = String(language || "");
    const region = (/^[a-z]{2,3}-([A-Za-z]{2})\b/i.exec(lang) || [])[1];
    const fallback = {
      clientVersion: CLIENT_VERSION,
      hl: HL.test(lang) ? lang : "en",
      gl: region ? region.toUpperCase() : "US",
      visitorData: "",
    };
    const found = {
      clientVersion: (CV.exec(str((x) => CV.test(x)) || "") || [])[0],
      hl: str((x) => HL.test(x)),
      gl: str((x) => GL.test(x)),
      visitorData: str((x) => x.startsWith("Cg") && x.length >= 20),
    };
    const client = { clientName: "WEB" };
    for (const field of ["clientVersion", "hl", "gl", "visitorData"]) {
      if (found[field] === undefined) drift.push("sw.js_data " + field + " not found");
      client[field] = found[field] ?? fallback[field];
    }
    return { client, drift };
  };

  const api = (endpoint) => "/youtubei/v1/" + endpoint + "?prettyPrint=false";

  const headers = (client) => {
    const h = {
      "Content-Type": "application/json",
      "X-Youtube-Client-Name": "1",
      "X-Youtube-Client-Version": client.clientVersion,
    };
    if (client.visitorData) h["X-Goog-Visitor-Id"] = client.visitorData;
    return h;
  };

  const context = (client, extra) => ({
    client: {
      clientName: "WEB",
      clientVersion: client.clientVersion,
      hl: client.hl,
      gl: client.gl,
      visitorData: client.visitorData,
      ...extra,
    },
  });

  const searchBody = (client, query, params) => {
    const body = { context: context(client), query: String(query || "") };
    if (params) body.params = params;
    return body;
  };
  const moreBody = (client, token) => ({ context: context(client), continuation: token });
  const browseBody = (client, browseId, params) => {
    const body = { context: context(client), browseId };
    if (params) body.params = params;
    return body;
  };
  const channelBody = (client, id, tab = "videos") => browseBody(client, id, CHANNEL_TABS[tab] || CHANNEL_TABS.videos);
  const channelSearchBody = (client, id, query) =>
    ({ ...browseBody(client, id, CHANNEL_SEARCH), query: String(query || "") });
  const playlistBody = (client, list) => browseBody(client, String(list).startsWith("VL") ? list : "VL" + list);
  const nextBody = (client, videoId) => ({ context: context(client), videoId });
  const mixBody = (client, seed, last, index) => {
    const body = { context: context(client), videoId: last || seed, playlistId: "RD" + seed };
    if (index != null) body.playlistIndex = index;
    return body;
  };
  const playerBody = (client, videoId, sts) => {
    const body = { context: context(client, { hl: "en" }), videoId };
    if (sts) body.playbackContext = { contentPlaybackContext: { signatureTimestamp: Number(sts) } };
    return body;
  };
  const resolveBody = (client, url) => ({ context: context(client), url });
  const accountBody = (client) => ({ context: context(client) });

  // The filter message is upload date (field 1), type (2), then duration (3), as YouTube writes them.
  const searchParams = ({ type = "videos", duration = "any", popular = false, upload = "any" } = {}) => {
    const t = typeof type === "number" ? type : TYPES[type] || 1;
    const d = t === 1 ? (typeof duration === "number" ? duration : DURATIONS[duration] || 0) : 0;
    const u = typeof upload === "number" ? upload : UPLOADS[upload] || 0;
    const inner = [...(u ? [0x08, u] : []), 0x10, t, ...(d ? [0x18, d] : [])];
    const bytes = [...(popular ? [0x08, 0x03] : []), 0x12, inner.length, ...inner];
    return btoa(String.fromCharCode(...bytes));
  };

  const suggestUrl = (client, q) =>
    "https://suggestqueries-clients6.youtube.com/complete/search?client=youtube&ds=yt" +
    "&hl=" + encodeURIComponent(client.hl) + "&gl=" + encodeURIComponent(client.gl) +
    "&q=" + encodeURIComponent(q) + "&xhr=t&xssi=t&oe=utf-8";

  const resultsUrl = (q, params) =>
    "/results?search_query=" + encodeURIComponent(q) + (params ? "&sp=" + encodeURIComponent(params) : "");

  const stsFrom = (t) => {
    const m = /"STS"\s*:\s*(\d+)|signatureTimestamp["']?\s*:\s*(\d+)/.exec(String(t || ""));
    return m ? Number(m[1] || m[2]) : null;
  };

  const signedHeaders = async (cookie, now = Date.now()) => {
    const m = /(?:^|;\s*)SAPISID=([^;]+)/.exec(String(cookie || ""));
    if (!m) return null;
    const ts = Math.floor(now / 1000);
    const data = new TextEncoder().encode(ts + " " + m[1] + " " + ORIGIN);
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-1", data));
    const hex = Array.from(digest, (b) => b.toString(16).padStart(2, "0")).join("");
    return { "Authorization": "SAPISIDHASH " + ts + "_" + hex, "X-Goog-AuthUser": "0", "X-Origin": ORIGIN };
  };

  // ---- rows

  const finish = (row) => {
    const t = row.title.trim().toLowerCase();
    if (t === "[private video]") { row.playable = false; row.overlay = "Private video"; }
    else if (t === "[deleted video]") { row.playable = false; row.overlay = "Removed"; }
    else if (row.live) row.overlay = "Live";
    else if (row.upcoming) row.overlay = "Upcoming";
    else if (row.playable === false) row.overlay = "Not available";
    return row;
  };

  const video = (f) => {
    const duration = f.duration || "";
    const row = {
      kind: "video",
      id: f.id,
      title: (f.title || "").trim(),
      channel: (f.channel || "").trim(),
      channelId: f.channelId || "",
      avatar: f.avatar || "",
      artist: !!f.artist,
      seconds: f.seconds || clock(duration),
      duration,
      views: f.views || "",
      age: f.age || "",
      thumb: mq(f.id),
      chapters: f.chapters || [],
      badges: f.badges || [],
      live: !!f.live,
      upcoming: !!f.upcoming,
      playable: f.playable ?? null,
      overlay: null,
      members: !!f.members,
    };
    if (f.index != null) row.index = f.index;
    return finish(row);
  };

  const list = (f) => ({
    kind: f.kind,
    id: f.id,
    title: (f.title || "").trim(),
    channel: (f.channel || "").trim(),
    channelId: f.channelId || "",
    count: f.count ?? null,
    videoId: f.videoId || "",
    thumb: f.videoId ? mq(f.videoId) : abs(f.thumb),
  });

  // Chapters from macro markers and chapter renderers, one per start time.
  const chaptersOf = (node) => {
    const out = new Map();
    for (const m of all(node, "macroMarkersListItemRenderer")) {
      const s = get(m, "onTap.watchEndpoint.startTimeSeconds") ?? clock(text(m.timeDescription));
      if (s != null && !out.has(s)) out.set(s, { title: text(m.title), seconds: s });
    }
    for (const c of all(node, "chapterRenderer")) {
      const s = Math.round(Number(c.timeRangeStartMillis) / 1000);
      if (Number.isFinite(s) && !out.has(s)) out.set(s, { title: text(c.title), seconds: s });
    }
    return [...out.values()].sort((a, b) => a.seconds - b.seconds);
  };

  const shortsUrl = (endpoint) => /^\/shorts\//.test(get(endpoint, "commandMetadata.webCommandMetadata.url") || "");

  // An Official Artist Channel: its owner badge, on a video or a channel row.
  const artistBadge = (v) => (v.ownerBadges || []).some((b) => get(b, "metadataBadgeRenderer.style") === ARTIST);

  // videoRenderer, compactVideoRenderer, gridVideoRenderer.
  const fromVideo = (v, ctx, key) => {
    if (!v.videoId) return note(ctx, key + " without videoId");
    const nav = v.navigationEndpoint || {};
    const times = (v.thumbnailOverlays || []).map((o) => o.thumbnailOverlayTimeStatusRenderer).filter(Boolean);
    const styles = times.map((o) => o.style || "DEFAULT");
    if (nav.reelWatchEndpoint || shortsUrl(nav) || styles.includes("SHORTS")) return drop(ctx, "shorts");
    const badges = (v.badges || []).map((b) => b.metadataBadgeRenderer).filter(Boolean);
    const liveBadge = (b) => b.style === "BADGE_STYLE_TYPE_LIVE_NOW";
    const by = v.ownerText || v.shortBylineText || v.longBylineText;
    const title = text(v.title);
    if (!title) note(ctx, key + " without title");
    const plain = times.find((o) => (o.style || "DEFAULT") === "DEFAULT");
    return video({
      id: v.videoId,
      title,
      channel: text(by),
      channelId: browseIdOf(by),
      avatar: abs(get(v, "channelThumbnailSupportedRenderers.channelThumbnailWithLinkRenderer.thumbnail.thumbnails.0.url")),
      artist: artistBadge(v),
      duration: text(v.lengthText) || (plain ? text(plain.text) : ""),
      views: text(v.viewCountText) || text(v.shortViewCountText),
      age: text(v.publishedTimeText),
      chapters: chaptersOf(v.expandableMetadata),
      badges: badges.filter((b) => !liveBadge(b)).map((b) => b.label).filter(Boolean),
      live: styles.includes("LIVE") || badges.some(liveBadge),
      upcoming: !!v.upcomingEventData || styles.includes("UPCOMING"),
      members: membersOnly(v),
    });
  };

  // The old playlist browse item.
  const fromPlaylistVideo = (v, ctx, key) => {
    if (!v.videoId) return note(ctx, key + " without videoId");
    const styles = (v.thumbnailOverlays || []).map((o) => get(o, "thumbnailOverlayTimeStatusRenderer.style") || "");
    return video({
      id: v.videoId,
      title: text(v.title),
      channel: text(v.shortBylineText),
      channelId: browseIdOf(v.shortBylineText),
      seconds: Number(v.lengthSeconds) || null,
      duration: text(v.lengthText),
      live: styles.includes("LIVE"),
      upcoming: !!v.upcomingEventData || styles.includes("UPCOMING"),
      playable: v.isPlayable === false ? false : null,
      members: membersOnly(v),
    });
  };

  // A Mix entry in the next response's playlist panel.
  const fromPanelVideo = (v, ctx, key) => {
    if (!v.videoId) return note(ctx, key + " without videoId");
    const by = v.shortBylineText || v.longBylineText;
    const styles = (v.thumbnailOverlays || []).map((o) => get(o, "thumbnailOverlayTimeStatusRenderer.style") || "");
    return video({
      id: v.videoId,
      title: text(v.title),
      channel: text(by),
      channelId: browseIdOf(by),
      duration: text(v.lengthText),
      live: styles.includes("LIVE"),
      upcoming: styles.includes("UPCOMING"),
      playable: v.unplayableText ? false : null,
      members: membersOnly(v),
      index: get(v, "navigationEndpoint.watchEndpoint.index"),
    });
  };

  const lockupBadges = (tv) => {
    const out = [];
    for (const o of (tv && tv.overlays) || []) {
      const list = [
        ...(get(o, "thumbnailBottomOverlayViewModel.badges") || []),
        ...(get(o, "thumbnailOverlayBadgeViewModel.thumbnailBadges") || []),
      ];
      for (const b of list) {
        const m = b.thumbnailBadgeViewModel;
        if (m) out.push({ text: m.text || "", style: m.badgeStyle || "", icon: get(m, "icon.sources.0.clientResource.imageName") || "" });
      }
    }
    return out;
  };
  // A members-only video: a badge style anywhere on its renderer says MEMBERS.
  const membersOnly = (v) => [...all(v, "badgeStyle"), ...all(v, "style")].some((s) => typeof s === "string" && /MEMBERS/.test(s));

  const fromLockup = (v, ctx) => {
    if (first(v.metadata, "feedAdMetadataViewModel") !== undefined) return drop(ctx, "ads");
    const type = String(v.contentType || "").replace("LOCKUP_CONTENT_TYPE_", "");
    const cmd = get(v, "rendererContext.commandContext.onTap.innertubeCommand") || {};
    if (type === "SHORTS" || cmd.reelWatchEndpoint || shortsUrl(cmd)) return drop(ctx, "shorts");
    const image = v.contentImage || {};
    const tv = image.thumbnailViewModel || get(image, "collectionThumbnailViewModel.primaryThumbnail.thumbnailViewModel") || {};
    const badges = lockupBadges(tv);
    if (badges.some((b) => /SHORTS/.test(b.style + " " + b.icon))) return drop(ctx, "shorts");
    const meta = get(v, "metadata.lockupMetadataViewModel") || {};
    const title = text(meta.title);
    const rows = (get(meta, "metadata.contentMetadataViewModel.metadataRows") || [])
      .map((r) => (r.metadataParts || []).filter((p) => text(p.text)))
      .filter((r) => r.length);
    const id = v.contentId || get(cmd, "watchEndpoint.videoId");
    if (!id) return note(ctx, "lockupViewModel without contentId");
    if (!title) note(ctx, "lockupViewModel without title");
    // A STATION is a channel's always-on live stream, listed among its uploads.
    if (type === "VIDEO" || type === "STATION") {
      // One row is "views · age" (a channel's own videos); two or more start with the channel.
      const owner = rows.length >= 2 ? rows[0][0] : null;
      const rest = (rows.length >= 2 ? rows.slice(1) : rows).flat().map((p) => text(p.text));
      const timed = badges.find((b) => clock(b.text) != null);
      return video({
        id,
        title,
        channel: owner ? text(owner.text) : "",
        channelId: owner ? browseIdOf(owner.text) : "",
        duration: timed ? timed.text : "",
        views: rest[0] || "",
        age: rest[1] || "",
        live: badges.some((b) => /LIVE/.test(b.style) || b.icon === "LIVE"),
        upcoming: badges.some((b) => /UPCOMING/i.test(b.style + " " + b.icon + " " + b.text)),
        members: membersOnly(v),
        index: get(cmd, "watchEndpoint.index"),
      });
    }
    if (type === "PLAYLIST" || type === "PODCAST" || type === "COURSE") {
      const mix = id.startsWith("RD") || badges.some((b) => b.icon === "MIX");
      // Row 0 is "Channel · Playlist"; on a channel's own tab it is "Playlist" alone.
      const owner = rows.length && rows[0].length >= 2 && browseIdOf(rows[0][0].text) ? rows[0][0] : null;
      const counted = badges.find((b) => countOf(b.text) != null);
      return list({
        kind: mix ? "mix" : "playlist",
        id,
        title,
        channel: owner ? text(owner.text) : "",
        channelId: owner ? browseIdOf(owner.text) : "",
        count: mix || !counted ? null : countOf(counted.text),
        videoId: get(cmd, "watchEndpoint.videoId") || (first(v.metadata, "watchEndpoint") || {}).videoId || "",
        thumb: get(tv, "image.sources.0.url"),
      });
    }
    return note(ctx, "lockupViewModel " + (v.contentType || "without contentType"));
  };

  // playlistRenderer, gridPlaylistRenderer, radioRenderer, compactRadioRenderer.
  const fromOldPlaylist = (v, ctx, key) => {
    const id = v.playlistId;
    if (!id) return note(ctx, key + " without playlistId");
    const mix = /radio/i.test(key) || id.startsWith("RD");
    const by = v.shortBylineText || v.longBylineText;
    return list({
      kind: mix ? "mix" : "playlist",
      id,
      title: text(v.title),
      channel: mix ? "" : text(by),
      channelId: mix ? "" : browseIdOf(by),
      count: mix ? null : countOf(v.videoCount ?? (text(v.videoCountText) || text(v.videoCountShortText))),
      videoId: get(v, "navigationEndpoint.watchEndpoint.videoId") || "",
      thumb: get(v, "thumbnails.0.thumbnails.0.url") || get(v, "thumbnail.thumbnails.0.url"),
    });
  };

  // channelRenderer: subscriberCountText holds the @handle and videoCountText the subscribers (2026-10-06).
  // A Topic channel has no handle, so it links by /channel/ and its subscriberCountText is the count, in any
  // language.
  const fromChannel = (v, ctx, key) => {
    if (!v.channelId) return note(ctx, key + " without channelId");
    const canon = get(v, "navigationEndpoint.browseEndpoint.canonicalBaseUrl") || "";
    const texts = [text(v.subscriberCountText), text(v.videoCountText)].filter(Boolean);
    const thumbs = get(v, "thumbnail.thumbnails") || [];
    const title = text(v.title);
    return {
      kind: "channel",
      id: v.channelId,
      title,
      handle: canon.startsWith("/@") ? decode(canon.slice(1)) : texts.find((t) => t.startsWith("@")) || "",
      subscribers: texts.find((t) => !t.startsWith("@")) || "",
      thumb: abs((thumbs[thumbs.length - 1] || {}).url),
      artist: artistBadge(v),
      topic: TOPIC.test(title) || (canon.startsWith("/channel/") && !text(v.subscriberCountText).includes("@")),
    };
  };

  // ---- the walker

  const ITEMS = new Map(Object.entries({
    videoRenderer: fromVideo,
    compactVideoRenderer: fromVideo,
    gridVideoRenderer: fromVideo,
    playlistVideoRenderer: fromPlaylistVideo,
    playlistPanelVideoRenderer: fromPanelVideo,
    lockupViewModel: fromLockup,
    channelRenderer: fromChannel,
    gridChannelRenderer: fromChannel,
    playlistRenderer: fromOldPlaylist,
    gridPlaylistRenderer: fromOldPlaylist,
    radioRenderer: fromOldPlaylist,
    compactRadioRenderer: fromOldPlaylist,
  }));
  const ADS = new Set([
    "searchPyvRenderer", "adSlotRenderer", "promotedSparklesWebRenderer", "promotedVideoRenderer",
    "compactPromotedVideoRenderer", "displayAdRenderer", "carouselAdRenderer", "inFeedAdLayoutRenderer",
  ]);
  const SHORTS = new Set(["reelShelfRenderer", "reelItemRenderer", "shortsLockupViewModel"]);
  const CONTINUATIONS = new Set(["continuationItemRenderer", "continuationItemViewModel"]);
  const CHIPS = new Set(["chipBarViewModel", "feedFilterChipBarRenderer"]);
  // Walked into without a drift note.
  const CONTAINERS = new Set([
    "itemSectionRenderer", "sectionListRenderer", "richGridRenderer", "richItemRenderer", "gridRenderer",
    "shelfRenderer", "verticalListRenderer", "horizontalListRenderer", "expandedShelfContentsRenderer",
    "playlistVideoListRenderer", "gridShelfViewModel", "horizontalCardListRenderer",
    "appendContinuationItemsAction", "reloadContinuationItemsCommand", "playlistVideoListContinuation",
    "sectionListContinuation", "itemSectionContinuation", "gridContinuation", "richGridContinuation",
  ]);
  // Known and not results: neither walked nor noted.
  const IGNORED = new Set([
    "messageRenderer", "backgroundPromoRenderer", "showingResultsForRenderer", "didYouMeanRenderer",
    "includingResultsForRenderer", "clarificationRenderer", "emergencyOneboxRenderer", "searchRefinementCardRenderer",
    "richSectionRenderer", "richListHeaderRenderer", "movieRenderer", "relatedChipCloudRenderer", "chipCloudRenderer",
    "universalWatchCardRenderer", "infoPanelContainerRenderer", "playlistVideoListHeaderRenderer", "ticketShelfRenderer",
    "statementBannerRenderer", "searchSubMenuRenderer", "menu", "thumbnail", "thumbnails", "accessibility",
    "accessibilityData", "trackingParams", "clickTrackingParams", "loggingDirectives", "commandMetadata", "subMenu",
    "responseContext", "frameworkUpdates", "horizontalShelfViewModel",
  ]);
  const LISTS = new Set(["contents", "items", "continuationItems", "results"]);
  const RENDERER = /(Renderer|ViewModel)$/;

  const note = (ctx, s) => { ctx.drift.add(s); return null; };
  const drop = (ctx, kind) => { ctx.dropped[kind]++; return null; };

  const add = (ctx, row) => {
    if (!row) return;
    const key = row.kind + ":" + row.id;
    if (ctx.seen.has(key)) return;
    ctx.seen.add(key);
    ctx.rows.push(row);
  };

  const chips = (v, ctx) => {
    const list = (v.chips || v.contents || []).map((c) => c.chipViewModel || c.chipCloudChipRenderer).filter(Boolean);
    const tokens = list.map((c) => first(c.tapCommand || c.navigationEndpoint || c, "token") || null);
    if (!tokens.some(Boolean)) return;
    ctx.sorts = tokens;
    const i = list.findIndex((c) => c.selected || c.isSelected);
    ctx.sort = i < 0 ? 0 : i;
  };

  const itemKey = (x) => Object.keys(x).find((k) => k !== "trackingParams" && k !== "clickTrackingParams");

  const walk = (node, ctx, key = "") => {
    if (Array.isArray(node)) {
      for (const x of node) {
        if (LISTS.has(key) && x && typeof x === "object" && !Array.isArray(x)) {
          const k = itemKey(x);
          if (k && RENDERER.test(k) && !ITEMS.has(k) && !ADS.has(k) && !SHORTS.has(k) && !CONTINUATIONS.has(k) &&
              !CHIPS.has(k) && !CONTAINERS.has(k) && !IGNORED.has(k)) note(ctx, "unknown " + k);
        }
        walk(x, ctx, key);
      }
      return;
    }
    if (!node || typeof node !== "object") return;
    for (const [k, v] of Object.entries(node)) {
      if (IGNORED.has(k)) continue;
      if (ITEMS.has(k)) add(ctx, ITEMS.get(k)(v || {}, ctx, k));
      else if (ADS.has(k)) drop(ctx, "ads");
      else if (SHORTS.has(k)) drop(ctx, "shorts");
      else if (CONTINUATIONS.has(k)) ctx.more = first(v, "token") || ctx.more;
      else if (CHIPS.has(k)) chips(v || {}, ctx);
      else walk(v, ctx, k);
    }
  };

  const fresh = () => ({
    rows: [], seen: new Set(), more: null, sorts: null, sort: null, dropped: { ads: 0, shorts: 0 }, drift: new Set(),
  });
  const done = (ctx, extra) => ({
    ...extra, rows: ctx.rows, more: ctx.more, sorts: ctx.sorts, sort: ctx.sort, dropped: ctx.dropped, drift: [...ctx.drift],
  });

  const CONTINUATION_KEYS = ["onResponseReceivedCommands", "onResponseReceivedActions", "onResponseReceivedEndpoints"];
  const walkContinuations = (json, ctx) => {
    let found = false;
    for (const k of CONTINUATION_KEYS) {
      if (json && json[k]) { walk(json[k], ctx, k); found = true; }
    }
    if (json && json.continuationContents) { walk(json.continuationContents, ctx, "continuationContents"); found = true; }
    return found;
  };

  const selectedTab = (json) => {
    const tabs = (get(json, "contents.twoColumnBrowseResultsRenderer.tabs") || [])
      .map((t) => t.tabRenderer || t.expandableTabRenderer)
      .filter((t) => t && t.content);
    return tabs.find((t) => t.selected) || tabs[0] || null;
  };

  // ---- parsers

  const parseSearch = (json) => {
    const ctx = fresh();
    const primary = get(json, "contents.twoColumnSearchResultsRenderer.primaryContents");
    if (primary) walk(primary, ctx, "primaryContents");
    const known = walkContinuations(json, ctx) || !!primary;
    if (!known) note(ctx, "search without twoColumnSearchResultsRenderer");
    return done(ctx, { known, estimated: Number(json && json.estimatedResults) || null });
  };

  const parseMore = (json) => {
    const ctx = fresh();
    const known = walkContinuations(json, ctx);
    if (!known) note(ctx, "continuation without onResponseReceived");
    return done(ctx, { known });
  };

  const parseChannel = (json) => {
    const ctx = fresh();
    const tab = selectedTab(json);
    if (tab) walk(tab.content, ctx, "content");
    const known = walkContinuations(json, ctx) || !!tab;
    if (!known) note(ctx, "channel without a selected tab");
    const m = get(json, "metadata.channelMetadataRenderer") || {};
    const h = first(json && json.header, "pageHeaderViewModel") || {};
    const c4 = first(json && json.header, "c4TabbedHeaderRenderer") || {};
    const parts = all(h.metadata, "metadataParts").flat().map((p) => text(p.text)).filter(Boolean);
    const vanity = /\/(@[^/?#]+)/.exec(m.vanityChannelUrl || "");
    const avatars = get(m, "avatar.thumbnails") || [];
    return done(ctx, {
      known,
      id: m.externalId || c4.channelId || "",
      title: m.title || text(get(h, "title.dynamicTextViewModel.text")) || text(c4.title),
      handle: parts.find((p) => p.startsWith("@")) || text(c4.channelHandleText) || (vanity ? decode(vanity[1]) : ""),
      subscribers: parts.find((p) => !p.startsWith("@") && /\d/.test(p)) || text(c4.subscriberCountText),
      avatar: abs((avatars[0] || {}).url) || abs(get(first(h.image, "avatarViewModel"), "image.sources.0.url")),
    });
  };

  // A playlist's owner: the run that links to the channel, without the "by " its header puts before it.
  const ownerName = (x) => {
    const run = x && typeof x === "object" && typeof x.content === "string" &&
      (x.commandRuns || []).find((r) => r && Number.isInteger(r.startIndex) && r.length > 0);
    return run ? x.content.slice(run.startIndex, run.startIndex + run.length).trim() : text(x).replace(/^by\s+/i, "");
  };
  const parsePlaylist = (json) => {
    const ctx = fresh();
    const tab = selectedTab(json);
    if (tab) walk(tab.content, ctx, "content");
    const known = walkContinuations(json, ctx) || !!tab;
    if (!known) note(ctx, "playlist without a selected tab");
    const h = first(json && json.header, "pageHeaderViewModel") || {};
    const old = first(json && json.header, "playlistHeaderRenderer") || {};
    const side = first(json && json.sidebar, "playlistSidebarPrimaryInfoRenderer") || {};
    const owner = first(json && json.sidebar, "videoOwnerRenderer");
    const stack = first(h.metadata, "avatarStackViewModel");
    const parts = all(h.metadata, "metadataParts").flat().map((p) => text(p.text)).filter(Boolean);
    const countText = text((side.stats || [])[0]) || parts.find((p) => /\d/.test(p)) || text(old.numVideosText);
    const by = (stack && stack.text) || (owner && owner.title) || old.ownerText;
    return done(ctx, {
      known,
      title: text(get(h, "title.dynamicTextViewModel.text")) || text(side.title) || text(old.title) ||
        get(json, "metadata.playlistMetadataRenderer.title") || "",
      channel: ownerName(by),
      channelId: browseIdOf(by),
      count: countOf(countText),
      countText,
    });
  };

  const parseNext = (json) => {
    const ctx = fresh();
    const two = get(json, "contents.twoColumnWatchNextResults") || {};
    const secondary = get(two, "secondaryResults.secondaryResults");
    if (secondary) walk(secondary, ctx, "secondaryResults");
    const known = walkContinuations(json, ctx) || !!secondary;
    if (!known) note(ctx, "next without secondaryResults");
    ctx.rows = ctx.rows.filter((r) => r.kind === "video");
    const chapters = chaptersOf(json);
    const id = get(json, "currentVideoEndpoint.watchEndpoint.videoId");
    const primary = first(two.results, "videoPrimaryInfoRenderer") || {};
    const owner = get(first(two.results, "videoSecondaryInfoRenderer"), "owner.videoOwnerRenderer") || {};
    const views = get(primary, "viewCount.videoViewCountRenderer") || {};
    const badges = (primary.badges || []).map((b) => b.metadataBadgeRenderer).filter(Boolean);
    const current = id && VIDEO.test(id) ? video({
      id,
      title: text(primary.title),
      channel: text(owner.title),
      channelId: browseIdOf(owner.title),
      views: text(views.viewCount),
      age: text(primary.dateText),
      chapters,
      live: !!views.isLive || badges.some((b) => b.style === "BADGE_STYLE_TYPE_LIVE_NOW"),
      members: membersOnly(primary.badges),
    }) : null;
    return done(ctx, { known, video: current, chapters });
  };

  const parseMix = (json) => {
    const ctx = fresh();
    const pl = get(json, "contents.twoColumnWatchNextResults.playlist.playlist") || {};
    for (const v of all(pl.contents, "playlistPanelVideoRenderer")) add(ctx, fromPanelVideo(v, ctx, "playlistPanelVideoRenderer"));
    const known = Array.isArray(pl.contents);
    if (!known) note(ctx, "mix without playlist.playlist");
    const last = [...ctx.rows].reverse().find((r) => r.index != null);
    return done(ctx, {
      known,
      id: pl.playlistId || "",
      title: text(pl.title) || text(pl.titleText),
      last: last ? { id: last.id, index: last.index } : null,
    });
  };

  const parsePlayer = (json) => {
    const drift = [];
    const ps = (json && json.playabilityStatus) || {};
    const vd = (json && json.videoDetails) || {};
    const status = ps.status || "";
    const error = get(ps, "errorScreen.playerErrorMessageRenderer") || {};
    const words = [ps.reason, ...(ps.messages || []), text(error.reason), text(error.subreason)].filter(Boolean).join(" ");
    // A stale request (no current signature timestamp) says "Video unavailable" for every video and asks
    // for a reload: it tells nothing about the video.
    const stale = all(ps.errorScreen, "signal").includes("RELOAD_PAGE");
    const live = !!vd.isLive || !!get(json, "microformat.playerMicroformatRenderer.liveBroadcastDetails.isLiveNow");
    const upcoming = !!vd.isUpcoming || status === "LIVE_STREAM_OFFLINE";
    let playable = null;
    let overlay = null;
    if (!status) drift.push("player without playabilityStatus");
    else if (stale) playable = null;
    else if (status === "OK") playable = true;
    else if (status === "LOGIN_REQUIRED") {
      // Private videos come as LOGIN_REQUIRED, with the word only in messages or the error screen.
      if (/private/i.test(words)) { playable = false; overlay = "Private video"; }
      else if (ps.desktopLegacyAgeGateReason || /\bage\b|age-restricted/i.test(words)) { playable = false; overlay = "Age-restricted"; }
      // Anything else is a sign-in wall (a bot check) and says nothing about the video.
    } else if (status === "AGE_CHECK_REQUIRED" || status === "AGE_VERIFICATION_REQUIRED") { playable = false; overlay = "Age-restricted"; }
    else if (status === "ERROR") { playable = false; overlay = /private/i.test(words) ? "Private video" : "Removed"; }
    else if (status === "UNPLAYABLE") { playable = false; overlay = "Not available"; }
    else if (status === "LIVE_STREAM_OFFLINE") playable = false;
    else if (status !== "CONTENT_CHECK_REQUIRED") drift.push("player status " + status);
    if (!overlay && live) overlay = "Live";
    else if (!overlay && upcoming) overlay = "Upcoming";
    return {
      id: vd.videoId || null,
      status,
      playable,
      overlay,
      live,
      upcoming,
      seconds: Number(vd.lengthSeconds) || null,
      title: vd.title || "",
      channel: vd.author || "",
      channelId: vd.channelId || "",
      stale,
      drift,
    };
  };

  const parseAccount = (json) => {
    const h = first(json, "activeAccountHeaderRenderer");
    if (!h) return null;
    const photos = get(h, "accountPhoto.thumbnails") || [];
    const name = text(h.accountName);
    const avatar = abs((photos[photos.length - 1] || {}).url);
    return name || avatar ? { name, handle: text(h.channelHandle), avatar } : null;
  };

  const parseResolve = (json) => {
    const id = get(json, "endpoint.browseEndpoint.browseId");
    return typeof id === "string" && id ? id : null;
  };

  const parseSuggest = (t) => {
    try {
      const d = JSON.parse(stripXssi(t));
      return (Array.isArray(d[1]) ? d[1] : []).map((x) => (Array.isArray(x) ? x[0] : x)).filter((s) => typeof s === "string");
    } catch {
      return [];
    }
  };

  // ytInitialData from page HTML: the object after the marker, by a string-aware brace scan.
  const initialData = (html) => {
    const s = String(html || "");
    let at = -1;
    for (const mark of ["var ytInitialData = ", 'window["ytInitialData"] = ', "ytInitialData = "]) {
      const i = s.indexOf(mark);
      if (i >= 0) { at = i + mark.length; break; }
    }
    if (at < 0) return null;
    const start = s.indexOf("{", at);
    if (start < 0 || s.slice(at, start).trim()) return null;
    let depth = 0;
    let quoted = false;
    let escaped = false;
    for (let j = start; j < s.length; j++) {
      const c = s[j];
      if (quoted) {
        if (escaped) escaped = false;
        else if (c === "\\") escaped = true;
        else if (c === '"') quoted = false;
      } else if (c === '"') quoted = true;
      else if (c === "{") depth++;
      else if (c === "}" && --depth === 0) {
        try { return JSON.parse(s.slice(start, j + 1)); } catch { return null; }
      }
    }
    return null;
  };

  // ---- links

  const videoRoute = (id, listId) => {
    if (!VIDEO.test(id || "")) return null;
    const q = listId && LIST.test(listId) && !listId.startsWith("RD") ? { list: listId, v: id } : { v: id };
    return "?" + new URLSearchParams(q);
  };

  const routeFor = (link) => {
    let u;
    try { u = new URL(String(link || "").trim()); } catch { return null; }
    if (u.protocol !== "http:" && u.protocol !== "https:") return null;
    const host = u.hostname.toLowerCase();
    const path = u.pathname;
    const q = u.searchParams;
    if (host === "youtu.be") return videoRoute(path.slice(1).split("/")[0], q.get("list"));
    if (!HOSTS.has(host)) return null;
    if (path === "/watch") return videoRoute(q.get("v"), q.get("list"));
    let m = /^\/(?:shorts|live)\/([^/]+)/.exec(path);
    if (m) return videoRoute(m[1]);
    if (path === "/playlist") {
      const listId = q.get("list") || "";
      return LIST.test(listId) && !listId.startsWith("RD") ? "?" + new URLSearchParams({ list: listId }) : null;
    }
    m = /^\/channel\/([^/]+)\/?/.exec(path);
    if (m) return UCID.test(m[1]) ? "?" + new URLSearchParams({ channel: m[1] }) : null;
    m = /^\/(@[^/]+)(?:\/|$)/.exec(path);
    if (m) return "?" + new URLSearchParams({ channel: decode(m[1]) });
    m = /^\/(c|user)\/([^/]+)(?:\/|$)/.exec(path);
    if (m) return "?" + new URLSearchParams({ channel: m[1] + "/" + decode(m[2]) });
    if (path === "/results") {
      const query = (q.get("search_query") || "").trim();
      return query ? "?" + new URLSearchParams({ q: query }) : null;
    }
    return null;
  };

  // ---- rules and counting

  let rules = null;

  const loadRules = (json) => {
    const compile = (list) => (list || []).map((r) => new RegExp(r.source, String(r.flags || "").replace(/[gy]/g, "")));
    rules = {
      form: json.normalize || "NFKC",
      lower: json.lowercase !== false,
      solo: compile(json.solo),
      other: compile(json.other),
      unless: (json.other || []).map((r) => compile(r.unless)), // each other rule's exceptions, in its order
      lesson: compile(json.lesson),
      piano: Array.isArray(json.piano) ? json.piano : PIANO_WORDS,
    };
    return rules;
  };

  const norm = (s) => {
    const t = String(s ?? "").normalize(rules ? rules.form : "NFKC");
    return !rules || rules.lower ? t.toLowerCase() : t;
  };

  const isOther = (title, query = "") => {
    if (!rules) return false;
    const t = norm(title);
    const q = norm(query);
    if (rules.lesson.some((r) => r.test(t) && !r.test(q))) return true;
    if (rules.solo.some((r) => r.test(t))) return false;
    return rules.other.some((r, i) => r.test(t) && !r.test(q) && !rules.unless[i].some((u) => u.test(t)));
  };

  const lessonChannel = (name) => LESSON_CHANNEL.test(String(name ?? "").normalize("NFKC"));

  const hasPiano = (query) => {
    const q = norm(query);
    return (rules ? rules.piano : PIANO_WORDS).some((w) => q.includes(w));
  };

  // The bias: " piano" goes on the request only, when the pill is on and the query has no piano word.
  const searchQuery = (query, pianoOn) => {
    const q = String(query || "");
    return pianoOn && q.trim() && !hasPiano(q) ? q.trim() + " piano" : q;
  };

  // A chapter that names a part of one piece: Intro, Verse 2, Pre-Chorus, Bridge (instrumental), Theme B, Var. 3,
  // Part II, Aメロ, サビ, 후렴, 副歌. Only form words make a part; numbers, ordinals and letters go with them.
  const PART = new RegExp("(?<![\\p{L}\\p{N}])(?:intro|introduction|verse|pre-?chorus|chorus|refrain|hook|bridge|break|" +
    "interlude|instrumental|solo|outro|coda|theme|reprise|variations?|var|cadenza|section|part|pt)(?![\\p{L}\\p{N}])|" +
    "イントロ|アウトロ|[a-z]?メロ|(?:大|落ち|ラス)?サビ|間奏|前奏|後奏|인트로|아웃트로|벌스|후렴|브릿지|간주|主歌|副歌|间奏|尾奏", "gu");
  const PART_MARK = /(?<![\p{L}\p{N}])(?:pre|first|second|third|fourth|last|final|[a-z]|i{1,3}|iv|vi{0,3}|ix|x{1,3}|\d+(?:st|nd|rd|th)?)(?![\p{L}\p{N}])/gu;
  const WORD = /[\p{L}\p{N}]/u;
  const chapterText = (title) => String(title ?? "").normalize("NFKC").toLowerCase();
  const isPart = (title) => {
    const t = chapterText(title);
    const rest = t.replace(PART, " ");
    return rest !== t && !WORD.test(rest.replace(PART_MARK, " "));
  };
  // How many different things the chapters name besides the parts of one piece; an untitled chapter is one each.
  const pieces = (chapters) => new Set((chapters || []).filter((c) => !isPart(c && c.title))
    .map((c, i) => chapterText(c && c.title).replace(/[^\p{L}\p{N}]+/gu, " ").trim() || "#" + i)).size;

  // Over 20:00, or chapters naming two or more pieces (a compilation or a lesson's steps); a cover whose chapters
  // are its Intro, Verses, Choruses and Outro is still one recording.
  const long = (row) => !!row && ((row.seconds || 0) > LONG || pieces(row.chapters) > 1);

  const counted = (row, query = "", state = {}) =>
    !!row && row.kind === "video" && !isOther(row.title, query) && !(state && state.converted) &&
    !(state && state.queue) && row.playable !== false && !row.live && !row.upcoming && !row.members && !long(row);

  const count = (rows, query = "", stateOf = () => ({})) =>
    (rows || []).reduce((n, row) => n + (counted(row, query, stateOf(row) || {}) ? 1 : 0), 0);

  const isRecording = (row, query = "") => counted(row, query) && (row.seconds || 0) >= 60;

  // ---- discovery

  // Measured by live_test's release gate, signed out, on 2026-10-08: 14 to 20 recordings of 20 on page 1, Top and
  // New, from 5 to 12 pianists (games Top 5, anime Top 6, the floor being 5). words is what a pianist's channel search
  // is asked when the genre's songs read from its videos are few (§3.19 Genre scope): "|" between words is "any of".
  const GENRES = Object.freeze([
    ["classical", "Classical", "classical piano", "chopin|beethoven|liszt|debussy|bach|mozart"],
    ["jazz", "Jazz", "jazz piano solo", "jazz|swing|blues|boogie|bossa"],
    ["ragtime", "Ragtime", "ragtime piano", "rag|ragtime|joplin|stride|boogie"],
    ["neoclassical", "Neoclassical", "neoclassical piano solo performance", "einaudi|yiruma|neoclassical|original|tiersen"],
    ["film", "Film", "film music piano solo", "film|movie|disney|soundtrack"],
    ["games", "Games", "game ost piano cover", "game|zelda|mario|pokemon|final fantasy"],
    ["anime", "Anime", "anime piano cover", "anime|opening|ending|OP|ED"],
    ["pop", "Pop", "pop piano cover", "pop|taylor swift|billie eilish|coldplay|ed sheeran"],
  ].map(([key, word, query, words]) => Object.freeze({ key, word, query, words })));

  // ---- genres of a title

  const literal = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

  // What a title names when it is one of a genre's songs, matched as whole words in any case. Measured on 1,884
  // titles judged by hand, read from pianists' channels on 2026-10-08 (tests\fixtures\genres.json keeps a share of
  // them), measured again on 2026-10-09: 97% of the titles accepted were in their genre, and 92% of the genre's songs
  // were accepted.
  const GENRE_WORDS = Object.freeze(Object.fromEntries(Object.entries({
    classical: ["classical", "chopin", "liszt", "beethoven", "mozart", "bach", "debussy", "rachmaninoff", "rachmaninov",
      "ravel", "satie", "schubert", "schumann", "brahms", "tchaikovsky", "grieg", "mendelssohn", "scriabin", "prokofiev",
      "mussorgsky", "haydn", "scarlatti", "handel", "händel", "vivaldi", "pachelbel", "clementi", "czerny", "albeniz",
      "albéniz", "granados", "fauré", "faure", "saint-saëns", "saint-saens", "franck", "poulenc", "kapustin", "medtner",
      "godowsky", "alkan", "busoni", "moonlight sonata", "für elise", "fur elise", "nocturne", "etude", "étude",
      "ballade", "sonata", "prelude", "prélude", "fugue", "waltz", "valse", "mazurka", "polonaise", "impromptu",
      "rhapsody", "fantaisie", "fantasia", "la campanella", "clair de lune", "gymnopédie", "gymnopedie",
      "bwv", "kv", "hob", "concerto", "barcarolle", "berceuse", "scherzo", "partita",
      "goldberg", "well-tempered", "liebestraum", "hungarian rhapsody", "rondo", "variations", "sarabande",
      "moment musical", "moments musicaux", "canon", "rentaro taki", "rentarō taki", "滝廉太郎", "ショパン",
      "ベートーヴェン", "ベートーベン", "モーツァルト", "バッハ", "ドビュッシー", "リスト", "ラフマニノフ", "サティ",
      "ラヴェル", "シューベルト", "シューマン", "ブラームス", "チャイコフスキー", "メンデルスゾーン", "グリーグ",
      "ヘンデル", "ハイドン", "スクリャービン", "プロコフィエフ", "ムソルグスキー", "カプースチン", "パッヘルベル",
      "エリーゼのために", "幻想即興曲", "ラ・カンパネラ", "月の光", "ジムノペディ", "トルコ行進曲", "英雄ポロネーズ", "別れの曲", "子犬のワルツ", "愛の夢", "乙女の祈り"],
    jazz: ["jazz", "swing", "bebop", "blues", "boogie", "boogie woogie", "stride", "bossa", "autumn leaves",
      "take five", "misty", "all the things you are", "giant steps", "fly me to the moon", "summertime", "round midnight",
      "'round midnight", "body and soul", "my funny valentine", "blue in green", "so what", "bill evans", "oscar peterson",
      "thelonious monk", "monk", "art tatum", "erroll garner", "keith jarrett", "chick corea", "herbie hancock",
      "brad mehldau", "duke ellington", "ellington", "gershwin", "cole porter", "satin doll", "take the a train",
      "cantaloupe island", "lullaby of birdland", "in a sentimental mood", "improv", "reharm"],
    ragtime: ["rag", "ragtime", "joplin", "scott joplin", "maple leaf", "the entertainer", "stride", "cakewalk", "novelty",
      "james scott", "joseph lamb", "eubie blake", "jelly roll", "james p. johnson", "fats waller", "zez confrey",
      "boogie woogie", "boogie-woogie", "honky tonk", "honky-tonk", "barrelhouse", "two-step"],
    neoclassical: ["neoclassical", "neo-classical", "einaudi", "yiruma", "max richter", "ólafur arnalds", "olafur arnalds",
      "nils frahm", "joep beving", "philip glass", "ryuichi sakamoto", "sakamoto", "yann tiersen", "comptine",
      "river flows in you", "nuvole bianche", "una mattina", "on the nature of daylight", "kiss the rain",
      "ambient", "relaxing piano", "calm piano", "sad piano", "emotional piano", "peaceful piano", "study music",
      "original composition", "original piece", "[original]", "(original)", "【original】", "improvisation", "improv"],
    film: ["film", "movie", "cinema", "soundtrack", "hans zimmer", "zimmer", "john williams", "howard shore",
      "ennio morricone", "morricone", "ramin djawadi", "alan silvestri", "james horner", "danny elfman", "thomas newman",
      "alexandre desplat", "ludwig göransson", "justin hurwitz", "interstellar", "inception", "dune", "oppenheimer",
      "harry potter", "hedwig", "star wars", "lord of the rings", "hobbit", "pirates of the caribbean", "he's a pirate", "彼こそが海賊", "jurassic",
      "schindler", "titanic", "la la land", "amelie", "amélie", "forrest gump", "godfather", "cinema paradiso", "gladiator",
      "dark knight", "batman", "avatar", "frozen", "disney", "pixar", "coco", "moana", "encanto", "toy story",
      "lion king", "beauty and the beast", "aladdin", "mulan", "little mermaid", "tangled", "cinderella", "sleeping beauty",
      "anastasia", "game of thrones", "stranger things", "westworld", "wednesday", "arcane", "succession",
      "how to train your dragon", "the notebook", "pride and prejudice", "requiem for a dream",
      "schindler's list", "back to the future", "indiana jones", "e.t.", "superman", "marvel",
      "avengers", "spider-man", "spiderman", "james bond", "007", "mission impossible", "top gun", "the godfather",
      "the shawshank", "braveheart", "last of the mohicans", "out of africa", "once upon a time in the west",
      "the good, the bad", "the mission", "il postino", "the truman show", "big fish", "edward scissorhands",
      "nightmare before christmas", "the greatest showman", "wicked", "mamma mia", "les misérables",
      "les miserables", "phantom of the opera", "musical", "broadway", "barbie", "wonka", "the holiday", "love actually",
      "home alone", "the polar express", "kingdom of heaven", "the dark knight", "joker", "tenet",
      "the batman", "top gun maverick", "blade runner", "vangelis", "chariots of fire", "ghibli", "spirited away",
      "howl's moving castle", "totoro", "mononoke", "ponyo", "kiki's delivery", "castle in the sky", "laputa", "hisaishi",
      "joe hisaishi"],
    games: ["gaming", "video game", "vgm", "nintendo", "playstation", "xbox", "zelda", "mario", "luigi", "pokemon",
      "pokémon", "kirby", "metroid", "donkey kong", "smash bros", "splatoon", "animal crossing", "xenoblade", "fire emblem",
      "final fantasy", "kingdom hearts", "dragon quest", "chrono trigger",
      "chrono cross", "undertale", "deltarune", "minecraft", "terraria", "celeste", "hollow knight", "silksong", "ori and",
      "cuphead", "genshin", "honkai", "star rail", "zenless", "wuthering waves", "persona", "nier", "halo infinite",
      "halo theme", "halo ce", "halo 4", "halo 5", "halo 3", "halo 2", "halo reach", "skyrim", "elder scrolls", "oblivion",
      "witcher", "dark souls", "elden ring", "bloodborne", "sekiro", "overwatch", "league of legends", "valorant", "fortnite",
      "among us", "half-life", "sonic", "street fighter", "tetris", "mega man", "megaman", "castlevania",
      "earthbound", "octopath", "stardew", "hades", "red dead", "bioshock", "mass effect", "cyberpunk 2077", "metal gear",
      "silent hill", "resident evil", "ace attorney", "phoenix wright", "apollo justice", "逆転裁判", "layton", "kid icarus", "star fox", "secret of mana", "xenogears",
      "bravely", "live a live", "omori", "oneshot", "to the moon", "rain world", "outer wilds", "fnaf", "five nights",
      "geometry dash", "plants vs", "angry birds", "roblox", "osu", "touhou", "blue archive", "arknights", "fate/grand",
      "hoyoverse", "mihoyo", "tears of the kingdom", "breath of the wild", "ocarina of time", "majora",
      "skyward sword", "twilight princess", "wind waker", "link's awakening", "super mario", "mario kart", "mario galaxy",
      "the last of us", "uncharted", "god of war", "shadow of the colossus", "ghost of tsushima",
      "spyro", "crash bandicoot", "rayman", "banjo", "pikmin", "wii", "gamecube", "n64", "snes", "jrpg",
      "toby fox", "nobuo uematsu", "yoko shimomura", "koji kondo", "mitsuda", "motoi sakuraba", "c418", "lena raine",
      "hoyo-mix", "hoyomix", "honkai impact", "genshin impact", "lies of p", "baldur", "clair obscur", "expedition 33",
      "balatro", "katamari", "mother 3", "tales of symphonia", "tales of vesperia", "tales of berseria",
      "tales of zestiria", "tales of arise", "tales of xillia", "tales of the abyss", "tales of phantasia",
      "tales of graces", "nier automata", "dragon ball z budokai", "deemo", "cytus", "arcaea",
      "phigros", "rhythm game", "ultrakill", "doki doki literature club", "ddlc", "limbus company", "lobotomy corporation",
      "library of ruina", "monster hunter", "tekken", "devil may cry", "bayonetta", "trails of cold steel", "trails in the sky",
      "trails from zero", "trails to azure", "trails through daybreak", "granblue", "fallout",
      "grand theft auto", "gta", "assassin's creed", "destiny 2", "warframe", "apex legends", "katana zero",
      "hotline miami", "hyper light drifter", "slay the spire", "dead cells", "hi-fi rush", "yoshi", "wario",
      "duet night abyss", "love and deepspace", "valiant hearts", "anno 1602", "anno 1800", "runescape", "osrs",
      "world of warcraft", "warcraft", "subnautica", "tower of fantasy", "max payne", "drakengard", "sumeru",
      "mondstadt", "liyue", "inazuma", "natlan", "blazblue", "guilty gear", "東方project", "東方アレンジ", "manosaba", "まのさば",
      "魔法少女ノ魔女裁判"],
    anime: ["anime", "opening", "naruto", "boruto", "one piece", "bleach", "attack on titan",
      "shingeki", "demon slayer", "kimetsu", "jujutsu", "chainsaw man", "spy x family", "spy family", "my hero academia",
      "boku no hero", "tokyo ghoul", "your lie in april", "shigatsu", "your name", "kimi no na wa", "weathering with you",
      "tenki no ko", "suzume", "ghibli", "spirited away", "totoro", "howl's", "mononoke", "kiki's", "laputa", "ponyo",
      "evangelion", "sword art online", "fullmetal", "death note", "hunter x hunter", "haikyuu", "frieren", "oshi no ko",
      "bocchi", "kaguya", "violet evergarden", "clannad", "steins", "re:zero", "konosuba", "mob psycho", "one punch",
      "dragon ball", "digimon", "sailor moon", "cowboy bebop", "k-on", "toradora", "angel beats", "anohana", "charlotte",
      "made in abyss", "vinland", "blue lock", "dandadan", "solo leveling", "kaiju no", "apothecary", "lycoris",
      "akame", "franxx", "gurren", "code geass", "jojo", "black clover", "fairy tail", "nana", "edgerunners",
      "a silent voice", "koe no katachi", "fruits basket", "haruhi", "madoka", "no game no life", "kimi ni todoke", "nodame",
      "yuri on ice", "free!", "shokugeki", "food wars", "the girl who leapt", "gintama", "inuyasha", "detective conan",
      "doraemon", "spirited", "makoto shinkai", "yuki kajiura", "hiroyuki sawano", "sawano", "radwimps",
      "yoasobi", "aimer", "kenshi yonezu", "kessoku band", "mrs. green apple", "king gnu",
      "official hige dandism", "kanaria", "yorushika", "zutomayo", "tk from",
      "fire force", "dr. stone", "the promised neverland", "neverland", "parasyte", "erased", "overlord", "noragami",
      "hyouka", "nichijou", "lucky star", "date a live", "kill la kill", "your lie", "sakamoto days",
      "kaiju", "re zero", "shield hero", "slime", "mushoku", "summertime render", "horimiya", "takagi", "nagatoro",
      "tokyo revengers", "mashle", "sousou no frieren", "giorno's theme", "il vento d'oro", "jolyne",
      "bling-bang-bang-born", "bling bang bang born", "kick back", "iris out", "アイドル", "hatsune miku", "初音ミク", "kikuo", "vocaloid", "ボカロ",
      "アニソン", "anison", "アニメ", "op主題歌", "主題歌"],
    pop: ["pop", "taylor swift", "billie eilish", "ed sheeran", "the weeknd", "coldplay", "adele", "bruno mars",
      "dua lipa", "olivia rodrigo", "harry styles", "ariana grande", "lady gaga", "sabrina carpenter", "chappell roan",
      "imagine dragons", "queen", "beatles", "michael jackson", "elton john", "billy joel", "justin bieber", "shawn mendes",
      "lewis capaldi", "sam smith", "rihanna", "beyonce", "beyoncé", "katy perry", "post malone", "charlie puth",
      "one direction", "bts", "blackpink", "newjeans", "stray kids", "rosé", "jungkook", "k-pop", "kpop",
      "linkin park", "radiohead", "oasis", "a-ha", "abba", "whitney houston", "mariah carey", "celine dion", "céline dion",
      "john legend", "alicia keys", "hozier", "benson boone", "gracie abrams", "tate mcrae", "doja cat", "sza",
      "kendrick", "drake", "eminem", "rick astley", "avicii", "alan walker", "the chainsmokers", "maroon 5", "onerepublic",
      "one republic", "james arthur", "passenger", "lana del rey", "lorde", "sia", "miley cyrus", "selena gomez",
      "demi lovato", "camila cabello", "teddy swims", "noah kahan", "zach bryan", "morgan wallen", "jvke", "gotye",
      "keane", "a thousand years", "christina perri", "someone you loved", "all of me", "golden hour",
      "die with a smile", "espresso", "birds of a feather", "levitating", "blinding lights",
      "shape of you", "viva la vida", "fix you", "bohemian rhapsody", "let it be", "yesterday",
      "pop song", "top hits", "viral", "tiktok", "charts", "laufey", "gigi perez", "sombr", "joji", "conan gray",
      "the neighbourhood", "arctic monkeys", "lord huron", "tom odell", "cigarettes after sex", "mitski", "frank ocean",
      "tyler, the creator", "the killers", "bon jovi", "nirvana", "metallica", "guns n' roses", "red hot chili peppers",
      "evanescence", "green day", "my chemical romance", "panic! at the disco", "twenty one pilots", "fall out boy",
      "paramore", "pink floyd", "led zeppelin", "david bowie", "elvis", "stevie wonder", "huntr/x", "kpop demon hunters",
      "taylor's version", "ac/dc", "system of a down", "xxxtentacion", "juice wrld", "kanye", "travis scott",
      "mr. brightside"],
  }).map(([key, words]) => [key, Object.freeze(words)])));
  // A form alone (an original Waltz, a Fantasia on a film's themes) is no proof of the classical repertoire: it
  // needs a composer, a catalogue or opus number, a "No." or a key beside it.
  const FORM_WORDS = Object.freeze(["nocturne", "etude", "étude", "ballade", "sonata", "prelude", "prélude", "fugue",
    "waltz", "valse", "mazurka", "polonaise", "impromptu", "rhapsody", "fantaisie", "fantasia", "concerto", "scherzo",
    "rondo", "variations", "barcarolle", "berceuse", "partita", "sarabande"]);
  const CATALOGUES = ["bwv", "kv", "hob"];

  // The words of a genre that name only a kind of music (its own name, Soundtrack, Opening, Sad piano): they put a
  // title in the genre only when it names no other genre's song, so "Halo 5 Soundtrack" is no film and "He plays
  // Mario for an audience expecting classical" no classical. The words Jazz and Ragtime are not among them: a jazz
  // Zelda is still jazz.
  const GENERIC = {
    classical: ["classical"],
    jazz: ["improv", "reharm"],
    neoclassical: ["neoclassical", "neo-classical", "ambient", "relaxing piano", "calm piano", "sad piano",
      "emotional piano", "peaceful piano", "study music", "original composition", "original piece", "[original]", "(original)",
      "【original】", "improvisation", "improv"],
    film: ["film", "movie", "cinema", "soundtrack", "musical", "broadway"],
    games: ["gaming", "video game", "vgm", "jrpg", "rhythm game"],
    anime: ["anime", "opening", "vocaloid", "ボカロ", "アニソン", "anison", "アニメ", "op主題歌", "主題歌"],
    pop: ["pop", "pop song", "top hits", "viral", "tiktok", "charts"],
  };

  // A title or name as its genres are read: NFKC, curly apostrophes straight, one character for one.
  const plain = (s) => String(s ?? "").normalize("NFKC").replace(/[‘’ʼ]/g, "'");
  // A word ends where no letter or digit goes on. Japanese has no spaces: a katakana word ends where the katakana
  // does, and one in kanji or kana anywhere.
  const KANA = /[\p{sc=Katakana}ー]/u;
  const CJK = /[\p{sc=Han}\p{sc=Hiragana}]/u;
  const edge = (c, after) => KANA.test(c) ? (after ? "(?![\\p{sc=Katakana}ー])" : "(?<![\\p{sc=Katakana}ー])")
    : CJK.test(c) ? "" : after ? "(?![\\p{L}\\p{N}])" : "(?<![\\p{L}\\p{N}])";
  // Longest first, so a match is the longest word at its place ("mario kart", not "mario").
  const wordsIn = (words) => words.length ? new RegExp([...new Set(words.map(plain))].sort((a, b) => b.length - a.length)
    .map((w) => edge(w[0], false) + literal(w) + edge(w.at(-1), true)).join("|"), "giu") : null;
  const KEYS = GENRES.map((g) => g.key);
  const STRONG = Object.fromEntries(KEYS.map((key) => [key, wordsIn(GENRE_WORDS[key].filter((w) =>
    !(GENERIC[key] || []).includes(w) && !(key === "classical" && (FORM_WORDS.includes(w) || CATALOGUES.includes(w)))))]));
  const WEAK = Object.fromEntries(KEYS.map((key) => [key, wordsIn(GENERIC[key] || [])]));
  // An opus has its dot, the word Opus or a No. after it, so an anime's "OP 1" or "Op1" is no opus, and a classical
  // "Op. 1" no opening. A Final Fantasy by its number: FF7, FF 16, FFXIV. A Cosmic Radio entry is a rhythm game's song.
  const ALSO = {
    anime: /(?<![\p{L}\p{N}])(?:(?:OP|ED)\d*|Op ?\d+)(?![\p{L}\p{N}])/gu,
    classical: /(?<![\p{L}\p{N}])(?:[Oo]p\.\s*\d|[Oo]pus\s*\d|[Oo]p\s*\d+[a-z]?\.?,?\s*(?:[Nn]o\.?|#)\s*\d)/gu,
    games: /(?<![\p{L}\p{N}])(?:ff ?(?:\d{1,2}|[ivx]{1,5})|#?cosmic ?radio(?: ?20\d\d)?)(?![\p{L}\p{N}])/giu,
  };
  // Songs that share a genre's word but belong elsewhere: the Pokemon anime's theme, a jazz standard, a series, a
  // singer's song, Schubert's Moments musicaux.
  const NOT = {
    games: /(?<![\p{L}\p{N}])(?:gotta'? catch|pok[eé]mon anime|pok[eé]mon op|fly me to the moon|talking to the moon|game of thrones|celeste\W+(?:strangers|stop this flame|hear my voice|a little love|father's son|both sides of the moon))(?![\p{L}\p{N}])/giu,
    film: /(?<![\p{L}\p{N}])moments? musica(?:l|ux)(?![\p{L}\p{N}])/giu,
  };
  // "Neo-classical" is no classical.
  const NEO = /neo[\s-]?classical/giu;
  const FORMS = wordsIn(FORM_WORDS);
  const PROOF = /(?<![\p{L}\p{N}])(?:op\.?\s*\d|bwv|kv?\.?\s*\d|hob|no\.?\s*\d|in [a-g](?:[ -](?:flat|sharp)|♭|♯|b|#)?[ -](?:major|minor|maj|min))/giu;

  const spansOf = (re, t) => [...t.matchAll(re)].map((m) => [m.index, m.index + m[0].length]);
  // Whether re matches t outside the spans (the channel's name): a match inside one is no evidence, as Scott Kirby
  // is no Kirby game, but one running past it is, as the channel Ace's "Ace Attorney" is.
  const outside = (re, t, spans) => {
    if (!re) return false;
    re.lastIndex = 0;
    for (let m; (m = re.exec(t));) {
      const a = m.index, b = a + m[0].length;
      if (!spans.some(([s, e]) => s <= a && b <= e)) return true;
      re.lastIndex = a + (t.codePointAt(a) > 0xffff ? 2 : 1);
    }
    return false;
  };
  // An anime's OP or ED is a weak word too: "BlazBlue CSII OP" is a game's.
  const strong = (key, t, s) => !outside(NOT[key], t, s) && (outside(STRONG[key], t, s) ||
    (key !== "anime" && outside(ALSO[key], t, s)) || (key === "classical" && outside(FORMS, t, s) && outside(PROOF, t, s)));
  const weak = (key, t, s) => !outside(NOT[key], t, s) && (outside(WEAK[key], t, key === "classical" ? [...s, ...spansOf(NEO, t)] : s) ||
    (key === "anime" && outside(ALSO.anime, t, s)));
  const named = (key, t, s) => strong(key, t, s) || (weak(key, t, s) && !KEYS.some((k) => k !== key && strong(k, t, s)));
  const read = (title, channel) => {
    const t = plain(title);
    const name = plain(channel).trim();
    return [t, name ? spansOf(new RegExp(literal(name), "giu"), t) : []];
  };
  const genresOf = (title, channel = "") => {
    const [t, s] = read(title, channel);
    return KEYS.filter((key) => named(key, t, s));
  };
  const inGenre = (key, title, channel = "") => KEYS.includes(key) && named(key, ...read(title, channel));

  // ---- curated pianists

  // Pianists chosen per genre and checked on 2026-10-08: each handle resolved to its id and each picture loaded.
  // A row is [id, name, handle, picture, genres]: the picture is its path on yt3.googleusercontent.com (avatarUrl),
  // and the first genre is the one the pianist is best known for. Each genre's pianists are in its strip's order,
  // the best known first (genrePianists). A 1 after the genres says the pianist posts nothing but the first genre's
  // songs, mostly under titles that name no genre or only the pianist (inScope). Each genre a pianist is listed under
  // had 4 or more of its songs among the pianist's uploads on 2026-10-09 (tests\fixtures\curated_songs.json keeps a
  // few), and a pianist with a second channel is listed once.
  const CURATED = Object.freeze([
    ["UCvCVf_k9yMLknYHb4mzhqpw", "FrankTedesco", "@FrankTedescoTV", "XQ9IwNtkBSPq7_dOlTjfEzLtAFwJZDjnU-RKDKqrrMNqLX2T6TEuCkA6XtpKtlO0n9_Bc90S", "games anime pop neoclassical"],
    ["UCkbZZsBpZ7VLykAIDhKvAmA","Chewie Melodies", "@ChewieMelodies", "TVtNVXanQJ1k_FbP31smPsde0-W3_kc2m5zuQOPQqs3olodIitkeXO2iKCbwA6lBdljU-9vX", "games"],
    ["UCk0UErv9b4Hn5ucNNjqD1UQ", "Kyle Landry", "@kylelandry", "mK8tnQBC8_4Cs9Y_URuDJyIsLGzH5HKD2h0HXImfRMXvHBx0tIMQoUx52EPEASy1z29Zz9Yvzg", "games film anime neoclassical"],
    ["UCS1OaeP_XPquT2_Zx9n3fEA", "ViddyWell", "@ViddyWellSA11", "sQMT_c2NEgVeN3JMVdBl9-dtyKd_S5spUI2RhPknebz0MVIz59tFOqU0CxdEQcnGb4epZ4VUkg", "games"],
    ["UCJokaMCgK_gIzFpqSxgZr2Q", "Aqare", "@AqareCover", "ytc/AIdro_myOB1g9mCfD7e3vxxussRmRT51CJwILV-b2Hc_0ahqJzQ", "games"],
    ["UCJPC5-8eqqmB5W-390I1roQ", "Ryan Z 'Piano Guy'", "@RyanZPianoGuy", "ytc/AIdro_mHCV8m0htc4uvSBB3rt_pM4Gx3YAU262ZCKjZYUdT6neI", "games"],
    ["UCkPRBxO9SbNE-2h0XXKz-jg", "AtinPiano", "@AtinPiano", "ytc/AIdro_llps5EQm8Z0XFfbbvn-DlXnGDawE1DOoCZa7QgZpBLaQE", "games film"],
    ["UCecOgOHJp4pETpm8CyFnn2g", "Pianothesia", "@pianothesia", "DhYFDpfv3tzN539v2d36AvvUW87oUEQAF2WH6ys-my2uT8MeSwmLzBhpeh5kJUeW9reUCl4xtoc", "games anime"],
    ["UChx_amn94HwwDIbDR0BSJwg", "Grim Cat Piano", "@GrimCatPiano", "edm9lBgxda4O5oSruH1HCw_T5kUrp6pGnnDjOXCapt1oe46G9ZtCNcmiOQY_v1m1QyPVezgcSw", "games pop"],
    ["UCveINmmZS2ER1ZAXNDgvAnQ", "Kazuo Seto", "@kazuoseto8022", "ytc/AIdro_nDF7ZtWWL2tylTLQ31vQMQhazw9QP34GywoHF0GRVp4Go", "games anime"],
    ["UCPfSgyy2vUKycbgK78l0alg", "Lime's Piano Room", "@LimesPianoRoom", "ytc/AIdro_lETcq4P41M1-svjLALUa9yHRm9G5gaOweAnoC9XcLSP_c", "games classical"],
    ["UCPJhNkHuSsLyRK3tKLpj99g", "MYRILOVEU", "@MYRILOVEU", "CXdeszvhG2hLyCxd8FyeHW8hhUGlcvPaEzOs0GExpyg7kjMfKLR95DrfAsTrDN-wQztFXVaI", "games film"],
    ["UCMo59d9-e48ib0NYq-gYTdA", "The Larping Piano", "@TheLarpingPiano", "8Kr4WsZRgl55r_oq93LRYryXwyrFcRgVCTlctowWBNI12xyNQ7ZdmRmDIqVZ2VtjF0BWQwBh", "games"],
    ["UCHUkY44rzCum8uo7_q76kmg", "ChaconneScott", "@ChaconneScott", "C2bUUVxzzrkA9zvdVG45TqYm8HgoSGci9nzOR-hlvxSEhYWS6x-lExxQBJGYxJEu9Z0npKM7", "games"],
    ["UChiKrz8bXsB3inE1YrXs8zg", "Erik C 'Piano Man'", "@ErikCPianoman", "ytc/AIdro_kSUwjkrmxInlSDjzl7H2usK_ltaTFVW6VHqNeDWvSNkfA", "games"],
    ["UCdq5eBlgPHMDkRL-ubgcUgw", "Caliko", "@Caliko", "ytc/AIdro_ktrN2DXJb8UReExrzJp9uF2LmK54y_ZL29xhaeZeO96LQ", "games"],
    ["UCPhMH41_XgMzPYZWGX2YDdQ", "Video Game Pianist", "@VideoGamePianist", "njSFAYnUR_qYxiXKebaP8hBltqELRxrfiJ52BOSOrgzESc3Zu8c_Z8i_BVRdI22f_mVdZZhwfg", "games classical"],
    ["UC1Y8isEO4vRNqcFXXEjtC-Q", "Kara Comparetto", "@KaraComparetto", "-oaDFtlD28ygXcI-y6CJ0AKKaO2-N_7AzeMMubWFescrbyjN8C8BdnSTFCF1TBbOyORLgwO5Lw", "games"],
    ["UCqYeJA27Ph4ElIBQeV5Cm-Q", "Akmigone", "@akmigone", "ytc/AIdro_n5DDgXI4OX6npfJchWMwgcfYpfWQw14QcS4z1Xkha72zc", "games film anime neoclassical"],
    ["UCFYA71ozM3EQSNTMHhC773A", "Moises Nieto", "@MoisesNieto", "v0Cvde0LRMv-pNf1tU-ZvgwagetbDVEPbzV0Qlw5OzcvomH4O5wJeGr3AX4hzFJ658hE5JOrKw", "games anime"],
    ["UClWSXx6yriDOAZgA8avoF2g", "Woop", "@Woopinah", "3bwi_je_DDGodtYEXmewzevTA5QwIh6YdTsVekJAM6YAS9Jk9eDHm6yuKLbqmSOsqPWZ8gjjFg", "games"],
    ["UC9ipQWOcNPRh9n60-Chmn8w", "Black Cow", "@BlackCow-n2y", "SMMY6hZUoMzOJol9anagygJiah7IlZZgncfk1bUIew3hAajkYA7uD5bWt2kAQBlDjbZpFynURA", "games"],
    ["UCZILZVoHALTGS7FlC37m1UQ", "Uaaaaak", "@uaaaaak5622", "dvmFc4hsi7nlZNjE9rZ2Q3nCTjUWXI-hIYkKMIcvzIz57X3587bGI-uX3_g4T_1Lq43ASkjlC5w", "games", 1],
    ["UCRG_SZa6oF5GP6QziJQF2ZA", "purpleschala", "@purpleschala", "ytc/AIdro_lpx2KFe7yf77Yl2GLVUXy23tJT9ky1qbgnr2pfJ0f-VKo", "games anime"],
    ["UCXjjZ5EhvOz2xgno_v5NhxQ", "Verdegrand", "@Verdegrand", "ytc/AIdro_lzIjDzqF71374EHmuYHEmQSMS7wTfI_bGgnqHlCdVD_g", "games"],
    ["UCgxtI7tTpLmhBbBbZz1VicA", "TheSlowPianist", "@TheSlowPianist", "ytc/AIdro_nvNvVqNWExoAbPAIegvHIzEhyuOhW9n9po9KJdUNE", "games"],
    ["UC5H-Xbrs-XU3wwmM-Rca6xQ", "Fenvalur", "@Fenvalur", "nEr-jyRNXNQacG6UHsDAdhh4eDSVhIfAhPgXWIfu-IvkZ2ZF_lojoBbgvW4wlLUxhPZoocR6", "games"],
    ["UCgZu4Zbo5h9XwgAwk2lgBKw", "Dawn.music.", "@Dawnmusic", "BpGRUuVIXXMdNc35XvGbfnh4IPWvGVexTijgNtZ-LUzAVZK2iikeC4ln0Mv1tyH-nGuLALB-KQ", "games"],
    ["UCj0XPTTJwConnhaWKEehJeA", "Sheep Piano", "@sheeppiano", "sk0hQXvBtSTAAifXP-wCxu1LxMgdgE-2AQeZcA86ipKz4dRYtQIsa_Hg_-iG6L7iyxTCdFyx4Q", "games"],
    ["UCtj5rIKd3atoia8j033p-Ow", "Frost Piano", "@FrostPiano", "uHpx0qE5V2Gq4ZZ1wJPtpMsHSTQwwrOJ_59dlnTkcOghZdp0kVD8nzXZchTlEnmzRELCz0N9xA", "games"],
    ["UCtZlymlp4AVGNcBRuL3aM_g", "Hsiyun Wu", "@hsiyunwupiano", "tHUyYsbVBxIlKeacqDBkl1riW3PYslr1Vw-HX7E4peMWc1RGiKtAMONT2VBlTmkABTldcLcG5g", "games"],
    ["UCcQ2X1YQ5CoIKEnbex25Y6A", "Moonlapse Piano", "@moonlapsepiano", "ivZMuq3xjuJjd0G2jL3zPFxfClGJKEqpp3Pk3JJVank0FC1zM25HdlPeFJQ-ts-_XBubMAsk4wo", "games"],
    ["UCohpUgc3N1PX4E-hL9kB09g", "UprisingGrand", "@UprisingGrand", "hSYsHQRpH0Dghr5zTfd2vXgoD2k2MyzT9VDtN64ES1dK8c3InBg5vsFpG0z_UsaPCp24_J8I", "games"],
    ["UCMbPp3hvvMrecZeMTHTGRKQ", "Pauliano", "@pauliano", "ytc/AIdro_lKhLGSo2clkObuZ9cI-c4yQCeTMvTM3aabMC-xc4Ykfg", "games pop"],
    ["UCakixR-W7U1VReTDEWb2PeQ", "Elesky", "@Elesky25", "HFjsFQdpZrLl5esAnAPWdJasEpIvl6B5Dar-LjUFUO3bJ_XueAHdefOzz1jXyrT6Z8k4hMAB", "games anime"],
    ["UC0qXX3G-8nxJmyCK4G5BuLQ", "Nokturn", "@Nokturn", "TA3hQ99WZ8iRWsbpNmWXxd7LrHPYPpXwdPZgSgR9VXrVOvlHCfuQ5M3FzYIfF1V-qv11QDWFoA", "games film"],
    ["UC_99CLE91nwBG7zNXQ9YUiA", "DS Music", "@DSMusic394", "ytc/AIdro_m7-tx5ibzKOgpo2N2S07ktZudrlOm2icFUz2JeQxxPmac", "games"],
    ["UCqIe3TblfS-VhqruQoVHfEA", "Gabb", "@GabbPiano", "WM0juvrEEVM9kkfMrzh0s9FmLtVwyqauq1IrvTM96Ht2Y6R08NfqsdiqN0i4cTgZWdbEZCbR_A", "games"],
    ["UCxsbM2QNyUy7t-IsMndjs0g", "Pablo Enver", "@PabloEnver", "wytNmlekWPH5HIBlprhguJiifL_XoNliBOKkil3KTf-sasMd1NGl-wLhCLVTLTuTWapSJMWzVQ", "games"],
    ["UCiJUJS15RxWUiAYxxU_VmuQ", "Animalisa Keys", "@AnimalisaKeys", "JpxlwVPe5OQumlaLMMoYTMJs7wXfvPsdqAn2j2zRnlXqj5AcEe3HpqSSrSNNRp98Z72iip-y", "games"],
    ["UCbW5MyrelzWzukKqDOKlSSA", "RigorMortis999", "@RigorMortis999", "ytc/AIdro_m2nAVvrh7mnqYMr-LVFaR0__aA7Q-UIc_Qd_bxj7blYg", "games"],
    ["UCy3rrvHq7iUjcpQddpefUHQ", "PinaCoco", "@PinaCoco", "15TOTwAaID_Tkrix7mJeZxj0lh3Gzlb4sav-qOReeL5bl9HtwtUtiobuCwfq7qh1WJNwhbs0", "games"],
    ["UChKZz57oSQOLdl7CDEyj1RA", "Callio-P", "@callio-p", "bAoOzkgF7YGXKDNUlahcivKgmhyxsGTM7QSdu0hbk54Fy5jovk7FYX2RjEvBdIFkBZjQ5Df6cg", "games"],
    ["UCyxUCxuysOofX7uTCZ32zfg", "SicaH", "@SicaH", "v2xLuIH0KZeijPEwbPA3WxCvS3Z2bs9Kznd1NjFqPYkucX1YnPt-a0Gl40mic1h0SpogEM6z", "games"],
    ["UC_QIhKCrDbSJqkjNxdxKvXg", "LotusFlower", "@LotusFlower", "aIBR7dxN9BHOePeMyd5WNmGpPI_yYEs1cJvW-q8pU8s4pXORHlJjTP79BlQ-ZFN7lP-kzhCVWQ", "games pop"],
    ["UC7yenHa0WFbrg6qy8ZFkneA", "GaryOak", "@GaryOak", "ytc/AIdro_k2bjaQtsAROHPnRmtBzSbvF61MlFVf6IStChtZVBKI3A", "games anime"],
    ["UCJACfGSTGhK1w2aJA5QGBKQ", "iLikePiano", "@iLikePiano", "38fnwSUFVpJa0HqJ_GHCD7-Z1M2ZVgxSkli6YTq4M0s2hYFc6HoFRIn5E4sa89A5tJagVtEq", "games classical"],
    ["UCY_YhEiw45EtHFfxgV78iVw", "DG Piano", "@DGPianoUK", "8y2gf2a80moIzR5-XL2NJRLO3kG1YWllpJ5Df26XnG8xt0m-zXrp4Rs_85tfiyVjboOwR3cFBYI", "games film"],
    ["UCn8X8u5gp_H6hQzgm5fbjUg", "sebastien skaf", "@SebastienSkaf", "WOZ19qmw5umrUmavoLAYEqvu3E1KV6GpJpgCoGUzck13btPIIPIHTiUz0OJeId1ZpuAc3u4p4pk", "games neoclassical anime"],
    ["UCWK6SX1sUmJxnOjhKKWTomg", "Laurence Manning", "@LaurenceManningPianiste", "ZffV7rtVBBV6auZ_GlV2APQa-70PAtPFEddBfgPXtEqrZ_Srh76tzVveCyequMus_BjBIHSb", "games neoclassical"],
    ["UCYZ9FBmydzUde79FmJ7Zwsg", "Ace", "@Acevgm", "ytc/AIdro_mtVnoTuqGYsE-_HQnldUbdUVcqV4FasTKrOZLOWZSwTkM", "games jazz"],
    ["UChlN90CEYlutTG5cxTNn1pA", "FiveNineSquared", "@FiveNineSquared", "4ihOenG1m4c58xU96MuxH03HQKOg5avPGKGxllBlgXQccUid96h3e_UwsGBD8r2knu0tM0pmlQ", "games"],
    ["UCadQn3lhwGlFafLri6GndoQ", "Chromatica", "@Chromatica", "uB9nqaJELCEYVw_GedRpSOLd_85F1_XgfOQY5Zc0Ovq7nyO_A6fJONwFhfoPdV41A9K5Z4lVVe8", "games anime"],
    ["UCzSTaB4J0OKYT2nvgvVflxw", "pianomary", "@pianomary", "rywbBDCQIXt5-mTgJySbnSfvkM573kRGz_1QhniUkbDeTexewSnSLPkyQmZ3LypGECJfalxYOw", "games pop"],
    ["UC0b7vKTl7tBfKwSA1AwizAQ", "Clef Heima", "@ClefHeima", "YlGtct41ayxnh9hepHR8jU-UrJRKDVvkb1xd-9UZXjqZ7UOwl1cGV6dlK2qpWa_pqP-0cDdLPBA", "games"],
    ["UCwB74JoORzHoC7E6umT931w", "CC Music - Piano and Stuff", "@ccmusicc", "gDycCLW3_NeG1G7W80WBjd3IGEMha4aiwU1MmImeJQtbvIz4GdAlC1XwJqDeSAHCUjH25R1RHFQ", "games"],
    ["UCaGKm2k20uGEUs5hCBXP4rw", "Dimitreus", "@Dimitreus", "0X394v2CoCVkUMYpg2wIpBfocgKyO971iNF2y0MPOIpeh4y1R_CnDv_qLEagti7Gys6TF5L2MA", "games"],
    ["UCyW-leqPXUunrXXxFjpZ7VA", "Animenz Piano Sheets", "@Animenzzz", "y18WB5VMJe3OgJmGBNjpsDNZ5kr4IBKWwLfStD7GDJyzXb4BPe-keA4i6Pvgyd_kG0JpFAUg", "anime"],
    ["UCBShw5O9wwBt9G59xuESZYg", "Theishter - Anime on Piano", "@TehIshter", "ytc/AIdro_lbCrtEEWboY3LXNv56KxMnEz-LzfdM-GiLOP3y228tIlU", "anime games"],
    ["UCgMvjHTX7jn4iIKNHjIUJiw", "PianoDeuss", "@PianoDeuss", "ytc/AIdro_l0AtcDE33l0u3vgCdlfe-0ecdt9xmGTJmxkfsTRqFhjQ", "anime"],
    ["UC8tyyA-UIbefEexcLatHmUQ", "HalcyonMusic", "@HalcyonMusic", "joDvUG3dA-tKcMTJ_mgGtAc2yckOz8Wq1-CrK1EJhFJxej1y_nb2p0UHtYPnikw3JFACB8IuQA", "anime"],
    ["UCpQUBJrRfcpWLro4Oziff7Q", "Fonzi M - Melodies on Piano", "@FonziMGM", "ytc/AIdro_lShgGxsmT1jFULGLz3zCSQdn6ISEDGfMrkbescdE4OALE", "anime"],
    ["UCcvLSRIWJIAGFDyWtzkbiHA", "marasy8", "@marasy8", "ytc/AIdro_mnDmy4CicDbE3sAr70vTUQmCcBFpHaw3GvxNXuiFmoUA", "anime games"],
    ["UCAYrMNl92jw6cpjdpBP8JyA", "Ru's Piano", "@ruruspiano", "ytc/AIdro_mz4k2MVx2f6JexgL7j5DcNooO-BueiuZ0u5i4yS1AoE1TJ", "anime games"],
    ["UCmKy7mz6tRLv7OFdSqbAkrg", "SLSMusic", "@SLSMusic", "8aqp6lJLTSMLmdnBYlit6ZtpqIw1IF5ubSXFObFFOU3RUGWs7SgJZwkl1JOgwgugQbydMXz-bA", "anime pop"],
    ["UCPGZyaFaJe0IXRIhVFXatBQ", "PianiCast", "@pianicast", "8_nt_Cetn0cxbSaWfimPPDAH5lspF0f2CJYjNoNFkIpqYNIIoWSmtmg8D1eBuKq_aKchr0OO", "anime"],
    ["UCZ3h7IyAMbrVgvmxTdj-rpA", "Yomii Japan Piano", "@WoooRen1006", "5mx62GMIjaWhIfSC8fMTY6m5M0zoMGc_fa7YchXwDgtv1GStJblv9hJYV5fFWBRhb0_CWVhzbU8", "anime"],
    ["UC_DopZ9OANqNGho0wFwADqA", "Keichan", "@keichan8981", "ytc/AIdro_kaOZTOEuMmeXuituX_5kbr9B4QyFOZaoIuwGSI0-rLHRY", "anime classical pop"],
    ["UCJOCM908-1AyEuJT0w1Cyrw", "Torby Brand", "@TorbyBrand", "FbQsReDg4MM1J1ETlsUnc_Q3R3p-5ufk8r3qh-PAZVFRhrVxMnZt4c8MilV7zD47vOto_JgSfiU", "anime games"],
    ["UCXbCo1dYKNceoxUC7rKMjZw", "zzz - Anime on Piano", "@zzzAnimeonPiano", "ytc/AIdro_l-4VA_BjYji1V7PCLScbF3ETbRdWFSiqgOu3yUe9P6AQ", "anime"],
    ["UCXcnRAydzstW5HehZYxtzzA", "phyxinon", "@phyxinon", "ytc/AIdro_l12eKKguNsg7DvKMwc5MzF-tvN4dA3a1nX8hFwOWnO1kw", "anime"],
    ["UCisLo7aylCwtpF0QjnyCDPA", "Anime Pro - Anime on Piano", "@AnimeProAnimeonPiano", "ytc/AIdro_mOH4UJ3Niw5LhcUSeJ0uB5xxlm835m4CneSDJnkn2J0xY", "anime games"],
    ["UCIMdJx2EBZKydtSajPZwjVg", "kokoko6891 (Naoko Katsura)", "@Naoko_Katsura", "LftDjKTuOQMVq5ED6I4pW17USthDoNBPjejj2cM5Odu84GRCv-1ADhNBDwRkwiEHbk9qFo5_Cs0", "anime"],
    ["UCpiEjwzOPvQrmoDk1OlAPKg", "HANPPYEOM", "@hanppyeom", "-g7hDVAaEpEmxGCA0wK9JwFEVNodTOeOX63uXUPCGnbfWQ8Dmq4ekRoxOkJ_RsP45jpsds5pWw", "anime pop"],
    ["UCRPN4vxGPlSKmutwd7r01LA", "Joe Jenkins 2", "@JoeJenkins2", "V76iUqD6wWMkZmx_CfFZ4bwtCiJPXUnpkG2bxo4-KV85Ru7uKxKgqz4SQvUbPFxwXJLwxl6s", "anime pop games"],
    ["UCkvf8iSqKy84GDmRgJ0EokQ", "Yeh Piano Covers", "@YehPianoCovers", "ytc/AIdro_lH_xFhwV7PFZ8nPBe4wGI9uKgmgRsAREOpneK6Kbz3VoE", "anime"],
    ["UCPsaUjCimoNXIE_tRpT0XKg", "SL K", "@slk6988", "ytc/AIdro_nhy6YCPYyhizwSN8LMe4ME3vSSJnA-98ze9hF9xId8RA", "anime"],
    ["UC9lrF5voFxRoPWIfZ-CEfVw", "PianoKage", "@pianokage", "CGdBzkR_Z2QX3aFxm16KcCJsTzAD8K4uwdQTdL_oSQNdDczj9cEF3S_LL-jpHrYfraCjRCYLZg", "anime pop"],
    ["UCgXC4APRLeD_5nvfNOi60Yw", "Sudachi Piano", "@SudachiPiano", "ytc/AIdro_mIGNFeQGI3Lj9tnngU2TPHOPt10MnWGTqHgrBlm-4CAw", "anime"],
    ["UC5DrLe0u9R-5d3Z4BeW6b7g", "Richard Yang", "@RichardYangPiano", "ytc/AIdro_nB20XIsIL3m4A-tYXMilmjDAta_wGDoCea-UtGiVVdMe2Z", "anime"],
    ["UCPhDs0ZHb17Vf8I4QBtkJFQ", "Jnundead - Anime on Piano", "@JnundeadAnimeonPiano", "TQ1wvJs7obLnDIUvGRfNHPThApgvrQCfnMR5KglYtFKwWomVvrTE6qOo909yv7fmbNK6Ns7AuQ", "anime"],
    ["UCFeAvEB-4U2uRonwWkXQrCQ", "Narutee", "@Narutee", "gf-gXpqDTWhK3U7-eefrQtBlZ1aHzWXyqDjHgl-ZO7V79N9il0lVAP3njqM0p3V9JbQHcG-Fqg", "anime"],
    ["UCsIsrZMw7njrcYULI8ZXjqQ", "YXY Piano Covers / Nayu", "@nayu_", "mjD4BbFmmmckOadgxMVJr5Ow2Cn4Zc5VrK1e0x6QZlQNmbZCCU-ivnKrg_y0Q9qxs5Vg48bbhlQ", "anime"],
    ["UCnXp_AxmNywJJlnIc46QWwg", "Piano VI", "@PianoVI", "zgeT5Pmnz2kPw7HRmh5SKziJswWarn02RVkwerhLfGe4GMFuXhL7Qupfm9TIfco9nYlrX6EZ", "anime"],
    ["UCDc5cGH--aCzuyV8QIiQ5rQ", "kevinseo", "@kevinseopiano", "2bUV5S6w3tQxREAb3BimvSusSRsGa52XFixoQG_IyYObeDtTNRLbhEN_9aqeaQOPLtynAJ5O", "anime"],
    ["UCnNsIujCWNj2N4VcFh4lnwg", "Kirblue Piano", "@kirblue", "nHO-ckSl6tk2JAvYLGMORrlKPPPjSwQ8fsAiyp7vK9jowtgGQuXo5bOPtX3uYkwAjkn7aiNt", "anime"],
    ["UCpHGeKwKQVHaH0VPVlyhfww", "Cosmic PIANO", "@CosmicPIANO", "d1cngLV9eHvVKbsy_abeYXOWdi0EXmOB8tzswZMqQnYvVIlHSV1VC1DGEHJHWM0Hilvl2GlV3g", "anime"],
    ["UCUiVkDMauCL8c6JUcVlfqHQ", "Senchatunes", "@Senchatunes", "vk9sjbTAlezATYY1FlqxFtlIw6dFCnnVQYb02XLyHRt1-QU5oksx2v9YfMakOST6kOtQvhdVhJY", "anime"],
    ["UCUeLRfXF3T2hVMqv1VgFwHg", "Brian Du - Anime on Piano", "@BrianDuAnimeonPiano", "FuyUm9URB3RhJEGKtmiUl9ZcZvbNM6o5qtRogQnc9NClIEn5WYsXE9Tt2Miug15rW0dPaZuL", "anime"],
    ["UCOyfoxuBvkfNUUCVCA5SFrg", "Wei Yan Anime Piano", "@WeiYanAnimePiano", "Y6dHM-vCInj-6VtOusGPAmd3_cJs1eqzn4LRuhEtUkAERL_3ZggGtidFMZ0E15Pz__z7THxh", "anime"],
    ["UClXY3_N1O6y-lwm51iMmXcA", "AnimePianoAngel", "@AnimePianoAngel", "7gpdSq78CJRh3Nzq1IykjseOnb3wNHoQte9MxgANpRvz_bDA3EgqzdcOeRPqEs14-3vCo7WHZd8", "anime"],
    ["UCcYncif5z2ETil8i7ZSijdg", "건반윤 (Keyboard Yoon)", "@Keyboard_Yoon", "u1lq9y98c4Z4i639MmZmSsMtKfzesYVbL4ukovZOR5OMjCGwfHleCzlN7BV0ftBXFqmC5QlvwA", "anime pop"],
    ["UC0zBTo2_58fzsmJY9ks-lmg", "ホアン リン Mim", "@mim5936", "ytc/AIdro_l8Vux73NSfNUOY6WxLl9o7N-Q8nCINIUUWP9JAGd6ghA", "anime"],
    ["UCUfYUIgp0-mfW9uXALe3-Pg", "MacQui", "@MacQui", "mtY-fl8IhVT_F37XOn-9hCwYYtQpZZLNCCO4Bcl4rCd-aNtHR2-d1O5mGEYfrFhZeN-fhhcqQnE", "anime games"],
    ["UCXMeRQ1PnZl6YBrz9I2tyjA", "海音良-Umitora-", "@UmitoraMusica", "bzifDxceu0JiHE2e8oj0PMI-3HNO-GdWjTylbAVGBD4Plftx1NyoelpCK7f3KvGX3Z7u04enMw", "anime", 1],
    ["UCRxsPawP4NzeGhUrZG4df9g", "PZM Anime Piano", "@PZMAnimePiano", "SfDsSe38LS6uircJryj1DzPkKVd4vJ99aqtJ2hQt2dmFGuGCb_itmGF44eNjQHW2_5WHu_s3EMI", "anime"],
    ["UCt4Z5TSGIUEhhn8Qc3M7jiw", "Pierrotz - Anime on Piano", "@pierrotzzz", "M5ZM6TW7boxkQyGmt1VEkqnKat9jnejHwwPjHLFFcGq6AmIgt3FwK_VHua2NFFzGGLf86Ao8yY0", "anime"],
    ["UC3llEu17KL80JnDqG3Jz5Sg", "KitsuneMelodies - Anime On Piano", "@KitsuneMelodies", "qVA2NEmjv3y017CvlvAldmRMp5MDnFntxr73jdlI_hHRpsj4pOvopJKury8ljkskMWWT3RyoaMI", "anime"],
    ["UCpzlBRnXLvj_I1EnL8hUc2Q", "SuuPIANO", "@suupiano", "FNHFXqVs5M1VbFOiOivLw2rOvUqIwQg9tdSwK0PhpTDljXx_vumEyRwHMK3OL5zyYJOSTYalUQ", "anime"],
    ["UCdQTky6j3rRsDI8URr25l7A", "The Flaming Piano", "@TheFlamingPiano", "btxZ0zcukqg7mI_7oaIFILzSh6y39ygqfV9VRih2F4N-pT0D5rtvmesLlMhqx8iEFF2cvCIQptE", "classical neoclassical"],
    ["UCPmCaKjzYF3pXYLfaRhacwA", "Kassia", "@Kassiapiano", "ytc/AIdro_n7fAJL2swWhy9EQZob0b05vaUwqYISGCNm7mXHHgwbtw", "classical"],
    ["UC7k87vWwWKQO6wv4mJtrLhg", "Traum Piano", "@TraumPiano", "SbyenzVk_ddwD-xjYbkxV3_61rF0Ebr4eS45f_k3UknwgmIPFv6onZPT3vjbg07agKiA2dQRLw", "classical"],
    ["UC_QG8miwKHFNuWY9VpkrI8w", "Cateen (Hayato Sumino)", "@cateen_hayatosumino", "4fMxOYG6gBCYo5HFOEgPursI_zmMMC872OO22Ui4MjSP31gLU2WCeJZCABzHHI67w7OX7fzcXA", "classical neoclassical"],
    ["UCRYAOInuvek3RWgA2aSZQ0g", "Haruka Ueda", "@Haruka-urarapiano", "hOpYMhWgg_PgPTjgr1-yTsip0m7cifIsmRD6kDkDDbDKOiLwz2KkZ-K9YwvhG0f3pfKRTxdk5Q", "classical pop anime film"],
    ["UCJCQQIPzVfLpZTfiKIP3KQg", "Tiffany Poon", "@TiffanyPoonpianist", "rit7KjrIIF_HMGYYreWWkWojpgWH1kS5hsQsPGJ8PJaTy-bT0hENIo51RJYfgrwudJoiVFkJ", "classical"],
    ["UCG0bm7Wi92xEnXfCyZSv48Q", "Ryan Wang", "@ryanwangpianist", "Vh2QHdaNQhfIihbat3l4xdcqYU15yC_yPvJ9ZnKw0pMEAAC48laZNkF18ahNCF2p3P-o9Msu", "classical"],
    ["UCiS_keQOQlUPs1eeR2aQYuQ", "Sophia Liu", "@SophiaLiu_Pianist", "zx5kLuhSvrxPznPNd2j0-UJOSuaw5rYtpRiecodZq4Cy2IyDhpoNLZuDkDjTqpd_pBjtIa_F", "classical neoclassical"],
    ["UCHBvrTaGP6-xynel6feIRNg", "George Harliono", "@georgeharliono", "OinUIhxA2zf5xN92t1AyqYn2sn2KESizk1FoU3IRy1uB1F8ROt5M4khWyHQpsBHkAbMeqZWeHg", "classical"],
    ["UC_nAAVC8cNZB2kCQiW-o9zQ", "Tzvi Erez", "@TzviErez", "jtb6-XcsFCHXzVohIXLyRO3GTgx7DIqUM_Dclx4PB0I8l5iPHgJ49_iIL3WDOVFbKensgN42sg", "classical"],
    ["UCVqHFNw3TooIAoe0q0FJyxA", "Jonathan Bradley", "@jonathanbradley8698", "oJuhACGORt6pQe8kKiKHqGBXxPaBLMmXJ7ce3Y8eInJOOL-GKvkhI4RAHpHNOeFIWUdQ8LHG7g", "classical"],
    ["UCaWvsVVZglbkOrx9dWLiqQg", "Ink Piano", "@Inkpiano76", "wsyQsTjxyyadYfpfdQogB_QnuLJ4y1mGKryr-yDjdnX1myo-CNbjxiNagwtj9tzZ8XiaWxOF", "classical"],
    ["UC5g3jHP1KMeiDw5f4CCCmVg", "Heegan Lee Shzen", "@heeganleeshzen", "ytc/AIdro_lhJk1iPVWbsYe5QFc2EM_CGZeYblrQeWm9KDRjlpnh2js", "classical pop"],
    ["UCy1GZr3pOif_XNV704Vn-fg", "Ivan Chepkin", "@ivanchepkin3637", "ytc/AIdro_m1xeExkn86gMqSysqFvLWc_EmwbsyJLOP02anyx-jp9Q", "classical"],
    ["UC6UbiyGEGkF5iuqKRsShCOg", "Valentina Lisitsa", "@ValentinaLisitsaQOR", "oRyNKS04_eXmq9JkKEKRJxbr4_mMS5s_Bcb4oLOFaTjjXRN7n1xEftlfNQTEd683vuoyvIPVwg", "classical"],
    ["UC49Kaj6V3vBLsAZnAlDDVSw", "Ryota Kikuchi", "@komuro_metal", "ytc/AIdro_kB7Y8mfSzx6GS-_9tdbbx5zzncld6fyI56x7E9gd03Bp0s", "classical pop film"],
    ["UCzIfezoeNoczXoaz9Bh-Jfg", "Jonah Ho", "@jonah_ho", "ytc/AIdro_lVN5LoXJCmDy1zi9mI90y-EAME7fyNlo2Y4TSV3e23Eow", "classical"],
    ["UCta2twF5zcJzMSAfVGpG5VQ", "Boris Giltburg", "@BorisGiltburgPiano", "ytc/AIdro_nqnXqhUJIDAxLArdZtOi9hHJPkrvAMfEzYPxOJZ6UMY8Y", "classical"],
    ["UCC6VUpint01i6f2AqkVzT7A", "Galina Efimova", "@galina_efim_pianist", "ytc/AIdro_kc1pt_08GCfKeGW7KlFbjfrEsPvBrr1zXPrPUAlbS85Qg", "classical"],
    ["UCJJTeoDQJgKS1s1mSfEXqEw", "Josu De Solaun", "@DeSolaun", "v8et9lFQbPoUpO6gOrRx2mWcUvqepDz_ytZEmTmPIy1aSyUVNLzFaE986uKDplFf1paezXi53Q", "classical"],
    ["UCVVcBcmS3KVjZPo_QDOP5CA", "Vladimir Rumyantsev", "@vladimirrumyantsevpiano", "rmJa2F14Av8rkYkuE8rV5BF3zY4uDh8ahnlVZmPoG0zsSfqIm7yxRYaqf3iKYIsu8tDd9NDI", "classical"],
    ["UCu6oj26incNAVYUgil2VQ8w", "Birru", "@javierbirruezo", "mrDmeCJefE3y5BRIxCfh84r2OhEN6teWTS4849bQLbipJSSY20IvRi0e1I5-kqKzp3b9dLLt", "classical pop"],
    ["UCSmwOCqzjnQksFn0t7ARkkw", "Jacob Koller", "@JacobKollerMusic", "ytc/AIdro_lx9NjVZa6cJkYPILCcDJ4xb9kotAhyyO8_TYAEvHyYnpU", "jazz anime pop"],
    ["UC9B2D-VzslJ_OTUXA8-V_dQ", "Yohan Kim", "@YohanKimMusic", "NtNBw62EuZZS4MCn9ruNJxxIijmDqNXK7gpowrpkUwkmiTCJPcredY7yivIgf6-nB_hXXYjQGO8", "jazz"],
    ["UC0GHYT_Blk1HqeKJps0h5oA", "Hiroshi Nakamura", "@jazzpianosheet", "ytc/AIdro_miaN_B0pxdyBMCWhRUK3Ly1gwE8AF1uCcY7MowSb-SgEE", "jazz"],
    ["UCHD71Cp2wuQnb7ee-cGJoZg", "Daeyoun Kim", "@rumbol2", "8UF8_0M4IGjTPSbWGwNw9S47R3q1x0O0VqYZtEVRPZ99PZIi9_FvuX1oa7DVYmgfHxb7fHSO", "jazz"],
    ["UCZXjqsi7hELtBZCaArO2d4A", "MIWHA", "@imiwha", "ytc/AIdro_kXY7IgyUToaOaa3_mjOkA3R8dwPUOGP-A3jbaFnR-AmlM", "jazz"],
    ["UCfZtzmK-txIdeiwvF_62XwA", "Luca Sestak", "@Luca_Sestak", "aC1cuQSZZGSPO6KpV7Kmf5AmB4Yruz-_-TsBqzFF9-zpBsIqRJU92QaH5KwxG1EtJMGZPXQpx7w", "jazz"],
    ["UCa6uvz0u3yrkxs-KU3MWouA", "Bijan Taghavi", "@BijanTaghavi", "9vhFjwSZIDknc-jkdnG1vRsvE1F7YgOmmWFGpjKINarW8BC7POY4tzKNa5jv6imTCc30udMo", "jazz", 1],
    ["UCtTIDxFxwVzAoJL7eY9Vnyg", "Stephanie Trick & Paolo Alderighi", "@StephanieTrickPaoloAlderighi", "ytc/AIdro_ldgwlDVqTXGn2K_g-8li0_9FFdn0dnEviuVGsnOVzCeA", "jazz", 1],
    ["UCRy78KbiyrOdPq0Jg3rOZCQ", "Cornelius Tsen", "@CorneliusTsen", "GIE2eiZi7NkD_6XHeKujCh7eMzZ1-hidlnjbr6Ska0n504ZMp15EJ564UTpXe14KGTK11nb-DA", "jazz"],
    ["UC29p2iFgxUTA9F5dT08iOiA", "Jack Wyndham", "@jackwyndham", "x9r7lHKjZSb6aHiF_xdlSM7gI8Md3dvBHX6B4jdF-5bHEdfZ5g0U46Tf9wMfuFcHv-V0nK6E", "jazz"],
    ["UCeY-vZ20Bk99GbkONMdWAOw", "Cili Marsall", "@cilimarsall", "ezuRJHolA-Worbx_FV4R-u66Q4e5C0wcuHbZmJxuhEjc-54Ky3mfo9y0xYfNB9lgmh1wRMjC", "jazz ragtime"],
    ["UCck_dnaQGV3AFbucvJ7x9EQ", "Mamiko Koshiyama (mamikossy)", "@mamikopiano", "TimKByCG_J4TkkyI_PtpWj4cubxmAV5DChPK_wU4ZoguQBpUY3p6fU46rTcAHVS03X3xQTkH", "jazz"],
    ["UCCzXR3fpEtrobs0DVjhiLBA", "LamiPiano", "@Lamipiano", "zrEkAlhbkEnuFVzc4yp-09tqHEq9LVmVHISyZQ9gvfBeOlk6t0AnweAWjkaPjQQ40XlEvSUc8R0", "jazz pop"],
    ["UCniHfocw4LKRQMGyHuM0Jnw", "amari", "@AmarisMusic", "-xBmnyxW55wTNAJZbu6LzOaOaKoF8qYxxbkLPgtTzINQih3CfYVB2lLUVl9J8T5HGQcqfFiULA", "jazz"],
    ["UCFOzbMUgcFWlmG-nfHOT_Zg", "Elena Fortin", "@elenafortinmusic", "zRtWh455tXQ14HD_L5pHB_zPaT3fNPMqHYuaK1DugNDbsS9kLndvGGa-VTADSLYq2xwq_OjtWpk", "jazz"],
    ["UClw8Huc_XZcz46GJh5Z0wuA", "Brendan Kavanagh", "@DrKBoogieWoogie", "guWea-DdYyLqdU8gU2u5fVL1Vizb91zs3VZy9ot_asUs1EXvDOgUMYIT-5u0Fpx2JZTovO4c4A", "ragtime jazz"],
    ["UC2sJae2b42U56oyacUqgtZQ", "Henri Herbert", "@HenriHerbert88", "eVKsPR4mdmo7IOOHNcFzf7spSXfo2V5rQPwDyHZDc_7qEgP0tJKaHJTIJSOEsAhLLiloOdQ3", "ragtime jazz"],
    ["UCPXlJQH2Rx5QG4aF8w52HlQ", "Dorian Henry", "@RagtimeDorianHenry", "HmzR_fmzdjTXuB0KKUOdZho6aVcWFhwEIFzOjDkonyA0gk5v1HikFGxrhdFcEujd5MoaabS1", "ragtime jazz"],
    ["UCan5TESkEUPcIcSYx1ZWBbQ", "Natalie Ragtime Piano", "@Natalie_Ragtimepiano", "11VRfe8Z-40YtHaXx9-LBFksvVHbxSLaWC5w-UqiQkfAdaHue2rlp4K5ca55ZJG6WEQExEK5jg", "ragtime"],
    ["UCldDNmPiOeMBq0xpuNo0ZXQ", "Christina Pepper", "@ChristinaPepper", "V7DhZSH4llfem2eQQ3IpWUIx09TBy04WxxxmxIdiTguWp1CsaO7M4281cuJFzjnbelpULVP82lw", "ragtime"],
    ["UCQollaVxznQ0iu1aTaFwY5w", "Max Libertor", "@maxlibertor", "nUc3BcukaGjguyD650Qej2HcUg5uoWPnq2nDG6xOVCp0ur6unvrqyr_gMagSGV0VoRHYRIT7iQ", "ragtime jazz"],
    ["UCwzAmROPHasu6Gof1I4qvYQ", "Victor Demange", "@victordemangepianiste", "XHNcu_Gi69QcslT7Eht3k0QAuf7bk-DO0De1QKEHsmRtbnuOi3_8J3q1G-BWG0aeAwW2APB1GSo", "ragtime jazz"],
    ["UCmpKmtTlLRPzuHc-aiQywaQ", "Ethan Uslan", "@EthanUslan", "ytc/AIdro_kTvjwW2J4d5FuQBMCuyU-398-29rIrndkiONHxd1fCG6c", "ragtime jazz"],
    ["UCyh9oqo1-gvNlDQvf98tqOQ", "Play it with Julia", "@playitwithjulia", "XG-u-ci1_Yle2aD3_rbyILn5Bq_YVkdInop9Le-iXWF2GS6m4jKxUlhYAPIw0Xo157EBeoWN", "ragtime jazz"],
    ["UC2Q4VxSPqYIgTMA2O9mzEDg", "LADYVA", "@Ladyvaofficial", "mczSGcFeq6LsQwl8d0OJL8AyC1zy-EgCrRf41MHYrkvyImZ9n51zeR29PupUqhZFPXJBL8KeEA", "ragtime jazz"],
    ["UCAsvWXk7D7sxDQxUx3Fv2wg", "Stefan Ulbricht", "@StefanUlbricht", "ytc/AIdro_mdO_8AjLYe61wf-lyNC6x1u4mV7U0oSEWwZGZa34GFKER6", "ragtime jazz"],
    ["UCYYxV1KM-cmE0MVP7hHRMGg", "Jonty Williams", "@JontyWilliamsMusic", "PdOG-Y1uB4UDmHCBfpA-Bkltb6Jg6DvPIhLURhXcD3pg08RyAK3PQsvGxRSsOaS_FmWdzlVJ", "ragtime jazz"],
    ["UCdeUYsSbagRXofRxj9jpf4Q", "Luca Talotta", "@LucaRagtime", "CODHPugpYtx09jsb6qHC9VvALGXfz7lHNeUBRTruJuLcGj_S1ParrXF1qEAIG-qHOSk33mQWyw", "ragtime"],
    ["UCinz8D5Ncr9Qf2AyzOOFD6A", "Jorg Hegemann", "@joerghegemannboogie", "RVRh8dJClMV2mYmkAB5ynvLCDoE60Gpr7TlqnspZNwkRg4WZfU6W_GFjvxdGTnezfI1Sf3B34w", "ragtime jazz"],
    ["UCTphUsut5MAeOWYDscQfLsQ", "Kristen Mosca", "@KristenMosca", "ytc/AIdro_kZxg_cuFiIuGTkg_uT4TM1bNIWPclCUbPk8_2orQ3jzXw", "ragtime film"],
    ["UCTjzQr4wvHoWjUrb4kBqgBw", "Martin Spitznagel", "@Spitzfire", "ytc/AIdro_nP3nEWHjoOoR_yaDXo7xnX9lz5CyPLdlFvvsPwqyVUUg", "ragtime"],
    ["UCNEI-oWOivQJ9IMn8GeCrMg", "Gabriel Piano", "@GabrielPiano1", "r2pYsqQG3PT9M7cdL9xyPvoNltql4ThtA8ReEMCEuz9Nw8tGVWbRzuBm-gcSlIbacP0Yj8pJmA", "pop"],
    ["UCsR6ZEA0AbBhrF-NCeET6vQ", "PianoX", "@Piano-X", "7Hw-pT3y5Ab29-yn7jkwS1dC4TBCTW_ormfZTq-TqCBi6GzT9G6HclEPWHbyloA1Kd70_nVB_g", "pop film"],
    ["UCeP0-mA85a1UM05qFMub7Ow", "Peter Bence", "@BencepeterOfficial", "ytc/AIdro_mfHIF2qXxT7oFh2WVDzhQ_q55U_j-DpNRWwYUtUzfbJeY", "pop film"],
    ["UCQ8THdZnqPVoLN9zuT-pjBg", "Francesco Parrino", "@FrancescoParrino", "F8LgYVwtvgDbEmgdqshrPmnD3STwoFJguJbn5pvMX_lGbLd1t1iRp0Ap16sUNClSRWHIZIZx", "pop film"],
    ["UCXDFQexmA_BUAdxXhSUNGIw", "Jova Musique - Pianella Piano", "@JovaMusique", "YBmZZxlx_wQIOmKq82YnGJSdiVu0G1xOwg8taKzeTlbuT5A1H48m59oLXnvrZNkIlCVmflN2", "pop"],
    ["UCr4fZBNv69P-09f98l7CshA", "HARAMI_PIANO (Harami-chan)", "@haramipiano_main", "ytc/AIdro_mwLrsy5zXu40N0dQnq8jxsaOebN-3owaEsEsKfPV4lseE", "pop film"],
    ["UC1IocYhHcmBkdvOKbncLb0w", "Peter Buka", "@PeterBuka", "yowG-jddca3vEySuq16SnbwHV4SXgKiqDjVOTDY5no8uHG6qc_Sz3gn9h3n1qoDA3rANc7aWh-g", "pop"],
    ["UClG-3qKNyHBvpjyatYdYlqw", "Katherine Cordova", "@KatherineCordova", "ytc/AIdro_mDifs5ePYa3AFWDbe7i90tvRASgcYcwSqY80UE0J4ekK8", "pop film neoclassical"],
    ["UCKscaP2HvovgMjkpY6KwfJQ", "Gamazda", "@Gamazda", "Ffzix9ewhjQocnu4Akb4CHFowhjnchuWgpCXrn3kMOY9emEWMN7CCM88sCNcXOyBKaPV38Axag", "pop"],
    ["UCbKM5fcSsaEFZRP-bjH8Y9w", "vkgoeswild", "@vkgoeswildofficial", "W_wOB9j0PLLUM7T9h8U69Wc3L6qEQG2DZNQULTienqoaHk3DK7m8oiFTCir1pFVCvOgVk-8YMXs", "pop"],
    ["UCJzX-p_YJLuDIkuIzGlySCQ", "Riyandi Kusuma", "@RiyandiKusuma", "ytc/AIdro_nPfhzraF3mcodJ-OjNMP1GamJRrfSWJSsC7JbYR0xLc8g", "pop film neoclassical anime"],
    ["UCwj-IGZKD33CwE0HZqN1hew", "Dreaming Piano", "@DREAM-in-PIANO", "e1dkyYG4nCGSoB1c9-fxqGObwHm2-cjy2kzIB7Yv1N_ox6Y9YTCDaV6m8jwemTPN4v3NEGv8fQ", "pop film"],
    ["UCIvxmYZSfLNbtA4OPMHqKBw", "Costantino Carrara", "@CostantinoCarraraMusic", "MrfzzH9t5yX-7qJZRBilPlhTqeb5s1sly04W_-OAVEn0RvzPG4TsyFZTGpjt17UZpVA5gDZxcsE", "pop film"],
    ["UCyB2ZqjYXy1-1MVsyVQvRVA", "Daria Pyshnenko", "@daria.pyshnenko", "a4rWYF9P6-NPsKcujiw9i2OT0Ce6HGejkwTQqO0QUMotUhCqZTx96acnXiV1br6L2RFxfxFr3A", "pop film"],
    ["UCQxsahIsaO7_nDwVlFRMQiA", "Ludovico Einaudi", "@ludovicoeinaudi", "YICHBuQgk2nm5IkVpgF7kM_-uVj1DIy-br8oYlBfNnriZABMx6gcw9Ny4gtel_IY4VJ-0hUk", "neoclassical", 1],
    ["UCj16_sROSTrwMSWW88fOJeg", "Yiruma", "@yirumaplaceofficial", "Zzk5cgLrMpIk6Ml-FVU2SMVC3NcqLgXAC1XHDnQTsEaEdEhwT0lPctDjoHcemTUDDRnn95C9UR4", "neoclassical"],
    ["UC_6zyKjvT-qt1Uf-_yqHb4Q", "Gibran Alcocer", "@GibranAlcocerOfficial", "-Qq_aV0tdYQL7Z5vqbjx2kLP4eYZvDGx6ZPIqFdvuy_8P7jIvoIL9AKg2hcefy6PKWrDJ8p8BQ", "neoclassical", 1],
    ["UCHeUFBUuXZaOZrPfQv74teQ", "Andrea Vanzo", "@AndreaVanzoComposer", "wrbmd_dRcK9ZRHWl82ufzvCHQ7vUUok49iZS0Gynhk0fkwfZVP8SD-RLYbJq4XwtLRSeCtBT", "neoclassical", 1],
    ["UC7RB6NE6qcJJDbmchiWq67Q", "Sofiane Pamart", "@SofianePamart", "osdQR1VpdDXSwV-mPde2h2ZZcOB_QafIbXDoxfiM4Q0gr59sDX9ub528DUARepQ3zuyAcmdsJpI", "neoclassical", 1],
    ["UC-HWNte9KM0GH2V3MTh0iOw", "Joep Beving", "@joepbeving", "0H4H1HDYLF841McvgbCXHxf5d8KTvyeCKrA-e9xh7QpbxN-s-vhFSrmDySPv01kwAfC95vtdvKY", "neoclassical", 1],
    ["UClQPk2WbC23z3eogxPbbOjw", "Jacob's Piano", "@JacobsPiano", "ytc/AIdro_lHrryRNw3ELF7CC_uy3pF916cIXryrYdppp3IWq5iT-4E", "neoclassical"],
    ["UCwikkShTpU5pvg2JqPlJCFg", "Fabrizio Paterlini", "@fpaterlini", "yMapf5brlCcZSi4xVMezBR_OEBP2m8ndkzckwOCctIFb9dL--utNE3NraBI_B3-nrDODeNSl41I", "neoclassical", 1],
    ["UCNf-R0Oh3D8N6QZT12SIGeQ", "Jean-Michel Blais", "@jeanmichelblais", "E5vCBaXSZZAE4eEUFEHuolHlYvtvmhhAXbWBOdGoPQv9ADuIHf5hs0munOhxqVn92q1hq0wSJg", "neoclassical", 1],
    ["UC1pJ-uL_yTocm5ZmHXXz_wA", "RIOPY", "@RIOPYmusic", "7KtNsdJoA7o10yQLz6Z4SjHF2NdGSHD6mDbJ2pf5AvrcRrUTcCWkNbfmV63-MBNkdDBElabQOA", "neoclassical", 1],
    ["UCKsXPYKshJKal76PBGRMdPA", "Alexis Ffrench", "@AlexisFfrenchMusic", "o1ZWHvG4l5X0nvENZmu9cP14oWbd6QUZrJWL66xztyDhRR0Wz3rB3-a_I2ODFPygJJ9zGPEHAQ", "neoclassical", 1],
    ["UCJE3-iLal6v-rX7in5rXk8A", "Dirk Maassen", "@dirkmaassen", "ytc/AIdro_m2OzI67d_9QPwu3Pva6_kOle0z7PtkMIrOxJ8v_nlgQ7M", "neoclassical", 1],
    ["UC86j_bjuNmRqypmalXErKzw", "Jamie Duffy", "@jamieduffymusic", "etgW8HTCOHEuEcONhoVTG3iekSXSiOMvrAsYhKpidILxs8f4tY03XdCFwtHPjz73A-kCzPDpkQ", "neoclassical", 1],
    ["UCYILTNMkIX16Z18QmwcDUlw", "James Malikey", "@jamesmalikey", "_l32ZI_rQXm2ZjtvUsCxj6aUlCF4tVvMpzV2z4-ERyQGrDMmtnjjgzBRjikNTXXoJQTJbFxDrQ", "neoclassical"],
    ["UCjyqGFuEZho7Wc7TT5MyFwg", "Michael Ortega", "@MichaelOrtega", "rI_DnZOV05LbWL1R9h3eHN-MgMV_utI-_UP91ToC6Ywgsyx6Ebb0bsuP0lFaMp5JzM0h2DFeIg", "neoclassical", 1],
    ["UCV_NSE1aLQt04b9CHUX4l7Q", "Luís Graça", "@luisgraca", "VRrrYeOmm73dNKkxjDy_FO5cqSFejTYYZRvAlm0HXmi8PScN6Pcy5QdPbWBmZAG3KKnOMTZYKJk", "neoclassical film classical"],
    ["UCs7O9sOUQiBGBxaaAguIwig", "Patrik Pietschmann", "@PatrikPietschmann", "ytc/AIdro_mBfw5hVIVibHxEqUKxYHm0y3aBssH4y48zsGa4Fl9HzE0", "film"],
    ["UCUfpQmWcv3JsVkQUGv8w__w", "Leiki Ueda", "@LeikiUeda", "ytc/AIdro_k1hOMC2pJb-rBqfZ1nbxnw79iug-6qL9n6cXbRfWUbwHr3", "film"],
    ["UCrghwzFBfu_5zobLn2zUIJg", "Brett Mitchell", "@brettmitchellconductor", "nkDOQN1NaSEw9YTk4fCrwDSAYO0btaC_5JKnOJf8dR4pbOp1LRfbBg51BTTzbMuFsYZQy7M4lm4", "film"],
    ["UC6Kpg-Ffzs6fK7flwq3QDdw", "Yoshi Sugiyama", "@yoshisugiyama9317", "c4KglXIDJqXzeNcnrGYzMPxJvvR06HidNXkoFkZXys9K03G5e6T8WWJtdo6oCDbPc3e19hRFav0", "film anime neoclassical games"],
    ["UC_QkOOvaeRihoP0EEAhGTZQ", "Matt Craig Piano", "@MattCraigPiano", "2G7Fg0Y4T5NQST6Xo07-ohyqv8ZPmXIOD86EksEyafvosgd8J02tr2uzNO9viOX_1I61r-5o", "film"],
    ["UCnGlXn04XOPwdqdRG5ttHLg", "Samuel Ramos Piano", "@samuelramos712", "gVIl7Z7FH0uRXqpdexlXBES3vrD1GJPErthxC4yHTuUMjynsL_S-jWemCAqHikF9qChC26uNDSY", "film pop"],
    ["UCS_eQYb7OAZJ_MOcdlKVg1w", "Laurie Théberge", "@LaurieTheberge", "CRcxEkArTowQLP-gLXrgV-pN_5AplW0k81U0g_prYkHEh8irS94k5juxX7s5DpTlDy-SA14m5ws", "film"],
    ["UCOIOKPmOrYYs0Idqn7fstSg", "Noud van Harskamp", "@NoudvanHarskamp", "ytc/AIdro_lPgohl7tS0IJfKY4EDVuSk-OFCdAuXWGxNb9z3sZN0H0Ho", "film games"],
    ["UC3-KJSW0feyMsbI2OJdKFig", "Samuel Fu Reborn", "@samuelfupiano", "Qdsm2gRo6RSf9Oh-yu3vNBJKQ1TJHVn23IGoFAI00RvLv0JU1w5DX5ekqVAReG-7tIdesjBxa74", "film"],
    ["UCFfgUYN2DiD8fqsOMPnCSZQ", "Vladimir Lobov", "@Vladimir_Lobov", "2wCUJSY4p_y3CVHJ_9TnmUmpcyMjvNDF-ZZTjG__GghL_iTPTrhyYniFh1ke7GJdt4OOTpIH", "film neoclassical classical"],
    ["UCDGvWJ9O65Ijxxb6oK0yw7w", "Euphony", "@EuphonyPiano", "DoM0RV8ZCf7IIV1bnGhWC99XzD1QOI9Xksh1rCaOFSzP4iKNYl5Ozj7iGy2uDIQvqM85Co5U", "film games"],
    ["UCpZIpFEwuuvREBlRP8hN_ng", "nattaliepiano", "@nattaliepiano", "OonEkzTjx7KlYWWlvk65A0UkCmDahyHO96ecY9ADzo7YwRujJACDfYumSldQatTFlX_F4jng", "film pop neoclassical"],
    ["UCG6UXdRkyMyDpOqzYkEE0_g", "Michael Zarubin", "@mzmaster", "TH4K4S7Y3JE40Iuw6Hf5L7ZpkOTwG3FqbZzqysoyFqMV8McCZK9YPEqHqoSUmm5aep9XZzsG3A", "film games"],
    ["UC4jcKDsQSPaQULAK7hj3FQg", "Silvio Caroli", "@SilvioCaroli", "ytc/AIdro_k7FnwMbbU3JV_euVNt7d4tKxGjg7QrUzPvqhQk3mBPuk0", "film"],
  ].map(([id, name, handle, avatar, genres, all]) =>
    Object.freeze({ id, name, handle, avatar, genres: Object.freeze(genres.split(" ")), all: all === 1 })));
  // Channels never shown as pianists, whatever their rows.
  const BLOCK = Object.freeze(["UCPZUQqtVDmcjm4NY5FkzqLA"]);
  const BLOCKED = new Set(BLOCK);
  const ALL = new Map(CURATED.filter((p) => p.all).map((p) => [p.id, p.genres[0]]));
  // Whether a video is one of a genre's songs for a scoped view: its title names one (inGenre), or it is a curated
  // pianist's who posts nothing but the genre's songs and its title names no genre.
  const inScope = (key, title, channel = "", id = null) => inGenre(key, title, channel) ||
    (!!id && ALL.get(id) === key && genresOf(title, channel).length === 0);

  const avatarUrl = (path) => (path ? "https://yt3.googleusercontent.com/" + path + "=s96-c-k-c0x00ffffff-no-rj" : "");
  const curatedFor = (key) => [
    ...CURATED.filter((p) => p.genres[0] === key),
    ...CURATED.filter((p) => p.genres[0] !== key && p.genres.includes(key)),
  ].filter((p) => !BLOCKED.has(p.id));
  const card = (p) => ({ id: p.id, name: p.name, avatar: avatarUrl(p.avatar), curated: true });

  const score = (p) => 3 * p.rec + (p.artist ? 2 : 0) + (hasPiano(p.name) ? 1 : 0);
  const ranked = (list) => list.map((p) => ({ ...p, score: score(p) })).sort((a, b) => b.score - a.score || a.at - b.at);

  // The channels of video rows, in row order, with what they bring: {id, name, avatar, artist, rec, at, rows}.
  // query is the words the rows were searched with, or a function of a row giving those of its search.
  const channelsOf = (rows, query, keyOf = (r) => r.channelId) => {
    const by = new Map();
    for (const row of rows) {
      if (!row || row.kind !== "video") continue;
      const key = keyOf(row);
      if (!key) continue;
      let p = by.get(key);
      if (!p) by.set(key, p = { id: row.channelId, name: row.channel, avatar: "", artist: false, rec: 0, at: by.size, rows: [] });
      if (!p.avatar && row.avatar) p.avatar = row.avatar;
      if (row.artist) p.artist = true;
      if (isRecording(row, typeof query === "function" ? query(row) : query)) p.rec++;
      p.rows.push(row);
    }
    return [...by.values()];
  };

  // The channels of the rows a strip may offer: a recording among them, not Topic, a lesson channel or blocked, not
  // one of mine, and not one this session saw post mostly anything but recordings.
  const offered = (rows, query, seen, mine) => {
    const shy = (id) => {
      const s = seen && seen[id];
      return !!s && s.rows >= 4 && s.rec / s.rows < 0.5;
    };
    return channelsOf(rows || [], query).filter((p) => p.rec > 0 && !mine.has(p.id) && !BLOCKED.has(p.id) &&
      !TOPIC.test(p.name) && !lessonChannel(p.name) && !shy(p.id));
  };
  const pianistOf = ({ id, name, avatar, artist, rec, score }) => ({ id, name, avatar, artist, rec, score });

  const pianists = (rows, query = "", seen = {}, own = null) =>
    ranked(offered(rows, query, seen, new Set(typeof own === "string" ? [own] : own || []))).slice(0, 12).map(pianistOf);

  // Record labels, the Official channels of composers and labels, piano makers and sheet-music channels post a
  // genre's songs without being its pianists.
  const LABEL = /(?<![\p{L}\p{N}])(?:records|recordings|vevo|official|grammophon|music channel|vault|masterpieces|& sons|sheets?|sheet music)(?![\p{L}\p{N}])/iu;

  // A genre view's strip: the curated first, then the channels of its rows that post the genre's songs (one of
  // theirs here at least, and half of them or more). The genre's first LEAD curated pianists always lead; the rest
  // take turns after them, STEP further on at each turn and round again, so each curated pianist has a place in
  // some turn. While channels are found, the curated keep SHOWN places at most and leave the others to them.
  const LEAD = 9, SHOWN = 18, STEP = 9;
  const genrePianists = (key, rows, seen = {}, max = 24, turn = 0) => {
    const list = curatedFor(key);
    const query = (GENRES.find((g) => g.key === key) || {}).query || "";
    const more = ranked(offered(rows, query, seen, new Set(list.map((p) => p.id))).filter((p) => {
      const yes = p.rows.filter((r) => inGenre(key, r.title, p.name)).length;
      return !LABEL.test(p.name || "") && yes >= 1 && yes * 2 >= p.rows.length;
    })).map(pianistOf);
    const rest = list.slice(LEAD);
    const at = rest.length ? ((Math.max(0, Math.floor(Number(turn)) || 0) * STEP) % rest.length) : 0;
    const curated = [...list.slice(0, LEAD), ...rest.slice(at), ...rest.slice(0, at)];
    const places = Math.max(Math.min(SHOWN, max), max - more.length);
    return [...curated.slice(0, places).map(card), ...more].slice(0, max);
  };

  // Home's curated cards: in turn from each genre your newest conversions are in, the genre with most first (ties in
  // GENRES order), or from every genre when no title names one.
  const homeCurated = (titles = [], skip = [], n = 12) => {
    const counts = GENRES.map((g) => [g.key, (titles || []).filter((t) => inGenre(g.key, t)).length]);
    const some = counts.filter(([, c]) => c > 0).sort((a, b) => b[1] - a[1]);
    const lists = (some.length ? some : counts).map(([key]) => curatedFor(key));
    const taken = new Set(skip || []);
    const out = [];
    for (let i = 0; out.length < n && lists.some((list) => i < list.length); i++) {
      for (const p of lists.map((list) => list[i])) {
        if (p && !taken.has(p.id) && out.length < n) {
          taken.add(p.id);
          out.push(card(p));
        }
      }
    }
    return out;
  };

  const rankChannels = (rows) => {
    const tier = (r) => (r.kind !== "channel" ? 1 : r.topic || lessonChannel(r.title) ? 2 : r.artist ? 0 : 1);
    return (rows || []).map((r, i) => [tier(r), i, r]).sort((a, b) => a[0] - b[0] || a[1] - b[1]).map((x) => x[2]);
  };

  // §2.5: what is left of a title once the channel, the bracketed parts and the words about the video are gone.
  const PIECE_WORDS = ["piano", "pianist", "cover", "version", "ver", "arrangement", "arr", "solo", "sheet music",
    "official", "live", "performance", "played", "plays", "by", "synthesia", "tutorial", "ost", "full", "hd", "4k",
    "relaxing"];
  const NOT_PIECE = new RegExp("(?<![\\p{L}\\p{N}])(?:" + PIECE_WORDS.map((w) => w.replace(" ", "\\s+")).join("|") +
    ")(?![\\p{L}\\p{N}])|ピアノ", "giu");
  const BRACKETS = /\([^)]*\)|\[[^\]]*\]|【[^】]*】|\{[^}]*\}/g;

  const pieceQuery = (title, channel = "") => {
    let t = String(title ?? "").normalize("NFKC").replace(/[‘’]/g, "'");
    const name = String(channel ?? "").normalize("NFKC").trim();
    if (name) t = t.replace(new RegExp(literal(name), "giu"), " ");
    const words = (part) => part.split(" / ")[0].replace(NOT_PIECE, " ").replace(/[^\p{L}\p{M}\p{N}\s.']/gu, " ")
      .split(/\s+/).filter((w) => /[\p{L}\p{N}]/u.test(w));
    const piece = t.replace(BRACKETS, " ").split("|").map(words).find((w) => w.length) || [];
    const q = piece.slice(0, 8).join(" ");
    return (q.match(/\p{L}/gu) || []).length < 3 ? "" : q + " piano";
  };

  const mergeSimilar = (seed, mix = [], search = [], shown = []) => {
    const s = seed || {};
    const keyOf = (r) => r.channelId || (r.channel ? "name:" + r.channel : "");
    // Row 0 of the Mix is the seed, with its channel when the seed came without one.
    const own = (mix || []).find((r) => r && r.id === s.id) || {};
    const skip = new Set([...BLOCK, ...(shown || []).filter(Boolean).map(keyOf)]);
    const name = s.channel || own.channel;
    for (const key of [s.channelId || own.channelId, name && "name:" + name]) {
      if (key) skip.add(key);
    }
    const order = [];
    const ids = new Set([s.id]);
    for (let i = 0; i < Math.max((mix || []).length, (search || []).length); i++) {
      for (const r of [(mix || [])[i], (search || [])[i]]) {
        if (r && r.kind === "video" && !ids.has(r.id) && !skip.has(keyOf(r)) && !TOPIC.test(r.channel || "")) {
          ids.add(r.id);
          order.push(r);
        }
      }
    }
    const best = (p) => p.rows.reduce((a, b) =>
      (isRecording(b) - isRecording(a) || b.artist - a.artist) > 0 ? b : a);
    return ranked(channelsOf(order, "", keyOf)).slice(0, 20).map(best);
  };

  // ---- folders

  // Windows' device names, which a folder name cannot start with, with or without an extension; COM and LPT take a
  // superscript 1, 2 or 3 too.
  const RESERVED = /^(?:CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9¹²³]|LPT[1-9¹²³])$/;
  const NOT_IN_NAMES = /[<>:"/\\|?*\p{Cc}\p{Cf}\p{Cs}]/u;
  const reserved = (name) => RESERVED.test(name.split(".")[0].replace(/ +$/, "").toUpperCase());
  const first80 = (s) => [...s].slice(0, 80).join("").replace(/[ .]+$/, "");

  const folderName = (name) => {
    let s = String(name || "").replace(new RegExp(NOT_IN_NAMES.source, "gu"), "").replace(/\s+/g, " ");
    s = first80(s.replace(/^[ .]+|[ .]+$/g, ""));
    if (reserved(s)) {
      const dot = s.indexOf(".");
      s = first80(dot < 0 ? s + "_" : s.slice(0, dot) + "_" + s.slice(dot));
    }
    return s || "YouTube";
  };

  const validSegment = (name) =>
    name.length >= 1 && name.length <= 255 && name !== "." && name !== ".." && !name.startsWith(" ") &&
    !/[ .]$/.test(name) && !NOT_IN_NAMES.test(name) && !reserved(name);

  const validDest = (dest) => {
    if (typeof dest !== "string") return false;
    const names = dest ? dest.split("/") : [];
    return names.length <= 4 && names.every(validSegment);
  };

  globalThis.QMParse = {
    CLIENT_VERSION, PIANO_WORDS, LESSON, GENRES, GENRE_WORDS, FORM_WORDS, CURATED, BLOCK, LABEL,
    clientFrom, api, headers, searchBody, moreBody, browseBody, channelBody, channelSearchBody, playlistBody, nextBody,
    mixBody, playerBody, resolveBody, accountBody, signedHeaders, searchParams, suggestUrl, resultsUrl, stsFrom,
    parseSearch, parseMore, parseChannel, parsePlaylist, parseNext, parseMix, parsePlayer, parseAccount,
    parseResolve, parseSuggest, initialData,
    routeFor, loadRules, isOther, hasPiano, searchQuery, long, counted, count, isRecording, lessonChannel,
    inGenre, genresOf, inScope, curatedFor, avatarUrl, genrePianists, homeCurated,
    pianists, rankChannels, pieceQuery, mergeSimilar, folderName, validDest,
  };
})();
