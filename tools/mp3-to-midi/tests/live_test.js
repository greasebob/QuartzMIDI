// live_test.js: sends parse.js's own requests to the real YouTube endpoints and checks the parsed shapes.
// Skipped unless QM_LIVE=1. QM_RECORD=1 also rewrites the fixtures, trimmed (each at most 50 KB) and
// scrubbed of the IP address, visitor data and ad text (ads are targeted at the machine's location):
//   set QM_LIVE=1 & set QM_RECORD=1
//   deno test --allow-read --allow-net --allow-env --allow-write tools\mp3-to-midi\tests\live_test.js
// Requests go out signed out, from deno, with no cookies; they never touch the owner's session.
// The discovery tests (a genre's Top, its next page and This week, a Channels search and a Mix) keep 8 items and
// no ads, at most 80 KB, and list what they asked in discovery.json; "--filter discovery" runs only them, so
// QM_RECORD=1 then rewrites only their fixtures. "live: discovery release gate" is §2.3's: every genre, Top and
// New, has 12 recordings of its first 20 rows and 5 pianists. Discovery requests are 3 s apart.
import "../page/parse.js";

const P = globalThis.QMParse;
const env = (name) =>
  Deno.permissions.querySync({ name: "env", variable: name }).state === "granted" ? Deno.env.get(name) : undefined;
const LIVE = env("QM_LIVE") === "1";
const RECORD = LIVE && env("QM_RECORD") === "1";

const BASE = "https://www.youtube.com";
const UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36";
const FIXTURES = new URL("./fixtures/", import.meta.url);
const MAX = 50 * 1024;

// The ids and queries the fixtures come from; parse_test.js reads them back from manifest.json.
const IDS = {
  query: "solo piano",
  adQueries: ["solo piano", "piano lessons", "digital piano", "piano keyboard"],
  liveQuery: "piano",
  liveParams: "EgJAAQ==",
  suggest: "chopin noc",
  chapters: "hQncT4Hswhw",
  channel: "UCyOfqgtsQaM3S-VZnsYnHjQ",
  handle: "https://www.youtube.com/@HALIDONMUSIC",
  playlist: "PLt_47Imx98wqc2_lOAg53v8Op-oks9KB5",
  members: "PL62pKfyAHw_xj4OOHjZ2bxbqqojSx7ZBj",
  uploads: "UUyOfqgtsQaM3S-VZnsYnHjQ",
  ok: "dQw4w9WgXcQ",
  age: "HtVdAasjOgU",
  private: "yZIXLfi8CZQ",
  removed: "aaaaaaaaaaa",
};

function ok(cond, msg) {
  if (!cond) throw new Error(msg);
}

async function getText(url) {
  const res = await fetch(url, { headers: { "User-Agent": UA, "Accept-Language": "en-US,en;q=0.9" } });
  ok(res.ok, url + " -> HTTP " + res.status);
  return await res.text();
}

async function post(client, endpoint, body) {
  const res = await fetch(BASE + P.api(endpoint), {
    method: "POST",
    headers: { ...P.headers(client), "User-Agent": UA, "Origin": BASE },
    body: JSON.stringify(body),
  });
  ok(res.ok, endpoint + " -> HTTP " + res.status);
  return await res.json();
}

// ---- trimming and scrubbing

const NOISE = new Set([
  "trackingParams", "clickTrackingParams", "loggingDirectives", "accessibility", "accessibilityData",
  "accessibilityContext", "loggingContext", "menu", "serviceEndpoints", "responseContext", "frameworkUpdates",
  "topbar", "watchEndpointSupportedOnesieConfig", "watchEndpointSupportedPrefetchConfig", "inlinePlaybackEndpoint",
  "itemPlayback", "playbackTracking", "streamingData", "playerConfig", "storyboards", "attestation", "adPlacements",
  "playerAds", "adSlots", "heartbeatParams", "annotations", "cards", "endscreen", "captions", "detailedMetadataSnippets",
  "searchVideoResultEntityKey", "lightColorPalette", "darkColorPalette", "themedPalette", "styleRuns", "onVisible",
  "onHidden", "impressionTracking", "visibility", "trackingInfo", "thumbnailHoverOverlayViewModel",
  "thumbnailHoverOverlayToggleActionsViewModel", "merchandiseShelfRenderer", "subMenu", "searchFilterButton",
  "unifiedSharePanel", "actionButtons", "playlistButtons", "overlay", "playerOverlayRenderer.autoplay", "autoplay",
  "webSearchboxConfig", "desktopTopbarRenderer", "microformat", "background", "banner", "attribution",
  "description", "topLevelButtons", "flexibleItems", "channelThumbnail", "avatar", "avatarStack", "decoratedAvatarViewModel", "commandContext.onLongPress",
  "onLongPress", "contentPreviewImageViewModel", "heroImage", "attributedDescription", "attributedDescriptionBodyText",
  "colorSampledDescriptionBodyText", "headerRuns", "shareButton", "serializedShareEntity", "adLayoutMetadata",
  "adLayoutData", "adAvatar", "loggingUrls", "onFirstVisible", "playerParams", "chipBar", "toggleButtonRenderer",
]);
// Keys the parsers read inside the noise above, kept by path.
const KEEP_PATHS = [/\.header\..*avatarStack/, /\.metadata\.channelMetadataRenderer\.avatar/];
const CAP_LISTS = new Set(["contents", "items", "continuationItems", "results", "ads"]);
const CAPPED = new Set([
  "videoRenderer", "compactVideoRenderer", "gridVideoRenderer", "lockupViewModel", "richItemRenderer",
  "playlistVideoRenderer", "playlistPanelVideoRenderer", "channelRenderer", "shortsLockupViewModel", "adSlotRenderer",
]);

const itemKey = (x) => Object.keys(x).find((k) => k !== "trackingParams" && k !== "clickTrackingParams");

// What makes an item worth keeping beside its siblings: live, upcoming, Shorts, ads, mixes, chapters...
const flavor = (x) => {
  const s = JSON.stringify(x);
  return [
    /LIVE_NOW|BADGE_STYLE_LIVE|"style":"LIVE"/.test(s) && "live",
    /UPCOMING|upcomingEventData/.test(s) && "upcoming",
    /reelWatchEndpoint/.test(s) && "short",
    (/LOCKUP_CONTENT_TYPE_(\w+)/.exec(s) || [])[1],
    /"contentId":"RD/.test(s) && "mix",
    /feedAdMetadataViewModel/.test(s) && "ad",
    /\[(Private|Deleted) video\]/.test(s) && "gone",
    /"isPlayable":false|unplayableText/.test(s) && "unplayable",
    /MEMBERS/.test(s) && "members",
    /macroMarkersListItemRenderer/.test(s) && "chapters",
    /"index":0\b/.test(s) && "first",
  ].filter(Boolean).join("+");
};

// Ads only need their shape, so the parsers can see them and drop them.
const ADS = new Set(["searchPyvRenderer", "adSlotRenderer", "feedAdMetadataViewModel", "promotedSparklesWebRenderer"]);
// Chapters, badges and a row's channel picture keep only what parse.js reads.
const ONLY = new Map([
  ["macroMarkersListItemRenderer", new Set(["title", "timeDescription", "onTap"])],
  ["chapterRenderer", new Set(["title", "timeRangeStartMillis"])],
  ["metadataBadgeRenderer", new Set(["style", "label"])],
  ["channelThumbnailWithLinkRenderer", new Set(["thumbnail"])],
]);

function trim(node, cap, key = "", path = "", ad = false) {
  if (Array.isArray(node)) {
    let arr = node;
    if (key === "thumbnails" || key === "sources" || ad) arr = arr.slice(0, 1);
    else if (CAP_LISTS.has(key)) {
      const seen = new Map();
      arr = arr.filter((x) => {
        const k = x && typeof x === "object" && !Array.isArray(x) ? itemKey(x) : "";
        if (!CAPPED.has(k)) return true;
        const sig = k + ":" + flavor(x[k]);
        seen.set(sig, (seen.get(sig) || 0) + 1);
        return seen.get(sig) <= cap;
      });
    }
    return arr.map((x, i) => trim(x, cap, key, path + "[" + i + "]", ad));
  }
  if (node && typeof node === "object") {
    const out = {};
    const only = ONLY.get(key);
    for (const [k, v] of Object.entries(node)) {
      const p = path + "." + k;
      if (only && !only.has(k)) continue;
      if (NOISE.has(k) && !KEEP_PATHS.some((re) => re.test(p))) continue;
      if (k === "webCommandMetadata" && v && typeof v === "object") { out[k] = v.url ? { url: trim(v.url, cap, "url", p, ad) } : {}; continue; }
      out[k] = trim(v, cap, k, p, ad || ADS.has(k));
    }
    return out;
  }
  if (typeof node !== "string") return node;
  // Ads are targeted at where the machine is, so only their enums survive; the parsers spot ads by key.
  if (ad) return /^[A-Z][A-Z0-9_]*$/.test(node) ? node : /^https?:/.test(node) ? "https://example.com/ad" : "Ad";
  if (/token|continuation|visitorData/i.test(key)) return node;
  // Paths and thumbnails stay whole; off-site links are cut short.
  const room = key === "url" && /^(\/|https:\/\/(i\d?\.ytimg|yt\d\.ggpht|www\.youtube)\.com)/.test(node) ? 400 : 160;
  return node.length > room ? node.slice(0, room) : node;
}

const SAFE_IP = "203.0.113.7";
const SAFE_VISITOR = "CgtGSVhUVVJFLU9OTFkoAA%3D%3D";
const secrets = { ips: new Set(), visitors: new Set() };

function scrub(text) {
  let s = text;
  for (const ip of secrets.ips) s = s.split(ip).join(SAFE_IP);
  for (const v of secrets.visitors) s = s.split(v).join(SAFE_VISITOR);
  s = s.replace(/(?<![\w/.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])/g, (m) => (UA.includes(m) ? m : SAFE_IP));
  s = s.replace(/("visitorData"\s*:\s*")[^"]*"/g, "$1" + SAFE_VISITOR + '"');
  for (const ip of secrets.ips) ok(!s.includes(ip), "the IP address is still in a fixture");
  for (const v of secrets.visitors) ok(!s.includes(v), "visitor data is still in a fixture");
  return s;
}

// Where the bytes went, for a fixture that will not shrink: by the innermost renderer, then by key.
function heaviest(text) {
  let data;
  try { data = JSON.parse(text); } catch { return ""; }
  const byRenderer = new Map();
  const byKey = new Map();
  const add = (map, k, n) => map.set(k, (map.get(k) || 0) + n);
  const walk = (v, renderer, key) => {
    if (Array.isArray(v)) v.forEach((x) => walk(x, renderer, key));
    else if (v && typeof v === "object") {
      for (const [k, x] of Object.entries(v)) walk(x, /(Renderer|ViewModel)$/.test(k) ? k : renderer, k);
    } else {
      const n = JSON.stringify(v).length + key.length + 4;
      add(byRenderer, renderer, n);
      add(byKey, renderer + "." + key, n);
    }
  };
  walk(data, "(top)", "");
  const top = (map) => [...map].sort((a, b) => b[1] - a[1]).slice(0, 6).map(([k, n]) => n + " " + k).join(", ");
  return "\n  " + top(byRenderer) + "\n  " + top(byKey);
}

const written = [];

function save(name, data, shape = (x) => x, render = JSON.stringify) {
  if (!RECORD) return;
  let text = "";
  for (const cap of [3, 2, 1]) {
    const body = typeof data === "string" ? data : render(shape(trim(data, cap)));
    text = scrub(body);
    if (new TextEncoder().encode(text).length <= MAX) break;
  }
  const size = new TextEncoder().encode(text).length;
  ok(size <= MAX, name + " is " + size + " bytes after trimming:" + heaviest(text));
  Deno.writeTextFileSync(new URL(name, FIXTURES), text);
  written.push(name + " " + size);
}

// next: only the chapter panels and the player bar's chapters matter.
const nextShape = (j) => {
  if (Array.isArray(j.engagementPanels)) {
    j.engagementPanels = j.engagementPanels.filter((p) => JSON.stringify(p).includes("macroMarkersListRenderer"));
  }
  if (j.playerOverlays) {
    const bar = JSON.stringify(j.playerOverlays).includes("chapterRenderer") ? j.playerOverlays : null;
    if (bar) j.playerOverlays = { playerOverlayRenderer: { decoratedPlayerBarRenderer: bar.playerOverlayRenderer?.decoratedPlayerBarRenderer } };
    else delete j.playerOverlays;
  }
  return j;
};

// player: what parsePlayer reads.
const playerShape = (raw) => (j) => {
  const vd = { ...(j.videoDetails || {}) };
  delete vd.keywords;
  delete vd.shortDescription;
  delete vd.thumbnail;
  const out = { playabilityStatus: j.playabilityStatus, videoDetails: j.videoDetails ? vd : undefined };
  const live = raw.microformat?.playerMicroformatRenderer?.liveBroadcastDetails;
  if (live) out.microformat = { playerMicroformatRenderer: { liveBroadcastDetails: live } };
  return out;
};

// sw.js_data: keep the shape, replace the IP and visitor data, and blank other long strings.
function swFixture(text) {
  const data = JSON.parse(text.replace(/^\)\]\}'\s*/, ""));
  const fix = (v) => {
    if (Array.isArray(v)) return v.map(fix);
    if (v && typeof v === "object") return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, fix(x)]));
    if (typeof v !== "string") return v;
    if (secrets.visitors.has(v)) return SAFE_VISITOR;
    if (secrets.ips.has(v)) return SAFE_IP;
    if (/^[A-Z]{2}$/.test(v)) return "US";
    if (v.length > 20 && !v.startsWith("Mozilla/") && !v.startsWith("https://")) return "x";
    return v;
  };
  return ")]}'\n" + JSON.stringify(fix(data));
}

const IP_RE = /^(?:(?:\d{1,3}\.){3}\d{1,3}|[0-9a-f]{0,4}(?::[0-9a-f]{0,4}){3,7})$/i;

// The caller's address sits at sw.js_data [0][2][0][0][3]; any other address-shaped string is taken too.
function learnSecrets(swText, client) {
  const data = JSON.parse(swText.replace(/^\)\]\}'\s*/, ""));
  const add = (set, v) => {
    set.add(v);
    set.add(encodeURIComponent(v));
    try { set.add(decodeURIComponent(v)); } catch { /* not encoded */ }
  };
  const at = data?.[0]?.[2]?.[0]?.[0]?.[3];
  if (typeof at === "string" && at) add(secrets.ips, at);
  for (const m of JSON.stringify(data).matchAll(/"([^"]{7,45})"/g)) if (IP_RE.test(m[1]) && !UA.includes(m[1])) add(secrets.ips, m[1]);
  if (client.visitorData) add(secrets.visitors, client.visitorData);
  ok(secrets.ips.size > 0, "sw.js_data: the caller's address was not found, so it cannot be scrubbed");
}

// ---- the run

Deno.test({ name: "live: YouTube endpoints still have the shapes parse.js reads", ignore: !LIVE, sanitizeResources: false }, async (t) => {
  const drift = new Set();
  const look = (name, res) => {
    for (const d of res.drift || []) drift.add(name + ": " + d);
    return res;
  };

  const swText = await getText(BASE + "/sw.js_data");
  const { client, drift: swDrift } = P.clientFrom(swText, "en-US");
  ok(swDrift.length === 0, "sw.js_data: " + swDrift.join(", "));
  learnSecrets(swText, client);
  // Recorded fixtures are asked for in English and for the US, so they carry no hint of where they came from.
  client.hl = "en";
  client.gl = "US";
  save("sw.txt", swFixture(swText));

  const html = await getText(BASE + P.resultsUrl(IDS.query));
  const sts = P.stsFrom(html);
  ok(sts > 0, "no STS in the results page");
  const manifest = { recorded: new Date().toISOString().slice(0, 10), clientVersion: client.clientVersion, sts, ...IDS };

  await t.step("results HTML fallback", () => {
    const data = P.initialData(html);
    ok(data, "no ytInitialData");
    const res = look("results.html", P.parseSearch(data));
    ok(res.known && res.rows.length >= 5, "results.html: " + res.rows.length + " rows");
    const shell = (json) =>
      '<!DOCTYPE html><html><head><script nonce="fixture">ytcfg.set({"STS":' + sts +
      ',"INNERTUBE_CLIENT_VERSION":"' + client.clientVersion + '"});</script></head><body>' +
      '<script nonce="fixture">var ytInitialData = ' + json + ";</script>" +
      '<script nonce="fixture">var ytInitialPlayerResponse = {"note":"} { braces in a string"};</script></body></html>';
    save("results.html", data, (x) => x, (j) => shell(JSON.stringify(j)));
  });

  await t.step("search: videos, Shorts, playlists and ads", async () => {
    let raw = null;
    for (const q of IDS.adQueries) {
      raw = await post(client, "search", P.searchBody(client, q));
      const res = P.parseSearch(raw);
      if (res.dropped.ads && res.dropped.shorts) { manifest.adQuery = q; break; }
    }
    const res = look("search", P.parseSearch(raw));
    ok(res.known && res.rows.length >= 5, "search: " + res.rows.length + " rows");
    ok(res.rows.some((r) => r.kind === "video"), "search: no videos");
    ok(res.more, "search: no continuation");
    manifest.ads = res.dropped.ads > 0;
    manifest.shorts = res.dropped.shorts > 0;
    save("search.json", raw);
  });

  let videosMore = null;
  await t.step("search: Videos, 3-20 min", async () => {
    const raw = await post(client, "search", P.searchBody(client, IDS.query, P.searchParams({ type: "videos", duration: "medium" })));
    const res = look("search_videos", P.parseSearch(raw));
    ok(res.rows.length >= 10 && res.rows.every((r) => r.kind === "video"), "videos: not all videos");
    const inRange = res.rows.filter((r) => r.seconds >= 180 && r.seconds <= 1260).length;
    ok(inRange >= res.rows.length * 0.9, "videos: " + inRange + " of " + res.rows.length + " within 3-20 min");
    ok(res.more, "videos: no continuation");
    videosMore = res.more;
    save("search_videos.json", raw);
  });

  await t.step("search: next page", async () => {
    const raw = await post(client, "search", P.moreBody(client, videosMore));
    const res = look("search_more", P.parseMore(raw));
    ok(res.known && res.rows.length >= 5, "more: " + res.rows.length + " rows");
    save("search_more.json", raw);
  });

  await t.step("search: live now", async () => {
    const raw = await post(client, "search", P.searchBody(client, IDS.liveQuery, IDS.liveParams));
    const res = look("search_live", P.parseSearch(raw));
    const live = res.rows.filter((r) => r.live);
    ok(live.length >= 1, "live: no live rows");
    ok(live.every((r) => r.overlay === "Live" && !P.counted(r)), "live: live rows must show Live and not count");
    manifest.live = live[0].id;
    save("search_live.json", raw);
  });

  await t.step("search: Playlists", async () => {
    const raw = await post(client, "search", P.searchBody(client, IDS.query, P.searchParams({ type: "playlists" })));
    const res = look("search_playlists", P.parseSearch(raw));
    ok(res.rows.some((r) => r.kind === "playlist" && r.count > 0), "playlists: none with a count");
    save("search_playlists.json", raw);
  });

  await t.step("search: Channels", async () => {
    const raw = await post(client, "search", P.searchBody(client, IDS.query, P.searchParams({ type: "channels" })));
    const res = look("search_channels", P.parseSearch(raw));
    ok(res.rows.some((r) => r.kind === "channel" && r.handle.startsWith("@") && /\d/.test(r.subscribers)), "channels: no handle and subscribers");
    save("search_channels.json", raw);
  });

  let channelMore = null;
  let popular = null;
  await t.step("channel: videos", async () => {
    const raw = await post(client, "browse", P.channelBody(client, IDS.channel, "videos"));
    const res = look("channel_videos", P.parseChannel(raw));
    ok(res.title && res.handle.startsWith("@") && /\d/.test(res.subscribers) && res.avatar, "channel: header " + JSON.stringify([res.title, res.handle, res.subscribers]));
    ok(res.rows.length >= 20 && res.rows.every((r) => r.kind === "video" && r.seconds > 0), "channel: rows");
    ok(res.sorts && res.sorts.length === 3 && res.sort === 0, "channel: sort chips");
    ok(res.more, "channel: no continuation");
    channelMore = res.more;
    popular = res.sorts[1];
    save("channel_videos.json", raw);
  });

  await t.step("channel: next page", async () => {
    const raw = await post(client, "browse", P.moreBody(client, channelMore));
    const res = look("channel_more", P.parseMore(raw));
    ok(res.rows.length >= 5, "channel more: " + res.rows.length + " rows");
    save("channel_more.json", raw);
  });

  await t.step("channel: Popular", async () => {
    const raw = await post(client, "browse", P.moreBody(client, popular));
    const res = look("channel_sort", P.parseMore(raw));
    ok(res.rows.length >= 5 && res.sort === 1, "channel sort: " + res.rows.length + " rows, sort " + res.sort);
    save("channel_sort.json", raw);
  });

  await t.step("channel: playlists", async () => {
    const raw = await post(client, "browse", P.channelBody(client, IDS.channel, "playlists"));
    const res = look("channel_playlists", P.parseChannel(raw));
    ok(res.rows.length >= 5 && res.rows.every((r) => r.kind === "playlist"), "channel playlists: rows");
    save("channel_playlists.json", raw);
  });

  let playlistEnd = null;
  await t.step("playlist", async () => {
    const raw = await post(client, "browse", P.playlistBody(client, IDS.playlist));
    const res = look("playlist", P.parsePlaylist(raw));
    ok(res.title && res.channel && res.count >= 100, "playlist: header " + JSON.stringify([res.title, res.channel, res.count]));
    ok(res.rows.length === 100 && res.more, "playlist: " + res.rows.length + " rows");
    playlistEnd = res.more;
    save("playlist.json", raw);
  });

  // Its last page holds only hidden videos, so it brings back just the shelf of other playlists.
  await t.step("playlist: last page", async () => {
    const raw = await post(client, "browse", P.moreBody(client, playlistEnd));
    const res = look("playlist_end", P.parseMore(raw));
    ok(res.known && !res.more, "playlist end: still a continuation");
    manifest.endRows = res.rows.length;
    save("playlist_end.json", raw);
  });

  // A playlist of members-only lessons (its first part is public): those rows say so, and none counts.
  await t.step("playlist: members only", async () => {
    const raw = await post(client, "browse", P.playlistBody(client, IDS.members));
    const res = look("playlist_members", P.parsePlaylist(raw));
    const members = res.rows.filter((r) => r.members);
    ok(members.length > 0, "members: none of " + res.rows.length + " rows has the badge");
    ok(P.count(members) === 0, "members: " + P.count(members) + " members-only rows counted");
    save("playlist_members.json", raw);
  });

  let uploadsMore = null;
  await t.step("playlist: a channel's uploads", async () => {
    const raw = await post(client, "browse", P.playlistBody(client, IDS.uploads));
    const res = look("playlist_uploads", P.parsePlaylist(raw));
    ok(res.channelId === IDS.channel && res.count > 1000 && res.more, "uploads: header " + JSON.stringify([res.channel, res.count]));
    manifest.station = res.rows.some((r) => r.live);
    uploadsMore = res.more;
    save("playlist_uploads.json", raw);
  });

  await t.step("playlist: next page", async () => {
    const raw = await post(client, "browse", P.moreBody(client, uploadsMore));
    const res = look("playlist_more", P.parseMore(raw));
    ok(res.rows.length >= 50 && res.rows.every((r) => r.kind === "video" && r.seconds > 0) && res.more, "playlist more: " + res.rows.length + " rows");
    save("playlist_more.json", raw);
  });

  await t.step("next: a video with chapters", async () => {
    const raw = await post(client, "next", P.nextBody(client, IDS.chapters));
    const res = look("next_video", P.parseNext(raw));
    ok(res.video && res.video.id === IDS.chapters && res.video.title, "next: no video");
    ok(res.chapters.length >= 3 && P.long(res.video), "next: chapters " + res.chapters.length);
    ok(res.rows.length >= 5, "next: related " + res.rows.length);
    save("next_video.json", raw, nextShape);
  });

  await t.step("next: a Mix", async () => {
    const raw = await post(client, "next", P.mixBody(client, IDS.chapters));
    const res = look("next_mix", P.parseMix(raw));
    ok(res.rows.length >= 10 && res.title.startsWith("Mix") && res.last, "mix: " + res.rows.length + " rows");
    save("next_mix.json", raw, nextShape);
  });

  const players = [
    ["player_ok.json", IDS.ok, sts, (p) => p.playable === true && !p.stale],
    ["player_stale.json", IDS.ok, null, (p) => p.stale && p.playable === null],
    ["player_private.json", IDS.private, sts, (p) => p.overlay === "Private video"],
    ["player_age.json", IDS.age, sts, (p) => p.overlay === "Age-restricted"],
    ["player_removed.json", IDS.removed, sts, (p) => p.overlay === "Removed"],
  ];
  await t.step("player", async () => {
    players.push(["player_live.json", manifest.live, sts, (p) => p.live && p.overlay === "Live"]);
    for (const [name, id, s, check] of players) {
      const raw = await post(client, "player", P.playerBody(client, id, s));
      const res = look(name, P.parsePlayer(raw));
      ok(check(res), name + ": " + JSON.stringify({ status: res.status, playable: res.playable, overlay: res.overlay, stale: res.stale }));
      save(name, raw, playerShape(raw));
    }
  });

  await t.step("suggestions, resolve and account", async () => {
    const suggest = await getText(P.suggestUrl(client, IDS.suggest));
    ok(P.parseSuggest(suggest).length >= 1, "suggest: none");
    save("suggest.txt", suggest);
    const resolved = await post(client, "navigation/resolve_url", P.resolveBody(client, IDS.handle));
    ok(P.parseResolve(resolved) === IDS.channel, "resolve: " + P.parseResolve(resolved));
    save("resolve.json", resolved);
    const account = await post(client, "account/account_menu", P.accountBody(client));
    ok(P.parseAccount(account) === null, "account: signed out must give null");
    save("account.json", account);
  });

  if (RECORD) {
    Deno.writeTextFileSync(new URL("manifest.json", FIXTURES), JSON.stringify(manifest, null, 1) + "\n");
    console.log("wrote\n  " + written.join("\n  "));
  }
  if (drift.size) console.warn("drift:\n  " + [...drift].join("\n  "));
});

// ---- discovery

// What the discovery fixtures come from; parse_test.js reads them back from discovery.json.
const DISCOVERY = {
  genre: "classical",
  top: P.searchParams({ type: "videos", duration: "medium" }),
  week: P.searchParams({ type: "videos", duration: "medium", upload: "week" }),
  month: P.searchParams({ type: "videos", duration: "medium", upload: "month" }),
  channels: "classical pianist",
  mix: "EvNsPyp5O2I",
};
const EIGHT = 8;
const DMAX = 80 * 1024;
const GAP = 3000;

let lastAsk = 0;
async function ask(client, endpoint, body) {
  const wait = lastAsk + GAP - Date.now();
  if (wait > 0) await new Promise((done) => setTimeout(done, wait));
  try {
    return await post(client, endpoint, body);
  } finally {
    lastAsk = Date.now();
  }
}

let discoveryClient = null;
async function signedOutClient() {
  if (!discoveryClient) {
    const swText = await getText(BASE + "/sw.js_data");
    const { client, drift } = P.clientFrom(swText, "en-US");
    ok(drift.length === 0, "sw.js_data: " + drift.join(", "));
    learnSecrets(swText, client);
    discoveryClient = { ...client, hl: "en", gl: "US" };
    lastAsk = Date.now();
  }
  return discoveryClient;
}

// Beside flavor(): verified artists, Topic channels, channels without a handle and bylines without a channel.
const discoveryFlavor = (x) => {
  const s = JSON.stringify(x);
  return [
    flavor(x),
    /VERIFIED_ARTIST/.test(s) && "artist",
    / - Topic"/.test(s) && "topic",
    /"canonicalBaseUrl":"\/channel\//.test(s) && "no handle",
    !/browseEndpoint/.test(s) && "no channel",
  ].filter(Boolean).join("+");
};

// 8 items in all, ads left out: the first of each flavor, then the next ones, in page order.
function firstEight(data) {
  const keyOf = (x) => (x && typeof x === "object" && !Array.isArray(x) ? itemKey(x) : "");
  const items = [];
  const walk = (node, key) => {
    if (Array.isArray(node)) {
      for (const x of node) {
        if (CAP_LISTS.has(key) && CAPPED.has(keyOf(x)) && !ADS.has(keyOf(x))) items.push(x);
        walk(x, key);
      }
    } else if (node && typeof node === "object") for (const [k, v] of Object.entries(node)) walk(v, k);
  };
  walk(data, "");
  const keep = new Set();
  const sigs = new Set();
  for (const x of items) {
    const sig = keyOf(x) + ":" + discoveryFlavor(x[keyOf(x)]);
    if (!sigs.has(sig) && keep.size < EIGHT) { sigs.add(sig); keep.add(x); }
  }
  for (const x of items) if (keep.size < EIGHT) keep.add(x);
  const prune = (node, key) => {
    if (Array.isArray(node)) {
      const kept = CAP_LISTS.has(key) ? node.filter((x) => !ADS.has(keyOf(x)) && (!CAPPED.has(keyOf(x)) || keep.has(x))) : node;
      return kept.map((x) => prune(x, key));
    }
    if (node && typeof node === "object") return Object.fromEntries(Object.entries(node).map(([k, v]) => [k, prune(v, k)]));
    return node;
  };
  return prune(data, "");
}

function saveDiscovery(name, data, shape = (x) => x) {
  if (!RECORD) return;
  const text = scrub(JSON.stringify(trim(firstEight(shape(data)), EIGHT)));
  const size = new TextEncoder().encode(text).length;
  ok(size <= DMAX, name + " is " + size + " bytes after trimming:" + heaviest(text));
  Deno.writeTextFileSync(new URL(name, FIXTURES), text);
  written.push(name + " " + size);
}

// The Mix: only the playlist panel and the seed parseMix reads.
const mixShape = (j) => ({
  contents: { twoColumnWatchNextResults: { playlist: j.contents?.twoColumnWatchNextResults?.playlist } },
  currentVideoEndpoint: j.currentVideoEndpoint,
});

const rulesFixture = () => JSON.parse(Deno.readTextFileSync(new URL("rules.json", FIXTURES)));
const videos = (res) => res.rows.filter((r) => r.kind === "video");

Deno.test({ name: "live: discovery fixtures", ignore: !LIVE, sanitizeResources: false }, async (t) => {
  const client = await signedOutClient();
  P.loadRules(rulesFixture());
  const genre = P.GENRES.find((g) => g.key === DISCOVERY.genre);
  const manifest = { recorded: new Date().toISOString().slice(0, 10), clientVersion: client.clientVersion,
    ...DISCOVERY, query: genre.query };
  const drift = [];
  const look = (name, res) => {
    drift.push(...(res.drift || []).map((d) => name + ": " + d));
    return res;
  };

  let more = null;
  await t.step("a genre's Top", async () => {
    const raw = await ask(client, "search", P.searchBody(client, genre.query, DISCOVERY.top));
    const res = look("discovery_top", P.parseSearch(raw));
    const rows = videos(res);
    ok(rows.length >= 10 && res.more, "top: " + rows.length + " videos");
    const pictures = rows.filter((r) => /^https:\/\/yt\d\.ggpht\.com\//.test(r.avatar)).length;
    ok(pictures >= rows.length * 0.9, "top: " + pictures + " of " + rows.length + " rows have a channel picture");
    more = res.more;
    saveDiscovery("discovery_top.json", raw);
  });

  await t.step("its next page", async () => {
    const raw = await ask(client, "search", P.moreBody(client, more));
    const res = look("discovery_more", P.parseMore(raw));
    ok(res.known && videos(res).length >= 5 && res.more, "more: " + videos(res).length + " videos");
    saveDiscovery("discovery_more.json", raw);
  });

  await t.step("This week", async () => {
    const raw = await ask(client, "search", P.searchBody(client, genre.query, DISCOVERY.week));
    const res = look("discovery_week", P.parseSearch(raw));
    const rows = videos(res);
    ok(rows.length >= 1, "week: no videos");
    const old = rows.filter((r) => /month|year/.test(r.age));
    ok(old.length === 0, "week: " + old.map((r) => r.age).join(", "));
    saveDiscovery("discovery_week.json", raw);
  });

  await t.step("Channels", async () => {
    const raw = await ask(client, "search", P.searchBody(client, DISCOVERY.channels, P.searchParams({ type: "channels" })));
    const res = look("discovery_channels", P.parseSearch(raw));
    const rows = res.rows.filter((r) => r.kind === "channel");
    ok(rows.length >= 10, "channels: " + rows.length + " rows");
    ok(rows.some((r) => r.artist), "channels: no verified artist");
    saveDiscovery("discovery_channels.json", raw);
  });

  await t.step("a Mix", async () => {
    const raw = await ask(client, "next", P.mixBody(client, DISCOVERY.mix));
    const res = look("discovery_mix", P.parseMix(raw));
    ok(res.rows.length >= 10 && res.rows[0].id === DISCOVERY.mix && res.last, "mix: " + res.rows.length + " rows");
    manifest.mixChannel = res.rows[0].channelId;
    saveDiscovery("discovery_mix.json", raw, mixShape);
  });

  if (RECORD) {
    Deno.writeTextFileSync(new URL("discovery.json", FIXTURES), JSON.stringify(manifest, null, 1) + "\n");
    console.log("wrote\n  " + written.join("\n  "));
  }
  if (drift.length) console.warn("drift:\n  " + drift.join("\n  "));
});

// §2.3: page 1 of every genre, Top and New (This month when This week has under 6 counted rows).
Deno.test({ name: "live: discovery release gate", ignore: !LIVE, sanitizeResources: false }, async () => {
  const client = await signedOutClient();
  P.loadRules(rulesFixture());
  const lines = [];
  const short = [];
  for (const g of P.GENRES) {
    for (const [sort, params] of [["Top", DISCOVERY.top], ["New", DISCOVERY.week]]) {
      let rows = videos(P.parseSearch(await ask(client, "search", P.searchBody(client, g.query, params))));
      let when = "";
      if (sort === "New" && P.count(rows, g.query) < 6) {
        rows = videos(P.parseSearch(await ask(client, "search", P.searchBody(client, g.query, DISCOVERY.month))));
        when = " (month)";
      }
      const first = rows.slice(0, 20);
      const rec = first.filter((r) => P.isRecording(r, g.query)).length;
      const pianists = P.pianists(first, g.query).length;
      lines.push(g.key + " " + sort + when + ": " + rec + "/" + first.length + " recordings, " + pianists + " pianists");
      if (rec < 12 || pianists < 5) {
        short.push(g.key + " " + sort);
        const why = (r) => (r.live || r.upcoming ? "live" : r.members ? "members" : r.playable === false ? "unplayable" :
          P.long(r) ? "long" : (r.seconds || 0) < 60 ? "short" : "dimmed");
        for (const r of first.filter((r) => !P.isRecording(r, g.query))) lines.push("  " + why(r) + ": " + r.title.slice(0, 70));
      }
    }
  }
  console.log("discovery gate\n  " + lines.join("\n  "));
  ok(short.length === 0, "under 12/20 recordings or 5 pianists: " + short.join(", "));
});
