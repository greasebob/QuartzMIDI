"""browse.py: links, routes, picks, QuartzMIDI's messages, the cookie-file guard, what it serves and where its
windows open."""

import ast
import base64
import contextlib
import ctypes
import datetime
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from ctypes import wintypes
from unittest import mock

TESTS = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(TESTS, "fixtures")
HERE = os.path.dirname(TESTS)
# The embedded Python's ._pth file leaves the converter's folder off the path.
sys.path.insert(0, HERE)

import browse
import convert
import piano_match
import signin
from browse import Api, Host, Mailbox, clean, coalescing_key, guard, pick_message, route_for, serve
from signin import MARGIN, fit_window, physical

VIDEO = "dQw4w9WgXcQ"
OTHER_VIDEO = "Zx9_Kp-3LmQ"
PLAYLIST = "PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG"
CHANNEL = "UCuAXFkgsw1L7xaCfnd5JJOw"


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as file:
        return json.load(file)


def lines(call, *args):
    """What call printed, as (kind, text) pairs, and what it returned."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        result = call(*args)
    return [tuple(line.split(": ", 1)) for line in out.getvalue().splitlines()], result


class Clean(unittest.TestCase):
    def test_videos(self):
        for url in (f"https://www.youtube.com/watch?v={VIDEO}", f"https://youtu.be/{VIDEO}",
                    f"https://m.youtube.com/watch?v={VIDEO}&t=4", f"https://music.youtube.com/watch?v={VIDEO}",
                    f"https://www.youtube.com/watch?v={VIDEO}&list={PLAYLIST}"):
            with self.subTest(url=url):
                self.assertEqual(clean(url, False), f"https://www.youtube.com/watch?v={VIDEO}")
        for url in ("https://www.youtube.com/watch?v=short", "https://example.com/watch?v=" + VIDEO,
                    "https://www.youtube.com/shorts/" + VIDEO, "", None, "not a link"):
            with self.subTest(url=url):
                self.assertIsNone(clean(url, False))

    def test_playlists(self):
        self.assertEqual(clean(f"https://www.youtube.com/watch?v={VIDEO}&list={PLAYLIST}", True),
                         f"https://www.youtube.com/playlist?list={PLAYLIST}")
        self.assertEqual(clean(f"https://www.youtube.com/playlist?list={PLAYLIST}", True),
                         f"https://www.youtube.com/playlist?list={PLAYLIST}")
        self.assertIsNone(clean(f"https://www.youtube.com/watch?v={VIDEO}&list=RD{VIDEO}", True))  # a Mix
        self.assertIsNone(clean(f"https://www.youtube.com/watch?v={VIDEO}", True))
        self.assertIsNone(clean(f"https://example.com/playlist?list={PLAYLIST}", True))


class Routes(unittest.TestCase):
    def test_routes_fixture(self):
        cases = fixture("routes.json")
        self.assertGreater(len(cases), 40)
        for case in cases:
            with self.subTest(url=case["url"]):
                self.assertEqual(route_for(case["url"]), case["route"])
                self.assertEqual(guard(case["url"]), case["guard"])

    def test_canvas_allows_robots_only_during_the_canvas_step(self):
        self.assertEqual(guard("https://www.youtube.com/robots.txt"), "external")
        self.assertEqual(guard("https://www.youtube.com/robots.txt", canvas=True), "allow")
        self.assertEqual(guard("http://www.youtube.com/quartzmidi"), "external")  # the page is https only
        self.assertEqual(guard("https://www.youtube.com/quartzmidiX"), "external")

    def test_routes_encode_like_url_search_params(self):
        self.assertEqual(route_for("https://www.youtube.com/results?search_query=a~b*c%20d"), "?q=a%7Eb*c+d")
        self.assertIsNone(route_for(f"https://www.youtube.com/playlist?list=RD{VIDEO}"))
        self.assertEqual(route_for(f"https://www.youtube.com/shorts/{VIDEO}?list={PLAYLIST}"), f"?v={VIDEO}")
        self.assertIsNone(route_for("https://www.youtube.com/channel/notachannel"))
        self.assertIsNone(route_for("https://www.youtube.com/results?search_query=%20"))

    def test_youtube_url_for_open_on_youtube(self):
        cases = {
            f"?v={VIDEO}": f"https://www.youtube.com/watch?v={VIDEO}",
            f"?list={PLAYLIST}&v={VIDEO}": f"https://www.youtube.com/watch?v={VIDEO}&list={PLAYLIST}",
            f"?list={PLAYLIST}": f"https://www.youtube.com/playlist?list={PLAYLIST}",
            f"?mix={VIDEO}": f"https://www.youtube.com/watch?v={VIDEO}&list=RD{VIDEO}",
            f"?channel={CHANNEL}": f"https://www.youtube.com/channel/{CHANNEL}",
            "?channel=%40ChopinFan": "https://www.youtube.com/@ChopinFan",
            "?channel=c%2FChopinFan": "https://www.youtube.com/c/ChopinFan",
            "?q=chopin+nocturne": "https://www.youtube.com/results?search_query=chopin+nocturne",
            f"?similar={VIDEO}": f"https://www.youtube.com/watch?v={VIDEO}&list=RD{VIDEO}",
            f"?channel={CHANNEL}&t=similar": f"https://www.youtube.com/channel/{CHANNEL}",
            # Home, and a genre, whose words only parse.js has, open YouTube's home.
            "": "https://www.youtube.com/",
            "?q=+": "https://www.youtube.com/",
            "?g=jazz&s=new": "https://www.youtube.com/",
            "?similar=short": "https://www.youtube.com/",
        }
        for route, url in cases.items():
            with self.subTest(route=route):
                self.assertEqual(browse.youtube_url(route), url)

    def test_home_is_no_longer_a_solo_piano_search(self):
        with open(os.path.join(HERE, "browse.py"), encoding="utf-8") as file:
            self.assertNotIn("solo piano", file.read().lower())


class Picks(unittest.TestCase):
    def test_video(self):
        message = pick_message({"kind": "video", "id": VIDEO, "title": "Nocturne", "again": False, "folder": None}, 7)
        self.assertEqual(message, {"type": "pick", "seq": 7, "key": "v:" + VIDEO, "kind": "video",
                                   "url": "https://www.youtube.com/watch?v=" + VIDEO, "title": "Nocturne",
                                   "again": False, "folder": None})
        again = pick_message({"kind": "video", "url": f"https://youtu.be/{VIDEO}", "again": True, "folder": "A:B"}, 8)
        self.assertEqual((again["key"], again["again"], again["folder"]), ("v:" + VIDEO, True, "AB"))  # FolderName

    def test_lists(self):
        items = [{"id": VIDEO, "title": "One"}, {"id": OTHER_VIDEO, "title": "Two"}]
        playlist = pick_message({"kind": "playlist", "id": PLAYLIST, "title": "Set", "items": items, "folder": "Set"}, 1)
        self.assertEqual((playlist["key"], playlist["url"], playlist["items"]),
                         ("p:" + PLAYLIST, "https://www.youtube.com/playlist?list=" + PLAYLIST, items))
        channel = pick_message({"kind": "channel", "id": CHANNEL, "title": "Fan", "folder": "Fan"}, 2)
        self.assertEqual((channel["key"], channel["url"]),
                         ("c:" + CHANNEL, f"https://www.youtube.com/channel/{CHANNEL}/videos"))
        self.assertNotIn("items", channel)
        mix = pick_message({"kind": "mix", "id": VIDEO, "title": "Mix - One", "items": items, "folder": "Mix - One"}, 3)
        self.assertEqual((mix["key"], mix["url"]), ("m:" + VIDEO, ""))
        selection = pick_message({"kind": "selection", "items": items}, 4)
        self.assertEqual((selection["key"], selection["url"], selection["folder"]), ("s:4", "", None))
        link = pick_message({"kind": "link", "url": " https://example.com/a.mp3 "}, 5)
        self.assertEqual((link["key"], link["url"]), ("link:https://example.com/a.mp3", "https://example.com/a.mp3"))

    def test_rejects(self):
        items = [{"id": VIDEO, "title": "One"}]
        for pick in (None, [], {}, {"kind": "song", "id": VIDEO}, {"kind": "video", "id": "short"},
                     {"kind": "video", "id": VIDEO + "&list=x"}, {"kind": "video", "url": "https://example.com/"},
                     {"kind": "video", "id": VIDEO, "title": "x" * 301}, {"kind": "video", "id": VIDEO, "items": items},
                     {"kind": "video", "id": VIDEO, "folder": 3}, {"kind": "playlist", "id": "RD" + VIDEO},
                     {"kind": "channel", "id": "@ChopinFan"}, {"kind": "mix", "id": VIDEO},
                     {"kind": "selection", "items": []}, {"kind": "selection", "items": [{"id": "bad"}]},
                     {"kind": "selection", "items": [{"id": VIDEO, "title": 5}]},
                     {"kind": "selection", "items": [{"id": VIDEO}] * 5001},
                     {"kind": "link", "url": "ftp://example.com/a.mp3"}, {"kind": "link", "url": "javascript:alert(1)"},
                     {"kind": "link", "url": "https://example.com/", "items": items}):
            with self.subTest(pick=str(pick)[:80]):
                self.assertIsNone(pick_message(pick, 1))
        self.assertIsNotNone(pick_message({"kind": "selection", "items": [{"id": VIDEO}] * 5000}, 1))

    # §5.4: a dest is "" (the MIDI folder) or 1 to 4 names joined by "/", each one Windows keeps as written.
    GOOD_DESTS = ("", "Anime", "Anime/Animenz Piano Sheets", "A/B/C/D", "Classical/Chopin", "Mix - Nocturne",
                  "Ünïcode 日本/ピアノ", ".hidden", "a.b", "Con Brio", "CONSOLE", "COM10", "LPT", "x" * 255,
                  "😀" * 127, "A b", "Jacob Koller - The Mad Arranger", "COM⁴", "CONIN", "CONOUT")
    BAD_DESTS = (3, True, [], {}, ["Anime"], "/", "A/", "/A", "A//B", "A/B/C/D/E", "A\\B", "A:B", 'A"B', "A<B",
                 "A>B", "A|B", "A?B", "A*B", ".", "..", "A/..", "A/.", "../A", " A", "A ", "A.", "A/B.", "...",
                 "x" * 256, "😀" * 128, "CON", "con", "Con.txt", "con .txt", "Aux", "NUL.tar.gz", "COM1", "lpt9.mid",
                 "PRN", "A/CON", "COM¹", "lpt³.mid", "CONIN$", "conout$.txt", "CONıN$",
                 "A\x00B", "A\x1fB", "A\x7fB", "A\x85B",
                 *("A%sB" % chr(code) for code in (0x200B, 0x202E, 0xFEFF, 0x00AD, 0x2066)), "\ud800", "A\nB", "A\tB")
    GOOD_ARTISTS = (None, CHANNEL, "UC" + "_-" * 11)
    BAD_ARTISTS = ("", "UC" + "x" * 21, "UC" + "x" * 23, "uc" + CHANNEL[2:], CHANNEL + "\n", "@ChopinFan",
                   "UC" + "x" * 21 + "!", 5, True, [CHANNEL])

    def test_dest_and_artist_accepted(self):
        for dest in self.GOOD_DESTS:
            with self.subTest(dest=dest):
                self.assertTrue(browse.valid_dest(dest))
                message = pick_message({"kind": "video", "id": VIDEO, "dest": dest}, 1)
                self.assertEqual(message["dest"], dest)  # as sent, never rewritten
        for artist in self.GOOD_ARTISTS:
            with self.subTest(artist=artist):
                message = pick_message({"kind": "channel", "id": CHANNEL, "dest": "Fan", "artist": artist}, 1)
                self.assertEqual(message.get("artist"), artist)
        message = pick_message({"kind": "video", "id": VIDEO, "title": "Nocturne", "folder": "Animenz Piano Sheets",
                                "dest": "Anime/Animenz Piano Sheets", "artist": CHANNEL}, 9)
        self.assertEqual(message, {"type": "pick", "seq": 9, "key": "v:" + VIDEO, "kind": "video",
                                   "url": "https://www.youtube.com/watch?v=" + VIDEO, "title": "Nocturne",
                                   "again": False, "folder": "Animenz Piano Sheets",
                                   "dest": "Anime/Animenz Piano Sheets", "artist": CHANNEL})
        # Without them (an older page, or no artist) the line is as before; "" is the MIDI folder, not absent.
        plain = pick_message({"kind": "video", "id": VIDEO, "folder": None, "dest": None, "artist": None}, 2)
        self.assertNotIn("dest", plain)
        self.assertNotIn("artist", plain)
        self.assertEqual(pick_message({"kind": "selection", "items": [{"id": VIDEO}], "dest": ""}, 3)["dest"], "")
        items = [{"id": VIDEO, "title": "One"}]
        for kind, ident in (("playlist", PLAYLIST), ("mix", VIDEO), ("selection", ""), ("link", "")):
            with self.subTest(kind=kind):
                pick = {"kind": kind, "id": ident, "dest": "Classical/Chopin", "url": "https://example.com/a.mp3"}
                if kind != "link":
                    pick = dict(pick, url="", items=items)
                self.assertEqual(pick_message(pick, 4)["dest"], "Classical/Chopin")

    def test_dest_and_artist_refused(self):
        for dest in self.BAD_DESTS:
            with self.subTest(dest=dest):
                self.assertFalse(browse.valid_dest(dest))
                self.assertIsNone(pick_message({"kind": "video", "id": VIDEO, "dest": dest}, 1))
                # The dest alone is why, so the answer says so and the page can say the folder is not allowed.
                for host in (Host(2), Host(1)):
                    self.assertEqual(lines(Api(host).pick, {"kind": "video", "id": VIDEO, "dest": dest}),
                                     ([], {"ok": False, "seq": None, "reason": "dest"}))
                # Something else is wrong as well: the plain refusal, as for any malformed pick.
                # An artist with it does not change that.
                self.assertEqual(lines(Api(Host(2)).pick, {"kind": "video", "id": VIDEO, "dest": dest, "artist": CHANNEL}),
                                 ([], {"ok": False, "seq": None, "reason": "dest"}))
                self.assertEqual(lines(Api(Host(2)).pick, {"kind": "video", "id": "short", "dest": dest}),
                                 ([], {"ok": False, "seq": None}))
                self.assertEqual(lines(Api(Host(2)).pick, {"kind": "video", "id": VIDEO, "dest": dest,
                                                           "artist": self.BAD_ARTISTS[0]}),
                                 ([], {"ok": False, "seq": None}))
        for artist in self.BAD_ARTISTS:
            with self.subTest(artist=artist):
                self.assertIsNone(pick_message({"kind": "video", "id": VIDEO, "dest": "Fan", "artist": artist}, 1))
                self.assertEqual(lines(Api(Host(2)).pick, {"kind": "video", "id": VIDEO, "artist": artist}),
                                 ([], {"ok": False, "seq": None}))
                self.assertEqual(lines(Api(Host(2)).pick, {"kind": "video", "id": VIDEO, "dest": "Fan", "artist": artist}),
                                 ([], {"ok": False, "seq": None}))
        # An artist goes only with the dest chosen for it.
        for pick in ({"kind": "channel", "id": CHANNEL, "artist": CHANNEL},
                     {"kind": "channel", "id": CHANNEL, "dest": None, "artist": CHANNEL}):
            with self.subTest(pick=pick):
                self.assertIsNone(pick_message(pick, 1))
                self.assertEqual(lines(Api(Host(2)).pick, pick), ([], {"ok": False, "seq": None}))
        # A malformed pick with a good dest, or none, is the plain refusal too.
        for pick in ({"kind": "video", "id": "short", "dest": "Fan"}, {"kind": "nope", "dest": ""}, "video", None):
            with self.subTest(pick=pick):
                self.assertEqual(lines(Api(Host(2)).pick, pick), ([], {"ok": False, "seq": None}))

    def test_api_dest(self):
        api = Api(Host(2))
        line = '{"type":"dest","key":"v:%s","dest":"Anime/Animenz Piano Sheets","artist":"%s"}' % (VIDEO, CHANNEL)
        self.assertEqual(lines(api.dest, "v:" + VIDEO, "Anime/Animenz Piano Sheets", CHANNEL), ([("qm", line)], True))
        self.assertEqual(lines(api.dest, "p:" + PLAYLIST, "", None),
                         ([("qm", '{"type":"dest","key":"p:%s","dest":""}' % PLAYLIST)], True))
        self.assertEqual(lines(api.dest, "s:7", "Ünïcode")[0], [("qm", '{"type":"dest","key":"s:7","dest":"Ünïcode"}')])
        for dest in self.BAD_DESTS + (None,):
            with self.subTest(dest=dest):
                self.assertEqual(lines(api.dest, "v:" + VIDEO, dest), ([], False))
        for artist in self.BAD_ARTISTS:
            with self.subTest(artist=artist):
                self.assertEqual(lines(api.dest, "v:" + VIDEO, "Fan", artist), ([], False))
        for key in ("", None, 3, ["v:" + VIDEO]):
            with self.subTest(key=key):
                self.assertEqual(lines(api.dest, key, "Fan"), ([], False))
        self.assertEqual(lines(Api(Host(1)).dest, "v:" + VIDEO, "Fan"), ([], False))

    def test_seq_always_rises(self):
        host = Host(2)
        seqs = [host.next_seq() for _ in range(50)]
        self.assertEqual(seqs, sorted(set(seqs)))
        host.last_seq = 10 ** 15
        self.assertEqual(host.next_seq(), 10 ** 15 + 1)

    def test_api_pick_protocol_2(self):
        printed, reply = lines(Api(Host(2)).pick, {"kind": "video", "id": VIDEO, "title": "Ünïcode"})
        self.assertTrue(reply["ok"])
        self.assertEqual(printed[0][0], "qm")
        self.assertEqual(json.loads(printed[0][1])["seq"], reply["seq"])
        self.assertIn('"title":"Ünïcode"', printed[0][1])  # compact, not ASCII-escaped
        self.assertEqual(lines(Api(Host(2)).pick, {"kind": "video", "id": "bad"}), ([], {"ok": False, "seq": None}))

    def test_api_pick_protocol_1(self):
        api = Api(Host(1))
        items = [{"id": VIDEO, "title": "One"}, {"id": OTHER_VIDEO, "title": "Two"}]
        self.assertEqual(lines(api.pick, {"kind": "video", "id": VIDEO})[0],
                         [("pick", "https://www.youtube.com/watch?v=" + VIDEO)])
        self.assertEqual(lines(api.pick, {"kind": "playlist", "id": PLAYLIST})[0],
                         [("pick-list", "https://www.youtube.com/playlist?list=" + PLAYLIST)])
        self.assertEqual(lines(api.pick, {"kind": "channel", "id": CHANNEL})[0],
                         [("pick-channel", f"https://www.youtube.com/channel/{CHANNEL}/videos")])
        self.assertEqual(lines(api.pick, {"kind": "mix", "id": VIDEO, "items": items})[0],
                         [("pick", "https://www.youtube.com/watch?v=" + VIDEO),
                          ("pick", "https://www.youtube.com/watch?v=" + OTHER_VIDEO)])
        self.assertEqual(lines(api.pick, {"kind": "link", "url": "https://example.com/a.mp3"})[0],
                         [("pick", "https://example.com/a.mp3")])

    def test_protocol_1_prints_none_of_the_new_lines(self):
        api = Api(Host(1))
        for call, args in ((api.drop, ("v:" + VIDEO,)), (api.order, (["v:" + VIDEO],)), (api.resume, ()),
                           (api.log, ("drift", "x")), (api.dest, ("v:" + VIDEO, "Anime"))):
            with self.subTest(call=call.__name__):
                self.assertEqual(lines(call, *args)[0], [])
        self.assertEqual(lines(api._host.tell, {"type": "mount", "surface": "served", "detail": ""})[0], [])

    def test_other_lines(self):
        api = Api(Host(2))
        self.assertEqual(lines(api.drop, "v:" + VIDEO), ([("qm", '{"type":"drop","key":"v:%s"}' % VIDEO)], True))
        self.assertEqual(lines(api.drop, "v:" + VIDEO, "p:" + PLAYLIST)[0],
                         [("qm", '{"type":"drop","key":"v:%s","parent":"p:%s"}' % (VIDEO, PLAYLIST))])
        self.assertEqual(lines(api.drop, "")[1], False)
        self.assertEqual(lines(api.order, ["a", "b"])[0], [("qm", '{"type":"order","keys":["a","b"]}')])
        self.assertEqual(lines(api.order, ["a", 3]), ([], False))
        self.assertEqual(lines(api.resume)[0], [("qm", '{"type":"resume"}')])
        self.assertEqual(lines(api.log, "drift", "line one\nline two\r\n" + "x" * 400)[0][0][0], "drift")
        drift = lines(api.log, "drift", "line one\nline two\r\n" + "x" * 400)[0][0][1]
        self.assertTrue(drift.startswith("line one line two x") and len(drift) == 300)
        self.assertEqual(lines(api.log, "script", "boom")[0], [("script error", "boom")])
        self.assertEqual(lines(api.log, "other", "boom"), ([], False))

    def test_open_external_is_https_only(self):
        api = Api(Host(2))
        for url in ("http://example.com/", "file:///C:/Windows/notepad.exe", "C:\\Windows\\notepad.exe", None):
            with self.subTest(url=url):
                self.assertFalse(api.open_external(url))

    def test_open_file_needs_a_known_mid(self):
        host, opened = Host(2), []
        host.open_in_app = lambda path: opened.append(path) or True
        api = Api(host)
        with tempfile.TemporaryDirectory() as folder:
            known, unknown = os.path.join(folder, "known.mid"), os.path.join(folder, "unknown.mid")
            for path in (known, unknown, os.path.join(folder, "known.txt")):
                open(path, "wb").close()
            ledger = os.path.join(folder, "converted.jsonl")
            with open(ledger, "w", encoding="utf-8") as file:
                file.write(json.dumps({"id": VIDEO, "path": known, "title": "", "at": ""}) + "\n")
            saved, convert.LEDGER = convert.LEDGER, ledger
            try:
                self.assertTrue(api.open_file(known))
                self.assertFalse(api.open_file(unknown))
                self.assertFalse(api.open_file(os.path.join(folder, "known.txt")))
                self.assertFalse(api.open_file(os.path.join(folder, "missing.mid")))
                host.receive({"type": "item", "key": "v:" + OTHER_VIDEO, "path": unknown})
                self.assertTrue(api.open_file(unknown))
            finally:
                convert.LEDGER = saved
        self.assertEqual(opened, [known, unknown])

    def test_api_exposes_only_methods(self):
        # pywebview hands the page every public attribute, and walks into objects.
        api = Api(Host(2))
        public = [name for name in dir(api) if not name.startswith("_")]
        self.assertEqual(sorted(public), sorted(["mounted", "pick", "drop", "order", "dest", "resume", "choose_folder",
                                                 "open_file", "raise_app", "open_external", "sign_in", "sign_out",
                                                 "log", "press"]))
        for name in public:
            self.assertTrue(callable(getattr(api, name)))


# A WebView2 for the trusted press: CoreWebView2 answers DevTools calls like Chromium, from a small model of the
# frames, the node at the point and the nodes inside Skip, and notes the threads.
TOP_FRAME = "TOP"
WATCH_FRAME = "WATCH"
WATCH_URL = "https://www.youtube.com/watch?v=" + VIDEO
SKIP_NODE = 40
PRESS_GAP_LATER = 61.0  # past the rate limit


class _Task:
    def __init__(self, core, result, done=True, error=None):
        self.core, self.Result, self.done, self.error = core, result, done, error

    def Wait(self, ms):
        self.core.waits.append(threading.current_thread().name)
        if self.error:
            raise self.error
        return self.done


class _Core:
    def __init__(self):
        self.calls = []  # (method, params as sent, thread name)
        self.waits = []  # the thread each task was awaited on
        self.hits = []  # (frame id, node id) the next hit tests find, in turn; then hit
        self.hit = (WATCH_FRAME, SKIP_NODE)
        self.skip = {SKIP_NODE}  # the nodes closest(SKIP) finds Skip from
        self.frames = [{"frame": {"id": WATCH_FRAME, "parentId": TOP_FRAME, "url": WATCH_URL}}]
        self.worlds = {}  # live isolated worlds: context id -> frame id
        self.made = 0
        self.fail = None  # a method, or a mouse event type, whose call throws
        self.hang = None  # a method, or a mouse event type, whose task never finishes
        self.refuse = None  # a method, or a mouse event type, whose task fails like a DevTools error
        self.gate = None  # (mouse event type, threading.Event): that call waits for the event
        self.reached = threading.Event()

    def navigate(self):
        """The frame loads another document: its isolated worlds are gone."""
        self.worlds.clear()

    def CallDevToolsProtocolMethodAsync(self, method, params):
        self.calls.append((method, params, threading.current_thread().name))
        sent = json.loads(params)
        name = sent.get("type") if method == "Input.dispatchMouseEvent" else method
        if self.gate and name == self.gate[0]:
            self.reached.set()
            self.gate[1].wait(5)
        if self.fail in (method, name):
            raise RuntimeError("no such call")
        error = RuntimeError("DevTools error") if self.refuse in (method, name) else None
        result = {}
        if method == "Page.getLayoutMetrics":
            result = {"cssLayoutViewport": {"clientWidth": 1266, "clientHeight": 823}}
        elif method == "DOM.getNodeForLocation":
            frame, node = self.hits.pop(0) if self.hits else self.hit
            result = {"backendNodeId": node, "frameId": frame}
        elif method == "Page.getFrameTree":
            result = {"frameTree": {"frame": {"id": TOP_FRAME, "url": "http://127.0.0.1:1/"},
                                    "childFrames": self.frames}}
        elif method == "Page.createIsolatedWorld":
            self.made += 1
            self.worlds[100 + self.made] = sent["frameId"]
            result = {"executionContextId": 100 + self.made}
        elif method == "DOM.resolveNode":
            if self.worlds.get(sent.get("executionContextId")) is None:
                error = RuntimeError("Cannot find context with specified id")
            result = {"object": {"type": "object", "objectId": f"node-{sent['backendNodeId']}"}}
        elif method == "Runtime.callFunctionOn":
            node = int(sent["objectId"].split("-")[1])
            # What ON_SKIP answers in a browser is tested in frame_test.js.
            found = sent["functionDeclaration"] == browse.ON_SKIP and sent["arguments"] == [{"value": browse.SKIP}]
            result = {"result": {"type": "boolean", "value": found and node in self.skip}}
        return _Task(self, json.dumps(result), self.hang not in (method, name), error)


class _Native:
    def __init__(self, core):
        self.browser = type("Browser", (), {"webview": type("WebView", (), {"CoreWebView2": core})()})()

    def Invoke(self, action):
        # Control.Invoke runs the delegate on the GUI thread and throws what it threw.
        thrown = []

        def run():
            try:
                action.fn()
            except Exception as error:
                thrown.append(error)

        gui = threading.Thread(target=run, name="gui")
        gui.start()
        gui.join()
        if thrown:
            raise thrown[0]


class _System:
    class Action:
        def __init__(self, fn):
            self.fn = fn


def _event(kind, x, y):
    event = {"type": kind, "x": x, "y": y, "button": "none" if kind == "mouseMoved" else "left",
             "buttons": 1 if kind == "mousePressed" else 0, "pointerType": "mouse"}
    if kind != "mouseMoved":
        event["clickCount"] = 1
    return event


class Press(unittest.TestCase):
    def setUp(self):
        self.host = Host(2)
        self.use(_Core())
        self.clock = [1000.0]
        patches = (mock.patch.dict(sys.modules, {"System": _System}),
                   mock.patch.object(browse.time, "monotonic", lambda: self.clock[0]))
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def use(self, core):
        self.core = core
        self.host.window = type("Window", (), {"native": _Native(core)})()

    def later(self, seconds=PRESS_GAP_LATER):
        self.core.calls.clear()
        self.clock[0] += seconds

    def methods(self):
        return [call[0] for call in self.core.calls]

    def mouse(self):
        return [json.loads(call[1]) for call in self.core.calls if call[0] == "Input.dispatchMouseEvent"]

    def kinds(self):
        return [(event["type"], event["x"], event["y"]) for event in self.mouse()]

    def test_one_left_click_on_skip_on_the_gui_thread(self):
        self.assertTrue(Api(self.host).press(1186.6, 231.5))
        self.assertEqual(self.methods(), [
            "Page.getLayoutMetrics",
            "DOM.getNodeForLocation", "Page.getFrameTree", "Page.createIsolatedWorld", "DOM.resolveNode",
            "Runtime.callFunctionOn",
            "Input.dispatchMouseEvent",
            "DOM.resolveNode", "Runtime.callFunctionOn", "DOM.getNodeForLocation",
            "Input.dispatchMouseEvent", "Input.dispatchMouseEvent", "Input.dispatchMouseEvent",
            "Runtime.releaseObjectGroup"])
        # Every call starts on the GUI thread and is awaited off it, where waiting cannot block its messages.
        self.assertEqual({call[2] for call in self.core.calls}, {"gui"})
        self.assertEqual(len(self.core.waits), len(self.core.calls))
        self.assertNotIn("gui", self.core.waits)
        self.assertEqual(self.core.calls[1][1], '{"x":1186,"y":231}')
        self.assertEqual(self.mouse(), [_event("mouseMoved", 1186, 231), _event("mousePressed", 1186, 231),
                                        _event("mouseReleased", 1186, 231), _event("mouseMoved", -1, -1)])
        # Skip is asked about in an isolated world of the hit's frame, made once, and its handles let go.
        resolves = [json.loads(call[1]) for call in self.core.calls if call[0] == "DOM.resolveNode"]
        self.assertEqual(resolves, [{"backendNodeId": SKIP_NODE, "executionContextId": 101,
                                     "objectGroup": browse.PRESS_GROUP}] * 2)
        self.assertEqual(json.loads(self.core.calls[3][1]), {"frameId": WATCH_FRAME, "worldName": "QuartzMIDI"})
        asked = json.loads(self.core.calls[5][1])
        self.assertEqual((asked["objectId"], asked["arguments"], asked["returnByValue"]),
                         (f"node-{SKIP_NODE}", [{"value": browse.SKIP}], True))
        self.assertEqual(json.loads(self.core.calls[-1][1]), {"objectGroup": browse.PRESS_GROUP})
        self.later()
        self.assertTrue(self.host.press(1186.6, 231.5))
        self.assertNotIn("Page.createIsolatedWorld", self.methods())

    def test_the_last_call_before_the_button_goes_down_is_the_hit_test(self):
        self.assertTrue(self.host.press(100, 100))
        pressed = next(i for i, call in enumerate(self.core.calls) if '"mousePressed"' in call[1])
        self.assertEqual(self.core.calls[pressed - 1][0], "DOM.getNodeForLocation")
        self.assertEqual(self.core.calls[pressed - 2][0], "Runtime.callFunctionOn")

    def test_refuses_what_is_not_a_point_in_the_viewport(self):
        for x, y in ((float("nan"), 10), (10, float("inf")), (True, 10), ("10", 10), (None, 10), ([10], 10),
                     (-0.5, 10), (10, -1), (1266, 10), (10, 823), (1e300, 10), (10 ** 400, 10)):
            with self.subTest(x=x, y=y):
                self.later()
                self.assertFalse(self.host.press(x, y))
                self.assertEqual(self.mouse(), [])
                self.assertNotIn("DOM.getNodeForLocation", self.methods())

    def test_presses_only_onto_the_watch_frame(self):
        cases = {
            "the page": lambda core: setattr(core, "hit", (TOP_FRAME, SKIP_NODE)),
            "another host": lambda core: core.frames[0]["frame"].update(url="https://example.com/watch?v=x"),
            "not /watch": lambda core: core.frames[0]["frame"].update(url="https://www.youtube.com/embed/x"),
            "http": lambda core: core.frames[0]["frame"].update(url="http://www.youtube.com/watch?v=x"),
            "not a child of the top": lambda core: core.frames[0]["frame"].update(parentId="OTHER"),
            "a grandchild": lambda core: core.frames.__setitem__(0, {
                "frame": {"id": "MID", "parentId": TOP_FRAME, "url": "https://www.youtube.com/embed/x"},
                "childFrames": [{"frame": {"id": WATCH_FRAME, "parentId": "MID", "url": WATCH_URL}}]}),
            "no frame id": lambda core: setattr(core, "hit", ("", SKIP_NODE)),
            "no node": lambda core: setattr(core, "hit", (WATCH_FRAME, None)),
        }
        for name, change in cases.items():
            with self.subTest(case=name):
                self.use(_Core())
                change(self.core)
                self.later()
                self.assertFalse(self.host.press(100, 100))
                self.assertEqual(self.mouse(), [])

    def test_presses_only_skip(self):
        # The /watch frame, but the node there is not Skip nor inside it: nothing at all is sent.
        self.core.skip = set()
        self.assertFalse(self.host.press(100, 100))
        self.assertEqual(self.mouse(), [])
        self.assertIn("Runtime.callFunctionOn", self.methods())
        # A page or YouTube script cannot answer for closest(): it is asked in an isolated world.
        resolve = next(json.loads(call[1]) for call in self.core.calls if call[0] == "DOM.resolveNode")
        self.assertIn(resolve["executionContextId"], self.core.worlds)
        for refused in ("Page.createIsolatedWorld", "DOM.resolveNode", "Runtime.callFunctionOn"):
            with self.subTest(failing=refused):
                self.use(_Core())
                self.core.refuse = refused
                self.later()
                self.assertFalse(self.host.press(100, 100))
                self.assertEqual(self.mouse(), [])

    def test_refuses_when_skip_has_gone_from_the_point_before_the_press(self):
        cases = {
            "another node": [(WATCH_FRAME, SKIP_NODE), (WATCH_FRAME, 41)],
            "the page": [(WATCH_FRAME, SKIP_NODE), (TOP_FRAME, 7)],
            "nothing": [(WATCH_FRAME, SKIP_NODE), ("", None)],
        }
        for name, hits in cases.items():
            with self.subTest(case=name):
                self.use(_Core())
                self.core.hits = list(hits)
                self.later()
                self.assertFalse(self.host.press(100, 100))
                # Only the move onto the point and the move away: the button never goes down.
                self.assertEqual(self.kinds(), [("mouseMoved", 100, 100), ("mouseMoved", -1, -1)])
                self.assertEqual(self.methods()[-1], "Runtime.releaseObjectGroup")
        # Skip's node has left Skip by the second look.
        self.use(_Core())
        original = self.core.CallDevToolsProtocolMethodAsync

        def leave(method, params):
            if method == "Input.dispatchMouseEvent":
                self.core.skip = set()
            return original(method, params)

        self.core.CallDevToolsProtocolMethodAsync = leave
        self.later()
        self.assertFalse(self.host.press(100, 100))
        self.assertEqual(self.kinds(), [("mouseMoved", 100, 100), ("mouseMoved", -1, -1)])

    def test_another_node_of_skip_after_the_move_is_looked_at_again(self):
        # The move brings up a hover state: the hit is another node, inside Skip, then that node again.
        self.core.skip = {SKIP_NODE, 41}
        self.core.hits = [(WATCH_FRAME, SKIP_NODE), (WATCH_FRAME, 41), (WATCH_FRAME, 41)]
        self.assertTrue(self.host.press(100, 100))
        self.assertIn(("mousePressed", 100, 100), self.kinds())
        # A node outside Skip, or the hit still moving after two looks, is refused.
        for hits, skip in (([(WATCH_FRAME, SKIP_NODE), (WATCH_FRAME, 41)], {SKIP_NODE}),
                           ([(WATCH_FRAME, SKIP_NODE), (WATCH_FRAME, 41), (WATCH_FRAME, 42), (WATCH_FRAME, 43)],
                            {SKIP_NODE, 41, 42, 43})):
            with self.subTest(hits=hits):
                self.use(_Core())
                self.core.hits, self.core.skip = list(hits), skip
                self.later()
                self.assertFalse(self.host.press(100, 100))
                self.assertNotIn("mousePressed", [kind for kind, _, _ in self.kinds()])

    def test_once_the_button_is_tried_it_is_released_and_the_pointer_leaves(self):
        both = [("mouseMoved", 100, 100), ("mousePressed", 100, 100), ("mouseReleased", 100, 100),
                ("mouseMoved", -1, -1)]
        for name, change in (("the press never answers", lambda core: setattr(core, "hang", "mousePressed")),
                             ("the press throws", lambda core: setattr(core, "fail", "mousePressed")),
                             ("the press fails", lambda core: setattr(core, "refuse", "mousePressed")),
                             ("the release throws", lambda core: setattr(core, "fail", "mouseReleased")),
                             ("the release never answers", lambda core: setattr(core, "hang", "mouseReleased"))):
            with self.subTest(case=name):
                self.use(_Core())
                change(self.core)
                self.later()
                self.assertFalse(self.host.press(100, 100))
                self.assertEqual(self.kinds(), both)
                self.assertEqual(self.methods()[-1], "Runtime.releaseObjectGroup")
        # The move onto Skip fails: the button is not tried, and the pointer still leaves.
        self.use(_Core())
        self.core.hang = "mouseMoved"
        self.later()
        self.assertFalse(self.host.press(100, 100))
        self.assertEqual(self.kinds(), [("mouseMoved", 100, 100), ("mouseMoved", -1, -1)])

    def test_rate_limit_and_retry(self):
        # A refused ask is let through again after PRESS_GAP, under the page's 250 ms poll.
        self.assertLess(browse.PRESS_GAP, 0.25)
        self.core.skip = set()
        self.assertFalse(self.host.press(100, 100))
        self.later(browse.PRESS_GAP / 2)
        self.assertFalse(self.host.press(100, 100))
        self.assertEqual(self.methods(), [])  # too soon: not even looked at
        self.core.skip = {SKIP_NODE}
        self.later(browse.PRESS_GAP)
        first = self.clock[0]
        self.assertTrue(self.host.press(100, 100))
        # Refused asks do not count towards the ten presses a minute; presses that put the button down do.
        for _ in range(browse.PRESS_MOST - 1):
            self.core.skip = set()
            self.later(browse.PRESS_GAP)
            self.assertFalse(self.host.press(100, 100))
            self.core.skip = {SKIP_NODE}
            self.later(browse.PRESS_GAP)
            self.assertTrue(self.host.press(100, 100))
        self.later(browse.PRESS_GAP)
        self.assertFalse(self.host.press(100, 100))  # ten in the last minute
        self.assertEqual(self.methods(), [])
        self.clock[0] = first + browse.PRESS_SPAN
        self.assertTrue(self.host.press(100, 100))  # the first has left the minute
        # A press that failed after the button was tried counts too.
        self.host.presses.clear()
        self.core.refuse = "mousePressed"
        for _ in range(browse.PRESS_MOST):
            self.later(browse.PRESS_GAP)
            self.assertFalse(self.host.press(100, 100))
        self.core.refuse = None
        self.later(browse.PRESS_GAP)
        self.assertFalse(self.host.press(100, 100))
        self.assertEqual(self.methods(), [])

    def test_two_presses_never_interleave(self):
        go = threading.Event()
        self.core.gate = ("mousePressed", go)
        result = []
        first = threading.Thread(target=lambda: result.append(self.host.press(100, 100)))
        first.start()
        self.assertTrue(self.core.reached.wait(5))
        self.clock[0] += PRESS_GAP_LATER
        sent = len(self.core.calls)
        self.assertFalse(self.host.press(200, 200))
        self.assertEqual(len(self.core.calls), sent)
        go.set()
        first.join(5)
        self.assertEqual(result, [True])
        self.assertEqual(self.kinds(), [("mouseMoved", 100, 100), ("mousePressed", 100, 100),
                                        ("mouseReleased", 100, 100), ("mouseMoved", -1, -1)])

    def test_the_isolated_world_is_made_again_for_a_new_document(self):
        self.assertTrue(self.host.press(100, 100))
        self.core.navigate()
        self.later()
        self.assertTrue(self.host.press(100, 100))
        self.assertEqual(self.methods().count("Page.createIsolatedWorld"), 1)
        self.assertEqual(self.core.made, 2)
        # Made for the hit's own frame, and a world that cannot be made refuses the press.
        self.assertEqual(set(self.core.worlds.values()), {WATCH_FRAME})
        self.core.navigate()
        self.core.refuse = "Page.createIsolatedWorld"
        self.later()
        self.assertFalse(self.host.press(100, 100))
        self.assertEqual(self.mouse(), [])

    def test_skip_is_what_frame_js_calls_skip(self):
        with open(os.path.join(HERE, "page", "frame.js"), encoding="utf-8") as file:
            found = re.search(r'const SKIP = "([^"]+)";', file.read())
        self.assertEqual(found.group(1), browse.SKIP)

    def test_a_failed_or_missing_call_presses_nothing(self):
        for name, change in (("CDP throws", lambda core: setattr(core, "fail", "Page.getLayoutMetrics")),
                             ("CDP never answers", lambda core: setattr(core, "hang", "DOM.getNodeForLocation")),
                             ("CDP fails", lambda core: setattr(core, "refuse", "Page.getFrameTree"))):
            with self.subTest(case=name):
                self.use(_Core())
                change(self.core)
                self.later()
                self.assertFalse(self.host.press(100, 100))
                self.assertEqual(self.mouse(), [])
        self.host.window = None
        self.later()
        self.assertFalse(self.host.press(100, 100))
        with mock.patch.dict(sys.modules, {"System": None}):
            self.use(_Core())
            self.later()
            self.assertFalse(self.host.press(100, 100))
            self.assertEqual(self.core.calls, [])


class Messages(unittest.TestCase):
    def test_coalescing_keys(self):
        cases = [({"type": "init"}, "init"), ({"type": "theme"}, "theme"), ({"type": "item", "key": "v:1"}, "item:v:1"),
                 ({"type": "child", "parent": "p:1", "key": "v:2"}, "child:p:1/v:2"), ({"type": "paused"}, "pause"),
                 ({"type": "resumed"}, "pause"), ({"type": "signed"}, "signed"), ({"type": "search"}, "search"),
                 ({"type": "signin"}, "signin"), ({"type": "folders", "folders": []}, "folders"),
                 ({"type": "unknown"}, None)]
        for message, key in cases:
            with self.subTest(message=message):
                self.assertEqual(coalescing_key(message), key)

    def drain(self, mailbox):
        taken = []
        with mailbox.ready:
            while mailbox.open and mailbox.waiting:
                taken.append(mailbox.waiting.popitem(last=False)[1])
        return taken

    def test_buffered_until_mounted_coalesced_in_place_init_first(self):
        mailbox = Mailbox()
        mailbox.put({"type": "item", "key": "a", "state": "queued"})
        mailbox.put({"type": "paused", "reason": "folder"})
        mailbox.put({"type": "item", "key": "b", "state": "queued"})
        mailbox.put({"type": "item", "key": "a", "state": "converting"})
        mailbox.put({"type": "resumed"})
        mailbox.put({"type": "init", "protocol": 2})
        mailbox.put({"type": "later"})
        mailbox.put({"type": "later"})
        self.assertEqual(self.drain(mailbox), [])
        mailbox.mount()
        self.assertEqual(self.drain(mailbox), [
            {"type": "init", "protocol": 2}, {"type": "item", "key": "a", "state": "converting"},
            {"type": "resumed"}, {"type": "item", "key": "b", "state": "queued"}, {"type": "later"}, {"type": "later"}])

    def test_a_new_page_gets_the_latest_state_but_not_one_shots(self):
        mailbox = Mailbox()
        mailbox.mount()
        for message in ({"type": "init"}, {"type": "theme", "skin": 1}, {"type": "search", "q": "x"},
                        {"type": "signin"}, {"type": "item", "key": "a"}):
            mailbox.put(message)
        self.assertEqual(len(self.drain(mailbox)), 5)
        mailbox.close()
        mailbox.put({"type": "theme", "skin": 2})
        mailbox.put({"type": "search", "q": "y"})
        mailbox.mount()
        self.assertEqual(self.drain(mailbox), [{"type": "init"}, {"type": "theme", "skin": 2},
                                               {"type": "item", "key": "a"}, {"type": "search", "q": "y"}])

    def test_folders_are_replayed_to_a_remounted_page(self):
        host = Host(2)
        init = {"type": "init", "protocol": 2, "folders": ["Anime"]}
        host.receive(init)
        host.mailbox.mount()
        self.assertEqual(self.drain(host.mailbox), [init])
        host.mailbox.close()  # the page reloads
        host.receive({"type": "folders", "folders": ["Anime", "Anime/Animenz Piano Sheets"]})
        latest = {"type": "folders", "folders": ["Anime", "Classical"]}
        host.receive(latest)
        host.mailbox.mount()
        self.assertEqual(self.drain(host.mailbox), [init, latest])  # coalesced, after init's own list
        host.mailbox.mount()  # and every later page has them too
        self.assertEqual(self.drain(host.mailbox), [init, latest])
        host.receive({"type": "item", "key": "v:" + VIDEO, "state": "queued", "dest": "Classical"})
        host.mailbox.close()
        host.mailbox.mount()
        self.assertEqual([message["type"] for message in self.drain(host.mailbox)], ["init", "folders", "item"])

    def test_init_words_before_the_page_sets_off_are_its_route(self):
        host = Host(2)
        host.receive({"type": "init", "protocol": 2, "search": " clair de lune "})
        self.assertEqual(host.route, "?q=clair+de+lune")
        host.mailbox.mount()
        self.assertEqual([message["type"] for message in self.drain(host.mailbox)], ["init"])

    def test_init_words_after_the_page_set_off_come_as_a_search(self):
        host, loaded = Host(2), []
        host.window = types.SimpleNamespace(load_url=loaded.append)
        core, webview2 = mock.MagicMock(), mock.MagicMock()
        modules = {"Microsoft": webview2, "Microsoft.Web": webview2, "Microsoft.Web.WebView2": webview2,
                   "Microsoft.Web.WebView2.Core": webview2}
        with mock.patch.dict(sys.modules, modules), \
                mock.patch.object(browse, "later", lambda target, *args, seconds=0: target(*args)):
            host.ready(types.SimpleNamespace(CoreWebView2=core), types.SimpleNamespace(IsSuccess=True))
        self.assertEqual(loaded, ["https://www.youtube.com/quartzmidi"])
        init = {"type": "init", "protocol": 2, "search": " clair de lune "}
        host.receive(init)
        self.assertEqual(host.route, "")  # too late for the route the page was loaded with
        host.mailbox.mount()
        self.assertEqual(self.drain(host.mailbox), [init, {"type": "search", "q": "clair de lune"}])
        host.mailbox.mount()
        self.assertEqual(self.drain(host.mailbox), [init])  # a search is acted on once
        for search in (None, "", "   ", 5):
            with self.subTest(search=search):
                host.receive({"type": "init", "protocol": 2, "search": search})
                self.assertEqual([message["type"] for message in self.drain(host.mailbox)], ["init"])

    def test_a_returned_one_shot_never_replaces_a_newer_one(self):
        mailbox = Mailbox()
        mailbox.mount()
        mailbox.put({"type": "search", "q": "x"})
        taken = mailbox.take()
        mailbox.close()
        mailbox.put({"type": "search", "q": "y"})
        mailbox.put_back(taken)
        mailbox.mount()
        self.assertEqual(self.drain(mailbox), [{"type": "search", "q": "y"}])
        mailbox.close()
        mailbox.put_back(taken)  # nothing newer: it waits for the next page
        mailbox.mount()
        self.assertEqual(self.drain(mailbox), [{"type": "search", "q": "x"}])
        mailbox.mount()
        self.assertEqual(self.drain(mailbox), [])  # and is never replayed after that

    def test_a_one_shot_the_page_missed_is_sent_again_unless_a_newer_came(self):
        host = Host(2)
        sent = []

        class Window:
            def evaluate_js(self, script):
                sent.append(json.loads(re.search(r"qm\.receive\((.*)\), true\)", script).group(1))["q"])
                if len(sent) == 1:
                    host.mailbox.put({"type": "search", "q": "y"})  # came while the page was away
                    return False
                host.mailbox.end()
                return True

        host.window = Window()
        host.mailbox.mount()
        host.mailbox.put({"type": "search", "q": "x"})
        host.deliver()
        self.assertEqual(sent, ["x", "y"])

    def test_take_waits_and_ends(self):
        mailbox = Mailbox()
        mailbox.mount()
        mailbox.put({"type": "signed", "in": True})
        self.assertEqual(mailbox.take(), {"type": "signed", "in": True})
        mailbox.end()
        self.assertIsNone(mailbox.take())

    def test_stdin_dispatch(self):
        host = Host(2)
        light = {"dark": False, "colours": {"canvas": "#E0E4E9"}, "shape": {}}
        stream = io.BytesIO(b"\n".join([
            json.dumps({"type": "init", "protocol": 2, "pid": 1234, "skin": light, "signedIn": False,
                        "search": "clair de lune"}).encode(),
            b"not json", b"[1, 2]", b'{"no": "type"}',
            json.dumps({"type": "item", "key": "v:" + VIDEO, "state": "done", "path": "C:\\Music\\a.mid"}).encode(),
            json.dumps({"type": "theme", "skin": browse.DEFAULT_SKIN}, ensure_ascii=False).encode(),
        ]) + b"\n")
        host.read(stream)
        self.assertTrue(host.first.is_set() and host.eof.is_set())
        self.assertEqual(host.pid, 1234)
        self.assertEqual(host.route, "?q=clair+de+lune")
        self.assertEqual(host.skin, browse.DEFAULT_SKIN)  # the theme after init
        self.assertIn(browse.same_file("C:\\Music\\a.mid"), host.paths)
        self.assertEqual([message["type"] for message in host.mailbox.latest.values()], ["init", "item", "theme"])
        self.assertTrue(host.mailbox.ended)  # the end of stdin closes the window

    def test_protocol_1_synthesised_init_has_no_pid(self):
        host = Host(1)
        host.receive({"type": "init", "protocol": 1, "skin": browse.DEFAULT_SKIN, "signedIn": False})
        self.assertIsNone(host.pid)
        self.assertEqual(next(iter(host.mailbox.latest)), "init")


class CookieFile(unittest.TestCase):
    def write(self, folder, text):
        path = os.path.join(folder, "cookies.txt")
        with open(path, "w", encoding="utf-8") as file:
            file.write(text)
        return path

    def test_written_by_signin_is_deleted(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.write(folder, "# Netscape HTTP Cookie File\n# Written by signin.py for yt-dlp. Do not share.\n")
            self.assertTrue(browse.forget_cookie_file(path))
            self.assertFalse(os.path.exists(path))

    def test_others_are_left_alone(self):
        with tempfile.TemporaryDirectory() as folder:
            for text in ("# Netscape HTTP Cookie File\n# This is a generated file!\n", "# Written by signin.py\n", ""):
                with self.subTest(text=text):
                    path = self.write(folder, text)
                    self.assertFalse(browse.forget_cookie_file(path))
                    self.assertTrue(os.path.exists(path))
            self.assertFalse(browse.forget_cookie_file(os.path.join(folder, "missing.txt")))

    def test_matches_what_signin_writes(self):
        import signin
        with tempfile.TemporaryDirectory() as folder:
            saved, signin.COOKIES = signin.COOKIES, os.path.join(folder, "cookies.txt")
            try:
                from http.cookies import SimpleCookie
                jar = SimpleCookie()
                jar["SID"] = "x"
                jar["SID"]["domain"] = ".youtube.com"
                signin.write_cookies([jar])
                self.assertTrue(browse.forget_cookie_file(signin.COOKIES))
            finally:
                signin.COOKIES = saved


class Served(unittest.TestCase):
    def test_csp_and_headers(self):
        self.assertEqual(browse.CSP, (
            "default-src 'self'; script-src 'self' 'unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' "
            "https://i.ytimg.com https://yt3.ggpht.com https://yt3.googleusercontent.com data: blob:; connect-src "
            "'self' https://suggestqueries-clients6.youtube.com; frame-src 'self'; font-src 'self'; media-src 'self' "
            "blob:"))
        headers = browse.header_text("text/css").split("\r\n")
        self.assertIn("Content-Security-Policy: " + browse.CSP, headers)
        self.assertIn("Cache-Control: no-store", headers)
        self.assertIn("X-Content-Type-Options: nosniff", headers)
        self.assertIn("Content-Type: text/css", headers)
        self.assertEqual(len(browse.header_text(None).split("\r\n")), 3)

    def test_paths(self):
        self.assertEqual(serve("/sw.js"), (404, None, b""))
        for path in ("/sw.js_data", "/", "/watch", "/youtubei/v1/search", "/quartzmidix", "/robots.txt"):
            with self.subTest(path=path):
                self.assertIsNone(serve(path))
        for path in ("/quartzmidi/", "/quartzmidi/boot.js", "/quartzmidi/index.html", "/quartzmidi/icons.svg",
                     "/quartzmidi/../browse.py", "/quartzmidi/fonts/../../browse.py", "/quartzmidi/fonts/x.ttf",
                     "/quartzmidi/fonts/sub/IBMPlexSans-Regular.ttf", "/quartzmidi/converted.jsonl"):
            with self.subTest(path=path):
                self.assertEqual(serve(path)[0], 404)

    def test_page_has_the_sprite_in_its_body(self):
        status, kind, body = serve("/quartzmidi")
        self.assertEqual((status, kind), (200, "text/html; charset=utf-8"))
        page = body.decode("utf-8")
        self.assertLess(page.index("<body"), page.index('<symbol id="i-'))
        self.assertLess(page.index('<symbol id="i-'), page.index('id="bar"'))
        self.assertEqual(page.count("<svg xmlns"), 1)

    def test_files_and_types(self):
        self.assertEqual(serve("/quartzmidi/rules.json"), (200, "application/json", piano_match.rules_json().encode()))
        status, kind, body = serve("/quartzmidi/fonts/IBMPlexSans-SemiBold.ttf")
        self.assertEqual((status, kind, body[:4]), (200, "font/ttf", b"\x00\x01\x00\x00"))
        status, kind, body = serve("/quartzmidi/QuartzMIDI.ico")
        self.assertEqual((status, kind, body[:4]), (200, "image/x-icon", b"\x00\x00\x01\x00"))
        for name, kind in (("parse.js", "text/javascript"), ("app.js", "text/javascript"),
                           ("frame.js", "text/javascript"), ("app.css", "text/css")):
            with self.subTest(name=name):
                exists = os.path.isfile(os.path.join(HERE, "page", name))
                self.assertEqual(serve("/quartzmidi/" + name)[:2], (200, kind) if exists else (404, None))

    def test_converted_json_lists_files_that_exist(self):
        with tempfile.TemporaryDirectory() as folder:
            kept, gone = os.path.join(folder, "kept.mid"), os.path.join(folder, "gone.mid")
            open(kept, "wb").close()
            ledger = os.path.join(folder, "converted.jsonl")
            with open(ledger, "w", encoding="utf-8") as file:
                for record in ({"id": VIDEO, "path": gone}, {"id": OTHER_VIDEO, "path": gone},
                               {"id": OTHER_VIDEO, "path": kept}, {"id": "aaaaaaaaaaa", "path": gone}):
                    file.write(json.dumps(record) + "\n")
                file.write("not json\n")
            saved, convert.LEDGER = convert.LEDGER, ledger
            try:
                self.assertEqual(json.loads(serve("/quartzmidi/converted.json")[2]), {OTHER_VIDEO: kept})
                os.remove(kept)  # re-read on every request
                self.assertEqual(json.loads(serve("/quartzmidi/converted.json")[2]), {})
            finally:
                convert.LEDGER = saved

    def artists(self, folder, records, tail=""):
        """artists.json for a ledger of these records, and its text."""
        ledger = os.path.join(folder, "converted.jsonl")
        with open(ledger, "w", encoding="utf-8") as file:
            for record in records:
                file.write(json.dumps(record, ensure_ascii=False) + "\n")
            file.write(tail)
        saved, convert.LEDGER = convert.LEDGER, ledger
        try:
            status, kind, body = serve("/quartzmidi/artists.json")
        finally:
            convert.LEDGER = saved
        self.assertEqual((status, kind), (200, "application/json"))
        return json.loads(body), body.decode("utf-8")

    def test_artists_json_from_the_ledger_without_paths(self):
        kassia, rousseau = "UC" + "k" * 22, "UC" + "r" * 22
        with tempfile.TemporaryDirectory() as folder:
            def record(video, at, **fields):
                return {"id": video, "path": os.path.join(folder, "Anime", video + ".mid"), "title": "T " + video,
                        "at": at, **fields}

            found, text = self.artists(folder, [
                record(VIDEO, "2026-10-01T10:00:00+01:00", channel="Animenz", channelId=CHANNEL,
                       dest="Anime/Animenz Piano Sheets", artist=True),
                record("aaaaaaaaaaa", "2026-09-30T10:00:00+01:00", channel="Animenz Piano Sheets", channelId=CHANNEL,
                       dest="Anime", artist=True),
                # Newer than the next line, whose clock, in another zone, reads later.
                record("bbbbbbbbbbb", "2026-10-02T09:30:00+00:00", channel="Kassia", channelId=kassia, dest="Kassia"),
                record("ccccccccccc", "2026-10-02T10:00:00+01:00", channel="Kassia Piano", channelId=kassia),
                record(OTHER_VIDEO, "2026-10-03T08:00:00+01:00"),  # from before channels were kept: a seed only
                record("ddddddddddd", "2026-09-01T08:00:00+01:00", channel="Rousseau", channelId=rousseau, dest="",
                       artist=True),
                record("eeeeeeeeeee", "2026-09-02T08:00:00+01:00", channel="Fake", channelId="UCnot-a-channel"),
                record("fffffffffff", "not a time", channel="Rousseau", channelId=rousseau, dest="../x", artist=True),
            ], tail='{"id": "ggggggggggg", "path": "C:\\\\Music\\\\g.mid", "chann')  # a partial last line
        self.assertEqual(found, {
            "artists": [
                {"id": kassia, "name": "Kassia", "n": 2, "at": "2026-10-02T09:30:00+00:00"},
                {"id": CHANNEL, "name": "Animenz", "n": 2, "at": "2026-10-01T10:00:00+01:00",
                 "dest": "Anime/Animenz Piano Sheets"},
                # The newest record that chose for the artist names its folder: here the MIDI folder.
                {"id": rousseau, "name": "Rousseau", "n": 2, "at": "2026-09-01T08:00:00+01:00", "dest": ""},
            ],
            "seeds": [
                {"id": OTHER_VIDEO, "title": "T " + OTHER_VIDEO, "channelId": None},
                {"id": "bbbbbbbbbbb", "title": "T bbbbbbbbbbb", "channelId": kassia},
                {"id": "ccccccccccc", "title": "T ccccccccccc", "channelId": kassia},
                {"id": VIDEO, "title": "T " + VIDEO, "channelId": CHANNEL},
                {"id": "aaaaaaaaaaa", "title": "T aaaaaaaaaaa", "channelId": CHANNEL},
                {"id": "eeeeeeeeeee", "title": "T eeeeeeeeeee", "channelId": None},
                {"id": "ddddddddddd", "title": "T ddddddddddd", "channelId": rousseau},
                {"id": "fffffffffff", "title": "T fffffffffff", "channelId": rousseau},
            ],
        })
        self.assertNotIn("path", text)
        self.assertNotIn(folder.replace("\\", "\\\\"), text)
        self.assertNotIn(".mid", text)

    def test_artists_json_keeps_the_newest_300_artists_and_20_seeds(self):
        start = datetime.datetime(2026, 9, 1, tzinfo=datetime.timezone.utc)
        records = [{"id": f"vid{number:08d}", "path": f"C:\\Music\\{number}.mid", "title": str(number),
                    "at": (start + datetime.timedelta(minutes=number)).isoformat(), "channel": f"Pianist {number}",
                    "channelId": f"UC{number:022d}"} for number in range(305)]
        with tempfile.TemporaryDirectory() as folder:
            found, _ = self.artists(folder, records)
            self.assertEqual([artist["id"] for artist in found["artists"]],
                             [f"UC{number:022d}" for number in range(304, 4, -1)])
            self.assertEqual([seed["id"] for seed in found["seeds"]],
                             [f"vid{number:08d}" for number in range(304, 284, -1)])
            self.assertEqual(self.artists(folder, [])[0], {"artists": [], "seeds": []})
            saved, convert.LEDGER = convert.LEDGER, os.path.join(folder, "missing.jsonl")
            try:
                self.assertEqual(json.loads(serve("/quartzmidi/artists.json")[2]), {"artists": [], "seeds": []})
            finally:
                convert.LEDGER = saved


class Pages(unittest.TestCase):
    def test_page_scripts_never_write_html(self):
        found = []
        page = os.path.join(HERE, "page")
        for name in os.listdir(page):
            if name.endswith(".js"):
                with open(os.path.join(page, name), encoding="utf-8") as file:
                    for number, line in enumerate(file, 1):
                        if re.search(r"innerHTML|outerHTML|insertAdjacentHTML|document\.write", line):
                            found.append(f"{name}:{number}")
        self.assertEqual(found, [])

    def test_index_uses_only_the_words_of_the_window(self):
        words = {"Back", "Forward", "Home", "Search YouTube", "Clear", "Piano", "Piano: On", "Sign in", "Account", "QuartzMIDI",
                 "Videos", "Playlists", "Channels", "Any length", "Sort", "Piano videos", "Convert", "Mix", "Play",
                 "Volume", "Enlarge", "Copy link", "Open on YouTube", "Queue", "Converted", "Collapse", "0",
                 "YouTube for QuartzMIDI"}
        with open(os.path.join(HERE, "page", "index.html"), encoding="utf-8") as file:
            page = file.read()
        texts = re.findall(r">\s*([^<>]*?\S)\s*<", page)
        attributes = re.findall(r'(?:title|placeholder|aria-label)="([^"]*)"', page)
        self.assertEqual(sorted(set(texts + attributes) - words), [])
        self.assertIn('<iframe id="player" tabindex="-1" style="pointer-events:none">', page)
        self.assertLess(page.index("app.css"), page.index("parse.js"))
        self.assertLess(page.index("parse.js"), page.index("app.js"))

    def test_canvas_boot(self):
        with tempfile.TemporaryDirectory() as folder:
            kept = os.path.join(folder, "kept.mid")
            open(kept, "wb").close()
            ledger = os.path.join(folder, "converted.jsonl")
            with open(ledger, "w", encoding="utf-8") as file:
                file.write(json.dumps({"id": VIDEO, "path": kept, "title": "One", "at": "2026-10-01T10:00:00+01:00",
                                       "channel": "Fan", "channelId": CHANNEL}) + "\n")
            saved, convert.LEDGER = convert.LEDGER, ledger
            try:
                scripts = browse.canvas_scripts("?q=x")
                self.assertTrue(scripts[0].startswith("globalThis.QMBoot = "))
                boot = json.loads(scripts[0][len("globalThis.QMBoot = "):scripts[0].rindex(";")])
                self.assertEqual(boot["converted"], {VIDEO: kept})
                self.assertEqual(boot["converted"], json.loads(serve("/quartzmidi/converted.json")[2]))
                self.assertEqual(boot["artists"]["seeds"], [{"id": VIDEO, "title": "One", "channelId": CHANNEL}])
                self.assertEqual(boot["artists"], json.loads(serve("/quartzmidi/artists.json")[2]))
            finally:
                convert.LEDGER = saved
        self.assertEqual((boot["surface"], boot["route"]), ("canvas", "?q=x"))
        # What the served page would fetch, carried in QMBoot instead.
        self.assertEqual(boot["title"], browse.TITLE)
        css = os.path.join(HERE, "page", "app.css")
        if os.path.isfile(css):
            with open(css, encoding="utf-8", newline="") as file:
                css = file.read()
        else:
            css = ""
        self.assertEqual(boot["css"], css)
        self.assertEqual(base64.b64decode(boot["icon"]), serve("/quartzmidi/QuartzMIDI.ico")[2])
        self.assertEqual(boot["rules"], json.loads(piano_match.rules_json()))
        self.assertEqual([font["weight"] for font in boot["fonts"]], [400, 500, 600])
        self.assertEqual(boot["icons"]["tag"], "svg")
        self.assertTrue(all(symbol["tag"] == "symbol" for symbol in boot["icons"]["children"]))
        self.assertEqual(boot["body"]["tag"], "body")
        tree = json.dumps(boot["body"])
        self.assertNotIn('"script"', tree)
        self.assertIn('"player"', tree)
        self.assertIn('"svg": true', tree)
        present = [name for name in ("parse.js", "app.js", "boot.js") if os.path.isfile(os.path.join(HERE, "page", name))]
        self.assertEqual(len(scripts), 1 + len(present))

    def test_failure_page(self):
        page = browse.failure_page(browse.DEFAULT_SKIN, "?q=chopin")
        for words in ("YouTube did not load.", "Retry", "Open on YouTube", "Paste a link", "Convert", "Sent"):
            self.assertIn(words, page)
        self.assertIn('"https://www.youtube.com/quartzmidi?q=chopin"', page)
        self.assertIn('"https://www.youtube.com/results?search_query=chopin"', page)
        self.assertIn("--canvas:#191C21;", page)
        self.assertNotIn("__", page.replace("__quartzPage", ""))
        self.assertNotRegex(page, r"(?:src|href)=\"https?:")  # nothing from the network
        light = browse.failure_page({"dark": False, "colours": {"canvas": "#E0E4E9"}, "shape": {"scale": 1.25}}, "")
        self.assertIn("--canvas:#E0E4E9;", light)
        self.assertIn("color-scheme:light", light)
        self.assertIn("zoom:1.25", light)
        self.assertIn('<style id="theme">' + browse.theme_css(browse.DEFAULT_SKIN) + "</style>",
                      browse.failure_page(browse.DEFAULT_SKIN, ""))

    def test_failure_page_takes_a_theme_in_place(self):
        class Window:
            def __init__(self, answer):
                self.answer, self.scripts, self.pages = answer, [], []

            def evaluate_js(self, script):
                self.scripts.append(script)
                return self.answer

            def load_html(self, page):
                self.pages.append(page)

        light = {"dark": False, "colours": {"canvas": "#E0E4E9"}}
        for answer, reloads in ((True, 0), (False, 1)):
            host = browse.Host(2)
            host.window, host.surface, host.skin = Window(answer), "failed", light
            host.retheme_failure(light)
            self.assertIn(json.dumps(browse.theme_css(light)), host.window.scripts[0])
            self.assertEqual(len(host.window.pages), reloads)

    def test_accent_ink(self):
        self.assertEqual(browse.ink_on("#4EA3EA"), "#101218")
        self.assertEqual(browse.ink_on("#0B6EC4"), "#FFFFFF")
        self.assertEqual(browse.colour({"colours": {"accent": "red"}}, "accent"), "#4EA3EA")


# Logical px asked for: the YouTube window and its minimum, and the sign-in window.
YOUTUBE, YOUTUBE_LEAST, SIGN_IN = (1280, 860), (640, 480), (520, 720)


def fitted(work, dpi, size, least=(200, 100), over=None):
    """fit_window's logical answer, and the physical rect pywebview makes of it."""
    x, y, width, height, minimum = fit_window(work, dpi, size, least, over)
    left, top = physical(x, dpi), physical(y, dpi)
    return (x, y, width, height, minimum), (left, top, left + physical(width, dpi), top + physical(height, dpi))


def as_placement(fit):
    return dict(zip(("x", "y", "width", "height", "min_size"), fit))


def fake_user32(monitors=None, windows=None, cursor=0, previous=0x1234, top=()):
    """The user32 work_area and app_window call, on a made-up desktop, and the list of what was asked.
    monitors: handle -> work area. windows: handle -> (rect, monitor, minimised). cursor: the cursor's
    monitor. top: (handle, class, pid) of the top-level windows, front to back."""
    monitors, windows, calls = monitors or {}, windows or {}, []

    def SetThreadDpiAwarenessContext(context):
        calls.append(("context", context.value))
        return previous

    def GetDpiForSystem():
        return 120

    def IsWindow(hwnd):
        return hwnd in windows

    def IsIconic(hwnd):
        return windows[hwnd][2]

    def GetWindowRect(hwnd, box):
        box._obj.left, box._obj.top, box._obj.right, box._obj.bottom = windows[hwnd][0]
        return 1

    def MonitorFromWindow(hwnd, flags):
        calls.append(("window", hwnd))
        return windows[hwnd][1]

    def GetCursorPos(point):
        calls.append(("cursor",))
        return 1

    def MonitorFromPoint(point, flags):
        return cursor

    def GetMonitorInfoW(monitor, info):
        work = info._obj.rcWork
        work.left, work.top, work.right, work.bottom = monitors[monitor]
        return 1

    def FindWindowExW(parent, after, kind, name):
        calls.append(("find", parent, kind, name))
        handles = [handle for handle, named, pid in top if named == kind]
        index = handles.index(after) + 1 if after else 0
        return handles[index] if index < len(handles) else None

    def GetWindowThreadProcessId(hwnd, owner):
        owner._obj.value = {handle: pid for handle, named, pid in top}[hwnd]
        return 1

    functions = (SetThreadDpiAwarenessContext, GetDpiForSystem, IsWindow, IsIconic, GetWindowRect, MonitorFromWindow,
                 GetCursorPos, MonitorFromPoint, GetMonitorInfoW, FindWindowExW, GetWindowThreadProcessId)
    return types.SimpleNamespace(**{function.__name__: function for function in functions}), calls


SYSTEM_AWARE = -2  # DPI_AWARENESS_CONTEXT_SYSTEM_AWARE: the px of pywebview's windows
MONITORENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.c_void_p, wintypes.HDC, ctypes.c_void_p, wintypes.LPARAM)


def live_user32():
    user32 = ctypes.WinDLL("user32")
    user32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    user32.GetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    user32.AreDpiAwarenessContextsEqual.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    user32.GetDpiForSystem.restype = wintypes.UINT
    user32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
    user32.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]
    user32.MonitorFromPoint.restype = ctypes.c_void_p
    user32.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
    user32.MonitorFromWindow.restype = ctypes.c_void_p
    user32.GetMonitorInfoW.argtypes = [ctypes.c_void_p, ctypes.POINTER(signin.MONITORINFO)]
    user32.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.c_void_p, MONITORENUMPROC, wintypes.LPARAM]
    user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int,
                                       ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                                       wintypes.HINSTANCE, wintypes.LPVOID]
    user32.CreateWindowExW.restype = wintypes.HWND
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.IsWindowVisible.argtypes = user32.DestroyWindow.argtypes = [wintypes.HWND]
    return user32


@contextlib.contextmanager
def thread_context(user32, context):
    previous = user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(context))
    try:
        yield
    finally:
        user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(previous))


def edges(box):
    return box.left, box.top, box.right, box.bottom


def monitor_work(user32, monitor):
    info = signin.MONITORINFO()
    info.cbSize = ctypes.sizeof(info)
    assert user32.GetMonitorInfoW(monitor, ctypes.byref(info))
    return edges(info.rcWork)


def cursor_work(user32):
    """The work area of the cursor's monitor and the system DPI, read in system-aware px apart from work_area."""
    with thread_context(user32, SYSTEM_AWARE):
        point = wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(point))
        return monitor_work(user32, user32.MonitorFromPoint(point, 2)), user32.GetDpiForSystem()


def work_areas(user32):
    """Every monitor's work area, in system-aware px."""
    found = []
    callback = MONITORENUMPROC(lambda monitor, dc, rect, data: found.append(monitor_work(user32, monitor)) or True)
    with thread_context(user32, SYSTEM_AWARE):
        user32.EnumDisplayMonitors(None, None, callback, 0)
    return found


@contextlib.contextmanager
def hidden_window(user32, kind, left, top, width, height):
    """A top-level window of class kind that is never shown or activated, made in system-aware px; its
    handle and window rect."""
    with thread_context(user32, SYSTEM_AWARE):
        # WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_OVERLAPPEDWINDOW without WS_VISIBLE
        hwnd = user32.CreateWindowExW(0x08000080, kind, "QuartzMIDI test", 0x00CF0000, left, top, width, height,
                                      None, None, None, None)
        assert hwnd and not user32.IsWindowVisible(hwnd)
        box = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(box))
    try:
        yield hwnd, edges(box)
    finally:
        user32.DestroyWindow(hwnd)


class Fit(unittest.TestCase):
    """Work areas are (left, top, right, bottom) in the px of pywebview's system-aware windows, which are
    physical px on a monitor at the system DPI. The taskbar takes 48 logical px at the bottom."""

    def inside(self, rect, work):
        self.assertTrue(work[0] <= rect[0] < rect[2] <= work[2] and work[1] <= rect[1] < rect[3] <= work[3],
                        f"{rect} is not inside {work}")

    def test_2560x1440_at_125_keeps_both_sizes(self):
        work = (0, 0, 2560, 1380)
        logical, rect = fitted(work, 120, YOUTUBE, YOUTUBE_LEAST)
        self.assertEqual(logical, (384, 123, 1280, 860, (640, 480)))
        self.assertEqual(rect, (480, 153, 2080, 1228))  # 1600x1075, centred
        logical, inner = fitted(work, 120, SIGN_IN, over=rect)
        self.assertEqual(logical, (764, 192, 520, 720, (200, 100)))
        self.assertEqual(inner, (955, 240, 1605, 1140))  # 650x900, centred on the YouTube window

    def test_1920x1080_at_100_keeps_both_sizes(self):
        work = (0, 0, 1920, 1032)
        logical, rect = fitted(work, 96, YOUTUBE, YOUTUBE_LEAST)
        self.assertEqual(logical, (320, 86, 1280, 860, (640, 480)))
        self.assertEqual(fitted(work, 96, SIGN_IN, over=rect)[0], (700, 156, 520, 720, (200, 100)))

    def test_1366x768_at_100_shrinks_only_the_height_that_does_not_fit(self):
        work = (0, 0, 1366, 720)
        logical, rect = fitted(work, 96, YOUTUBE, YOUTUBE_LEAST)
        self.assertEqual(logical, (43, 8, 1280, 720 - 2 * MARGIN, (640, 480)))
        self.assertEqual(rect, (43, 8, 1323, 712))
        # 720 high fits a 720 work area exactly: kept, with no room for the margin.
        logical, inner = fitted(work, 96, SIGN_IN, over=rect)
        self.assertEqual(logical, (423, 0, 520, 720, (200, 100)))
        self.assertEqual(inner, (423, 0, 943, 720))

    def test_1366x768_at_125_shrinks_both_axes_and_stays_wide(self):
        work = (0, 0, 1366, 708)
        logical, rect = fitted(work, 120, YOUTUBE, YOUTUBE_LEAST)
        self.assertEqual(logical, (8, 8, 1077, 551, (640, 480)))
        self.assertEqual(rect, (10, 10, 1356, 698))  # 10 physical px (8 logical) clear of each edge
        self.assertGreaterEqual(logical[2] - 16, 900)  # the page still gets the wide layout (§2)
        logical, inner = fitted(work, 120, SIGN_IN, over=rect)
        self.assertEqual(logical, (287, 8, 520, 551, (200, 100)))
        self.assertEqual(inner, (358, 10, 1008, 698))

    def test_1024x768_at_125_fits_the_fallback_display(self):
        work = (0, 0, 1024, 708)
        logical, rect = fitted(work, 120, YOUTUBE, YOUTUBE_LEAST)
        # 804 would be 1005 physical: 803 (1003) is the widest that keeps 10 px each side.
        self.assertEqual(logical, (9, 8, 803, 551, (640, 480)))
        self.assertEqual(rect, (11, 10, 1014, 698))
        self.assertEqual(fitted(work, 120, SIGN_IN, over=rect),
                         ((150, 8, 520, 551, (200, 100)), (187, 10, 837, 698)))
        # signin.py on its own centres on the work area, which here is the same place.
        self.assertEqual(fitted(work, 120, SIGN_IN)[1], (187, 10, 837, 698))

    def test_1920x1080_at_150_keeps_a_width_that_fits_exactly(self):
        logical, rect = fitted((0, 0, 1920, 1008), 144, YOUTUBE, YOUTUBE_LEAST)
        self.assertEqual(logical, (0, 8, 1280, 656, (640, 480)))
        self.assertEqual(rect, (0, 12, 1920, 996))

    def test_a_monitor_left_of_the_primary(self):
        work = (-1920, 0, 0, 1080)
        logical, rect = fitted(work, 96, YOUTUBE, YOUTUBE_LEAST)
        self.assertEqual(logical, (-1600, 110, 1280, 860, (640, 480)))
        self.assertEqual(fitted(work, 96, SIGN_IN, over=rect)[0], (-1220, 180, 520, 720, (200, 100)))
        # A 1366x768 monitor left of a 125% primary and 60 px lower: pywebview truncates -1356.25 to -1356.
        work = (-1366, 60, 0, 828)
        logical, rect = fitted(work, 120, YOUTUBE, YOUTUBE_LEAST)
        self.assertEqual(logical, (-1085, 56, 1077, 599, (640, 480)))
        self.assertEqual(rect, (-1356, 70, -10, 818))
        self.inside(fitted(work, 120, SIGN_IN, over=rect)[1], work)

    def test_the_sign_in_window_stays_inside_when_the_youtube_window_hangs_off(self):
        logical, rect = fitted((0, 0, 2560, 1380), 120, SIGN_IN, over=(1500, 300, 3100, 1375))
        self.assertEqual(logical, (1520, 310, 520, 720, (200, 100)))
        self.assertEqual(rect, (1900, 387, 2550, 1287))  # the margin kept on the right

    def test_the_minimum_shrinks_with_the_window(self):
        self.assertEqual(fitted((0, 0, 640, 440), 96, YOUTUBE, YOUTUBE_LEAST)[0], (8, 8, 624, 424, (624, 424)))
        # A work area too small for a margin gets none.
        self.assertEqual(fitted((0, 0, 30, 30), 96, SIGN_IN)[0], (0, 0, 30, 30, (30, 30)))

    def test_any_work_area_holds_the_window_centred(self):
        for dpi in (96, 120, 144, 168, 192):
            edge = -(-MARGIN * dpi // 96)
            for left in range(-2600, 2600, 173):
                for width, height in ((640, 480), (1024, 708), (1366, 728), (1920, 1040), (2560, 1380)):
                    work = (left, left // 3, left + width, left // 3 + height)
                    for size, least in ((YOUTUBE, YOUTUBE_LEAST), (SIGN_IN, (200, 100))):
                        (x, y, w, h, minimum), rect = fitted(work, dpi, size, least)
                        self.inside(rect, work)
                        for axis, room, got in ((0, width, w), (1, height, h)):
                            if physical(size[axis], dpi) <= room:
                                # Kept; a logical px less only where it fits so tightly that no whole
                                # logical place lands it inside.
                                slack = room - physical(size[axis], dpi)
                                self.assertIn(got, (size[axis], size[axis] - 1) if slack < -(-dpi // 96) else
                                              (size[axis],), (work, dpi, size))
                            else:  # the largest size that keeps the margin
                                self.assertLessEqual(physical(got, dpi), room - 2 * edge)
                                self.assertGreater(physical(got + 1, dpi), room - 2 * edge)
                            self.assertEqual(minimum[axis], min(least[axis], got))
                            self.assertLessEqual(abs(rect[axis] + rect[axis + 2] - work[axis] - work[axis + 2]), 4,
                                                 (work, dpi, size, rect))

    def test_placement_hands_the_fit_to_create_window(self):
        with mock.patch.object(signin, "work_area", return_value=((0, 0, 1024, 708), 120, None)) as area:
            self.assertEqual(signin.placement(YOUTUBE, YOUTUBE_LEAST),
                             {"x": 9, "y": 8, "width": 803, "height": 551, "min_size": (640, 480)})
            area.assert_called_once_with(0, 0)
            signin.placement(YOUTUBE, YOUTUBE_LEAST, near=0xA99)
            area.assert_called_with(0, 0xA99)
        # Centred on over's rect, not on the work area, which would put it at x=764 y=192.
        with mock.patch.object(signin, "work_area", return_value=((0, 0, 2560, 1380), 120, (1500, 300, 3100, 1375))) \
                as area:
            self.assertEqual(signin.placement(SIGN_IN, over=0x5150, near=0xA99),
                             {"x": 1520, "y": 310, "width": 520, "height": 720, "min_size": (200, 100)})
        area.assert_called_once_with(0x5150, 0xA99)
        with mock.patch.object(signin, "work_area", return_value=None):
            self.assertEqual(signin.placement(SIGN_IN), {"width": 520, "height": 720, "min_size": (200, 100)})

    def test_work_area_takes_the_monitor_of_over_then_near_then_the_cursor(self):
        monitors = {1: (0, 0, 2560, 1380), 2: (2560, 0, 4480, 1040), 3: (-1366, 60, 0, 768)}
        youtube, app = (2700, 100, 3900, 900), (-1200, 100, -400, 700)
        windows = {0x5150: (youtube, 2, False), 0x5151: (youtube, 2, True),
                   0xA99: (app, 3, False), 0xA9A: ((-32000, -32000, -31840, -31972), 3, True)}
        cases = (
            ((0x5150, 0xA99), (monitors[2], 120, youtube), [("window", 0x5150)]),  # over, centred on it
            ((0x5151, 0xA99), (monitors[3], 120, None), [("window", 0xA99)]),     # over minimised: near's monitor
            ((0, 0xA99), (monitors[3], 120, None), [("window", 0xA99)]),          # QuartzMIDI's monitor
            ((0, 0xA9A), (monitors[3], 120, None), [("window", 0xA9A)]),          # where it was before it was minimised
            ((0x5151, 0), (monitors[1], 120, None), [("cursor",)]),
            ((0xDEAD, 0xBEEF), (monitors[1], 120, None), [("cursor",)]),          # closed windows
            ((0, 0), (monitors[1], 120, None), [("cursor",)]),
        )
        system_aware = ctypes.c_void_p(SYSTEM_AWARE).value
        for (over, near), found, asked in cases:
            user32, calls = fake_user32(monitors, windows, cursor=1)
            with mock.patch.object(ctypes, "WinDLL", return_value=user32):
                self.assertEqual(signin.work_area(over, near), found, (over, near))
            # Read in system-aware px, then the thread's own context is put back.
            self.assertEqual(calls, [("context", system_aware)] + asked + [("context", 0x1234)], (over, near))
        # Before Windows 10 1607 there is no thread context to read in.
        user32, calls = fake_user32(monitors, windows, cursor=1, previous=0)
        with mock.patch.object(ctypes, "WinDLL", return_value=user32):
            self.assertIsNone(signin.work_area(0x5150))
        with mock.patch.object(ctypes, "WinDLL", return_value=types.SimpleNamespace()):
            self.assertIsNone(signin.work_area(0x5150))

    def test_work_area_reads_system_aware_px_whatever_the_thread(self):
        """Above 100%, a read in any other px gives other numbers: 819x566 at 96 DPI on 1024x768 at 125%."""
        user32 = live_user32()
        for context in (-1, -4):  # unaware, per-monitor v2
            with thread_context(user32, context):
                before = cursor_work(user32)
                work, dpi, rect = signin.work_area()
                after = cursor_work(user32)  # in case the cursor went to another monitor meanwhile
                self.assertTrue(user32.AreDpiAwarenessContextsEqual(user32.GetThreadDpiAwarenessContext(),
                                                                    ctypes.c_void_p(context)))
            self.assertIn((work, dpi), (before, after))
            self.assertIsNone(rect)

    def test_work_area_and_placement_on_a_real_window(self):
        """A window that is never shown or activated, on a monitor other than the cursor's where there is one."""
        user32 = live_user32()
        cursor, dpi = cursor_work(user32)
        work = next((area for area in work_areas(user32) if area != cursor), cursor)
        with hidden_window(user32, "STATIC", work[0] + 40, work[1] + 40, 400, 500) as (hwnd, rect):
            with thread_context(user32, SYSTEM_AWARE):
                self.assertEqual(monitor_work(user32, user32.MonitorFromWindow(hwnd, 2)), work)
            self.assertEqual(signin.work_area(hwnd), (work, dpi, rect))
            self.assertEqual(signin.work_area(0, hwnd), (work, dpi, None))
            placed = signin.placement(SIGN_IN, over=hwnd)
            self.assertEqual(placed, as_placement(fit_window(work, dpi, SIGN_IN, (200, 100), rect)))
            # Centred on the window, which is not where centring on the work area puts it.
            self.assertNotEqual(placed, as_placement(fit_window(work, dpi, SIGN_IN)))
            self.assertEqual(signin.placement(SIGN_IN, near=hwnd), as_placement(fit_window(work, dpi, SIGN_IN)))

    def test_app_window_is_quartzmidis_own_window_in_that_process(self):
        top = ((0x10, "ImGui Platform", 4242), (0x20, "QuartzMIDI", 999), (0x30, "QuartzMIDI", 4242),
               (0x40, "QuartzMIDI", 4242))
        user32, calls = fake_user32(top=top)
        with mock.patch.object(ctypes, "WinDLL", return_value=user32):
            self.assertEqual(signin.app_window(4242), 0x30)  # the frontmost
            self.assertEqual(signin.app_window(31337), 0)
            self.assertEqual(signin.app_window(None), 0)
            self.assertEqual(signin.app_window(0), 0)
        self.assertEqual(set(calls), {("find", None, "QuartzMIDI", None)})
        # A real window, found by its class and this process.
        user32 = live_user32()
        with hidden_window(user32, "STATIC", 0, 0, 400, 500) as (hwnd, rect), \
                mock.patch.object(signin, "APP_CLASS", "STATIC"):
            self.assertEqual(signin.app_window(os.getpid()), hwnd)
            self.assertEqual(signin.app_window(3), 0)  # no process has pid 3
        with mock.patch.object(signin, "APP_CLASS", "STATIC"):
            self.assertEqual(signin.app_window(os.getpid()), 0)

    def test_app_class_is_the_one_quartzmidi_registers(self):
        shell = os.path.join(HERE, "..", "..", "ui", "Shell.cpp")
        if not os.path.isfile(shell):
            self.skipTest("the add-on without QuartzMIDI's source")
        with open(shell, encoding="utf-8") as file:
            text = file.read()
        self.assertEqual(re.search(r'WNDCLASSEXW wc = \{[^}]*L"([^"]+)"', text).group(1), signin.APP_CLASS)
        self.assertIn("CreateWindowW(wc.lpszClassName", text)

    def test_quartzmidis_window_is_of_the_pid_from_init_else_of_the_parent(self):
        host = Host(2)
        with mock.patch.object(browse, "app_window", return_value=0xA99) as find, \
                mock.patch.object(browse.os, "getppid", return_value=4242):
            self.assertEqual(host.app(), 0xA99)
            host.receive({"type": "init", "protocol": 2, "pid": 97776})
            self.assertEqual(host.app(), 0xA99)
        self.assertEqual(find.call_args_list, [mock.call(4242), mock.call(97776)])

    def test_main_fits_the_youtube_window_on_quartzmidis_monitor(self):
        made, placed = [], {"x": 9, "y": 8, "width": 803, "height": 551, "min_size": (640, 480)}
        stub = types.ModuleType("webview")
        stub.create_window = lambda *a, **k: made.append(k) or mock.MagicMock()
        stub.start = lambda *a, **k: None
        with tempfile.TemporaryDirectory() as folder, mock.patch.dict(sys.modules, {"webview": stub}), \
                mock.patch.dict(os.environ), mock.patch.object(browse, "PROFILE", folder), \
                mock.patch.object(browse, "COOKIES", os.path.join(folder, "none.txt")), \
                mock.patch.object(browse, "placement", return_value=placed) as placement, \
                mock.patch.object(browse, "app_window", return_value=0xA99) as find, \
                mock.patch.object(browse.os, "getppid", return_value=4242), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(browse.main([]), 0)
        find.assert_called_once_with(4242)  # the first protocol's init has no pid
        placement.assert_called_once_with(YOUTUBE, YOUTUBE_LEAST, near=0xA99)
        self.assertEqual({key: made[0][key] for key in placed}, placed)

    def test_the_sign_in_window_is_fitted_on_the_youtube_window(self):
        host, made = Host(2), []
        host.hwnd, host.pid = 0x5150, 97776
        host.webview = mock.MagicMock()
        host.webview.create_window.side_effect = lambda *a, **k: made.append(k) or mock.MagicMock()
        placed = {"x": 150, "y": 8, "width": 520, "height": 551, "min_size": (200, 100)}
        with mock.patch.object(browse, "placement", return_value=placed) as placement, \
                mock.patch.object(browse, "app_window", return_value=0xA99) as find, \
                mock.patch.dict(sys.modules, {"System": mock.MagicMock()}):
            self.assertTrue(host.sign_in())
        find.assert_called_once_with(97776)
        placement.assert_called_once_with(SIGN_IN, over=0x5150, near=0xA99)
        self.assertEqual({key: made[0][key] for key in placed}, placed)

    def test_signin_main_opens_on_quartzmidis_monitor(self):
        made, placed = [], {"x": 150, "y": 8, "width": 520, "height": 551, "min_size": (200, 100)}
        stub = types.ModuleType("webview")
        stub.create_window = lambda *a, **k: made.append(k) or mock.MagicMock()
        stub.start = lambda *a, **k: None
        with tempfile.TemporaryDirectory() as folder, mock.patch.dict(sys.modules, {"webview": stub}), \
                mock.patch.object(signin, "PROFILE", folder), \
                mock.patch.object(signin, "placement", return_value=placed) as placement, \
                mock.patch.object(signin, "app_window", return_value=0xA99) as find, \
                mock.patch.object(signin.os, "getppid", return_value=4242), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(signin.main(), 1)  # closed with no sign-in
        find.assert_called_once_with(4242)  # QuartzMIDI started it
        placement.assert_called_once_with(SIGN_IN, near=0xA99)
        self.assertEqual({key: made[0][key] for key in placed}, placed)


# A pywebview whose window is closed as soon as it starts.
STUB_WEBVIEW = '''
class _Hook:
    def __iadd__(self, handler):
        return self


class _Events:
    def __init__(self):
        self.before_show = _Hook()
        self.closed = _Hook()


class _Window:
    def __init__(self):
        self.events = _Events()


def create_window(*a, **k):
    return _Window()


def start(*a, **k):
    return None
'''


class Exit(unittest.TestCase):
    def test_a_closed_window_exits_cleanly_with_stdin_open(self):
        # QuartzMIDI keeps stdin open after the window closes, so the reader thread
        # is still in a read when the interpreter shuts down.
        with tempfile.TemporaryDirectory() as stub, tempfile.TemporaryDirectory() as profile:
            with open(os.path.join(stub, "webview.py"), "w", encoding="utf-8") as file:
                file.write(STUB_WEBVIEW)
            code = "\n".join([
                "import sys", f"sys.path[:0] = [{stub!r}, {HERE!r}]",
                # browse imports PROFILE from signin: the owner's browser folder is never touched.
                "import browse", f"browse.PROFILE = {profile!r}",
                "sys.exit(browse.main(['--hello', '2']))"])
            process = subprocess.Popen([sys.executable, "-c", code], stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            watchdog = threading.Timer(60, process.kill)
            watchdog.start()
            try:
                output = process.stdout.read().decode("utf-8", errors="replace")
                process.wait(timeout=30)
            finally:
                watchdog.cancel()
                if process.poll() is None:
                    process.kill()
                process.stdin.close()
                process.stdout.close()
        self.assertEqual(process.returncode, 0, output)
        self.assertIn("done: closed", output)
        self.assertNotIn("Fatal Python error", output)


class Imports(unittest.TestCase):
    def test_webview_is_imported_only_inside_main(self):
        with open(os.path.join(HERE, "browse.py"), encoding="utf-8") as file:
            tree = ast.parse(file.read())
        owners = []

        def visit(node, owner):
            for child in ast.iter_child_nodes(node):
                if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                    visit(child, owner + [child.name])
                    continue
                if isinstance(child, ast.Import) and any(alias.name.split(".")[0] == "webview" for alias in child.names):
                    owners.append(".".join(owner))
                if isinstance(child, ast.ImportFrom) and (child.module or "").split(".")[0] == "webview":
                    owners.append(".".join(owner))
                visit(child, owner)

        visit(tree, [])
        self.assertEqual(owners, ["main"])
        self.assertNotIn("webview", sys.modules.get("browse").__dict__)


if __name__ == "__main__":
    unittest.main()
