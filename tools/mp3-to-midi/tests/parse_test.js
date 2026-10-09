// parse_test.js: QMParse on the recorded fixtures (live_test.js with QM_RECORD=1 writes them) and on
// small made-up responses for the shapes YouTube did not return that day.
//   deno test --allow-read tools\mp3-to-midi\tests
import "../page/parse.js";

const P = globalThis.QMParse;
const FIXTURES = new URL("./fixtures/", import.meta.url);
const read = (name) => Deno.readTextFileSync(new URL(name, FIXTURES));
const fixture = (name) => JSON.parse(read(name));
const M = fixture("manifest.json");

const VIDEO = /^[A-Za-z0-9_-]{11}$/;
const UCID = /^UC[A-Za-z0-9_-]{22}$/;
const OVERLAYS = [null, "Private video", "Removed", "Live", "Upcoming", "Not available", "Age-restricted"];
const client = { clientName: "WEB", clientVersion: P.CLIENT_VERSION, hl: "en", gl: "US", visitorData: "CgtGSVhUVVJFLU9OTFkoAA%3D%3D" };

function check(cond, msg) {
  if (!cond) throw new Error(msg);
}
const same = (a, b, msg) => check(JSON.stringify(a) === JSON.stringify(b), msg + ": " + JSON.stringify(a) + " != " + JSON.stringify(b));
const noDrift = (res, name) => same(res.drift, [], name + " drift");

// Every row has the shape app.js reads.
function shaped(row, name) {
  const where = name + " " + JSON.stringify(row).slice(0, 160);
  if (row.kind === "video") {
    check(VIDEO.test(row.id) && row.title && row.thumb === "https://i.ytimg.com/vi/" + row.id + "/mqdefault.jpg", where);
    check(row.seconds === null || (Number.isInteger(row.seconds) && row.seconds > 0), where + " seconds");
    check(Array.isArray(row.chapters) && Array.isArray(row.badges), where + " lists");
    check(typeof row.live === "boolean" && typeof row.upcoming === "boolean", where + " live");
    check(typeof row.members === "boolean", where + " members");
    check([true, false, null].includes(row.playable) && OVERLAYS.includes(row.overlay), where + " playable");
    check(row.channelId === "" || UCID.test(row.channelId), where + " channelId");
    check(row.avatar === "" || row.avatar.startsWith("https://"), where + " avatar");
    check(typeof row.artist === "boolean", where + " artist");
  } else if (row.kind === "playlist" || row.kind === "mix") {
    check(row.id && row.title && typeof row.thumb === "string", where);
    check(row.kind === "mix" ? row.count === null : row.count === null || row.count > 0, where + " count");
    check(row.videoId === "" || VIDEO.test(row.videoId), where + " videoId");
  } else {
    check(row.kind === "channel" && UCID.test(row.id) && row.title, where);
    check(typeof row.artist === "boolean" && typeof row.topic === "boolean", where + " artist and topic");
  }
}
function list(res, name, { min = 1, kinds = null } = {}) {
  check(res.known, name + " not known");
  check(res.rows.length >= min, name + " has " + res.rows.length + " rows");
  for (const r of res.rows) {
    shaped(r, name);
    if (kinds) check(kinds.includes(r.kind), name + " kind " + r.kind);
  }
  noDrift(res, name);
  return res;
}

// ---- client context and requests

Deno.test("sw.js_data: the client context", () => {
  const { client: c, drift } = P.clientFrom(read("sw.txt"), "de-AT");
  same(drift, [], "drift");
  same([c.clientName, c.clientVersion, c.hl, c.gl], ["WEB", M.clientVersion, "en", "US"], "client");
  check(c.visitorData.startsWith("Cg") && c.visitorData.length >= 20, "visitorData " + c.visitorData);
});

Deno.test("sw.js_data: found by shape, so a shuffled copy gives the same client", () => {
  const data = JSON.parse(read("sw.txt").replace(/^\)\]\}'\s*/, ""));
  const inner = data[0][2][0][0];
  const want = P.clientFrom(read("sw.txt")).client;
  data[0][2][0][0] = [null, [...inner].reverse()];
  const got = P.clientFrom(")]}'\n" + JSON.stringify(data));
  same(got.drift, [], "drift");
  same(got.client, want, "client");
});

Deno.test("sw.js_data: a version with an experiment label", () => {
  const text = read("sw.txt").replace('"' + M.clientVersion + '"', '"' + M.clientVersion + '-canary_control_2.20261007.01.00"');
  check(text !== read("sw.txt"), "no version to label");
  const got = P.clientFrom(text);
  same([got.client.clientVersion, got.drift], [M.clientVersion, []], "version");
});

Deno.test("sw.js_data: missing fields fall back and show in drift", () => {
  const data = JSON.parse(read("sw.txt").replace(/^\)\]\}'\s*/, ""));
  const inner = data[0][2][0][0];
  data[0][2][0][0] = inner.map((x) => (typeof x === "string" && (x.startsWith("Cg") || /^[A-Z]{2}$/.test(x)) ? null : x));
  const got = P.clientFrom(JSON.stringify(data), "de-AT");
  same(got.drift, ["sw.js_data gl not found", "sw.js_data visitorData not found"], "drift");
  same([got.client.hl, got.client.gl, got.client.visitorData], ["en", "AT", ""], "fallbacks");

  const none = P.clientFrom("<html>not json</html>", "pt-BR");
  same(none.client, { clientName: "WEB", clientVersion: P.CLIENT_VERSION, hl: "pt-BR", gl: "BR", visitorData: "" }, "no sw.js_data");
  check(none.drift.length === 4, "drift " + none.drift);
  same(P.clientFrom("", "").client.hl + P.clientFrom("", "").client.gl, "enUS", "no language");
});

Deno.test("request bodies and headers", () => {
  same(P.api("search"), "/youtubei/v1/search?prettyPrint=false", "api");
  same(P.headers(client)["X-Goog-Visitor-Id"], client.visitorData, "visitor header");
  check(!("X-Goog-Visitor-Id" in P.headers({ ...client, visitorData: "" })), "no empty visitor header");
  same(P.headers(client)["X-Youtube-Client-Name"], "1", "client name header");
  same(P.searchBody(client, "chopin", "EgIQAQ=="), { context: { client }, query: "chopin", params: "EgIQAQ==" }, "search");
  check(!("params" in P.searchBody(client, "chopin")), "search without params");
  same(P.moreBody(client, "tok"), { context: { client }, continuation: "tok" }, "more");
  same(P.channelBody(client, M.channel, "playlists").params, "EglwbGF5bGlzdHPyBgoKCEIGCgIQaCIA", "channel playlists tab");
  same(P.channelBody(client, M.channel).params, "EgZ2aWRlb3PyBgQKAjoA", "channel videos tab");
  same(P.browseBody(client, M.channel, "EgZ2aWRlb3PyBgQKAjoA"), P.channelBody(client, M.channel), "browse");
  same(P.browseBody(client, "VL" + M.playlist), { context: { client }, browseId: "VL" + M.playlist }, "browse without params");
  same(P.playlistBody(client, M.playlist).browseId, "VL" + M.playlist, "playlist");
  same(P.nextBody(client, M.ok).videoId, M.ok, "next");
  same(P.mixBody(client, M.chapters), { context: { client }, videoId: M.chapters, playlistId: "RD" + M.chapters }, "mix");
  const more = P.mixBody(client, M.chapters, "t_Kd_G7p6ZQ", 24);
  same([more.videoId, more.playlistId, more.playlistIndex], ["t_Kd_G7p6ZQ", "RD" + M.chapters, 24], "mix next");
  const player = P.playerBody({ ...client, hl: "de" }, M.ok, "20728");
  same(player.context.client.hl, "en", "player asks in English");
  same(player.playbackContext, { contentPlaybackContext: { signatureTimestamp: 20728 } }, "player sts");
  check(!("playbackContext" in P.playerBody(client, M.ok)), "player without sts");
  same(P.resolveBody(client, M.handle).url, M.handle, "resolve");
  same(P.accountBody(client), { context: { client } }, "account");
  same(P.resultsUrl("a b&c", "EgIQAQ=="), "/results?search_query=a%20b%26c&sp=EgIQAQ%3D%3D", "results url");
  const s = new URL(P.suggestUrl(client, "chopin noc"));
  same([s.searchParams.get("q"), s.searchParams.get("hl"), s.searchParams.get("gl")], ["chopin noc", "en", "US"], "suggest url");
});

Deno.test("searchParams bytes", () => {
  same(P.searchParams({ type: "videos" }), "EgIQAQ==", "videos");
  same(P.searchParams({ type: "videos", duration: "medium" }), "EgQQARgF", "videos 3-20");
  same(P.searchParams({ type: "videos", popular: true }), "CAMSAhAB", "popular videos");
  same(P.searchParams({ type: "videos", duration: "medium", popular: true }), "CAMSBBABGAU=", "popular videos 3-20");
  same(P.searchParams({ type: "videos", duration: "short" }), "EgQQARgE", "videos under 4");
  same(P.searchParams({ type: "videos", duration: "long" }), "EgQQARgC", "videos over 20");
  same(P.searchParams({ type: "playlists" }), "EgIQAw==", "playlists");
  same(P.searchParams({ type: "channels" }), "EgIQAg==", "channels");
  same(P.searchParams({ type: "playlists", duration: "medium" }), "EgIQAw==", "duration is for videos only");
  same(P.searchParams({ type: 1, duration: 5 }), "EgQQARgF", "numbers");
  same(P.searchParams(), "EgIQAQ==", "default");
  same(P.searchParams({ type: "videos", duration: "medium", upload: "week" }), "EgYIAxABGAU=", "This week 3-20");
  same(P.searchParams({ type: "videos", duration: "medium", upload: "month" }), "EgYIBBABGAU=", "This month 3-20");
  same(P.searchParams({ type: "videos", duration: "medium", popular: true, upload: "any" }), "CAMSBBABGAU=", "popular 3-20");
  same(["hour", "today", "year"].map((upload) => P.searchParams({ upload })), ["EgQIARAB", "EgQIAhAB", "EgQIBRAB"], "upload dates");
  same(P.searchParams({ duration: "medium", popular: true, upload: "week" }), "CAMSBggDEAEYBQ==", "popular this week");
  same(P.searchParams({ upload: 3, duration: 5 }), P.searchParams({ upload: "week", duration: "medium" }), "upload as a number");
  same(P.searchParams({ upload: "decade" }), "EgIQAQ==", "unknown upload is any");
});

Deno.test("signature timestamp", () => {
  same(P.stsFrom(read("results.html")), M.sts, "results.html");
  same(P.stsFrom('{"signatureTimestamp":19876}'), 19876, "player config");
  same(P.stsFrom("signatureTimestamp: 19877"), 19877, "player script");
  same(P.stsFrom(""), null, "none");
});

Deno.test("signed headers", async () => {
  const h = await P.signedHeaders("PREF=f6=40; __Secure-3PAPISID=other; SAPISID=abcDEF123/xyz; SID=x", 1700000000000);
  same(h, {
    "Authorization": "SAPISIDHASH 1700000000_58ffb74adab4bbd2a6ed5813e9fcf5c319254fcb",
    "X-Goog-AuthUser": "0",
    "X-Origin": "https://www.youtube.com",
  }, "SAPISIDHASH");
  same(await P.signedHeaders("__Secure-3PAPISID=other"), null, "no SAPISID");
  same(await P.signedHeaders(""), null, "no cookie");
});

// ---- search

Deno.test("search: videos and a Mix, with ads and Shorts dropped", () => {
  const res = list(P.parseSearch(fixture("search.json")), "search", { kinds: ["video", "playlist", "mix"] });
  check(res.rows.some((r) => r.kind === "video" && r.seconds > 0 && r.channelId && r.views), "a full video row");
  if (M.ads) check(res.dropped.ads > 0, "ads not dropped");
  if (M.shorts) check(res.dropped.shorts > 0, "Shorts not dropped");
  check(res.estimated > 0 && typeof res.more === "string" && res.more.length > 20, "estimated and more");
  same([res.sorts, res.sort], [null, null], "no sorts");
  check(!res.rows.some((r) => r.kind === "mix" && (r.channel || r.count !== null)), "a Mix has no channel or count");
});

Deno.test("search: Videos 3-20 min", () => {
  const res = list(P.parseSearch(fixture("search_videos.json")), "search_videos", { kinds: ["video"] });
  check(res.rows.every((r) => r.seconds >= 180 && r.seconds <= 1260), "durations " + res.rows.map((r) => r.seconds));
  check(res.rows.every((r) => !P.long(r)), "none long");
});

Deno.test("search: next page", () => {
  const res = list(P.parseMore(fixture("search_more.json")), "search_more", { kinds: ["video", "playlist", "mix"] });
  check(res.more, "more");
});

Deno.test("search: live now", () => {
  const res = list(P.parseSearch(fixture("search_live.json")), "search_live", { kinds: ["video"] });
  check(res.rows.every((r) => r.live && r.overlay === "Live" && r.seconds === null), "live rows");
  check(res.rows.some((r) => r.id === M.live), "the recorded live video");
  same(P.count(res.rows), 0, "live rows never count");
});

Deno.test("search: Playlists", () => {
  const res = list(P.parseSearch(fixture("search_playlists.json")), "search_playlists", { kinds: ["playlist", "mix"] });
  const pl = res.rows.filter((r) => r.kind === "playlist");
  check(pl.length && pl.every((r) => r.count > 0 && UCID.test(r.channelId) && r.channel), "playlists");
  check(res.rows.filter((r) => r.kind === "mix").every((r) => r.id.startsWith("RD")), "mixes");
  check(res.rows.every((r) => r.thumb.startsWith("https://")), "thumbs");
});

Deno.test("search: Channels", () => {
  const res = list(P.parseSearch(fixture("search_channels.json")), "search_channels", { kinds: ["channel"] });
  check(res.rows.some((r) => /^@\S+$/.test(r.handle) && /\d/.test(r.subscribers)), "handle and subscribers");
  check(res.rows.every((r) => r.thumb.startsWith("https://") && !r.subscribers.startsWith("@")), "thumbs");
});

Deno.test("results HTML fallback", () => {
  const html = read("results.html");
  const data = P.initialData(html);
  check(data && data.contents, "ytInitialData");
  list(P.parseSearch(data), "results.html", { kinds: ["video", "playlist", "mix"] });
});

Deno.test("initialData: braces in strings, other markers, and junk", () => {
  same(P.initialData('<script>var ytInitialData = {"a":"} {\\"}","b":{"c":[1,"{"]}};var x = {};</script>'),
    { a: '} {"}', b: { c: [1, "{"] } }, "braces in strings");
  same(P.initialData('window["ytInitialData"] = {"k":1};'), { k: 1 }, "window marker");
  same(P.initialData("ytInitialData = {\"k\":2};"), { k: 2 }, "bare marker");
  same(P.initialData("var ytInitialData = null; x = {}"), null, "not an object");
  same(P.initialData('var ytInitialData = {"cut": "off'), null, "cut off");
  same(P.initialData("var ytInitialData = {bad json};"), null, "bad JSON");
  same(P.initialData(""), null, "empty");
});

// ---- channels and playlists

const handle = M.handle.slice(M.handle.lastIndexOf("/") + 1);

Deno.test("channel: videos tab", () => {
  const res = list(P.parseChannel(fixture("channel_videos.json")), "channel_videos", { kinds: ["video"] });
  same([res.id, res.handle.toLowerCase()], [M.channel, handle.toLowerCase()], "header");
  check(res.title && /\d/.test(res.subscribers) && res.avatar.startsWith("https://"), "title, subscribers, avatar");
  check(res.rows.every((r) => r.seconds > 0 && r.channel === "" && r.views && r.age), "own videos: views and age, no channel");
  check(res.sorts.length === 3 && res.sorts.every((t) => typeof t === "string" && t) && res.sort === 0, "Latest, Popular, Oldest");
  check(res.more, "more");
});

Deno.test("channel: next page and Popular", () => {
  const more = list(P.parseMore(fixture("channel_more.json")), "channel_more", { kinds: ["video"] });
  check(more.more, "more");
  const sorted = list(P.parseMore(fixture("channel_sort.json")), "channel_sort", { kinds: ["video"] });
  check(sorted.sorts.length === 3 && sorted.sort === 1 && sorted.more, "Popular selected");
});

Deno.test("channel: playlists tab", () => {
  const res = list(P.parseChannel(fixture("channel_playlists.json")), "channel_playlists", { kinds: ["playlist"] });
  same(res.id, M.channel, "id");
  // A playlist with its own square cover opens by browse, so it has no first video.
  check(res.rows.every((r) => r.count > 0 && r.channel === "" && r.thumb.startsWith("https://")), "own playlists");
  check(res.rows.some((r) => VIDEO.test(r.videoId)), "a first video");
});

Deno.test("playlist", () => {
  const res = list(P.parsePlaylist(fixture("playlist.json")), "playlist", { kinds: ["video"] });
  check(res.title && res.channel && UCID.test(res.channelId), "header " + JSON.stringify([res.title, res.channel]));
  check(res.count >= 100 && /\d/.test(res.countText), "count " + res.countText);
  same(res.rows.map((r) => r.index), res.rows.map((_, i) => i), "indexes");
  check(res.rows.every((r) => r.seconds > 0) && res.more, "durations and more");
});

// Every value under `key`, depth first; the first one changes a recorded response in place.
const allOf = (node, key, out = []) => {
  if (Array.isArray(node)) for (const x of node) allOf(x, key, out);
  else if (node && typeof node === "object") {
    for (const [k, v] of Object.entries(node)) {
      if (k === key) out.push(v);
      allOf(v, key, out);
    }
  }
  return out;
};
const firstOf = (node, key) => allOf(node, key)[0] || null;

Deno.test("playlist: the owner, without the header's \"by\"", () => {
  same(P.parsePlaylist(fixture("playlist.json")).channel, "Kassia", "as recorded");
  const owned = (text) => {
    const json = fixture("playlist.json");
    firstOf(json.header, "avatarStackViewModel").text = text;
    return P.parsePlaylist(json);
  };
  const link = { browseEndpoint: { browseId: "UCPmCaKjzYF3pXYLfaRhacwA" } };
  const linked = owned({ content: "by Lauren S", commandRuns: [{ startIndex: 3, length: 8, onTap: { innertubeCommand: link } }] });
  same([linked.channel, linked.channelId], ["Lauren S", "UCPmCaKjzYF3pXYLfaRhacwA"], "the linked run");
  same(owned({ content: "by Lauren S" }).channel, "Lauren S", "no runs");
});

Deno.test("playlist: members-only videos are marked and never count", () => {
  const raw = fixture("playlist_members.json");
  const res = list(P.parsePlaylist(raw), "playlist_members", { kinds: ["video"] });
  const badged = new Set(allOf(raw, "lockupViewModel").filter((l) => /MEMBERS/.test(JSON.stringify(l))).map((l) => l.contentId));
  same(res.rows.map((r) => r.members), res.rows.map((r) => badged.has(r.id)), "members, as badged");
  const members = res.rows.filter((r) => r.members);
  check(members.length >= 3 && members.length < res.rows.length, "members " + members.length + " of " + res.rows.length);
  same(P.count(members), 0, "members-only rows counted");
  check(members.every((r) => P.counted({ ...r, members: false })), "only being members-only stops them counting");
  same(P.count(res.rows), P.count(res.rows.filter((r) => !r.members)), "the public lesson still counts");
});

Deno.test("members only: a MEMBERS badge style on a search row or a lockup", () => {
  const lockup = (contentId, badgeStyle) => ({ lockupViewModel: { contentId, contentType: "LOCKUP_CONTENT_TYPE_VIDEO",
    metadata: { lockupMetadataViewModel: { title: { content: "Etude piano" }, metadata: { contentMetadataViewModel: { metadataRows: [
      { metadataParts: [{ text: { content: "1 day ago" } }] },
      { badges: [{ badgeViewModel: { badgeText: "Badge", badgeStyle } }] },
    ] } } } } } });
  const res = P.parseSearch(searchOf([
    { videoRenderer: { videoId: "dQw4w9WgXcQ", title: { runs: [{ text: "Nocturne piano" }] },
      badges: [{ metadataBadgeRenderer: { style: "BADGE_STYLE_TYPE_MEMBERS_ONLY", label: "Members only" } }] } },
    { videoRenderer: { videoId: "aaaaaaaaaaa", title: { runs: [{ text: "Nocturne piano" }] },
      badges: [{ metadataBadgeRenderer: { style: "BADGE_STYLE_TYPE_SIMPLE", label: "New" } }] } },
    lockup("bbbbbbbbbbb", "BADGE_MEMBERS_ONLY"),
    lockup("ccccccccccc", "BADGE_DEFAULT"),
  ]));
  noDrift(res, "members search");
  same(res.rows.map((r) => [r.id, r.members]), [["dQw4w9WgXcQ", true], ["aaaaaaaaaaa", false], ["bbbbbbbbbbb", true],
    ["ccccccccccc", false]], "members");
  same(res.rows.map((r) => P.counted(r)), [false, true, false, true], "counted");
  same(P.count(res.rows), 2, "count");
});

Deno.test("playlist: the last page is only the shelf of other playlists", () => {
  const res = P.parseMore(fixture("playlist_end.json"));
  same([res.known, res.rows.length, res.more], [true, M.endRows, null], "end");
  noDrift(res, "playlist_end");
});

Deno.test("playlist: a channel's uploads, and the next page", () => {
  const res = list(P.parsePlaylist(fixture("playlist_uploads.json")), "playlist_uploads", { kinds: ["video"] });
  same(res.channelId, M.channel, "owner");
  check(res.count > 1000 && res.more, "count and more");
  if (M.station) check(res.rows.some((r) => r.live && r.overlay === "Live" && !P.counted(r)), "the live station");
  const more = list(P.parseMore(fixture("playlist_more.json")), "playlist_more", { kinds: ["video"] });
  check(more.rows.every((r) => r.index >= 99 && r.seconds > 0) && more.more, "second page");
});

// ---- watch

Deno.test("next: a video with chapters and related videos", () => {
  const res = list(P.parseNext(fixture("next_video.json")), "next_video", { kinds: ["video"] });
  const v = res.video;
  check(v && v.id === M.chapters && v.title && UCID.test(v.channelId) && v.views, "video " + JSON.stringify(v));
  same(v.seconds, null, "next carries no duration");
  check(res.chapters.length >= 3 && res.chapters[0].seconds === 0, "chapters " + res.chapters.length);
  check(res.chapters.every((c, i) => c.title && (i === 0 || c.seconds > res.chapters[i - 1].seconds)), "chapters in order, once each");
  same(v.chapters, res.chapters, "video chapters");
  check(P.long(v) && !P.counted(v), "chapters make it long");
  check(res.more, "related more");
});

Deno.test("next: a Mix", () => {
  const res = list(P.parseMix(fixture("next_mix.json")), "next_mix", { min: 2, kinds: ["video"] });
  same(res.id, "RD" + M.chapters, "id");
  check(res.title.startsWith("Mix"), "title " + res.title);
  same(res.rows[0].id, M.chapters, "starts at the seed");
  same(res.rows.map((r) => r.index), res.rows.map((_, i) => i), "indexes");
  const last = res.rows[res.rows.length - 1];
  same(res.last, { id: last.id, index: last.index }, "last");
  check(res.rows.every((r) => r.seconds > 0), "durations");
});

Deno.test("player", () => {
  const p = (name) => {
    const res = P.parsePlayer(fixture(name));
    noDrift(res, name);
    return res;
  };
  const okv = p("player_ok.json");
  same([okv.id, okv.status, okv.playable, okv.overlay, okv.stale], [M.ok, "OK", true, null, false], "OK");
  check(okv.seconds > 0 && okv.title && UCID.test(okv.channelId), "OK details");
  const stale = p("player_stale.json");
  same([stale.status, stale.playable, stale.overlay, stale.stale], ["UNPLAYABLE", null, null, true], "stale");
  const priv = p("player_private.json");
  same([priv.status, priv.playable, priv.overlay], ["LOGIN_REQUIRED", false, "Private video"], "private");
  const age = p("player_age.json");
  same([age.id, age.playable, age.overlay], [M.age, false, "Age-restricted"], "age");
  const removed = p("player_removed.json");
  same([removed.status, removed.playable, removed.overlay], ["ERROR", false, "Removed"], "removed");
  const live = p("player_live.json");
  same([live.id, live.playable, live.live, live.overlay], [M.live, true, true, "Live"], "live");
});

Deno.test("player: made-up statuses", () => {
  const p = (ps, extra = {}) => P.parsePlayer({ playabilityStatus: ps, ...extra });
  same(p({ status: "UNPLAYABLE", reason: "Video unavailable" }).overlay, "Not available", "unplayable");
  same(p({ status: "LOGIN_REQUIRED", reason: "Sign in to confirm you're not a bot" }).playable, null, "a bot check says nothing");
  same(p({ status: "LOGIN_REQUIRED", reason: "Sign in to view this page" }).overlay, null, "page is not age");
  same(p({ status: "AGE_CHECK_REQUIRED" }).overlay, "Age-restricted", "age check");
  same(p({ status: "ERROR", reason: "This video is private" }).overlay, "Private video", "private error");
  const offline = p({ status: "LIVE_STREAM_OFFLINE" }, { videoDetails: { videoId: M.ok, isUpcoming: true } });
  same([offline.playable, offline.upcoming, offline.overlay], [false, true, "Upcoming"], "upcoming");
  same(p({ status: "CONTENT_CHECK_REQUIRED" }).drift, [], "content check");
  same(p({ status: "SOMETHING_NEW" }).drift, ["player status SOMETHING_NEW"], "new status");
  same(P.parsePlayer({}).drift, ["player without playabilityStatus"], "empty");
  same(P.parsePlayer(null).playable, null, "null");
});

// ---- account, resolve, suggestions

Deno.test("account, resolve and suggestions", () => {
  same(P.parseAccount(fixture("account.json")), null, "signed out");
  same(P.parseAccount({
    actions: [{ openPopupAction: { popup: { multiPageMenuRenderer: { header: { activeAccountHeaderRenderer: {
      accountName: { simpleText: "Kai" },
      channelHandle: { simpleText: "@kai" },
      accountPhoto: { thumbnails: [{ url: "//yt3.ggpht.com/a=s48" }, { url: "//yt3.ggpht.com/a=s88" }] },
    } } } } } }],
  }), { name: "Kai", handle: "@kai", avatar: "https://yt3.ggpht.com/a=s88" }, "signed in");
  same(P.parseAccount(null), null, "null");
  same(P.parseResolve(fixture("resolve.json")), M.channel, "resolve");
  same(P.parseResolve({}), null, "resolve nothing");
  const words = P.parseSuggest(read("suggest.txt"));
  check(words.length >= 1 && words.every((w) => typeof w === "string" && w.startsWith(M.suggest.split(" ")[0])), "suggestions " + words);
  same(P.parseSuggest(")]}'\n[\"q\",[[\"a\",0],\"b\"]]"), ["a", "b"], "suggest shapes");
  same(P.parseSuggest("<html>"), [], "suggest junk");
});

// ---- made-up shapes

const searchOf = (items) => ({
  contents: { twoColumnSearchResultsRenderer: { primaryContents: { sectionListRenderer: { contents: [
    { itemSectionRenderer: { contents: items } },
  ] } } } },
});
const byline = (name, id) => ({ runs: [{ text: name, navigationEndpoint: { browseEndpoint: { browseId: id } } }] });
const CH = "UC" + "a".repeat(22);

Deno.test("videoRenderer: live, upcoming, Shorts and chapters", () => {
  const res = P.parseSearch(searchOf([
    { videoRenderer: { videoId: "aaaaaaaaaaa", title: { runs: [{ text: " Nocturne " }] }, ownerText: byline("Ann ", CH),
      lengthText: { simpleText: "1:02:03" }, viewCountText: { simpleText: "5 views" }, publishedTimeText: { simpleText: "1 day ago" },
      badges: [{ metadataBadgeRenderer: { label: "4K" } }],
      expandableMetadata: { expandableMetadataRenderer: { expandedContent: { horizontalCardListRenderer: { cards: [
        { macroMarkersListItemRenderer: { title: { simpleText: "Intro" }, timeDescription: { simpleText: "0:00" } } },
        { macroMarkersListItemRenderer: { title: { simpleText: "Main" }, onTap: { watchEndpoint: { startTimeSeconds: 95 } } } },
        { macroMarkersListItemRenderer: { title: { simpleText: "Main again" }, timeDescription: { simpleText: "1:35" } } },
      ] } } } } } },
    { videoRenderer: { videoId: "bbbbbbbbbbb", title: { simpleText: "Live" },
      badges: [{ metadataBadgeRenderer: { style: "BADGE_STYLE_TYPE_LIVE_NOW", label: "LIVE" } }] } },
    { videoRenderer: { videoId: "ccccccccccc", title: { simpleText: "Soon" }, upcomingEventData: { startTime: "1800000000" } } },
    { videoRenderer: { videoId: "ddddddddddd", title: { simpleText: "Short" }, navigationEndpoint: { reelWatchEndpoint: {} } } },
    { videoRenderer: { videoId: "eeeeeeeeeee", title: { simpleText: "Short too" },
      thumbnailOverlays: [{ thumbnailOverlayTimeStatusRenderer: { style: "SHORTS" } }] } },
    { videoRenderer: { videoId: "aaaaaaaaaaa", title: { simpleText: "Again" } } },
    { reelShelfRenderer: {} },
    { promotedSparklesWebRenderer: {} },
    { shelfRenderer: { content: { verticalListRenderer: { items: [
      { videoRenderer: { videoId: "fffffffffff", title: { simpleText: "In a shelf" },
        thumbnailOverlays: [{ thumbnailOverlayTimeStatusRenderer: { text: { simpleText: "3:00" } } }] } },
    ] } } } },
    { continuationItemRenderer: { continuationEndpoint: { continuationCommand: { token: "NEXT" } } } },
  ]));
  noDrift(res, "made-up search");
  same(res.rows.map((r) => r.id), ["aaaaaaaaaaa", "bbbbbbbbbbb", "ccccccccccc", "fffffffffff"], "ids, deduplicated");
  const [a, live, soon, shelf] = res.rows;
  same([a.title, a.channel, a.channelId, a.seconds, a.views, a.age, a.badges], ["Nocturne", "Ann", CH, 3723, "5 views", "1 day ago", ["4K"]], "full row");
  same(a.chapters, [{ title: "Intro", seconds: 0 }, { title: "Main", seconds: 95 }], "chapters once per start");
  same([live.live, live.overlay, live.badges], [true, "Live", []], "live");
  same([soon.upcoming, soon.overlay], [true, "Upcoming"], "upcoming");
  same(shelf.seconds, 180, "duration from the overlay");
  same([res.dropped.shorts, res.dropped.ads, res.more], [3, 1, "NEXT"], "dropped and more");
});

Deno.test("videoRenderer: the channel's picture and a verified artist", () => {
  const picture = (url) => ({ channelThumbnailWithLinkRenderer: { thumbnail: { thumbnails: [{ url, width: 68 }, { url: url + "x" }] } } });
  const badge = (style) => ({ metadataBadgeRenderer: { style } });
  const res = P.parseSearch(searchOf([
    { videoRenderer: { videoId: "aaaaaaaaaaa", title: { simpleText: "A" }, ownerText: byline("Kassia", CH),
      channelThumbnailSupportedRenderers: picture("//yt3.ggpht.com/a=s68"),
      ownerBadges: [badge("BADGE_STYLE_TYPE_VERIFIED"), badge("BADGE_STYLE_TYPE_VERIFIED_ARTIST")] } },
    { videoRenderer: { videoId: "bbbbbbbbbbb", title: { simpleText: "B" }, channelThumbnailSupportedRenderers: picture("https://yt3.ggpht.com/b"),
      ownerBadges: [badge("BADGE_STYLE_TYPE_VERIFIED")] } },
    { videoRenderer: { videoId: "ccccccccccc", title: { simpleText: "C" } } },
  ]));
  noDrift(res, "made-up pictures");
  same(res.rows.map((r) => [r.avatar, r.artist]), [["https://yt3.ggpht.com/a=s68", true], ["https://yt3.ggpht.com/b", false], ["", false]], "rows");
});

// Channels with a handle show it where the subscriber count was, and the count where the video count was.
const channelOf = (id, title, { handle = "", count = "1.2K subscribers", badge = "" } = {}) => ({ channelRenderer: {
  channelId: id, title: { simpleText: title },
  navigationEndpoint: { browseEndpoint: { browseId: id, canonicalBaseUrl: handle ? "/" + handle : "/channel/" + id } },
  subscriberCountText: { simpleText: handle || count }, ...(handle ? { videoCountText: { simpleText: count } } : {}),
  ...(badge ? { ownerBadges: [{ metadataBadgeRenderer: { style: badge } }] } : {}),
} });
const ucid = (c) => "UC" + c.repeat(22);

Deno.test("channelRenderer: artist and Topic, and rankChannels", () => {
  const res = P.parseSearch(searchOf([
    channelOf(ucid("a"), "Kassia", { handle: "@Kassia", badge: "BADGE_STYLE_TYPE_VERIFIED_ARTIST" }),
    channelOf(ucid("b"), "Frédéric Chopin", { count: "52 k abonnés" }),
    channelOf(ucid("c"), "Rousseau", { handle: "@Rousseau", badge: "BADGE_STYLE_TYPE_VERIFIED" }),
    channelOf(ucid("d"), "Release - Topic", { handle: "@release" }),
    channelOf(ucid("e"), "Piano Academy", { handle: "@pianoacademy" }),
    { videoRenderer: { videoId: "aaaaaaaaaaa", title: { simpleText: "Nocturne" } } },
  ]));
  noDrift(res, "made-up channels");
  const name = (r) => (r.kind === "channel" ? r.title : r.id);
  same(res.rows.map((r) => (r.kind === "channel" ? [r.title, r.artist, r.topic] : r.id)), [["Kassia", true, false],
    ["Frédéric Chopin", false, true], ["Rousseau", false, false], ["Release - Topic", false, true], ["Piano Academy", false, false],
    "aaaaaaaaaaa"], "artist and topic");
  same(P.rankChannels(res.rows).map(name), ["Kassia", "Rousseau", "aaaaaaaaaaa", "Frédéric Chopin", "Release - Topic", "Piano Academy"],
    "artists first; Topic and lesson channels last");
  same(name(res.rows[1]), "Frédéric Chopin", "the rows are left as they were");
  same(P.rankChannels(null), [], "nothing");
});

Deno.test("next: a Mix reads only its playlist panel", () => {
  const panel = (videoId, index) => ({ playlistPanelVideoRenderer: { videoId, title: { simpleText: "Nocturne" },
    lengthText: { simpleText: "4:00" }, shortBylineText: byline("Ann", CH), navigationEndpoint: { watchEndpoint: { videoId, index } } } });
  const res = P.parseMix({ contents: { twoColumnWatchNextResults: {
    playlist: { playlist: { playlistId: "RDaaaaaaaaaaa", title: { simpleText: "Mix - Ann" },
      contents: [panel("aaaaaaaaaaa", 0), panel("bbbbbbbbbbb", 1)] } },
    secondaryResults: { secondaryResults: { results: [panel("ccccccccccc", 0)] } },
  } } });
  noDrift(res, "made-up Mix");
  same(res.rows.map((r) => [r.id, r.index, r.channelId]), [["aaaaaaaaaaa", 0, CH], ["bbbbbbbbbbb", 1, CH]], "rows");
  same([res.id, res.title, res.last], ["RDaaaaaaaaaaa", "Mix - Ann", { id: "bbbbbbbbbbb", index: 1 }], "header");
});

Deno.test("the old playlist shape: private, deleted and unplayable", () => {
  const item = (videoId, title, extra = {}) => ({ playlistVideoRenderer: { videoId, title: { runs: [{ text: title }] }, ...extra } });
  const res = P.parseMore({ onResponseReceivedActions: [{ appendContinuationItemsAction: { continuationItems: [
    item("aaaaaaaaaaa", "Waltz", { lengthSeconds: "216", shortBylineText: byline("Kassia", CH), isPlayable: true }),
    item("bbbbbbbbbbb", "[Private video]", { isPlayable: false }),
    item("ccccccccccc", "[Deleted video]", { isPlayable: false }),
    item("ddddddddddd", "Blocked here", { isPlayable: false }),
    item("eeeeeeeeeee", "Live", { thumbnailOverlays: [{ thumbnailOverlayTimeStatusRenderer: { style: "LIVE" } }] }),
  ] } }] });
  noDrift(res, "old playlist");
  same(res.rows.map((r) => [r.playable, r.overlay]), [[null, null], [false, "Private video"], [false, "Removed"],
    [false, "Not available"], [null, "Live"]], "rows");
  same([res.rows[0].seconds, res.rows[0].channelId], [216, CH], "first row");
  same(P.count(res.rows), 1, "only the playable video counts");
});

Deno.test("chips: the old chip bar", () => {
  const res = P.parseChannel({
    contents: { twoColumnBrowseResultsRenderer: { tabs: [{ tabRenderer: { selected: true, content: { richGridRenderer: {
      header: { feedFilterChipBarRenderer: { contents: [
        { chipCloudChipRenderer: { navigationEndpoint: { continuationCommand: { token: "L" } } } },
        { chipCloudChipRenderer: { isSelected: true, navigationEndpoint: { continuationCommand: { token: "P" } } } },
        { chipCloudChipRenderer: { navigationEndpoint: { continuationCommand: { token: "O" } } } },
      ] } },
      contents: [],
    } } } }] } },
  });
  same([res.sorts, res.sort], [["L", "P", "O"], 1], "sorts");
});

Deno.test("drift is reported, never thrown", () => {
  const res = P.parseSearch(searchOf([
    { fancyNewRenderer: { videoId: "aaaaaaaaaaa" } },
    { lockupViewModel: { contentId: "bbbbbbbbbbb", contentType: "LOCKUP_CONTENT_TYPE_HOLOGRAM",
      metadata: { lockupMetadataViewModel: { title: { content: "x" } } } } },
    { videoRenderer: { title: { simpleText: "no id" } } },
  ]));
  same(res.rows, [], "rows");
  same(res.drift.sort(), ["lockupViewModel LOCKUP_CONTENT_TYPE_HOLOGRAM", "unknown fancyNewRenderer", "videoRenderer without videoId"], "drift");
  for (const parse of [P.parseSearch, P.parseMore, P.parseChannel, P.parsePlaylist, P.parseNext, P.parseMix]) {
    for (const junk of [null, undefined, {}, [], "text", 7, { contents: { twoColumnSearchResultsRenderer: null } }]) {
      const out = parse(junk);
      check(out.known === false && Array.isArray(out.rows) && out.rows.length === 0 && out.drift.length >= 1,
        parse.name + "(" + JSON.stringify(junk) + ")");
    }
  }
});

// ---- links

Deno.test("routeFor: the shared route fixtures", () => {
  const cases = fixture("routes.json");
  check(cases.length >= 40, "routes.json has " + cases.length);
  const wrong = cases.filter((c) => P.routeFor(c.url) !== c.route).map((c) => [c.url, P.routeFor(c.url), c.route]);
  same(wrong, [], "wrong routes");
  for (const bad of ["javascript:alert(1)", "file:///C:/x", "", null, "not a url", "https://www.youtube.com/watch?v=short"]) {
    same(P.routeFor(bad), null, "routeFor " + bad);
  }
});

// ---- counting

Deno.test("long, counted and count", () => {
  const row = (f = {}) => ({ kind: "video", id: "aaaaaaaaaaa", title: "Nocturne", seconds: 300, chapters: [], playable: null,
    live: false, upcoming: false, ...f });
  same([P.long(row({ seconds: 1200 })), P.long(row({ seconds: 1201 }))], [false, true], "long");
  same(P.long(row({ seconds: null })), false, "unknown length");
  // Chapters naming two or more pieces make a compilation; the parts of one piece leave one recording.
  const chapters = (...titles) => row({ chapters: titles.map((title, i) => ({ title, seconds: i * 30 })) });
  for (const titles of [
    ["Nocturne", "Waltz"], ["", ""], ["Opening 1", "Ending 1", "Opening 2"], ["Hedwig's Theme", "Leaving Hogwarts"],
    ["Tutorial", "Left hand", "Right hand", "Both hands"], ["Interstellar", "Inception", "Dark Knight", "Dunkirk"],
    ["Intro", "Why So Serious?", "A Watchful Guardian", "Outro"],
  ]) {
    check(P.long(chapters(...titles)), "long: " + titles.join(" / "));
  }
  for (const titles of [
    [], ["Nocturne"], [""], ["Intro", "Verse 1", "Pre-Chorus", "Chorus", "Verse 2", "Bridge", "Final Chorus", "Outro"],
    ["Intro", "Skyfall", "Chorus", "Bridge", "Outro"], ["Le Festin", "Outro"], ["Theme", "Var. 1", "Var. II", "Coda"],
    ["Intro", "Hedwig's Theme", "Outro"], ["Intro", "Verse", "Chorus", "Guitar Solo", "Outro"],
    ["Part I", "Part 2", "Pt. 3"], ["イントロ", "Aメロ", "Bメロ", "サビ", "落ちサビ", "アウトロ"], ["인트로", "벌스", "후렴", "브릿지"],
    ["前奏", "主歌", "副歌", "间奏", "尾奏"], ["Intro", "Bridge (instrumental)", "Last Chorus"],
  ]) {
    check(!P.long(chapters(...titles)), "one recording: " + titles.join(" / "));
  }
  check(P.counted(row()), "a plain video counts");
  for (const [name, r, state] of [
    ["live", row({ live: true }), {}], ["upcoming", row({ upcoming: true }), {}], ["unplayable", row({ playable: false }), {}],
    ["long", row({ seconds: 4000 }), {}], ["converted", row(), { converted: true }], ["queued", row(), { queue: true }],
    ["a playlist", { ...row(), kind: "playlist" }, {}],
  ]) {
    check(!P.counted(r, "", state), name + " counts");
  }
  P.loadRules(fixture("rules.json"));
  check(!P.counted(row({ title: "Canon - Violin & Piano" }), "canon"), "other instruments do not count");
  check(P.counted(row({ title: "Canon - Violin & Piano" }), "canon violin"), "unless the query asks for them");
  const rows = [row(), row({ id: "bbbbbbbbbbb" }), row({ id: "ccccccccccc", live: true }), row({ id: "ddddddddddd", seconds: 4000 })];
  same(P.count(rows), 2, "count");
  same(P.count(rows, "", (r) => ({ converted: r.id === "aaaaaaaaaaa" })), 1, "count with state");
  same(P.count(null), 0, "count nothing");
});

Deno.test("searchQuery adds piano when asked and missing", () => {
  same(P.searchQuery("chopin nocturne", true), "chopin nocturne piano", "added");
  same(P.searchQuery("  chopin  ", true), "chopin piano", "trimmed");
  same(P.searchQuery("Chopin PIANO", true), "Chopin PIANO", "already there");
  same(P.searchQuery("千本桜 ピアノ", true), "千本桜 ピアノ", "CJK piano");
  same(P.searchQuery("chopin", false), "chopin", "bias off");
  same(P.searchQuery("   ", true), "   ", "blank");
});

Deno.test("isOther is false until the rules load", () => {
  // A fresh copy of parse.js, so the rules other tests loaded are not there.
  const fresh = {};
  new Function("globalThis", Deno.readTextFileSync(new URL("../page/parse.js", import.meta.url)))(fresh);
  same(fresh.QMParse.isOther("Canon - Violin & Piano"), false, "before");
  fresh.QMParse.loadRules(fixture("rules.json"));
  same(fresh.QMParse.isOther("Canon - Violin & Piano"), true, "after");
});

// ---- discovery

const D = fixture("discovery.json");
const RULES = fixture("rules.json");
const verified = (v) => JSON.stringify(v.ownerBadges || []).includes("BADGE_STYLE_TYPE_VERIFIED_ARTIST");
// A made-up video row as the parsers make it: a 5-minute recording unless told otherwise.
const vrow = (id, channelId, channel, f = {}) => ({ kind: "video", id, title: "Nocturne", channel, channelId, avatar: "",
  artist: false, seconds: 300, chapters: [], playable: null, live: false, upcoming: false, members: false, ...f });

Deno.test("GENRES: eight frozen genres, each asking for piano", () => {
  P.loadRules(RULES);
  same(P.GENRES.map((g) => g.key), ["classical", "jazz", "ragtime", "neoclassical", "film", "games", "anime", "pop"], "keys");
  same(P.GENRES.map((g) => g.word), ["Classical", "Jazz", "Ragtime", "Neoclassical", "Film", "Games", "Anime", "Pop"], "pills");
  same(P.GENRES.map((g) => g.query), ["classical piano", "jazz piano solo", "ragtime piano",
    "neoclassical piano solo performance", "film music piano solo", "game ost piano cover", "anime piano cover",
    "pop piano cover"], "the queries the release gate measured");
  check(Object.isFrozen(P.GENRES) && P.GENRES.every((g) => Object.isFrozen(g)), "frozen");
  check(P.GENRES.every((g) => P.hasPiano(g.query)), "a piano word in every query");
  same(P.GENRES.find((g) => g.key === D.genre).query, D.query, "the recorded genre");
  same([D.top, D.week, D.month], ["EgQQARgF", "EgYIAxABGAU=", "EgYIBBABGAU="], "the recorded params");
});

Deno.test("discovery: a genre's Top, its next page and This week", () => {
  for (const [name, parse] of [["discovery_top.json", P.parseSearch], ["discovery_more.json", P.parseMore],
    ["discovery_week.json", P.parseSearch]]) {
    const raw = fixture(name);
    const res = list(parse(raw), name, { min: 8, kinds: ["video"] });
    check(res.more, name + " more");
    check(res.rows.every((r) => /^https:\/\/yt\d\.ggpht\.com\//.test(r.avatar)), name + " channel pictures");
    const artists = allOf(raw, "videoRenderer").filter(verified).map((v) => v.videoId);
    same(res.rows.filter((r) => r.artist).map((r) => r.id), artists, name + " artists");
    check(artists.length > 0 && artists.length < res.rows.length, name + " artists and others");
  }
  const week = P.parseSearch(fixture("discovery_week.json")).rows;
  check(week.every((r) => /\b(minute|hour|day|week)s? ago/.test(r.age)), "This week: " + week.map((r) => r.age));
});

Deno.test("discovery: Channels, verified artists first", () => {
  const raw = fixture("discovery_channels.json");
  const res = list(P.parseSearch(raw), "discovery_channels", { min: 8, kinds: ["channel"] });
  same(res.rows.filter((r) => r.artist).map((r) => r.id), allOf(raw, "channelRenderer").filter(verified).map((c) => c.channelId), "artists");
  check(res.rows.every((r) => !r.topic), "no Topic channel");
  same(P.rankChannels(res.rows).map((r) => r.title), ["Riyandi Kusuma", "Tiffany Poon", "Glenn Gould", "Piamint", "Andrijapianist",
    "TheThinkingPianist", "Isla Lao | Young Classical Pianist", "London Contemporary School of Piano"], "rank");
});

Deno.test("discovery: a Mix", () => {
  const res = list(P.parseMix(fixture("discovery_mix.json")), "discovery_mix", { min: 8, kinds: ["video"] });
  same([res.id, res.rows[0].id, res.rows[0].channelId], ["RD" + D.mix, D.mix, D.mixChannel], "seed");
  check(res.rows.every((r, i) => i === 0 || r.index > res.rows[i - 1].index), "indexes in order");
  check(res.rows.every((r) => r.seconds > 0), "durations");
});

Deno.test("pianists: score, ties and 12 at most", () => {
  P.loadRules(RULES);
  const rows = [
    vrow("a1", ucid("e"), "Ella"),
    vrow("a2", ucid("a"), "Ann", { avatar: "https://yt3.ggpht.com/ann" }),
    vrow("a3", ucid("a"), "Ann"),
    vrow("a4", ucid("b"), "Bo", { artist: true }),
    vrow("a5", ucid("c"), "Cy Piano"),
    vrow("a6", ucid("d"), "Dee"),
    vrow("a7", ucid("d"), "Dee", { seconds: 30 }),
    vrow("a8", "", "Nobody"),
    { kind: "playlist", id: "PL1", channelId: ucid("p"), channel: "Lists" },
  ];
  same(P.pianists(rows, D.query), [
    { id: ucid("a"), name: "Ann", avatar: "https://yt3.ggpht.com/ann", artist: false, rec: 2, score: 6 },
    { id: ucid("b"), name: "Bo", avatar: "", artist: true, rec: 1, score: 5 },
    { id: ucid("c"), name: "Cy Piano", avatar: "", artist: false, rec: 1, score: 4 },
    { id: ucid("e"), name: "Ella", avatar: "", artist: false, rec: 1, score: 3 },
    { id: ucid("d"), name: "Dee", avatar: "", artist: false, rec: 1, score: 3 },
  ], "cards");
  const many = [..."abcdefghijklmn"].map((c, i) => vrow("m" + i, ucid(c), "P" + c));
  same(P.pianists(many).map((p) => p.name), [..."abcdefghijkl"].map((c) => "P" + c), "12 at most, in order");
  same(P.pianists(null), [], "nothing");
});

Deno.test("pianists: Topic, lessons, the view's own channel, no recordings and a low share are left out", () => {
  P.loadRules(RULES);
  const rows = [
    vrow("b1", ucid("t"), "Frédéric Chopin - Topic"),
    vrow("b2", ucid("l"), "Piano Lessons Academy"),
    vrow("b3", ucid("o"), "Own"),
    vrow("b4", ucid("z"), "Zero", { title: "Canon - Violin & Piano" }),
    vrow("b5", ucid("z"), "Zero", { live: true }),
    vrow("b6", ucid("s"), "Shy"),
    vrow("b7", ucid("h"), "Half"),
    vrow("b8", ucid("n"), "New"),
    vrow("b9", ucid("k"), "Kept"),
  ];
  const seen = { [ucid("s")]: { rec: 1, rows: 4 }, [ucid("h")]: { rec: 2, rows: 4 }, [ucid("n")]: { rec: 0, rows: 3 } };
  const names = (...args) => P.pianists(rows, ...args).map((p) => p.name);
  same(names("", seen, ucid("o")), ["Half", "New", "Kept"], "left out");
  same(names("", seen, [ucid("o"), ucid("k")]), ["Half", "New"], "own channels as a list");
  same(names("canon violin"), ["Own", "Zero", "Shy", "Half", "New", "Kept"], "the query exempts its own words");
});

Deno.test("pianists: rows from several searches count each against its own query", () => {
  P.loadRules(RULES);
  const rows = [
    vrow("q1", ucid("z"), "Zero", { title: "Canon - Violin & Piano" }),
    vrow("q2", ucid("w"), "Wynn", { title: "Waltz - Violin & Piano" }),
    vrow("q3", ucid("k"), "Kept"),
  ];
  const shelf = { q1: "canon violin", q2: "waltz piano" };
  const names = (query) => P.pianists(rows, query).map((p) => p.name);
  same(names((row) => shelf[row.id] || ""), ["Zero", "Kept"], "Zero's shelf asked for violin, Wynn's did not");
  same(names("canon violin waltz piano"), ["Zero", "Wynn", "Kept"], "one query of them all counts Wynn too");
  same(names(() => undefined), ["Kept"], "no query for a row is no words");
});

Deno.test("discovery: pianists from the saved pages", () => {
  P.loadRules(RULES);
  const more = P.parseMore(fixture("discovery_more.json")).rows;
  const week = P.parseSearch(fixture("discovery_week.json")).rows;
  const names = (rows) => P.pianists(rows, D.query).map((p) => p.name);
  check(more.some((r) => r.channel.endsWith(" - Topic")) && !names(more).some((n) => n.endsWith(" - Topic")), "Topic left out");
  check(more.some((r) => r.channel === "Yellow Notes") && !names(more).includes("Yellow Notes"), "no recordings left out");
  check(week.some((r) => P.lessonChannel(r.channel)) && !names(week).some((n) => P.lessonChannel(n)), "schools left out");
  const first = P.pianists(week, D.query)[0];
  same([first.name, first.rec, first.artist, first.score], ["Serkmusic", 3, true, 11], "the most recordings first");
});

Deno.test("pieceQuery: the piece, searched beside the Mix", () => {
  for (const [title, channel, want] of [
    ["Ludovico Einaudi - Nuvole Bianche (Piano Solo)", "Ludovico Einaudi", "Nuvole Bianche piano"],
    ["【Piano】", "", ""],
    ["Chopin - Nocturne Op. 9 No. 2 | Piano Cover | Sheet Music", "Rousseau", "Chopin Nocturne Op. 9 No. 2 piano"],
    ["Piano Cover | Merry-Go-Round of Life", "", "Merry Go Round of Life piano"],
    ["River Flows In You / Yiruma (piano cover)", "", "River Flows In You piano"],
    ["Un Sospiro - Liszt (Piano Solo) / Live at Carnegie Hall", "", "Un Sospiro Liszt piano"],
    ["KASSIA - Swan Lake", "Kassia", "Swan Lake piano"],
    ["C++ Piano - Clair de Lune", "C++ Piano", "Clair de Lune piano"],
    ["Clair de Lune (Studio Version)", "Johann Debussy", "Clair de Lune piano"],
    ["Lang Lang - Chopin: Nocturne No. 2 in E-Flat Major, Op. 9 No. 2", "Lang Lang", "Chopin Nocturne No. 2 in E Flat Major piano"],
    ["Beethoven’s Moonlight Sonata | Piano Cover", "", "Beethoven's Moonlight Sonata piano"],
    ["Fur Elise Sheet Music by Beethoven", "", "Fur Elise Beethoven piano"],
    ["Gymnopédie No. 1 [4K] HD Full Version", "", "Gymnopédie No. 1 piano"],
    ["{Live} Kiss the Rain - Yiruma", "Yiruma", "Kiss the Rain piano"],
    ["Arr. by Kyle Landry: Unravel", "", "Kyle Landry Unravel piano"],
    ["Liverpool Arrival", "", "Liverpool Arrival piano"],
    ["Ｃｈｏｐｉｎ Ｎｏｃｔｕｒｎｅ", "", "Chopin Nocturne piano"],
    ["千本桜 ピアノ", "", "千本桜 piano"],
    ["Op. 2", "", ""],
    [null, null, ""],
  ]) {
    same(P.pieceQuery(title, channel), want, String(title));
  }
});

Deno.test("mergeSimilar: the seed and its channel left out, one best row per channel, 20 at most", () => {
  P.loadRules(RULES);
  const seed = vrow("seed", ucid("s"), "Seed");
  const mix = [seed, vrow("m1", ucid("a"), "Ann"), vrow("m2", ucid("s"), "Seed"), vrow("m3", ucid("b"), "Bo")];
  const search = [vrow("s1", ucid("c"), "Cy"), vrow("seed", ucid("s"), "Seed"), vrow("s3", ucid("d"), "Dee"),
    { kind: "playlist", id: "PL1", channelId: ucid("p"), channel: "Lists" }];
  const ids = (...args) => P.mergeSimilar(...args).map((r) => r.id);
  same(ids(seed, mix, search), ["s1", "m1", "s3", "m3"], "ties in turn from each source");
  same(ids({ id: "seed" }, mix, search), ["s1", "m1", "s3", "m3"], "a seed without a channel takes the Mix's");
  same(ids(seed, mix, search, [vrow("x", ucid("c"), "Cy")]), ["m1", "s3", "m3"], "channels already shown");
  const best = ids(seed,
    [seed, vrow("a-short", ucid("a"), "Ann", { seconds: 30 }), vrow("a-artist", ucid("a"), "Ann", { artist: true }), vrow("b1", ucid("b"), "Bo")],
    [vrow("a-rec", ucid("a"), "Ann"), vrow("b-first", ucid("b"), "Bo"), vrow("c-first", "", "Cy Piano"), vrow("c-again", "", "Cy Piano")]);
  same(best, ["a-artist", "c-first", "b-first"], "by score; a recording, then an artist, then the first");
  const many = [..."ABCDEFGHIJKLMNOPQRSTUVWXY"].map((c, i) => vrow("r" + i, ucid(c), "P" + c));
  same(ids(seed, many, []), many.slice(0, 20).map((r) => r.id), "20 at most");
  same(ids(null, null, null), [], "nothing");
});

Deno.test("mergeSimilar: Topic channels are left out, as the pianist strip leaves them", () => {
  P.loadRules(RULES);
  const seed = vrow("seed", ucid("s"), "Seed");
  const mix = [seed, vrow("t1", ucid("t"), "Frédéric Chopin - Topic", { artist: true }), vrow("m1", ucid("a"), "Ann")];
  const search = [vrow("t2", "", "Yiruma - Topic"), vrow("s1", ucid("c"), "Cy"), vrow("t3", ucid("u"), "Lang Lang - Topic")];
  same(P.mergeSimilar(seed, mix, search).map((r) => r.id), ["s1", "m1"], "no Topic row");
  same(P.mergeSimilar(seed, [seed, vrow("t4", ucid("v"), "Topic"), vrow("t5", ucid("x"), "Topic Hits")], []).map((r) => r.id),
    ["t4", "t5"], "only the auto-made \" - Topic\" channels");
});

Deno.test("discovery: Similar from the saved Mix and Top", () => {
  P.loadRules(RULES);
  const mix = P.parseMix(fixture("discovery_mix.json")).rows;
  const top = P.parseSearch(fixture("discovery_top.json")).rows;
  check(top.some((r) => r.id === D.mix), "the seed is in the search too");
  const out = P.mergeSimilar({ id: D.mix }, mix, top);
  check(out.length >= 8 && out.length <= 20, "rows " + out.length);
  check(out.every((r) => r.kind === "video" && r.id !== D.mix && r.channelId !== D.mixChannel), "no seed and none of its channel");
  check(out.every((r) => !r.channel.endsWith(" - Topic")), "no Topic channel");
  same(new Set(out.map((r) => r.channelId)).size, out.length, "one row per channel");
});

// ---- genres of a title and the curated pianists (§3.19)

Deno.test("GENRES: the words a pianist's channel search asks when few, and the frozen word lists", () => {
  for (const g of P.GENRES) {
    const words = g.words.split("|");
    check(words.length >= 4 && words.every((w) => w && w === w.trim()), g.key + " words: " + g.words);
  }
  same(Object.keys(P.GENRE_WORDS), P.GENRES.map((g) => g.key), "a list per genre, in GENRES order");
  check(Object.isFrozen(P.GENRE_WORDS) && Object.values(P.GENRE_WORDS).every((w) => Object.isFrozen(w) && w.length), "frozen");
  check(Object.isFrozen(P.FORM_WORDS) && P.FORM_WORDS.every((w) => P.GENRE_WORDS.classical.includes(w)), "forms are classical words");
});

Deno.test("inGenre: whole words, the channel's name left out, forms with proof, and the vetoes", () => {
  for (const [key, title, channel, want] of [
    ["anime", "My Dearest - Guilty Crown OP 1 Piano Cover", "", true],
    ["classical", "My Dearest - Guilty Crown OP 1 Piano Cover", "", false],
    ["classical", "Chopin - Waltz Op. 64 No. 2", "", true],
    ["classical", "Waltz in A minor", "", true],
    ["classical", "Original Waltz", "", false],
    ["classical", "Moonlight Sonata (Piano)", "", true],
    ["classical", "Bohemian Rhapsody - Queen (Piano)", "", false],
    ["pop", "Bohemian Rhapsody - Queen (Piano)", "", true],
    ["games", "Fly Me to the Moon (Evangelion ED) Piano", "", false],
    ["anime", "Fly Me to the Moon (Evangelion ED) Piano", "", true],
    ["games", "Scott Kirby Piano: Maple Leaf Rag", "Scott Kirby", false],
    ["ragtime", "Scott Kirby Piano: Maple Leaf Rag", "Scott Kirby", true],
    ["games", "Kirby Super Star - Gourmet Race (Piano)", "", true],
    ["games", "Halo - Beyonce (Piano Cover)", "", false],
    ["games", "Halo Theme (Piano)", "", true],
    ["games", "Game of Thrones - Main Theme (Piano)", "", false],
    ["film", "Game of Thrones - Main Theme (Piano)", "", true],
    ["anime", "Spirited Away - One Summer's Day (Piano)", "", true],
    ["film", "Spirited Away - One Summer's Day (Piano)", "", true],
    ["games", "Pokemon Theme - Gotta Catch 'Em All (Piano)", "", false],
    ["games", "Q&A and announcement", "", false],
    ["games", "ＺＥＬＤＡ Song of Storms", "", true],
    ["games", "Zeldas", "", false],
    ["nope", "Zelda", "", false],
    ["games", null, null, false],
    // The channel's name is left out only where it stands: the channel Ace keeps Ace Attorney and Love and Deepspace.
    ["games", "Ace Attorney - Objection (Piano)", "Ace", true],
    ["games", "Love and Deepspace - Main Theme (Piano)", "Ace", true],
    // A word that names a genre only now and then: a song's own title, another genre's word or a loose one.
    ["games", "Bruno Mars - Talking to the Moon (Piano)", "", false],
    ["pop", "Bruno Mars - Talking to the Moon (Piano)", "", true],
    ["anime", "Game of Thrones/Westworld Opening Theme (Piano)", "", false],
    ["film", "Game of Thrones/Westworld Opening Theme (Piano)", "", true],
    ["anime", "Florence + The Machine - Howl (Piano)", "", false],
    ["anime", "Howl's Moving Castle - Merry Go Round of Life", "", true],
    ["games", "Celeste - Strangers (Piano)", "", false],
    ["games", "Celeste - Resurrections (Piano)", "", true],
    ["games", "Tales of the Unexpected Theme (Piano)", "", false],
    ["games", "Tales of Vesperia - Ring a Bell (Piano)", "", true],
    ["classical", "Neo-Classical Piano Improvisation", "", false],
    ["neoclassical", "Neo-Classical Piano Improvisation", "", true],
    ["film", "Schubert - Moment Musical No. 3", "", false],
    ["classical", "Schubert - Moment Musical No. 3", "", true],
    ["film", "Les Misérables - I Dreamed a Dream (Musical)", "", true],
    // A genre's loose word counts only when no other genre's own word is there.
    ["classical", "He plays Mario for an audience expecting classical", "", false],
    ["games", "He plays Mario for an audience expecting classical", "", true],
    ["classical", "Bad Apple!! [Touhou x Classical]", "", false],
    ["games", "Bad Apple!! [Touhou x Classical]", "", true],
    ["film", "Halo 5 - Fan Made Soundtrack (Piano)", "", false],
    ["games", "Halo 5 - Fan Made Soundtrack (Piano)", "", true],
    ["anime", "BlazBlue CSII OP (Piano)", "", false],
    ["games", "BlazBlue CSII OP (Piano)", "", true],
    ["anime", "My Dearest OP (Piano)", "", true],
    // Names the lists once missed.
    ["games", "FF16 - Away (Piano)", "", true],
    ["games", "FFVII - Aerith's Theme", "", true],
    ["games", "RuneScape - Sea Shanty 2 (Piano)", "", true],
    ["games", "World of Warcraft - Elwynn Forest (Piano)", "", true],
    ["games", "Subnautica - Abandon Ship (Piano)", "", true],
    ["games", "Tower of Fantasy - Main Theme (Piano)", "", true],
    ["games", "Max Payne 2 - Theme (Piano)", "", true],
    ["games", "Drakengard 3 - Kaiten Mokuba (Piano)", "", true],
    ["games", "Sumeru - Genshin Impact 3.6 (Piano)", "", true],
    ["anime", "JoJo - Giorno's Theme (Piano)", "", true],
    ["anime", "Creepy Nuts - Bling-Bang-Bang-Born (Piano)", "", true],
    ["classical", "Canon in D (Piano)", "", true],
    ["classical", "Rentarō Taki - Kōjō no Tsuki (Piano)", "", true],
    // A Japanese composer's name is a word of its own in Katakana, and not a piece of a longer one.
    ["classical", "『別れの曲』ショパン", "", true],
    ["classical", "リスト - ラ・カンパネラ", "", true],
    ["classical", "プレイリスト", "", false],
    // An anime's opening by its number is no opus: an opus without its dot or the word Opus has its No.
    ["classical", "『Hello,world!』 - Kekkai Sensen Op1 {Piano}", "SL K", false],
    ["anime", "『Hello,world!』 - Kekkai Sensen Op1 {Piano}", "SL K", true],
    ["classical", "『KaiKai Kitan』 - Jujutsu Kaisen Op 1 {Piano}", "SL K", false],
    ["anime", "『KaiKai Kitan』 - Jujutsu Kaisen Op 1 {Piano}", "SL K", true],
    ["classical", "Op 27 No 2 (Piano)", "", true],
    ["classical", "Op. 28 (Piano)", "", true],
    // Songs the lists missed on 2026-10-09, a rhythm game's contest, and a pianist's own piece.
    ["anime", "KICK BACK (piano)", "", true],
    ["anime", "IRIS OUT (piano)", "", true],
    ["anime", "アイドル [FULL] (piano)", "", true],
    ["film", "［ストリートピアノ］彼こそが海賊［連弾］", "", true],
    ["film", "He's a Pirate (Piano)", "", true],
    ["games", "【耳コピ】ボクとワタシの【まのさば】", "", true],
    ["games", "[#CosmicRadio2024] Uaaaaak - Dramatic Entrance!", "으엨 희 / Uaaaaak", true],
    ["games", "Uaaaaak - Painted Cosmos 【#CosmicRadio2025】", "으엨 희 / Uaaaaak", true],
    ["games", "[Original] Uaaaaak - Where the Waves Begin", "으엨 희 / Uaaaaak", false],
    ["neoclassical", "[Original] Uaaaaak - Where the Waves Begin", "으엨 희 / Uaaaaak", true],
    ["neoclassical", "Falling Star - Johannes Johansson (Original)", "", true],
  ]) {
    same(P.inGenre(key, title, channel), want, key + ": " + title);
  }
  same(P.genresOf("Spirited Away - One Summer's Day (Piano)"), ["film", "anime"], "genresOf, in GENRES order");
  same(P.genresOf("Scott Kirby Piano: Maple Leaf Rag", "Scott Kirby"), ["ragtime"], "genresOf leaves the channel out");
  same(P.genresOf("Nocturne"), [], "none");
  for (const p of P.CURATED) {
    same(P.genresOf(p.name, p.name), [], p.name + ": the name alone names no genre");
    check(P.inGenre("games", "Zelda's Lullaby - " + p.name + " Piano", p.name), p.name + ": the name hides no song's word");
    check(P.inGenre("games", p.name + " plays Ace Attorney", p.name), p.name + ": nor one running past it");
  }
});

Deno.test("inGenre: on the titles judged by hand, 95% of those accepted are in genre and 85% of the genre's are accepted", () => {
  const rows = fixture("genres.json");
  check(rows.length >= 500 && rows.every((r) => P.GENRES.some((g) => g.key === r[0]) && (r[3] === 1 || r[3] === 0)), "the fixture's shape");
  let hit = 0, accepted = 0, yes = 0;
  for (const [key, channel, title, judged] of rows) {
    const a = P.inGenre(key, title, channel);
    if (a) accepted++;
    if (judged) yes++;
    if (a && judged) hit++;
  }
  check(hit / accepted >= 0.95, "precision " + (hit / accepted).toFixed(3));
  check(hit / yes >= 0.85, "recall " + (hit / yes).toFixed(3));
});

Deno.test("inScope: the genre's songs, and a pianist who posts nothing else keeps the titles that name no genre", () => {
  const id = (name) => P.CURATED.find((p) => p.name === name).id;
  for (const [key, title, channel, who, want] of [
    // Titles as these pianists' uploads were read on 2026-10-09.
    ["neoclassical", "RIOPY - Feeling Safe (Official Audio)", "RIOPY", "RIOPY", true],
    ["neoclassical", "Gibran Alcocer - Idea 8 (Official Visualizer)", "Gibran Alcocer", "Gibran Alcocer", true],
    ["neoclassical", "Fabrizio Paterlini - Week #3/ Week #10 (Live)", "Fabrizio Paterlini", "Fabrizio Paterlini", true],
    ["games", "Cryogenic (piano)", "Uaaaaak", "Uaaaaak", true],
    ["anime", "【ピアノ楽譜】しかばねの踊り/Dance of the Corpses", "海音良-Umitora-", "海音良-Umitora-", true],
    // Only its first genre, and not a title that names another genre's song.
    ["pop", "RIOPY - Feeling Safe (Official Audio)", "RIOPY", "RIOPY", false],
    ["neoclassical", "Interstellar - Main Theme (Piano)", "RIOPY", "RIOPY", false],
    ["film", "Interstellar - Main Theme (Piano)", "RIOPY", "RIOPY", true],
    // A pianist who posts more than one genre keeps only the titles that name it.
    ["neoclassical", "improvising in Bb Major", "kylelandry", "Kyle Landry", false],
    ["neoclassical", "neoclassical improvisation no188 - Db Major", "kylelandry", "Kyle Landry", true],
    ["games", "Super Mario Bros Theme (Piano)", "kylelandry", "Kyle Landry", true],
    // A pianist marked all still loses the titles that name another genre's song or their own pieces.
    ["games", "KICK BACK (piano)", "으엨 희 / Uaaaaak", "Uaaaaak", false],
    ["games", "アイドル (piano)", "으엨 희 / Uaaaaak", "Uaaaaak", false],
    ["games", "[Original] Uaaaaak - Where the Waves Begin", "으엨 희 / Uaaaaak", "Uaaaaak", false],
    ["games", "[#CosmicRadio2024] Uaaaaak - Dramatic Entrance!", "으엨 희 / Uaaaaak", "Uaaaaak", true],
    ["games", "Mesmerizer (piano)", "으엨 희 / Uaaaaak", "Uaaaaak", true],
    ["anime", "［ストリートピアノ］彼こそが海賊［連弾］", "海音良-Umitora-", "海音良-Umitora-", false],
    ["anime", "【耳コピ】ボクとワタシの【まのさば】", "海音良-Umitora-", "海音良-Umitora-", false],
    // A pianist whose titles name only themselves.
    ["neoclassical", "Joep Beving - Ab Ovo", "Joep Beving", "Joep Beving", true],
    ["neoclassical", "Ludovico Einaudi - Experience (Official Visualizer)", "Ludovico Einaudi", "Ludovico Einaudi", true],
    ["jazz", "Herbie Hancock - Transcription - Pinocchio Comping", "Bijan Taghavi", "Bijan Taghavi", true],
    ["jazz", "MARGIE | Stephanie Trick", "Stephanie Trick & Paolo Alderighi", "Stephanie Trick & Paolo Alderighi", true],
  ]) {
    same(P.inScope(key, title, channel, id(who)), want, key + ": " + title);
  }
  same(P.inScope("neoclassical", "RIOPY - Feeling Safe (Official Audio)", "RIOPY"), false, "no channel, no pass");
  same(P.inScope("neoclassical", "RIOPY - Feeling Safe (Official Audio)", "RIOPY", "UC" + "x".repeat(22)), false, "nor another's");
  check(P.CURATED.some((p) => p.all) && P.CURATED.every((p) => !p.all || p.genres.length === 1), "all lists one genre");
});

Deno.test("CURATED: each pianist's id, name, handle, picture, genres and all, frozen, at least 8 per genre, none blocked", () => {
  const keys = P.GENRES.map((g) => g.key);
  check(Object.isFrozen(P.CURATED) && P.CURATED.length >= 150, "frozen, " + P.CURATED.length);
  same(new Set(P.CURATED.map((p) => p.id)).size, P.CURATED.length, "one row per id");
  for (const p of P.CURATED) {
    const where = JSON.stringify(p);
    same(Object.keys(p), ["id", "name", "handle", "avatar", "genres", "all"], where + " fields");
    check(typeof p.all === "boolean", where + " all");
    check(UCID.test(p.id) && !P.BLOCK.includes(p.id), where + " id");
    check(p.name && p.name === p.name.trim() && p.name === p.name.normalize("NFC"), where + " name");
    check(/^@[^\s@]+$/.test(p.handle), where + " handle");
    check(/^[A-Za-z0-9_-][A-Za-z0-9_\/-]*[A-Za-z0-9_-]$/.test(p.avatar), where + " picture path");
    check(Object.isFrozen(p) && Object.isFrozen(p.genres) && p.genres.length && p.genres.every((g) => keys.includes(g)) &&
      new Set(p.genres).size === p.genres.length, where + " genres");
  }
  for (const key of keys) check(P.curatedFor(key).length >= 8, key + " has " + P.curatedFor(key).length);
  same(P.avatarUrl("ytc/AIdro_x"), "https://yt3.googleusercontent.com/ytc/AIdro_x=s96-c-k-c0x00ffffff-no-rj", "a picture's address");
  same(P.avatarUrl(""), "", "none");
});

Deno.test("curatedFor: those best known for the genre first, then the others in it, each in the list's order", () => {
  for (const g of P.GENRES) {
    const list = P.curatedFor(g.key);
    const firsts = list.filter((p) => p.genres[0] === g.key).length;
    check(list.every((p, i) => (i < firsts) === (p.genres[0] === g.key) && p.genres.includes(g.key)), g.key + " firsts first");
    const at = (p) => P.CURATED.indexOf(p);
    check(list.every((p, i) => i === 0 || i === firsts || at(p) > at(list[i - 1])), g.key + " in the list's order");
    same(list.length, P.CURATED.filter((p) => p.genres.includes(g.key)).length, g.key + " all of them");
  }
  same(P.curatedFor("nope"), [], "an unknown genre");
});

Deno.test("CURATED: every genre a pianist is listed under keeps songs of theirs, as their uploads were read on 2026-10-09", () => {
  const songs = fixture("curated_songs.json");
  same(Object.keys(songs).sort(), P.CURATED.map((p) => p.id).sort(), "a read of each pianist");
  for (const p of P.CURATED) {
    const [channel, ...titles] = songs[p.id];
    for (const g of p.genres) check(titles.some((t) => P.inScope(g, t, channel, p.id)), p.name + " keeps a song in " + g);
    // A pianist marked all has songs that name no genre, which only all keeps.
    if (p.all) check(titles.some((t) => P.inScope(p.genres[0], t, channel, p.id) && !P.genresOf(t, channel).length), p.name + " needs all");
  }
});

Deno.test("CURATED: no pianist twice, as a second channel under the same name", () => {
  const words = (s) => " " + s.toLowerCase().normalize("NFKD").replace(/[^\p{L}\p{N} ]/gu, "").replace(/\s+/g, " ").trim() + " ";
  for (const a of P.CURATED) {
    for (const b of P.CURATED) if (a !== b) check(!words(a.name).includes(words(b.name)), a.name + " is " + b.name + " again");
  }
});

Deno.test("BLOCK: never a pianist in a strip or Similar", () => {
  P.loadRules(RULES);
  const [blocked] = P.BLOCK;
  const rows = [vrow("k1", blocked, "Blocked"), vrow("k2", blocked, "Blocked"), vrow("k3", ucid("a"), "Ann")];
  same(P.pianists(rows).map((p) => p.name), ["Ann"], "pianists");
  same(P.mergeSimilar(vrow("seed", ucid("s"), "Seed"), rows, []).map((r) => r.id), ["k3"], "mergeSimilar");
  check(!P.genrePianists("classical", rows).some((p) => p.id === blocked), "genrePianists");
  check(!P.homeCurated([], [], 500).some((p) => p.id === blocked), "homeCurated");
});

Deno.test("genrePianists: the curated first, then the channels posting the genre's songs, by score, max at most", () => {
  P.loadRules(RULES);
  const curated = P.curatedFor("games");
  const game = ["Zelda - Song of Storms piano", "Undertale - Megalovania piano", "Super Mario Bros theme piano", "Pokemon Center piano"];
  const rows = [
    ...game.slice(0, 3).map((t, i) => vrow("t" + i, ucid("t"), "Two", { title: i === 2 ? "Nocturne piano" : t })),
    ...game.map((t, i) => vrow("m" + i, ucid("m"), "Many", { title: t })),
    ...game.slice(0, 3).map((t, i) => vrow("o" + i, ucid("o"), "One", { title: i ? "Nocturne piano" : t })),
    ...game.slice(0, 2).map((t, i) => vrow("l" + i, ucid("l"), "Game Records", { title: t })),
    vrow("n0", ucid("n"), "None", { title: "Nocturne piano" }),
    vrow("c0", curated[0].id, curated[0].name, { title: game[0] }),
  ];
  const cards = P.genrePianists("games", rows, {}, 1000);
  same(cards.slice(0, curated.length), curated.map((p) => ({ id: p.id, name: p.name, avatar: P.avatarUrl(p.avatar), curated: true })),
    "every curated pianist of the genre first, as cards");
  same(cards.slice(curated.length).map((p) => [p.name, p.rec]), [["Many", 4], ["Two", 3]],
    "then 4 of 4 and 2 of 3, not 1 of 3, a label, none or a curated one again");
  same(P.genrePianists("games", rows).length, 24, "24 at most");
  same(P.genrePianists("games", rows, {}, 5).map((p) => p.id), curated.slice(0, 5).map((p) => p.id), "max");
  const rags = rows.filter((r) => r.id !== "c0").map((r) => ({ ...r, title: "Maple Leaf Rag piano" }));
  same(P.genrePianists("ragtime", rags, {}, 1000).slice(P.curatedFor("ragtime").length).map((p) => p.name),
    ["Many", "Two", "One", "None"], "all in the genre: by score, ties in row order");
});

Deno.test("genrePianists: the first 9 curated stay, the others take turns 9 on at a time, and 6 places are left to those found", () => {
  P.loadRules(RULES);
  const SONG = {
    games: "Zelda - Song of Storms (Piano)", anime: "Unravel - Tokyo Ghoul OP (Piano)", classical: "Chopin - Nocturne Op. 9 No. 2 (Piano)",
    jazz: "Autumn Leaves - Jazz Piano", ragtime: "Maple Leaf Rag (Piano)", pop: "Bohemian Rhapsody - Queen (Piano)",
    neoclassical: "Neo-Classical Piano Improvisation", film: "Interstellar - Main Theme (Piano)",
  };
  for (const g of P.GENRES) {
    check(P.inGenre(g.key, SONG[g.key]), g.key + ": the made-up song");
    const list = P.curatedFor(g.key).map((p) => p.id);
    const rows = "abcdefgh".split("").map((c, i) => vrow(g.key + i, ucid(c), "Found " + c, { title: SONG[g.key] }));
    const seen = new Set();
    for (let turn = 0; turn < 20; turn++) {
      const ids = P.genrePianists(g.key, rows, {}, 24, turn).map((p) => p.id);
      const curated = ids.filter((id) => list.includes(id));
      same(ids.slice(0, 9), list.slice(0, 9), g.key + ": the first 9 at turn " + turn);
      check(ids.length === 24 && curated.length <= 18 && ids.slice(curated.length).every((id) => !list.includes(id)),
        g.key + ": 18 curated at most, then those found, at turn " + turn);
      curated.forEach((id) => seen.add(id));
    }
    same(seen.size, list.length, g.key + ": each curated pianist has a place within 20 turns");
    same(P.genrePianists(g.key, [], {}, 24, 0).map((p) => p.id), list.slice(0, 24), g.key + ": none found, turn 0: the list's order");
  }
  const games = P.curatedFor("games").map((p) => p.id);
  const rest = games.slice(9);
  same(P.genrePianists("games", [], {}, 24, 1).map((p) => p.id), [...games.slice(0, 9), ...rest.slice(9, 24)], "turn 1: 9 on");
  const last = Math.ceil(rest.length / 9);
  same(P.genrePianists("games", [], {}, 24, last).map((p) => p.id).slice(9),
    [...rest, ...rest].slice((last * 9) % rest.length, (last * 9) % rest.length + 15), "round again");
  same(P.genrePianists("games", [], {}, 24, -3), P.genrePianists("games", [], {}, 24, 0), "a turn below 0 is turn 0");
});

Deno.test("homeCurated: in turn from the genres of your conversions, most first, else from every genre", () => {
  const firstOf = (key) => P.curatedFor(key)[0].id;
  const ids = (...args) => P.homeCurated(...args).map((p) => p.id);
  const none = ids();
  same(none.slice(0, 8), P.GENRES.map((g) => firstOf(g.key)), "no titles: each genre's first, in GENRES order");
  same([none.length, new Set(none).size], [12, 12], "12, none twice");
  const games = P.curatedFor("games").map((p) => p.id);
  const anime = P.curatedFor("anime").map((p) => p.id);
  const two = ids(["Zelda - Song of Storms", "Undertale - Megalovania", "Naruto - Blue Bird", "Nocturne"]);
  same(two.slice(0, 2), [games[0], anime[0]], "games has two, anime one");
  check(two.every((id) => games.includes(id) || anime.includes(id)), "only those genres");
  same(ids(["Naruto - Blue Bird", "Zelda - Song of Storms"])[0], games[0], "a tie in GENRES order");
  const skip = [firstOf("classical"), firstOf("jazz")];
  const skipped = ids([], skip);
  check(!skipped.some((id) => skip.includes(id)) && skipped[0] === firstOf("ragtime"), "yours are skipped");
  same(ids([], [], 3).length, 3, "n at most");
  const all = ids([], [], 1000);
  same([all.length, new Set(all).size], [P.CURATED.length, P.CURATED.length], "each pianist once, in several genres too");
  same(P.homeCurated(["Zelda"])[0], { id: games[0], name: P.curatedFor("games")[0].name, avatar: P.avatarUrl(P.curatedFor("games")[0].avatar), curated: true }, "a card");
});

Deno.test("channelSearchBody: the channel's search tab, asked the genre's words", () => {
  same(P.channelSearchBody(client, M.channel, P.GENRES[5].words),
    { context: { client }, browseId: M.channel, params: "EgZzZWFyY2jyBgQKAloA", query: P.GENRES[5].words }, "the body");
  same(P.channelSearchBody(client, M.channel).query, "", "no words");
});

// ---- folders

Deno.test("folderName: the shared fixture, as convert.py names folders", () => {
  const cases = fixture("foldernames.json");
  check(cases.length >= 40, "foldernames.json has " + cases.length);
  const wrong = cases.filter((c) => P.folderName(c.name) !== c.folder).map((c) => [c.name, P.folderName(c.name), c.folder]);
  same(wrong, [], "wrong folders");
  check(cases.every((c) => P.validDest(c.folder)), "every folder name is a valid segment");
  same(P.folderName(undefined), "YouTube", "nothing");
});

// The same table as test_browse.py's GOOD_DESTS and BAD_DESTS.
Deno.test("validDest: up to 4 safe folder names", () => {
  const good = ["", "Anime", "Anime/Animenz Piano Sheets", "A/B/C/D", "Classical/Chopin", "Mix - Nocturne", "Ünïcode 日本/ピアノ",
    ".hidden", "a.b", "Con Brio", "CONSOLE", "COM10", "LPT", "x".repeat(255), "😀".repeat(127), "A b", "Jacob Koller - The Mad Arranger",
    "COM⁴", "CONIN", "CONOUT"];
  const bad =[3, true, [], {}, ["Anime"], null, undefined, "/", "A/", "/A", "A//B", "A/B/C/D/E", "A\\B", "A:B", 'A"B', "A<B", "A>B",
    "A|B", "A?B", "A*B", ".", "..", "A/..", "A/.", "../A", " A", "A ", "A.", "A/B.", "...", "x".repeat(256), "😀".repeat(128), "CON",
    "con", "Con.txt", "con .txt", "Aux", "NUL.tar.gz", "COM1", "lpt9.mid", "PRN", "A/CON", "COM¹", "lpt³.mid", "CONIN$", "conout$.txt", "CONıN$", "A\x00B", "A\x1fB", "A\x7fB", "A\x85B",
    ...[0x200b, 0x202e, 0xfeff, 0x00ad, 0x2066].map((c) => "A" + String.fromCharCode(c) + "B"), "\ud800", "A\nB", "A\tB"];
  same(good.filter((d) => !P.validDest(d)), [], "refused");
  same(bad.filter((d) => P.validDest(d)), [], "allowed");
});
