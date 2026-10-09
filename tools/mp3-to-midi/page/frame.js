// The dock's /watch frame (docs/design/youtube-window.md §1.7, §3.6). browse.py adds this script to every
// document the WebView creates; it acts only in a child /watch frame on www.youtube.com, which is the dock's.
// It makes YouTube's player fill the frame and hides the rest, holds a prewarmed video muted and paused, and
// gets ads out of the way. The page drives the player itself through #movie_player and the flags on window.
(() => {
  "use strict";
  if (window.top === window || location.hostname !== "www.youtube.com" || location.pathname !== "/watch" || window.__qmFrame) return;
  window.__qmFrame = true;
  const search = new URLSearchParams(location.search);

  // ---- player CSS
  const sheet = new CSSStyleSheet();
  sheet.replaceSync([
    "html,body{overflow:hidden!important;background:#000!important}",
    "#masthead-container,#secondary,#below,#chat,ytd-popup-container,tp-yt-iron-overlay-backdrop,ytd-enforcement-message-view-model,#cinematics,#cinematics-container{display:none!important}",
    "#movie_player{position:fixed!important;inset:0!important;width:100vw!important;height:100vh!important;z-index:2147483647!important;background:#000!important}",
    "#movie_player .html5-video-container{position:absolute!important;inset:0!important;width:100%!important;height:100%!important}",
    "ytd-app,#page-manager{background:#000!important;margin-top:0!important}",
    "#movie_player video{width:100%!important;height:100%!important;left:0!important;top:0!important;object-fit:contain}",
    ".ytp-chrome-top,.ytp-chrome-bottom,.ytp-gradient-top,.ytp-gradient-bottom,.ytp-ce-element,.ytp-endscreen-content,.ytp-pause-overlay,.ytp-paid-content-overlay,.iv-branding,.ytp-watermark,.branding-img-container{display:none!important}",
    "#movie_player.ad-showing video,#movie_player.ad-showing .ytp-ad-player-overlay-layout,#movie_player.ad-showing .ytp-ad-player-overlay,#movie_player.ad-showing .ytp-ad-module{opacity:0!important}",
    // That opacity puts Skip under the video's own z-index, where the trusted press would hit the video.
    "#movie_player.ad-showing .html5-video-container{z-index:auto!important}",
    // During an ad nothing of the player but Skip takes pointer input, so a layer YouTube lays over Skip, such as
    // .ytp-iv-video-content, neither hides it from the hit tests nor catches its press.
    "#movie_player.ad-showing *{pointer-events:none!important}",
    "#movie_player.ad-showing .ytp-skip-ad-button,#movie_player.ad-showing .ytp-skip-ad-button *,#movie_player.ad-showing .ytp-ad-skip-button,#movie_player.ad-showing .ytp-ad-skip-button *,#movie_player.ad-showing .ytp-ad-skip-button-modern,#movie_player.ad-showing .ytp-ad-skip-button-modern *{pointer-events:auto!important}",
    // A frame under 120 px tall, the narrow dock's, lays Skip out above its top, out of the press's reach.
    "@media (max-height:120px){#movie_player.ad-showing .ytp-skip-ad-button,#movie_player.ad-showing .ytp-ad-skip-button,#movie_player.ad-showing .ytp-ad-skip-button-modern{position:fixed!important;left:0!important;top:0!important;right:auto!important;bottom:auto!important;margin:0!important}}",
  ].join("\n"));
  const adopt = () => {
    if (!document.adoptedStyleSheets.includes(sheet)) document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];
  };
  adopt();

  // ---- hold: a prewarmed video stays muted and paused until the page sets __qmHold to false. A pause the
  // user asked for (__qmUserPaused, set by the page) holds the same way, so autonav cannot start a video.
  // Releasing either goes to release() in the ads part.
  const flag = (name, value) => {
    let on = value;
    Object.defineProperty(window, name, {
      configurable: true,
      get: () => on,
      set: (next) => {
        on = next;
        if (!next) release();
      },
    });
  };
  flag("__qmHold", search.get("qmhold") === "1");
  flag("__qmUserPaused", false);
  const play = HTMLMediaElement.prototype.play;
  HTMLMediaElement.prototype.play = function () {
    if (window.__qmHold) {
      this.muted = true;
      this.pause();
      return Promise.resolve();
    }
    if (window.__qmUserPaused) {
      this.pause();
      return Promise.resolve();
    }
    return play.call(this);
  };

  // A frame YouTube sends to accounts is an age-restricted video (browse.py cancels it; the page marks it).
  // The passive sign-in check a fresh profile makes goes there too, and says nothing about the video.
  if (window.navigation) {
    window.navigation.addEventListener("navigate", (e) => {
      try {
        const url = new URL(e.destination.url);
        if (/^accounts\./.test(url.hostname) && url.searchParams.get("passive") !== "true") window.__qmSent = "accounts";
      } catch { /* not a URL */ }
    });
  }

  // ---- ads. Video ads are cut short rather than stripped: stripping them, as Brave and uBlock do, made
  // YouTube hold each video back 5 to 13 seconds, its penalty for ad blockers. An ad plays muted and unseen
  // at 16x. YouTube ignores a scripted click on Skip, so once Skip shows the page presses it with trusted input
  // (the Skip part below); the scripted click stays for a player that takes one. When an ad stalls for a second,
  // its stream held back by YouTube, the video is loaded again at the same time, at most twice, which starts it
  // sooner than waiting; loading it at once brought longer ads. A paused ad that has data within 3 seconds of a
  // release is still starting, not stalled: it is played once more and not loaded again. An ad parked at its end,
  // its time within 0.3 s of its duration and not moving, neither paused nor ended, is a stall too, settling or
  // not. So is the video when an ad has ended and YouTube keeps it unstarted, playing with no data: waited out,
  // that took 10 to 14 seconds; loaded again, it played within a second. The loop runs while the video plays or
  // an unheld ad shows, and for a few seconds after a pause, when YouTube's ad blocker warning may have paused it.
  let saved = null; // the video's muted and rate from before the ad
  let adTime = -1; // the ad's time at the last tick
  let afterAd = false; // an ad ended and the video has had no data since
  let id = ""; // the video, read while no ad shows
  let lastTime = 0;
  let reloads = 0;
  let reloadedAt = -1e9;
  let stalledAt = 0;
  let releasedAt = -1e9; // the last release of the hold or a user pause
  let retried = false; // the ad was played again after that release
  let resumed = false;
  let sized = null;
  let quietSince = 0;
  let timer = 0;
  const SKIP = ".ytp-skip-ad-button, .ytp-ad-skip-button, .ytp-ad-skip-button-modern";
  // Skip is laid out; the short frame's fixed Skip has no offsetParent.
  const shown = (el) => !!el && el.getClientRects().length > 0;
  const own = (next) => {
    if (next === id) return;
    const want = window.__qmWant; // {id, start}: what the page loaded last
    id = next;
    lastTime = want && want.id === id ? want.start || 0 : id === search.get("v") ? Number.parseFloat(search.get("t")) || 0 : 0;
    reloads = 0;
    resumed = false;
    afterAd = false;
  };
  const tick = () => {
    const player = document.getElementById("movie_player");
    const video = player && player.querySelector("video");
    if (!video || typeof player.getVideoData !== "function") return;
    adopt();
    if (sized !== player) {
      sized = player;
      window.dispatchEvent(new Event("resize"));
    }
    const now = performance.now();
    const ad = player.classList.contains("ad-showing");
    if (!ad) {
      const data = player.getVideoData() || {};
      if (data.video_id) own(data.video_id);
      const state = player.getPlayerState();
      if (state === 1 || state === 2 || state === 3) lastTime = player.getCurrentTime() || lastTime;
    } else {
      // An ad before the video was read: the page's last load says which video it is for.
      const want = window.__qmWant;
      const next = (want && want.id) || (!id && search.get("v"));
      if (next) own(next);
    }
    const held = window.__qmHold || window.__qmUserPaused;
    let stalled = false;
    let settling = false;
    if (ad) {
      if (!saved) saved = { muted: video.muted, rate: video.playbackRate };
      video.muted = true;
      if (video.playbackRate !== 16) video.playbackRate = 16;
      const skip = document.querySelector(SKIP);
      if (shown(skip)) skip.click();
      const parked = !video.paused && Number.isFinite(video.duration) && video.duration - video.currentTime <= 0.3 &&
        Math.abs(video.currentTime - adTime) < 0.05;
      adTime = video.currentTime;
      stalled = !held && (video.paused || video.ended || !Number.isFinite(video.duration) || parked);
      settling = now - releasedAt < 3000 && Number.isFinite(video.duration) && video.readyState >= 2 && !video.ended && !parked;
      if (settling && !held && video.paused && now - releasedAt > 1000 && !retried) {
        retried = true;
        play.call(video).catch(() => {});
        player.playVideo();
      }
    } else {
      if (saved) {
        stalledAt = 0;
        adTime = -1;
        afterAd = true;
        video.muted = saved.muted;
        video.playbackRate = saved.rate;
        saved = null;
      }
      if (video.readyState >= 2 || player.getPlayerState() !== -1) afterAd = false;
      stalled = afterAd && !held && !video.paused && !video.ended;
    }
    if (!stalled || settling) stalledAt = 0;
    else if (!stalledAt) stalledAt = now;
    if (id && reloads < 2 && now - reloadedAt > 1500 && stalledAt && now - stalledAt > 1000) {
      reloads++;
      reloadedAt = now;
      stalledAt = 0;
      player.loadVideoById({ videoId: id, startSeconds: lastTime });
    }
    // YouTube's ad blocker warning pauses the video; the warning is hidden and the video resumed, once.
    if (!resumed && !ad && !held && video.paused && !video.ended && document.querySelector("ytd-enforcement-message-view-model")) {
      resumed = true;
      video.play();
    }
    // A held or paused ad waits too: the release wakes the loop. A held video needs no warning watch.
    if ((ad && !held) || !video.paused) quietSince = 0;
    else if (!quietSince) quietSince = now;
    else if (held || now - quietSince > 3000) {
      clearInterval(timer);
      timer = 0;
      quietSince = 0;
    }
  };
  const wake = () => {
    if (!timer) timer = setInterval(tick, 200);
  };
  // A release wakes the loop, which a held ad waits without, and starts a held ad: the hold swallowed
  // YouTube's play, so the ad would stay paused and look stalled.
  const release = () => {
    wake();
    releasedAt = performance.now();
    retried = false;
    stalledAt = 0;
    const player = document.getElementById("movie_player");
    const video = player && player.querySelector("video");
    if (!video || !player.classList.contains("ad-showing") || !video.paused || video.ended) return;
    if (window.__qmHold || window.__qmUserPaused) return;
    video.muted = true;
    play.call(video).catch(() => {});
    if (typeof player.playVideo === "function") player.playVideo();
  };
  // ---- Skip, pressed by the page with trusted input (§3.6). __qmSkip() says where Skip is, in this frame's CSS
  // px, while an unheld ad shows it and it is the element at its own centre, and which ad it belongs to, so the
  // page presses each ad's Skip once; otherwise null.
  const skipAt = () => {
    const player = document.getElementById("movie_player");
    const video = player && player.querySelector("video");
    if (!video || !player.classList.contains("ad-showing") || window.__qmHold || window.__qmUserPaused) return null;
    const skip = document.querySelector(SKIP);
    if (!shown(skip)) return null;
    const r = skip.getBoundingClientRect();
    const x = r.left + r.width / 2;
    const y = r.top + r.height / 2;
    if (r.width < 4 || r.height < 4 || x < 0 || y < 0 || x >= window.innerWidth || y >= window.innerHeight) return null;
    const hit = document.elementFromPoint(x, y);
    if (!hit || !skip.contains(hit)) return null;
    // Each ad of a pod has its own stream; its duration is no key, since it reads NaN for a moment at the ad's end.
    const data = (typeof player.getVideoData === "function" && player.getVideoData()) || {};
    const ad = (data.video_id || "") + "|" + (video.currentSrc || video.src || "");
    return { ad, x, y, width: window.innerWidth, height: window.innerHeight };
  };
  Object.defineProperty(window, "__qmSkip", { value: skipAt });
  // The page lets the frame take pointer input only for that press. Of trusted pointer input, only Skip during
  // an ad gets any, so a press that comes as the ad ends never pauses the video or opens the advertiser; and a
  // press never moves focus into the frame, where the page's keys would stop working. YouTube's own scripted
  // events are left alone. __qmClicks counts the trusted clicks that reached Skip, since a layout that moves after
  // browse.py's last look sends its press elsewhere: the page counts an ad as pressed only when this count went up.
  const onSkip = (target) => {
    const player = document.getElementById("movie_player");
    return !!player && player.classList.contains("ad-showing") && !!target && typeof target.closest === "function" && !!target.closest(SKIP);
  };
  let clicks = 0;
  Object.defineProperty(window, "__qmClicks", { get: () => clicks });
  for (const type of ["pointerdown", "mousedown", "pointerup", "mouseup", "click", "dblclick", "auxclick", "contextmenu"]) {
    window.addEventListener(type, (e) => {
      if (!e.isTrusted) return;
      if (type === "mousedown") e.preventDefault();
      if (onSkip(e.target)) {
        if (type === "click") clicks++;
        return;
      }
      e.preventDefault();
      e.stopImmediatePropagation();
    }, true);
  }
  // The frame never takes focus (§1.7). YouTube's player focuses itself as it starts, which would take the page's
  // keys, and with the window in the background the page hears nothing of what had focus before it.
  for (const proto of [HTMLElement.prototype, SVGElement.prototype]) proto.focus = function () {};
  window.focus = () => {};
  // Focus the browser still moves here, with no script asking, is told to the page, which gives it back.
  window.addEventListener("focus", () => {
    const owner = window.frameElement;
    if (owner) owner.dispatchEvent(new Event("qm-focus"));
  }, true);
  for (const type of ["loadstart", "play", "playing", "waiting", "pause"]) document.addEventListener(type, wake, true);
  document.addEventListener("DOMContentLoaded", wake);
  wake();
})();
