"""YouTube in a window of its own, shaped for finding piano recordings to convert.

The window's own page (page/) is served at https://www.youtube.com/quartzmidi
inside a pywebview (WebView2) window on the same profile as signin.py, so its
InnerTube requests, the signed-in cookies and the /watch player frame are all
same-origin, and nothing depends on YouTube's markup. If that page does not
mount, its scripts are run on YouTube's robots.txt instead (the canvas), and
failing that a local page still converts a pasted link.

    python browse.py --hello 2   QuartzMIDI's protocol 2: JSON lines both ways
    python browse.py             protocol 1, for a QuartzMIDI from before the redesign

Lines on stdout, protocol 2:

    hello: 2                     the first line
    qm: <JSON object>            pick, drop, order, dest, resume, choose-folder, signed or mount
    drift: <text>                YouTube answered in a shape the page did not expect
    script error: <text>         the page threw

Lines on stdout, protocol 1:

    pick: <video link>           convert this video; a Mix or a selection sends one line per video
    pick-list: <playlist link>   convert this playlist
    pick-channel: <channel link> convert this channel's piano videos
    signed: <path of cookies.txt>

Both:

    done: closed                 the window was closed
    error: <reason for stopping>

In protocol 2, stdin carries one JSON object per line from QuartzMIDI (init,
theme, folders, item, child, paused, resumed, signed, search and signin). Each
is passed to the page's qm.receive(), and init's search words, once the page is
on its way, as a search of their own; the end of stdin closes the window.
"""

import argparse
import base64
import ctypes
import json
import math
import os
import re
import subprocess
import sys
import threading
import time
import traceback
from collections import OrderedDict
from ctypes import wintypes
from datetime import datetime
from html.parser import HTMLParser
from urllib.parse import parse_qs, quote, quote_plus, unquote, urlencode, urlparse, urlsplit
from xml.etree import ElementTree

# The embedded Python's ._pth file leaves the script's own folder off the path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert import CHANNEL, FolderName, converted, ledger_records, parse_dest
from piano_match import rules_json
from signin import COOKIES, PROFILE, SIGNED_IN, app_window, placement, write_cookies

TITLE = "YouTube for QuartzMIDI"
HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "page")
# Three folders up: the QuartzMIDI folder in a release, the checkout in a dev tree.
ROOT = os.path.dirname(os.path.dirname(HERE))
ORIGIN = "https://www.youtube.com"
BASE = "/quartzmidi"
CANVAS = ORIGIN + "/robots.txt"
CONSENT_PAGE = "https://consent.youtube.com/m?continue="
SIGN_IN_TITLE = "Sign in to YouTube"
SIGN_IN = "https://accounts.google.com/ServiceLogin?service=youtube&continue=https://www.youtube.com/signin_passive"
# Signing out forgets every cookie these sites hold.
SIGNED_OUT = ("https://www.youtube.com", "https://accounts.google.com", "https://www.google.com")
WATCH = "https://www.youtube.com/watch?v="
PLAYLIST = "https://www.youtube.com/playlist?list="
HOSTS = {"youtube.com", "www.youtube.com", "m.youtube.com", "music.youtube.com"}
CONSENT = {"consent.youtube.com", "consent.google.com"}
ACCOUNTS = {"accounts.google.com", "accounts.youtube.com"}
VIDEO = re.compile(r"[A-Za-z0-9_-]{11}")
LIST = re.compile(r"[A-Za-z0-9_-]{2,64}")
CHANNEL_ID = re.compile(r"UC[A-Za-z0-9_-]{22}")
FONT = re.compile(r"fonts/[A-Za-z0-9_-]+\.ttf")
COLOUR = re.compile(r"#[0-9A-Fa-f]{6}(?:[0-9A-Fa-f]{2})?")
# ElasticOverscroll repeats pywebview's own flag; the autoplay policy lets the
# dock's player start before the page has had a click.
BROWSER_ARGUMENTS = "--disable-features=ElasticOverscroll --autoplay-policy=no-user-gesture-required"

CSP = ("default-src 'self'; script-src 'self' 'unsafe-eval'; style-src 'self' 'unsafe-inline'; "
       "img-src 'self' https://i.ytimg.com https://yt3.ggpht.com https://yt3.googleusercontent.com data: blob:; "
       "connect-src 'self' https://suggestqueries-clients6.youtube.com; frame-src 'self'; font-src 'self'; "
       "media-src 'self' blob:")
HEADERS = {"Content-Security-Policy": CSP, "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff"}
TYPES = {".js": "text/javascript", ".css": "text/css", ".ttf": "font/ttf", ".ico": "image/x-icon"}
SERVED = {"app.js", "parse.js", "frame.js", "app.css", "QuartzMIDI.ico"}
FONTS = (("IBMPlexSans-Regular.ttf", 400), ("IBMPlexSans-Medium.ttf", 500), ("IBMPlexSans-SemiBold.ttf", 600))

KINDS = ("video", "playlist", "channel", "mix", "selection", "link")
LISTS = ("playlist", "channel", "mix", "selection")
ARTISTS_MOST = 300  # artists.json: the pianists converted from most recently
SEEDS_MOST = 20     # and the videos converted most recently
# Messages for the page that are acted on once and never replayed to a later page.
ONCE = {"search", "signin"}
# Skip, pressed with trusted input through the DevTools Protocol (§3.6, §8): a press is one left click, sent only
# where the browser's own hit test finds YouTube's Skip in the dock's /watch frame, checked again just before the
# button goes down. An ask is let through at most every PRESS_GAP seconds, under the page's 250 ms poll, so a
# refused press is asked again on the next poll; at most PRESS_MOST presses a minute put the button down.
SKIP = ".ytp-skip-ad-button, .ytp-ad-skip-button, .ytp-ad-skip-button-modern"  # frame.js's SKIP
# Called on the node the hit test found, with SKIP: an element, a text node (its parent) or a ::before or ::after
# (a CSSPseudoElement, its element).
ON_SKIP = ("function (skip) {"
           " const e = this.nodeType === 1 ? this : this.nodeType ? this.parentElement : this.element;"
           " return !!e && e.nodeType === 1 && !!e.closest(skip); }")
PRESS_GAP = 0.2
PRESS_SPAN = 60.0
PRESS_MOST = 10
PRESS_GROUP = "quartzmidi-skip"  # the DevTools object group a press's node handles are kept in
DEVTOOLS_WAIT = 2000  # ms for one DevTools call

# The app's built-in dark skin (Blue Dark), until init brings the one in use.
DEFAULT_SKIN = {
    "dark": True,
    "colours": {
        "canvas": "#191C21", "structure": "#20242A", "card": "#272B31", "elevated": "#2E333A",
        "elevatedHot": "#363C44", "recessed": "#14171B", "inkPrimary": "#E7E9EC", "inkSecondary": "#A3AAB5",
        "inkTertiary": "#79818D", "accent": "#4EA3EA", "ok": "#4FB277", "okInk": "#8FDCAA", "warn": "#D0A259",
        "bad": "#E0685A", "accentSoft": "#4EA3EA24", "accentLine": "#4EA3EA66", "okSoft": "#4FB27729",
        "okBorder": "#4FB27766", "hairline": "#FFFFFF14", "strong": "#FFFFFF24", "topHighlight": "#FFFFFF0F",
        "contact": "#0000004D", "ambient": "#00000038", "inner": "#00000073",
    },
    "shape": {
        "scale": 1, "controlHeight": 32, "textBody": 14, "textMeta": 12, "textHeading": 14, "textTitle": 20,
        "radiusWindow": 12, "radiusCard": 12, "radiusControl": 10, "radiusElement": 8, "windowPad": 16,
        "panelPad": 16, "contactOffset": 1, "contactBlur": 1, "ambientOffset": 2, "ambientBlur": 6,
        "innerOffset": 1, "innerBlur": 2,
    },
}


SAYING = threading.Lock()


def say(kind, text):
    # Picks arrive on several threads; each line is written whole.
    with SAYING:
        sys.stdout.write(f"{kind}: {text}\n")
        sys.stdout.flush()


def tell(message):
    say("qm", json.dumps(message, separators=(",", ":"), ensure_ascii=False))


def later(target, *args, seconds=0):
    """Runs target on a thread of its own, after seconds, so a GUI handler never waits."""
    if seconds:
        timer = threading.Timer(seconds, target, args)
    else:
        timer = threading.Thread(target=target, args=args)
    timer.daemon = True
    timer.start()


def guarded(handler):
    """A WebView2 event handler that prints its failure instead of passing it to .NET."""
    def call(*args):
        try:
            handler(*args)
        except Exception:
            traceback.print_exc()
    return call


def finite(value):
    # An int is always finite, and one too large for a float must not throw.
    if isinstance(value, bool):
        return False
    return isinstance(value, int) or (isinstance(value, float) and math.isfinite(value))


def clean(url, playlist):
    """A canonical watch or playlist link, or None for anything else."""
    try:
        parts = urlparse(str(url))
    except ValueError:
        return None
    host = (parts.hostname or "").lower()
    query = parse_qs(parts.query)
    if playlist:
        if host not in HOSTS:
            return None
        found = (query.get("list") or [""])[0]
        # A Mix (RD...) is made up per viewer and cannot be read as a playlist.
        return f"https://www.youtube.com/playlist?list={found}" if LIST.fullmatch(found) and not found.startswith("RD") else None
    if host == "youtu.be":
        found = parts.path.strip("/")
    elif host in HOSTS and parts.path == "/watch":
        found = (query.get("v") or [""])[0]
    else:
        return None
    return f"https://www.youtube.com/watch?v={found}" if VIDEO.fullmatch(found) else None


def channel_videos(url):
    """A channel's Videos tab, or None for a link that is not a channel's."""
    found = CHANNEL.match(str(url))
    return found.group(0) + "/videos" if found else None


def is_page(path):
    return path == BASE or path.startswith(BASE + "/")


def form(params):
    """"?" and the params encoded as URLSearchParams encodes them, so a route is the
    same string here and in parse.js."""
    return "?" + "&".join(f"{name}={quote_plus(value, safe='*').replace('~', '%7E')}"
                          for name, value in params.items())


def route_for(url):
    """The window's route for a YouTube link ("?v=ID", "?list=ID&v=ID", "?channel=..."
    or "?q=words"), or None for anything else. QMParse.routeFor in parse.js is its twin."""
    try:
        parts = urlparse(str(url).strip())
        host = (parts.hostname or "").lower()
    except ValueError:
        return None
    if parts.scheme.lower() not in ("http", "https"):
        return None
    query = parse_qs(parts.query)

    def first(name):
        return (query.get(name) or [""])[0]

    segments = parts.path.split("/") + ["", ""]
    found = first("list")
    if host == "youtu.be":
        video = segments[1]
    elif host not in HOSTS:
        return None
    elif parts.path == "/watch":
        video = first("v")
    elif segments[1] in ("shorts", "live"):
        video, found = segments[2], ""
    elif parts.path == "/playlist":
        # A Mix is made up per viewer and has no playlist page of its own.
        ok = LIST.fullmatch(found) and not found.startswith("RD")
        return form({"list": found}) if ok else None
    elif parts.path == "/results":
        words = first("search_query").strip()
        return form({"q": words}) if words else None
    else:
        head, name = segments[1], unquote(segments[2])
        if head.startswith("@") and len(head) > 1:
            channel = unquote(head)
        elif head == "channel" and CHANNEL_ID.fullmatch(name):
            channel = name
        elif head in ("c", "user") and name:
            channel = f"{head}/{name}"
        else:
            return None
        return form({"channel": channel})
    if not VIDEO.fullmatch(video):
        return None
    # A Mix in the link is ignored: the video is what was shared.
    if LIST.fullmatch(found) and not found.startswith("RD"):
        return form({"list": found, "v": video})
    return form({"v": video})


def guard(url, canvas=False):
    """What a top-frame navigation does: "allow", "route" (to route_for), "signin",
    "external" (the default browser) or "cancel"."""
    try:
        parts = urlparse(str(url).strip())
        host = (parts.hostname or "").lower()
    except ValueError:
        return "cancel"
    scheme = parts.scheme.lower()
    if scheme not in ("http", "https"):
        return "cancel"
    if scheme == "https" and host == "www.youtube.com" and (
            is_page(parts.path) or (canvas and parts.path == "/robots.txt")):
        return "allow"
    if route_for(url):
        return "route"
    if host in CONSENT:
        return "allow"  # the user answers it; nothing here clicks its buttons
    if host in ACCOUNTS:
        return "signin"
    return "external"


def youtube_url(route):
    """The YouTube page that shows what a route shows, for "Open on YouTube"."""
    query = parse_qs(str(route).lstrip("?"))

    def first(name):
        return (query.get(name) or [""])[0]

    video, found, mix, channel, words = first("v"), first("list"), first("mix"), first("channel"), first("q").strip()
    similar = first("similar")
    if VIDEO.fullmatch(video):
        return WATCH + video + (f"&list={found}" if LIST.fullmatch(found) else "")
    if LIST.fullmatch(found):
        return PLAYLIST + found
    if VIDEO.fullmatch(mix):
        return f"{WATCH}{mix}&list=RD{mix}"
    if CHANNEL_ID.fullmatch(channel):
        return f"{ORIGIN}/channel/{channel}"
    if channel.startswith(("@", "c/", "user/")):
        return f"{ORIGIN}/{quote(channel, safe='@/')}"
    if VIDEO.fullmatch(similar):
        return f"{WATCH}{similar}&list=RD{similar}"  # the Mix that Similar reads
    if words:
        return f"{ORIGIN}/results?" + urlencode({"search_query": words})
    # Home, and a genre, whose words only parse.js knows.
    return ORIGIN + "/"


def valid_dest(dest):
    """Whether dest is a folder in the MIDI folder as §5.4 has it: "" for the MIDI folder itself, or up to four
    names joined by "/", each one Windows keeps as written. convert.py's check, so both refuse the same."""
    return parse_dest(dest) is not None


def valid_artist(artist):
    """None, or the channel id an artist's folder is kept under."""
    return artist is None or (isinstance(artist, str) and CHANNEL_ID.fullmatch(artist) is not None)


def pick_message(pick, seq):
    """The qm pick line's object for the page's pick, or None when the pick is malformed. A dest or artist
    that is not valid, or an artist without a dest, makes it malformed: it is refused, never rewritten."""
    if not isinstance(pick, dict) or pick.get("kind") not in KINDS:
        return None
    kind = pick["kind"]
    title, ident, url = pick.get("title") or "", pick.get("id") or "", pick.get("url") or ""
    folder, items, dest, artist = pick.get("folder"), pick.get("items"), pick.get("dest"), pick.get("artist")
    if not all(isinstance(value, str) for value in (title, ident, url)) or len(title) > 300 or len(url) > 2048:
        return None
    if folder is not None and not isinstance(folder, str):
        return None
    if (dest is not None and not valid_dest(dest)) or not valid_artist(artist) or (artist is not None and dest is None):
        return None
    listed = []
    if items is not None:
        if not isinstance(items, list) or len(items) > 5000:
            return None
        for item in items:
            name = item.get("title") or "" if isinstance(item, dict) else None
            if not isinstance(name, str) or len(name) > 300 or not isinstance(item.get("id"), str) \
                    or not VIDEO.fullmatch(item["id"]):
                return None
            listed.append({"id": item["id"], "title": name})
    if listed and kind not in LISTS:
        return None
    rule = {"video": VIDEO, "mix": VIDEO, "playlist": LIST, "channel": CHANNEL_ID}.get(kind)
    if ident and rule and not rule.fullmatch(ident):
        return None
    if kind == "video":
        link = clean(url or WATCH + ident, False)
        key = link and "v:" + link[len(WATCH):]
    elif kind == "playlist":
        link = clean(url or PLAYLIST + ident, True)
        key = link and "p:" + link[len(PLAYLIST):]
    elif kind == "channel":
        link = f"{ORIGIN}/channel/{ident}/videos" if CHANNEL_ID.fullmatch(ident) else None
        key = link and "c:" + ident
    elif kind == "mix":
        link = "" if VIDEO.fullmatch(ident) and listed else None
        key = "m:" + ident
    elif kind == "selection":
        link = "" if listed else None
        key = f"s:{seq}"
    else:
        link = url.strip() if url.strip().lower().startswith(("http://", "https://")) else None
        key = link and "link:" + link
    if link is None or not key:
        return None
    message = {"type": "pick", "seq": seq, "key": key, "kind": kind, "url": link, "title": title,
               "again": pick.get("again") is True, "folder": FolderName(folder) if folder else None}
    if listed:
        message["items"] = listed
    # Only a page with Save to sends these; without a dest the app goes by folder, as before.
    if dest is not None:
        message["dest"] = dest
    if artist:
        message["artist"] = artist
    return message


def pick_refusal(pick):
    """Why a pick pick_message refused, when the page can say so: "dest" when its dest is the only thing
    wrong (the same pick into the MIDI folder would go), else None for the plain refusal."""
    if not isinstance(pick, dict) or pick.get("dest") is None or valid_dest(pick.get("dest")):
        return None
    return "dest" if pick_message(dict(pick, dest=""), 0) is not None else None


def old_lines(message):
    """The protocol 1 lines that ask an older QuartzMIDI for the same pick."""
    kind, url = message["kind"], message["url"]
    if kind == "playlist":
        return [("pick-list", url)]
    if kind == "channel":
        return [("pick-channel", url)]
    if kind in ("mix", "selection"):
        return [("pick", WATCH + item["id"]) for item in message.get("items", [])]
    return [("pick", url)]


def coalescing_key(message):
    """The key under which a newer message replaces a waiting one (§5.3), or None."""
    kind = message.get("type")
    if kind == "init":
        return "init"
    if kind == "item":
        return f"item:{message.get('key', '')}"
    if kind == "child":
        return f"child:{message.get('parent', '')}/{message.get('key', '')}"
    if kind in ("paused", "resumed"):
        return "pause"
    if kind in ("theme", "folders", "signed", "search", "signin"):
        return kind
    return None


class Mailbox:
    """QuartzMIDI's messages on their way to the page. They wait, coalesced by key,
    while no page can take them, and each page that mounts gets the latest of
    every kind first, since a new page knows nothing."""

    def __init__(self):
        self.ready = threading.Condition()
        self.latest = OrderedDict()   # the last message of each key, for the next page
        self.waiting = OrderedDict()  # what the current page has not had yet
        self.open = False
        self.ended = False
        self.count = 0

    def put(self, message):
        key = coalescing_key(message)
        with self.ready:
            if key is None:
                self.count += 1
                key = ("once", self.count)
            elif message.get("type") not in ONCE:
                self.latest[key] = message
                if key == "init":
                    self.latest.move_to_end(key, last=False)
            self.waiting[key] = message
            if key == "init":
                self.waiting.move_to_end(key, last=False)
            self.ready.notify_all()

    def put_back(self, message):
        """A one-shot the page did not take waits again, unless a newer one of its key came meanwhile."""
        key = coalescing_key(message)
        with self.ready:
            self.waiting.setdefault(key, message)
            self.ready.notify_all()

    def mount(self):
        with self.ready:
            waiting = OrderedDict(self.latest)
            for key, message in self.waiting.items():
                waiting.setdefault(key, message)
            self.waiting = waiting
            self.open = True
            self.ready.notify_all()

    def close(self):
        with self.ready:
            self.open = False

    def end(self):
        with self.ready:
            self.ended = True
            self.ready.notify_all()

    def take(self):
        """The next message for the page, waiting for one; None once the window is gone."""
        with self.ready:
            while not self.ended and not (self.open and self.waiting):
                self.ready.wait()
            return None if self.ended else self.waiting.popitem(last=False)[1]


def forget_cookie_file(path):
    """Deletes cookies.txt if signin.py wrote it; a file the user made is left alone."""
    try:
        with open(path, encoding="utf-8", errors="replace") as file:
            file.readline()
            mark = file.readline()
    except OSError:
        return False
    if not mark.startswith("# Written by signin.py"):
        return False
    try:
        os.remove(path)
        return True
    except OSError:
        return False


def read_bytes(path):
    try:
        with open(path, "rb") as file:
            return file.read()
    except OSError:
        return None


def read_text(path):
    data = read_bytes(path)
    return None if data is None else data.decode("utf-8", errors="replace")


def asset_path(name):
    """The file for page/<name>. A dev tree has no copies of the fonts and the icon,
    so they come from where the release copies them from."""
    path = os.path.join(PAGE, *name.split("/"))
    if os.path.isfile(path):
        return path
    if name.startswith("fonts/"):
        path = os.path.join(ROOT, "third_party", "fonts", "ibm-plex-sans", name[len("fonts/"):])
    elif name == "QuartzMIDI.ico":
        path = os.path.join(ROOT, "ui", "QuartzMIDI.ico")
    else:
        return None
    return path if os.path.isfile(path) else None


def sprite_text():
    sprite = read_text(os.path.join(PAGE, "icons.svg")) or ""
    return re.sub(r"^\s*<\?xml[^>]*\?>", "", sprite).strip()


def page_html():
    """index.html with the icon sprite put inside <body>, or None without index.html."""
    page = read_text(os.path.join(PAGE, "index.html"))
    if page is None:
        return None
    body, sprite = re.search(r"<body[^>]*>", page, re.I), sprite_text()
    if body and sprite:
        page = page[:body.end()] + "\n" + sprite + page[body.end():]
    return page.encode("utf-8")


def converted_json():
    """{video id: .mid path} from the ledger, for the files that still exist."""
    records = ledger_records()
    found = {}
    for video in records:
        path = converted(records, video)
        if path:
            found[video] = path
    return json.dumps(found, ensure_ascii=False)


def ledger_time(record):
    """When a ledger record was written, as a timestamp; a record that does not say is the oldest."""
    try:
        return datetime.fromisoformat(record.get("at")).timestamp()
    except (TypeError, ValueError, OverflowError, OSError):
        return float("-inf")


def artists_json():
    """What Home starts from (§4.1), from the ledger and never with a path: {"artists": [{id, name, n, at,
    dest?}], "seeds": [{id, title, channelId}]}. Artists are the channels converted from, newest first, with
    the dest of their newest record that chose one for the artist; seeds are the videos converted most
    recently."""
    records = list(ledger_records().values())
    # Newest first; of two written at the same time, the later line.
    newest = sorted(range(len(records)), key=lambda index: (ledger_time(records[index]), index), reverse=True)
    artists, chosen, seeds = {}, set(), []
    for record in (records[index] for index in newest):
        video, title, channel = record["id"], record.get("title"), record.get("channelId")
        channel = channel if isinstance(channel, str) and CHANNEL_ID.fullmatch(channel) else None
        if len(seeds) < SEEDS_MOST and VIDEO.fullmatch(video):
            seeds.append({"id": video, "title": title if isinstance(title, str) else "", "channelId": channel})
        if channel is None or (channel not in artists and len(artists) >= ARTISTS_MOST):
            continue
        at, name = record.get("at"), record.get("channel")
        artist = artists.setdefault(channel, {"id": channel, "name": "", "n": 0,
                                              "at": at if isinstance(at, str) else ""})
        artist["n"] += 1
        if not artist["name"] and isinstance(name, str):
            artist["name"] = name
        if channel not in chosen and record.get("artist") is True:
            chosen.add(channel)
            if valid_dest(record.get("dest")):
                artist["dest"] = record["dest"]
    return json.dumps({"artists": list(artists.values()), "seeds": seeds}, ensure_ascii=False)


def serve(path):
    """What www.youtube.com answers for a path the window owns, as (status, content
    type, body), or None for a path left to YouTube."""
    if path == "/sw.js":
        # Without its service worker, YouTube cannot answer for /quartzmidi first.
        return 404, None, b""
    if not is_page(path):
        return None
    name = path[len(BASE) + 1:]
    if path == BASE:
        body = page_html()
        return (200, "text/html; charset=utf-8", body) if body is not None else (404, None, b"")
    if name == "rules.json":
        return 200, "application/json", rules_json().encode("utf-8")
    if name == "converted.json":
        return 200, "application/json", converted_json().encode("utf-8")
    if name == "artists.json":
        return 200, "application/json", artists_json().encode("utf-8")
    if name in SERVED or FONT.fullmatch(name):
        file = asset_path(name)
        body = read_bytes(file) if file else None
        if body is not None:
            return 200, TYPES[os.path.splitext(name)[1]], body
    return 404, None, b""


def header_text(kind):
    headers = dict(HEADERS, **({"Content-Type": kind} if kind else {}))
    return "\r\n".join(f"{name}: {value}" for name, value in headers.items())


def element_json(element):
    """An xml.etree element as {tag, attrs, children}, namespaces dropped."""
    return {"tag": element.tag.rpartition("}")[2],
            "attrs": {name.rpartition("}")[2]: value for name, value in element.attrib.items()},
            "children": [element_json(child) for child in element]}


def sprite_tree():
    """icons.svg as JSON, for the canvas to build with createElementNS."""
    try:
        return element_json(ElementTree.fromstring(sprite_text()))
    except ElementTree.ParseError:
        return None


class BodyTree(HTMLParser):
    """index.html's <body> as JSON: elements as {tag, attrs, children}, with svg: true
    inside an <svg>, and text as strings. Scripts are left out."""

    VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.body = None
        self.stack = []
        self.skipping = 0

    def node(self, tag, attrs):
        node = {"tag": tag, "attrs": {name: value or "" for name, value in attrs}, "children": []}
        if tag == "svg" or (self.stack and self.stack[-1].get("svg")):
            node["svg"] = True
        return node

    def handle_starttag(self, tag, attrs):
        if tag == "body":
            self.body = self.node(tag, attrs)
            self.stack = [self.body]
        elif tag == "script":
            self.skipping += 1
        elif self.stack and not self.skipping:
            node = self.node(tag, attrs)
            self.stack[-1]["children"].append(node)
            if tag not in self.VOID:
                self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        if self.stack and not self.skipping:
            self.stack[-1]["children"].append(self.node(tag, attrs))

    def handle_endtag(self, tag):
        if tag == "script":
            self.skipping = max(0, self.skipping - 1)
        elif tag == "body":
            self.stack = []
        else:
            for depth in range(len(self.stack) - 1, 0, -1):
                if self.stack[depth]["tag"] == tag:
                    del self.stack[depth:]
                    break

    def handle_data(self, data):
        if self.stack and not self.skipping and data.strip():
            self.stack[-1]["children"].append(data)


def body_tree():
    parser = BodyTree()
    parser.feed(read_text(os.path.join(PAGE, "index.html")) or "")
    parser.close()
    return parser.body


def canvas_scripts(route):
    """The canvas boot, run on robots.txt: QMBoot first, then parse.js, app.js and
    boot.js (each when present), each as a script of its own. QMBoot carries what the
    served page fetches: app.css, the fonts, the sprite, the icon, rules.json,
    converted.json and artists.json."""
    fonts = []
    for name, weight in FONTS:
        file = asset_path("fonts/" + name)
        data = read_bytes(file) if file else None
        if data:
            fonts.append({"weight": weight, "data": base64.b64encode(data).decode("ascii")})
    icon = asset_path("QuartzMIDI.ico")
    icon = read_bytes(icon) if icon else None
    boot = {"surface": "canvas", "route": route, "title": TITLE, "css": read_text(os.path.join(PAGE, "app.css")) or "",
            "fonts": fonts, "icons": sprite_tree(), "body": body_tree(),
            "icon": base64.b64encode(icon).decode("ascii") if icon else None,
            "rules": json.loads(rules_json()), "converted": json.loads(converted_json()),
            "artists": json.loads(artists_json())}
    scripts = ["globalThis.QMBoot = " + json.dumps(boot, ensure_ascii=True) + ";\ntrue"]
    for name in ("parse.js", "app.js", "boot.js"):
        text = read_text(os.path.join(PAGE, name))
        if text is not None:
            scripts.append(text + "\n;true")
    return scripts


def colour(skin, key):
    """A skin colour as #RRGGBB(AA), or the built-in dark skin's when the skin lacks it."""
    colours = skin.get("colours") if isinstance(skin, dict) else None
    value = colours.get(key) if isinstance(colours, dict) else None
    return value if isinstance(value, str) and COLOUR.fullmatch(value) else DEFAULT_SKIN["colours"][key]


def number(skin, key):
    shape = skin.get("shape") if isinstance(skin, dict) else None
    value = shape.get(key) if isinstance(shape, dict) else None
    ok = isinstance(value, (int, float)) and not isinstance(value, bool) and 0 <= value <= 1000
    return value if ok else DEFAULT_SKIN["shape"][key]


def is_dark(skin):
    dark = skin.get("dark") if isinstance(skin, dict) else None
    return dark if isinstance(dark, bool) else True


def ink_on(background):
    """#FFFFFF or #101218, whichever has the higher WCAG contrast with the colour."""
    def luminance(text):
        channels = [int(text[at:at + 2], 16) / 255 for at in (1, 3, 5)]
        r, g, b = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
        return 0.2126 * r + 0.7152 * g + 0.0722 * b

    base = luminance(background)

    def contrast(other):
        return (max(base, other) + 0.05) / (min(base, other) + 0.05)

    return "#FFFFFF" if contrast(1.0) >= contrast(luminance("#101218")) else "#101218"


def theme_css(skin):
    """The §4.2 variables the failure page uses, from the skin."""
    def c(key):
        return colour(skin, key)

    def px(key):
        return f"{number(skin, key):g}px"

    values = {
        "--canvas": c("canvas"), "--raised": c("elevated"), "--hot": c("elevatedHot"), "--well": c("recessed"),
        "--ink": c("inkPrimary"), "--ink2": c("inkSecondary"), "--ink3": c("inkTertiary"),
        "--accent": c("accent"), "--accent-line": c("accentLine"), "--accent-ink": ink_on(c("accent")),
        "--accent-hot": "color-mix(in srgb, var(--accent) 85%, var(--ink))", "--warn": c("warn"),
        "--line": c("hairline"), "--line2": c("strong"), "--hi": f"inset 0 1px 0 {c('topHighlight')}",
        "--raise": f"0 {px('contactOffset')} {px('contactBlur')} {c('contact')}, "
                   f"0 {px('ambientOffset')} {px('ambientBlur')} {c('ambient')}",
        "--sunk": f"inset 0 {px('innerOffset')} {px('innerBlur')} {c('inner')}",
        "--h": px("controlHeight"), "--r-control": px("radiusControl"), "--pad-window": px("windowPad"),
        "--fs-body": px("textBody"), "--fs-title": px("textTitle"),
    }
    return (":root{" + "".join(f"{name}:{value};" for name, value in values.items())
            + f"color-scheme:{'dark' if is_dark(skin) else 'light'}}}html{{zoom:{number(skin, 'scale'):g}}}")


FAILURE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>YouTube for QuartzMIDI</title>
<style id="theme">__THEME__</style>
<style>
__FONTS__
*{box-sizing:border-box}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:var(--canvas);
  color:var(--ink);font:var(--fs-body)/calc(var(--fs-body) + 6px) "IBM Plex Sans","Segoe UI Variable Text","Segoe UI",system-ui,sans-serif}
main{width:100%;max-width:480px;padding:var(--pad-window);display:flex;flex-direction:column;align-items:center;gap:16px}
h1{margin:0;display:flex;align-items:center;gap:8px;font-size:var(--fs-title);line-height:calc(var(--fs-title) + 8px);font-weight:600}
h1 svg{width:24px;height:24px;color:var(--warn);flex:none}
.row{display:flex;gap:8px;width:100%;justify-content:center}
.paste{margin-top:8px}
button{height:var(--h);display:inline-flex;align-items:center;gap:6px;padding:0 12px;flex:none;border-radius:var(--r-control);
  border:1px solid var(--line);background:var(--raised);box-shadow:var(--raise),var(--hi);color:var(--ink);font:inherit;cursor:pointer}
button svg{width:16px;height:16px;color:var(--ink2)}
button:hover{background:var(--hot)}
button:active{background:var(--raised);box-shadow:var(--sunk)}
button:focus-visible,input:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
button.primary{background:var(--accent);border-color:transparent;color:var(--accent-ink);font-weight:600}
button.primary svg{color:inherit}
button.primary:hover{background:var(--accent-hot)}
button.primary:active{background:var(--accent);box-shadow:var(--sunk)}
button.ghost{background:none;border-color:transparent;box-shadow:none;color:var(--ink2);cursor:default}
.field{flex:1;min-width:0;position:relative;display:flex;align-items:center}
.field svg{position:absolute;left:10px;width:16px;height:16px;color:var(--ink3);pointer-events:none}
input{width:100%;height:var(--h);padding:0 12px 0 34px;border-radius:var(--r-control);border:1px solid var(--line);
  background:var(--well);box-shadow:var(--sunk);color:var(--ink);font:inherit}
input:hover{border-color:var(--line2)}
input:focus{border-color:var(--accent-line)}
input::placeholder{color:var(--ink3)}
</style>
</head>
<body>
__SPRITE__
<main>
<h1><svg aria-hidden="true"><use href="#i-triangle-alert"/></svg><span>YouTube did not load.</span></h1>
<div class="row">
<button id="retry" type="button"><svg aria-hidden="true"><use href="#i-refresh-cw"/></svg><span>Retry</span></button>
<button id="youtube" type="button"><svg aria-hidden="true"><use href="#i-external-link"/></svg><span>Open on YouTube</span></button>
</div>
<div class="row paste">
<label class="field"><svg aria-hidden="true"><use href="#i-link"/></svg><input id="link" type="text" placeholder="Paste a link" aria-label="Paste a link" spellcheck="false" autocomplete="off"></label>
<button id="convert" class="primary" type="button" data-state="idle"><svg aria-hidden="true"><use href="#i-audio-lines"/></svg><span>Convert</span></button>
</div>
</main>
<script>
(() => {
  const PAGE = __PAGE__, YOUTUBE = __YOUTUBE__;
  const api = () => window.pywebview && window.pywebview.api;
  const field = document.getElementById('link'), convert = document.getElementById('convert');
  const looks = {idle: ['audio-lines', 'Convert', 'primary'], sent: ['check', 'Sent', 'ghost'], retry: ['rotate-ccw', 'Retry', '']};
  const show = state => {
    const [icon, word, look] = looks[state];
    convert.dataset.state = state;
    convert.className = look;
    convert.querySelector('use').setAttribute('href', '#i-' + icon);
    convert.querySelector('span').textContent = word;
  };
  let busy = false;
  const send = async () => {
    const url = field.value.trim();
    if (!url || busy || convert.dataset.state === 'sent') return;
    busy = true;
    let ok = false;
    try {
      const reply = await api().pick({kind: 'link', url});
      ok = !!(reply && reply.ok);
    } catch (error) {
      ok = false;
    }
    busy = false;
    show(ok ? 'sent' : 'retry');
  };
  document.getElementById('retry').addEventListener('click', () => { location.href = PAGE; });
  document.getElementById('youtube').addEventListener('click', () => { const bridge = api(); if (bridge) bridge.open_external(YOUTUBE); });
  convert.addEventListener('click', send);
  field.addEventListener('keydown', event => { if (event.key === 'Enter') send(); });
  field.addEventListener('input', () => { if (convert.dataset.state !== 'idle') show('idle'); });
})();
</script>
</body>
</html>
"""


def failure_page(skin, route):
    """§2.11: local HTML in the last skin. Retry loads the served page again, which
    starts the mount chain over."""
    fonts = []
    for name, weight in FONTS:
        file = asset_path("fonts/" + name) if weight in (400, 600) else None
        data = read_bytes(file) if file else None
        if data:
            fonts.append('@font-face{font-family:"IBM Plex Sans";font-weight:%d;src:url(data:font/ttf;base64,%s) '
                         'format("truetype")}' % (weight, base64.b64encode(data).decode("ascii")))

    def script_value(value):
        return json.dumps(value, ensure_ascii=True).replace("</", "<\\/")

    replacements = {"__FONTS__": "\n".join(fonts), "__THEME__": theme_css(skin), "__SPRITE__": sprite_text(),
                    "__PAGE__": script_value(ORIGIN + BASE + route), "__YOUTUBE__": script_value(youtube_url(route))}
    return re.sub("|".join(replacements), lambda found: replacements[found.group(0)], FAILURE)


def caption(handle, skin):
    """The skin's title-bar colours on a window, as Shell.cpp ApplyCaption sets the app's."""
    if not handle:
        return

    def colorref(key):
        text = colour(skin, key)
        return int(text[1:3], 16) | int(text[3:5], 16) << 8 | int(text[5:7], 16) << 16

    dwm = ctypes.WinDLL("dwmapi")
    dwm.DwmSetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
    # 38 (backdrop) to none: pywebview asks for Mica on a dark system, which
    # would cover the caption colour, and the app's own window has none.
    for attribute, value in ((20, int(is_dark(skin))), (38, 1), (34, colorref("structure")),
                             (35, colorref("structure")), (36, colorref("inkPrimary"))):
        dwm.DwmSetWindowAttribute(handle, attribute, ctypes.byref(ctypes.c_uint(value)), 4)


def dress(form, skin):
    """Title-bar colours and the QuartzMIDI icon on a window, on its GUI thread. The
    colours stay through system theme changes, which pywebview would follow."""
    handle = form.Handle.ToInt64()
    caption(handle, skin())
    form.update_title_bar_theme = lambda: caption(handle, skin())
    icon = asset_path("QuartzMIDI.ico")
    if icon:
        from System.Drawing import Icon
        form.Icon = Icon(icon)
    return handle


class COPYDATASTRUCT(ctypes.Structure):
    _fields_ = [("dwData", ctypes.c_size_t), ("cbData", wintypes.DWORD), ("lpData", ctypes.c_void_p)]


def user32():
    library = ctypes.WinDLL("user32")
    library.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
    library.FindWindowExW.restype = wintypes.HWND
    library.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    library.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
                                            wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
    library.SendMessageTimeoutW.restype = ctypes.c_ssize_t
    return library


def find_receiver(pid):
    """QuartzMIDI's message-only "QuartzMIDI.Open" window, and its process: the one of
    process pid, or the first one when pid is None. EnumWindows never lists them."""
    library, after = user32(), None
    while True:
        after = library.FindWindowExW(wintypes.HWND(-3), after, "QuartzMIDI.Open", None)  # -3: HWND_MESSAGE
        if not after:
            return None, 0
        owner = wintypes.DWORD()
        library.GetWindowThreadProcessId(after, ctypes.byref(owner))
        if pid is None or owner.value == pid:
            return after, owner.value


def send_open(receiver, path):
    """WM_COPYDATA with the path as UTF-16 and no terminator; empty raises the window."""
    data = path.encode("utf-16-le")
    buffer = ctypes.create_string_buffer(data, len(data) or 1)
    block = COPYDATASTRUCT(0x514D4F50, len(data), ctypes.cast(buffer, ctypes.c_void_p) if data else None)
    result = ctypes.c_size_t()
    sent = user32().SendMessageTimeoutW(receiver, 0x4A, 0, ctypes.addressof(block), 2, 5000, ctypes.byref(result))
    return bool(sent) and bool(result.value)  # 2: SMTO_ABORTIFHUNG


def app_exe(pid):
    """QuartzMIDI's exe: the running one's, else the one three folders up."""
    if pid:
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.OpenProcess.restype = wintypes.HANDLE
        process = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if process:
            buffer, size = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            found = kernel32.QueryFullProcessImageNameW(wintypes.HANDLE(process), 0, buffer, ctypes.byref(size))
            kernel32.CloseHandle(wintypes.HANDLE(process))
            if found and os.path.isfile(buffer.value):
                return buffer.value
    path = os.path.join(ROOT, "QuartzMIDI.exe")
    return path if os.path.isfile(path) else None


def launch_app(exe, path):
    subprocess.Popen([exe, path] if path else [exe], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, close_fds=True)


def allow_foreground(pid):
    if pid:
        ctypes.windll.user32.AllowSetForegroundWindow(wintypes.DWORD(pid))


def same_file(path):
    return os.path.normcase(os.path.abspath(path))


class Host:
    """The window's state, shared by the GUI thread, the stdin reader and the page's calls."""

    def __init__(self, protocol):
        self.protocol = protocol
        self.lock = threading.RLock()
        self.mailbox = Mailbox()
        self.first = threading.Event()  # the first stdin line has been read
        self.eof = threading.Event()    # stdin has ended
        self.closed = threading.Event()
        self.skin = DEFAULT_SKIN
        self.pid = None                 # QuartzMIDI's process, from init
        self.paths = set()              # .mid files QuartzMIDI named in item and child messages
        self.last_seq = 0
        self.written = None             # the session cookies.txt holds, as last written
        self.webview = None             # the pywebview module, which main() imports
        self.window = None
        self.hwnd = 0
        self.signin = None              # the sign-in window while it is open
        self.signin_hwnd = 0
        self.route = ""                 # the route the served page was last loaded with
        self.step = None                # the mount step under way: "served", "canvas" or "failed"
        self.surface = None             # the step the current page mounted with
        self.attempt = 0                # bumped by every top-frame load, so a stale timer does nothing
        self.why = ""                   # why the chain left the served page
        self.local = True               # a load_html page is on its way and may pass the guard
        self.asked = None               # when the last press asked for was let through
        self.presses = []               # when the presses of the last PRESS_SPAN seconds put the button down
        self.pressing = threading.Lock()  # held for a whole press, so two never interleave
        self.worlds = {}                # frame id -> its isolated world, where Skip is looked for

    # QuartzMIDI's messages

    def read(self, stream):
        """Reads QuartzMIDI's JSON lines until stdin ends, then closes the window."""
        for line in stream:
            try:
                message = json.loads(line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line)
            except ValueError:
                message = None
            if isinstance(message, dict) and isinstance(message.get("type"), str):
                self.receive(message)
            self.first.set()
        self.first.set()
        self.finish()

    def receive(self, message):
        kind = message["type"]
        if kind in ("init", "theme") and isinstance(message.get("skin"), dict):
            self.theme(message["skin"])
        late = None
        if kind == "init":
            pid, search = message.get("pid"), message.get("search")
            words = search.strip() if isinstance(search, str) else ""
            with self.lock:
                if isinstance(pid, int) and not isinstance(pid, bool) and pid > 0:
                    self.pid = pid
                if words and self.step is None:
                    self.route = form({"q": words})
                elif words:
                    late = words
        if kind in ("item", "child") and isinstance(message.get("path"), str) and message["path"]:
            with self.lock:
                self.paths.add(same_file(message["path"]))
        self.mailbox.put(message)
        if late:
            # The page is already on its way to its own route; the words come after init as a search.
            self.mailbox.put({"type": "search", "q": late})

    def theme(self, skin):
        with self.lock:
            self.skin = skin
            handles, failed = (self.hwnd, self.signin_hwnd), self.surface == "failed"
        for handle in handles:
            caption(handle, skin)
        if failed:
            later(self.retheme_failure, skin)

    def retheme_failure(self, skin):
        """The failure page takes the new skin in place, so a pasted link stays."""
        script = ("(css => { const style = document.getElementById('theme'); if (!style) return false; "
                  "style.textContent = css; return true; })(%s)" % json.dumps(theme_css(skin)))
        try:
            done = self.window.evaluate_js(script)
        except Exception:
            done = False
        with self.lock:
            failed = self.surface == "failed"
        if failed and not done:
            self.show_failure()

    def finish(self):
        """The end of stdin: both windows close, and main() prints done: closed."""
        self.eof.set()
        self.mailbox.end()
        for window in (self.signin, self.window):
            if window not in (None, True):
                try:
                    window.destroy()
                except Exception:
                    pass

    def deliver(self):
        """Passes QuartzMIDI's messages to the mounted page, one at a time."""
        while True:
            message = self.mailbox.take()
            if message is None:
                return
            script = ("globalThis.qm ? (qm.receive(" + json.dumps(message, ensure_ascii=True) + "), true) : false")
            try:
                taken = self.window.evaluate_js(script)
            except Exception as failure:
                taken = True
                if type(failure).__name__ == "JavascriptException":
                    self.log("script", f"qm.receive({message.get('type')}): {failure}")
            if taken is False and message.get("type") in ONCE:
                # The page went away under it; the next page has the rest replayed.
                self.mailbox.put_back(message)
                time.sleep(0.2)

    # Lines for QuartzMIDI

    def next_seq(self):
        with self.lock:
            self.last_seq = max(int(time.time() * 1000), self.last_seq + 1)
            return self.last_seq

    def tell(self, message):
        if self.protocol == 2:
            tell(message)

    def log(self, kind, text):
        label = {"drift": "drift", "script": "script error"}.get(kind)
        if not label:
            return False
        if self.protocol == 2:
            say(label, re.sub(r"[\r\n\v\f\x85  ]+", " ", str(text)).strip()[:300])
        return True

    def signed(self, signed_in, page=True):
        if self.protocol == 2:
            tell({"type": "signed", "in": signed_in})
        elif signed_in:
            say("signed", COOKIES)
        if page:
            self.mailbox.put({"type": "signed", "in": signed_in})

    # The window

    def prepare(self, window):
        """before_show, on the GUI thread, before anything loads."""
        form = window.native
        self.hwnd = dress(form, lambda: self.skin)
        # Replaced before pywebview attaches it, so new windows pass the guard.
        form.browser.on_new_window_request = guarded(self.on_new_window)
        form.browser.webview.CoreWebView2InitializationCompleted += guarded(self.ready)

    def ready(self, sender, args):
        """§1.3: no service worker, the server, frame.js and the guards, then the page."""
        if not args.IsSuccess:
            return
        from Microsoft.Web.WebView2.Core import CoreWebView2BrowsingDataKinds
        core = sender.CoreWebView2
        waits = []
        try:
            waits.append((core.Profile.ClearBrowsingDataAsync(CoreWebView2BrowsingDataKinds.ServiceWorkers), 3000))
        except Exception:
            traceback.print_exc()
        core.WebResourceRequested += guarded(self.on_request)
        try:
            # pywebview's * filter sees documents only; a worker's script is fetched as a
            # service worker request, which this filter brings to on_request too.
            from Microsoft.Web.WebView2.Core import CoreWebView2WebResourceContext, CoreWebView2WebResourceRequestSourceKinds
            core.AddWebResourceRequestedFilter(ORIGIN + "/sw.js*", CoreWebView2WebResourceContext.All,
                                               CoreWebView2WebResourceRequestSourceKinds.All)
        except Exception:
            traceback.print_exc()
        frame = read_text(os.path.join(PAGE, "frame.js"))
        if frame:
            waits.append((core.AddScriptToExecuteOnDocumentCreatedAsync(frame), 10000))
        core.NavigationStarting += guarded(self.on_navigation)
        core.FrameNavigationStarting += guarded(self.on_frame_navigation)
        core.NavigationCompleted += guarded(self.on_completed)

        # The GUI thread must keep running for these to finish, so a worker waits.
        def first():
            for task, milliseconds in waits:
                try:
                    task.Wait(milliseconds)
                except Exception:
                    traceback.print_exc()
            with self.lock:
                # From here a late init's words are a search, not the route.
                self.step = self.step or "served"
                route = self.route
            self.window.load_url(ORIGIN + BASE + route)

        later(first)

    def on_request(self, sender, args):
        parts = urlsplit(str(args.Request.Uri))
        if parts.scheme != "https" or parts.hostname != "www.youtube.com":
            return
        answer = serve(parts.path)
        if answer is None:
            return
        from System import Array, Byte
        from System.IO import MemoryStream
        status, kind, body = answer
        stream = MemoryStream(Array[Byte](body)) if body else MemoryStream()
        args.Response = sender.Environment.CreateWebResourceResponse(
            stream, status, "OK" if status == 200 else "Not Found", header_text(kind))

    def on_navigation(self, sender, args):
        url = str(args.Uri)
        if url.partition(":")[0].lower() in ("about", "data"):
            with self.lock:
                allowed, self.local = self.local or url == "about:blank", False
            if allowed:
                self.leave(url)
            else:
                args.Cancel = True
            return
        with self.lock:
            action = guard(url, canvas=self.step == "canvas")
        if action == "allow":
            self.leave(url)
            return
        args.Cancel = True
        if action != "cancel":
            later(self.act, action, url)

    def on_frame_navigation(self, sender, args):
        host = (urlparse(str(args.Uri)).hostname or "").lower()
        if host in CONSENT:
            # Consent loads as a page of its own, never in a frame the CSP blocks.
            args.Cancel = True
            current = str(sender.Source)
            if not is_page(urlparse(current).path):
                current = ORIGIN + BASE + self.route
            later(self.window.load_url, CONSENT_PAGE + quote(current, safe="-_.!~*'()"))
        elif host in ACCOUNTS:
            args.Cancel = True  # an age-restricted video; the page marks it

    def on_new_window(self, sender, args):
        args.Handled = True
        url = str(args.Uri)
        action = guard(url)
        if action != "cancel":
            later(self.act, action, url)

    def act(self, action, url):
        if action == "route":
            self.window.load_url(ORIGIN + BASE + route_for(url))
        elif action == "allow":
            self.window.load_url(url)
        elif action == "signin":
            self.sign_in()
        elif action == "external":
            try:
                os.startfile(url)
            except OSError:
                pass

    def leave(self, url):
        """A new document is on its way into the top frame; messages wait for it to mount."""
        parts = urlparse(url)
        with self.lock:
            self.attempt += 1
            self.mailbox.close()
            if parts.scheme in ("http", "https"):
                self.surface, self.local = None, False
                if parts.hostname == "www.youtube.com" and is_page(parts.path):
                    self.step = "served"
                    self.route = "?" + parts.query if parts.query else ""

    # The mount chain

    def on_completed(self, sender, args):
        parts = urlparse(str(sender.Source))
        with self.lock:
            attempt, step = self.attempt, self.step
        if parts.hostname != "www.youtube.com":
            return
        if step == "served" and is_page(parts.path):
            later(self.served_timeout, attempt, seconds=8)
        elif step == "canvas" and parts.path == "/robots.txt":
            if args.IsSuccess:
                later(self.boot_canvas, attempt)
            else:
                later(self.fail, f"{self.why}; robots.txt did not load ({args.WebErrorStatus})")

    def served_timeout(self, attempt):
        with self.lock:
            if attempt != self.attempt or self.surface:
                return
            self.step = "canvas"
            self.why = "the served page did not mount in 8 s"
        self.window.load_url(CANVAS)

    def boot_canvas(self, attempt):
        try:
            for script in canvas_scripts(self.route):
                if attempt != self.attempt:
                    return
                self.window.evaluate_js(script)
        except Exception as failure:
            if attempt == self.attempt:
                error = failure.args[0] if failure.args else None  # pywebview passes the JS error as a dict
                message = error.get("message") if isinstance(error, dict) else None
                self.fail(f"{self.why}; the canvas threw {(message or str(failure))[:200]}")
            return
        later(self.canvas_timeout, attempt, seconds=5)

    def canvas_timeout(self, attempt):
        with self.lock:
            if attempt != self.attempt or self.surface:
                return
        self.fail(f"{self.why}; the canvas did not mount in 5 s")

    def fail(self, why):
        with self.lock:
            self.step = self.surface = "failed"
        self.tell({"type": "mount", "surface": "failed", "detail": why})
        self.show_failure()

    def show_failure(self):
        with self.lock:
            self.local = True
            page = failure_page(self.skin, self.route)
        self.window.load_html(page)

    def mounted(self, surface):
        with self.lock:
            if surface not in ("served", "canvas") or surface != self.step:
                return False
            if self.surface == surface:
                return True
            self.surface = surface
            detail = BASE + self.route if surface == "served" else self.why
        self.tell({"type": "mount", "surface": surface, "detail": detail})
        self.mailbox.mount()
        return True

    # QuartzMIDI's window

    def app(self):
        """QuartzMIDI's own window, of the pid from init, else of the process that started this one;
        0 when neither is QuartzMIDI's."""
        return app_window(self.pid or os.getppid())

    def open_in_app(self, path):
        """§3.16: QuartzMIDI opens the .mid, or comes to the front for an empty path."""
        receiver, owner = find_receiver(self.pid)
        if receiver:
            allow_foreground(owner)
            return send_open(receiver, path)
        exe = app_exe(self.pid)
        if not exe:
            return False
        launch_app(exe, path)
        return True

    # Signing in and out

    def sign_in(self):
        """§1.6: a window of its own on the same profile, closed once YouTube is reached."""
        with self.lock:
            if self.signin is not None or self.closed.is_set() or not self.webview:
                return bool(self.signin)
            self.signin = True  # opening
        try:
            # On the YouTube window's monitor, centred on it, fitted to the work area; on QuartzMIDI's
            # monitor while the YouTube window is minimised.
            window = self.webview.create_window(SIGN_IN_TITLE, SIGN_IN,
                                                background_color=colour(self.skin, "canvas")[:7],
                                                **placement((520, 720), over=self.hwnd, near=self.app()))
        except Exception:
            with self.lock:
                self.signin = None
            raise
        window.events.loaded += lambda: self.signin_loaded(window)
        window.events.closed += lambda: self.signin_closed(window)
        form = window.native

        def dress_signin():
            self.signin_hwnd = dress(form, lambda: self.skin)

        from System import Action
        form.Invoke(Action(dress_signin))
        with self.lock:
            self.signin = window
        return True

    def signin_loaded(self, window):
        try:
            if (urlparse(window.get_current_url() or "").hostname or "") != "www.youtube.com":
                return
            jars = window.get_cookies()
        except Exception:
            return  # the window is closing
        values = signed_in_values(jars)
        if values:
            try:
                write_cookies(jars)
                self.written = values
            except OSError:
                pass  # watch() writes it on a later pass
        window.destroy()
        if values:
            self.signed(True)

    def signin_closed(self, window):
        with self.lock:
            if self.signin is window:
                self.signin, self.signin_hwnd = None, 0

    def sign_out(self):
        """§1.6: forgets the session in the profile and cookies.txt."""
        from Microsoft.Web.WebView2.Core import CoreWebView2Cookie
        from System import Action
        from System.Collections.Generic import List
        from System.Threading.Tasks import Task
        browser = self.window.native.browser
        deleted = threading.Semaphore(0)

        # Started on the GUI thread; each list comes back there, through
        # pywebview's scheduler, and is deleted there too.
        def start():
            manager = browser.webview.CoreWebView2.CookieManager

            def delete(task):
                try:
                    for cookie in task.Result:
                        manager.DeleteCookie(cookie)
                finally:
                    deleted.release()

            for site in SIGNED_OUT:
                manager.GetCookiesAsync(site).ContinueWith(Action[Task[List[CoreWebView2Cookie]]](delete),
                                                           browser.syncContextTaskScheduler)

        self.window.native.Invoke(Action(start))
        for _ in SIGNED_OUT:
            deleted.acquire(timeout=5)
        forget_cookie_file(COOKIES)
        self.written = None
        self.signed(False)
        return True

    # Trusted input: Skip (§3.6, §8)

    def on_gui(self, action):
        """Runs action on the window's GUI thread, the only one CoreWebView2 may be called on, and waits for it."""
        from System import Action
        self.window.native.Invoke(Action(action))

    def devtools(self, method, params):
        """One DevTools Protocol call, started on the GUI thread and awaited here: its result, or None if it
        failed or this WebView2 has no such call."""
        box = {}

        def start():
            core = self.window.native.browser.webview.CoreWebView2
            box["task"] = core.CallDevToolsProtocolMethodAsync(method, json.dumps(params, separators=(",", ":")))

        try:
            self.on_gui(start)
            task = box.get("task")
            if task is None or not task.Wait(DEVTOOLS_WAIT):
                return None
            result = json.loads(task.Result or "{}")
        except Exception:
            return None
        return result if isinstance(result, dict) else None

    def watch_frame(self, frame_id):
        """Whether the frame is the dock's /watch frame: a child of the top frame on www.youtube.com/watch.
        Never the page itself, and never any other frame."""
        tree = (self.devtools("Page.getFrameTree", {}) or {}).get("frameTree") or {}
        top = (tree.get("frame") or {}).get("id")
        for child in tree.get("childFrames") or []:
            frame = child.get("frame") or {}
            if top and frame_id and frame_id == frame.get("id") and frame.get("parentId") == top:
                parts = urlsplit(str(frame.get("url") or ""))
                return parts.scheme == "https" and parts.hostname == "www.youtube.com" and parts.path == "/watch"
        return False

    def on_skip(self, node, frame_id):
        """Whether the node is YouTube's Skip or inside it, asked in an isolated world of its frame, where no
        script of the page or of YouTube can change what closest() answers. The hit test can find a text node or
        one of Skip's ::before or ::after, a CSSPseudoElement, which is asked through the element it belongs to.
        The world is made once per frame and made again when the frame's document has changed."""
        for again in (False, True):
            world = self.worlds.get(frame_id)
            if again or world is None:
                made = self.devtools("Page.createIsolatedWorld", {"frameId": frame_id, "worldName": "QuartzMIDI"})
                world = (made or {}).get("executionContextId")
                if not isinstance(world, int) or isinstance(world, bool):
                    return False
                self.worlds[frame_id] = world
            found = self.devtools("DOM.resolveNode", {"backendNodeId": node, "executionContextId": world,
                                                      "objectGroup": PRESS_GROUP})
            handle = ((found or {}).get("object") or {}).get("objectId")
            if handle:
                break
        else:
            return False
        answer = self.devtools("Runtime.callFunctionOn", {
            "objectId": handle, "returnByValue": True, "arguments": [{"value": SKIP}],
            "functionDeclaration": ON_SKIP})
        return ((answer or {}).get("result") or {}).get("value") is True

    def hit(self, x, y):
        """What the browser's own hit test at (x, y) finds, the same one real input takes: (frame id, node id),
        or None."""
        found = self.devtools("DOM.getNodeForLocation", {"x": x, "y": y}) or {}
        frame_id, node = found.get("frameId"), found.get("backendNodeId")
        if not isinstance(frame_id, str) or not frame_id or not isinstance(node, int) or isinstance(node, bool):
            return None
        return frame_id, node

    def skip_at(self, x, y):
        """(frame id, node id) when the hit at (x, y) is YouTube's Skip in the dock's /watch frame, else None."""
        found = self.hit(x, y)
        if found is None or not self.watch_frame(found[0]) or not self.on_skip(found[1], found[0]):
            return None
        return found

    def still_skip(self, x, y, found):
        """Skip is looked for again just before the button goes down, after the move, which may bring up
        YouTube's hover chrome or come during a layout change: closest() first, then the hit test as the last
        call, whose node must be one closest() has just placed in Skip, in the same frame."""
        frame_id, node = found
        if not self.on_skip(node, frame_id):
            return False
        for _ in range(2):
            now = self.hit(x, y)
            if now is None or now[0] != frame_id:
                return False
            if now[1] == node:
                return True
            node = now[1]
            if not self.on_skip(node, frame_id):
                return False
        return False

    def mouse(self, kind, x, y):
        event = {"type": kind, "x": x, "y": y, "button": "none" if kind == "mouseMoved" else "left",
                 "buttons": 1 if kind == "mousePressed" else 0, "pointerType": "mouse"}
        if kind != "mouseMoved":
            event["clickCount"] = 1
        return self.devtools("Input.dispatchMouseEvent", event) is not None

    def press(self, x, y):
        """One left click at (x, y), top-level viewport CSS px, with trusted input: what the page asks for when
        YouTube shows Skip. Sent only inside the viewport and only while the browser's own hit test finds Skip
        there, before the pointer moves onto it and again just before the button goes down. Once the button is
        tried, it is always released and the pointer leaves the window. False when refused or when a call failed,
        so the page asks again; asks and presses are rate-limited (PRESS_GAP, PRESS_MOST). A layout that moves
        after the last check sends the press elsewhere, where the page or frame.js swallows it, so the page counts
        an ad as pressed only when frame.js saw the click reach Skip."""
        if not (finite(x) and finite(y)):
            return False
        if not self.pressing.acquire(blocking=False):
            return False
        try:
            now = time.monotonic()
            with self.lock:
                self.presses = [at for at in self.presses if now - at < PRESS_SPAN]
                if len(self.presses) >= PRESS_MOST or (self.asked is not None and now - self.asked < PRESS_GAP):
                    return False
                self.asked = now
            return self.click(int(math.floor(x)), int(math.floor(y)))
        finally:
            self.pressing.release()

    def click(self, x, y):
        view = (self.devtools("Page.getLayoutMetrics", {}) or {}).get("cssLayoutViewport") or {}
        width, height = view.get("clientWidth"), view.get("clientHeight")
        if not (finite(width) and finite(height) and 0 <= x < width and 0 <= y < height):
            return False
        found = self.skip_at(x, y)
        if found is None:
            return False
        tried = done = False
        try:
            if self.mouse("mouseMoved", x, y) and self.still_skip(x, y, found):
                with self.lock:
                    self.presses.append(time.monotonic())
                tried = True
                done = self.mouse("mousePressed", x, y)
        finally:
            if tried:
                done = self.mouse("mouseReleased", x, y) and done
            # Out of the window, so no YouTube hover chrome stays and nothing of the page is hovered.
            self.mouse("mouseMoved", -1, -1)
            self.devtools("Runtime.releaseObjectGroup", {"objectGroup": PRESS_GROUP})
        return done

    def on_closed(self):
        self.closed.set()
        self.mailbox.end()
        window = self.signin
        if window not in (None, True):
            later(window.destroy)


class Api:
    """What the page calls through pywebview.api. Only methods are public: pywebview
    exposes every public attribute to the page."""

    def __init__(self, host):
        self._host = host

    def mounted(self, surface):
        return self._host.mounted(surface)

    def pick(self, pick):
        host = self._host
        message = pick_message(pick, host.next_seq())
        if not message:
            reason = pick_refusal(pick)
            return {"ok": False, "seq": None, "reason": reason} if reason else {"ok": False, "seq": None}
        if host.protocol == 2:
            tell(message)
        else:
            for kind, link in old_lines(message):
                say(kind, link)
        return {"ok": True, "seq": message["seq"]}

    def drop(self, key, parent=None):
        if self._host.protocol != 2 or not isinstance(key, str) or not key or not (parent is None or isinstance(parent, str)):
            return False
        tell({"type": "drop", "key": key, **({"parent": parent} if parent else {})})
        return True

    def order(self, keys):
        if self._host.protocol != 2 or not isinstance(keys, list) or len(keys) > 5000 \
                or not all(isinstance(key, str) and key for key in keys):
            return False
        tell({"type": "order", "keys": keys})
        return True

    def dest(self, key, dest, artist=None):
        """A waiting pick's new folder, from the tray; refused, never rewritten, when it is not valid."""
        if self._host.protocol != 2 or not isinstance(key, str) or not key or not valid_dest(dest) \
                or not valid_artist(artist):
            return False
        tell({"type": "dest", "key": key, "dest": dest, **({"artist": artist} if artist else {})})
        return True

    def resume(self):
        if self._host.protocol != 2:
            return False
        tell({"type": "resume"})
        return True

    def choose_folder(self):
        if self._host.protocol != 2:
            return False
        # QuartzMIDI's folder picker may then come to the front.
        allow_foreground(self._host.pid)
        tell({"type": "choose-folder"})
        return True

    def open_file(self, path):
        host = self._host
        if not isinstance(path, str) or not path.lower().endswith(".mid") or not os.path.isfile(path):
            return False
        known = {same_file(record["path"]) for record in ledger_records().values()}
        with host.lock:
            known |= host.paths
        return same_file(path) in known and host.open_in_app(path)

    def raise_app(self):
        return self._host.open_in_app("")

    def open_external(self, url):
        if not isinstance(url, str) or not url.lower().startswith("https://"):
            return False
        try:
            os.startfile(url)
            return True
        except OSError:
            return False

    def sign_in(self):
        return self._host.sign_in()

    def sign_out(self):
        return self._host.sign_out()

    def log(self, kind, text):
        return self._host.log(kind, text)

    def press(self, x, y):
        return self._host.press(x, y)


def signed_in_values(jars):
    return {name: morsel.value for jar in jars for name, morsel in jar.items() if name in SIGNED_IN}


def watch(window, closed, host):
    # Writes the session for yt-dlp once YouTube shows a signed-in account, and
    # again whenever the account changes.
    while not closed.is_set():
        try:
            url = window.get_current_url() or ""
            jars = window.get_cookies() if "youtube.com" in url else None
        except Exception:
            jars = None  # the window is closing or still loading
        if jars:
            values = signed_in_values(jars)
            if values and values != host.written:
                try:
                    write_cookies(jars)
                    host.written = values
                    host.signed(True, page=False)
                except OSError:
                    pass  # convert.py has it open; try again on the next pass
        closed.wait(5)


def arguments(argv):
    parser = argparse.ArgumentParser(description="The YouTube window for QuartzMIDI.")
    parser.add_argument("--hello", type=int, default=1, help="the protocol QuartzMIDI speaks")
    return parser.parse_known_args(argv)[0]


def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    options = arguments(argv)
    try:
        import webview
    except ImportError:
        say("error", "pywebview is not installed for this Python.")
        return 2

    host = Host(2 if options.hello == 2 else 1)
    host.webview = webview
    if host.protocol == 2:
        # A reader of its own: at exit the interpreter closes sys.stdin, which would
        # wait on this thread's lock and abort.
        stdin = open(sys.stdin.fileno(), "rb", closefd=False)
        threading.Thread(target=host.read, args=(stdin,), daemon=True).start()
        host.first.wait(1)  # init, if QuartzMIDI sent it in time; a late one is applied when it comes
        say("hello", "2")
    else:
        host.receive({"type": "init", "protocol": 1, "skin": DEFAULT_SKIN, "signedIn": os.path.exists(COOKIES)})

    os.makedirs(PROFILE, exist_ok=True)
    os.environ["WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"] = BROWSER_ARGUMENTS
    background = colour(host.skin, "canvas")[:7]
    # Opens blank; ready() loads the page once the server and the guards are in place. 1280x860, or
    # smaller to fit the work area, centred on QuartzMIDI's monitor, else the cursor's: QuartzMIDI
    # also opens it on its own once setup or a sign-in ends, when the cursor may be anywhere.
    window = webview.create_window(TITLE, html=f"<body style='background:{background}'></body>", js_api=Api(host),
                                   background_color=background, **placement((1280, 860), (640, 480), near=host.app()))
    host.window = window
    window.events.before_show += lambda: host.prepare(window)
    window.events.closed += host.on_closed

    def run():
        if host.eof.is_set():
            host.finish()  # stdin ended before the window was there to close
            return
        threading.Thread(target=watch, args=(window, host.closed, host), daemon=True).start()
        threading.Thread(target=host.deliver, daemon=True).start()

    webview.start(run, private_mode=False, storage_path=PROFILE)
    host.closed.set()
    host.mailbox.end()
    say("done", "closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
