"""convert.py: folder names, dests, ids, the ledger, short reasons, and whole runs
with yt-dlp and transcribe faked. Nothing is downloaded or transcribed."""

import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import types
import unicodedata
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime
from unittest import mock
from urllib.parse import parse_qs

try:
    import _winapi  # CreateJunction, for a real junction
except ImportError:
    _winapi = None

TESTS = os.path.dirname(os.path.abspath(__file__))
HERE = os.path.dirname(TESTS)
# The embedded Python's ._pth file leaves the converter's folder off the path.
sys.path.insert(0, HERE)

import convert
from convert import DownloadFailure, FolderFailure, FolderName, TranscribeFailure, short_reason, video_id

V1, V2, V3, V4, V5 = "Aa1_Bb2-Cc3", "Dd4_Ee5-Ff6", "Gg7_Hh8-Ii9", "Jj0_Kk1-Ll2", "Mm3_Nn4-Oo5"
UC1, UC2 = "UCuAXFkgsw1L7xaCfnd5JJOw", "UC_x5XG1OV2P6uZZ5FSM9Ttw"
WATCH = "https://www.youtube.com/watch?v="
BOT = "[youtube] {}: Sign in to confirm you're not a bot. Use --cookies-from-browser or --cookies for the authentication."
PRIVATE = "[youtube] {}: Private video. Sign in if you've been granted access to this video"
REMOVED = "[youtube] {}: Video unavailable. This video has been removed by the uploader"


class Helpers(unittest.TestCase):
    def test_folder_name(self):
        # Shared with the page's folderName (parse_test.js).
        with open(os.path.join(TESTS, "fixtures", "foldernames.json"), encoding="utf-8") as file:
            cases = json.load(file)
        self.assertGreater(len(cases), 40)
        for case in cases:
            with self.subTest(name=ascii(case["name"])):
                folder = FolderName(case["name"])
                self.assertEqual(folder, case["folder"])
                self.assertEqual(FolderName(folder), folder)
                self.assertTrue(convert.valid_segment(folder))

    def test_valid_segment(self):
        valid = ["Chopin", "Animenz Piano Sheets", ".hidden", "Chopin  Nocturnes", "a" * 255, "ピアノ",
                 "\U0001f3b9" * 127, "CONSOLE", "COM10", "Op. 9", "x.y", "Kassia\u3000Topic", "COM0", "COM\u2074",
                 "CONIN", "CONOUT"]
        invalid = ["", ".", "..", " Chopin", "Chopin ", "Chopin.", "A/B", "A\\B", "A:B", 'A"B', "A<B", "A|B", "A?", "A*",
                   "a" * 256, "\U0001f3b9" * 128, "Tab\there", "A\x7f", "A\x85", "Chopin\u200b", "\ufeffA", "A\u202e",
                   "A\u00ad", "Lone\ud800", "CON", "con", "Nul.txt", "COM1", "LPT9.mid", "AUX .txt", "COM\u00b9",
                   "com\u00b2.mid", "LPT\u00b3", "CONIN$", "conout$.txt", "CON\u0131N$", None, 5]
        for name in valid:
            with self.subTest(name=ascii(name)):
                self.assertTrue(convert.valid_segment(name))
        for name in invalid:
            with self.subTest(name=ascii(name)):
                self.assertFalse(convert.valid_segment(name))
        self.assertEqual((convert.units("a\U0001f3b9"), convert.units("ピアノ")), (3, 3))

    def test_format_characters_agree_with_the_app(self):
        # The app's IsFormatCharacter table is this Python's Cf category.
        header = os.path.join(HERE, "..", "..", "engine", "AudioToMidi.hpp")
        if not os.path.exists(header):
            self.skipTest("the app's source is not here")
        with open(header, encoding="utf-8") as file:
            table = re.search(r"kRanges\[\]\{(.*?)\};", file.read(), re.S).group(1)
        ranges = [(int(first, 16), int(last, 16)) for first, last in re.findall(r"\{0x(\w+), 0x(\w+)\}", table)]
        self.assertEqual({code for first, last in ranges for code in range(first, last + 1)},
                         {code for code in range(0x110000) if unicodedata.category(chr(code)) == "Cf"})

    def test_parse_dest(self):
        cases = {
            "": [], "Anime": ["Anime"], "Anime/Animenz Piano Sheets": ["Anime", "Animenz Piano Sheets"],
            "A/B/C/D": ["A", "B", "C", "D"], "A/B/C/D/E": None, "A//B": None, "/A": None, "A/": None, "/": None,
            "A\\B": None, "..": None, "../A": None, "A/..": None, "A/./B": None, "C:/A": None, "CON/A": None,
            "Chopin\u200b": None, " A": None,
        }
        for dest, expected in cases.items():
            with self.subTest(dest=ascii(dest)):
                self.assertEqual(convert.parse_dest(dest), expected)
        for dest in (None, 5, ["A"]):
            self.assertIsNone(convert.parse_dest(dest))

    def test_short_reason(self):
        cases = [
            (DownloadFailure(PRIVATE.format(V1)), "Private video."),
            (DownloadFailure("Video unavailable. This video is a private video"), "Private video."),
            (DownloadFailure("Sign in to confirm your age. This video may be inappropriate for some users."),
             "Age-restricted video."),
            (DownloadFailure("This video is age-restricted"), "Age-restricted video."),
            (DownloadFailure("This live event will begin in 3 hours."), "Live stream."),
            (DownloadFailure("Premieres in 2 days"), "Live stream."),
            (DownloadFailure("This channel is live"), "Live stream."),
            (DownloadFailure("Video unavailable. The uploader has not made this video available in your country"),
             "Not available in your country."),
            (DownloadFailure(REMOVED.format(V1)), "Removed video."),
            (DownloadFailure("This video is unavailable"), "Removed video."),
            (DownloadFailure("The YouTube account associated with this video has been terminated."), "Removed video."),
            (DownloadFailure("This video has been deleted"), "Removed video."),
            (DownloadFailure("HTTP Error 404: Not Found"), "Download failed."),
            (TranscribeFailure("onnxruntime: private video of a removed model"), "Transcription failed."),
            (RuntimeError("The file does not exist: x.mp3"), "Conversion failed."),
            (FolderFailure("Folder not allowed."), "Folder not allowed."),
            (FolderFailure("Path too long."), "Path too long."),
        ]
        for failure, expected in cases:
            with self.subTest(failure=str(failure)):
                self.assertEqual(short_reason(failure), expected)

    def test_reason_for_unwraps_and_names_a_bot_block(self):
        try:
            try:
                raise RuntimeError("ERROR: " + BOT.format(V1))
            except RuntimeError as failure:
                raise DownloadFailure(str(failure)) from failure
        except DownloadFailure as wrapped:
            reason = convert.reason_for(wrapped)
        self.assertTrue(reason.startswith("YouTube refused this connection as a bot."))
        self.assertTrue(convert.is_blocked(reason))
        self.assertFalse(convert.is_blocked(convert.reason_for(DownloadFailure(PRIVATE.format(V1)))))
        failure = TranscribeFailure("wrapped")
        failure.__cause__ = ValueError("ERROR: the model")
        self.assertEqual(convert.reason_for(failure), "the model")

    def test_video_id(self):
        cases = {
            WATCH + V1: V1, "https://music.youtube.com/watch?v=" + V1 + "&list=RDAMVM" + V1: V1,
            "HTTPS://YOUTU.BE/" + V1: V1, "https://youtu.be/" + V1 + "/": V1,
            "https://m.youtube.com/shorts/" + V1 + "?feature=share": V1, "https://youtube.com/live/" + V1: V1,
            "https://www.youtube.com/watch?v=short": None, "https://www.youtube.com/watch?v=" + V1 + "x": None,
            "https://www.youtube.com/watch?list=PL123": None, "https://www.youtube.com/shorts/": None,
            "https://www.youtube.com/live": None, "https://www.youtube.com/embed/" + V1: None,
            "https://youtu.be/": None, "https://example.com/watch?v=" + V1: None, "youtu.be/" + V1: None,
            "https://[bad": None, "": None, None: None,
        }
        for link, expected in cases.items():
            with self.subTest(link=link):
                self.assertEqual(video_id(link), expected)

    def test_video_id_agrees_with_the_shared_routes(self):
        with open(os.path.join(TESTS, "fixtures", "routes.json"), encoding="utf-8") as file:
            routes = json.load(file)
        for case in routes:
            with self.subTest(url=case["url"]):
                route = parse_qs((case["route"] or "?")[1:])
                self.assertEqual(video_id(case["url"]), (route.get("v") or [None])[0])

    def test_one_line(self):
        self.assertEqual(convert.one_line("a\tb\nc\r\nd\u2028e\x85f"), "a b c  d e f")
        self.assertEqual(convert.one_line(7), "7")

    def test_piano_entries(self):
        entries = [convert.Entry(WATCH + V1, "Nocturne - Piano", V1, None),
                   convert.Entry(WATCH + V2, "Canon - Violin & Piano", V2, None),
                   convert.Entry(WATCH + V3, "Canon - Violin & Piano Solo", V3, None),
                   convert.Entry(WATCH + V4, "Duet (Your Lie in April)", V4, None)]
        self.assertEqual([entry.id for entry in convert.piano_entries(entries)], [V1, V3, V4])

    def test_channel_links(self):
        self.assertTrue(convert.is_channel("https://www.youtube.com/@ChopinFan/videos"))
        self.assertFalse(convert.is_channel(WATCH + V1))


class Destinations(unittest.TestCase):
    """resolve_dest against real folders; nothing it resolves is made."""

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.out = os.path.join(self.root, "MIDI")
        os.mkdir(self.out)
        self.outside = os.path.join(self.root, "Elsewhere")
        os.mkdir(self.outside)

    def folder(self, *parts):
        path = os.path.join(self.out, *parts)
        os.makedirs(path)
        return path

    def junction(self, name, target):
        path = os.path.join(self.out, name)
        _winapi.CreateJunction(target, path)
        self.addCleanup(os.rmdir, path)  # the junction only, before rmtree
        return path

    def refused(self, dest):
        with self.assertRaises(FolderFailure) as caught:
            convert.resolve_dest(self.out, dest)
        self.assertEqual(str(caught.exception), "Folder not allowed.")

    def test_the_root(self):
        self.assertEqual(convert.resolve_dest(self.out, ""), self.out)
        self.assertEqual(convert.resolve_dest(self.out, []), self.out)
        self.assertEqual(convert.relative_dest(self.out, self.out), "")

    def test_existing_folders_are_used_without_case_or_spacing(self):
        chopin = self.folder("Classical", "Chopin  Nocturnes")
        self.assertEqual(convert.resolve_dest(self.out, "classical/CHOPIN NOCTURNES"), chopin)
        self.assertEqual(convert.resolve_dest(self.out, ["CLASSICAL", "chopin\u3000 nocturnes"]), chopin)
        self.assertEqual(convert.relative_dest(chopin, self.out), "Classical/Chopin  Nocturnes")

    def test_an_exact_name_comes_first(self):
        spaced, single = self.folder("A  B"), self.folder("A B")
        self.assertEqual(convert.resolve_dest(self.out, "A  B"), spaced)
        self.assertEqual(convert.resolve_dest(self.out, "A B"), single)

    def test_new_names_go_through_folder_name(self):
        self.folder("Classical")
        self.assertEqual(convert.resolve_dest(self.out, "classical/.Chopin  Ballades/Op. 23"),
                         os.path.join(self.out, "Classical", "Chopin Ballades", "Op. 23"))
        self.assertEqual(convert.resolve_dest(self.out, "Anime\u3000Covers"), os.path.join(self.out, "Anime Covers"))
        self.assertEqual(os.listdir(self.out), ["Classical"])
        self.assertEqual(os.listdir(os.path.join(self.out, "Classical")), [])

    def test_existing_names_are_not_truncated(self):
        name = " ".join(["Ludovico Einaudi Nuvole Bianche"] * 4)  # 127 characters
        long = self.folder(name)
        self.assertEqual(convert.resolve_dest(self.out, name.upper()), long)
        self.assertEqual(convert.resolve_dest(self.out, name.upper() + "/" + name),
                         os.path.join(long, FolderName(name)))
        self.assertEqual(len(FolderName(name)), 80)

    def test_a_new_name_reaches_a_folder_its_folder_name_matches(self):
        chopin = self.folder("Chopin")
        self.assertEqual(convert.resolve_dest(self.out, ".chopin"), chopin)

    def test_refused(self):
        for dest in ("..", "../Elsewhere", "Classical/..", ".", "A//B", "/A", "A/", "A\\B", "C:", "CON", "lpt1.mid",
                     "Chopin\u200b", "\u202eevil", "Tab\there", " Chopin", "Chopin.", "A/B/C/D/E", "a" * 256,
                     ["A", ".."], ["A/B"], None, 5):
            with self.subTest(dest=ascii(dest)):
                self.refused(dest)
        self.assertEqual((os.listdir(self.out), os.listdir(self.outside)), ([], []))

    def test_a_file_is_refused(self):
        with open(os.path.join(self.out, "Chopin"), "wb"):
            pass
        self.refused("chopin")
        self.refused("Chopin/Nocturnes")

    @unittest.skipUnless(_winapi, "junctions are Windows only")
    def test_a_junction_is_refused(self):
        self.junction("Link", self.outside)
        self.junction("Inner", self.folder("Real"))
        for dest in ("Link", "link/Chopin", "INNER", ".Inner", "Inner/Chopin"):
            with self.subTest(dest=dest):
                self.refused(dest)
        self.assertEqual(convert.resolve_dest(self.out, "real"), os.path.join(self.out, "Real"))
        self.assertEqual(os.listdir(self.outside), [])

    def test_a_symlink_is_refused(self):
        link = os.path.join(self.out, "Link")
        try:
            os.symlink(self.outside, link, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("this account cannot make symlinks")
        self.addCleanup(os.remove if os.name != "nt" else os.rmdir, link)
        self.refused("link")

    def test_only_a_redirecting_reparse_point_is_refused(self):
        # Neither a symlink nor a junction by is_symlink() and is_junction(): only the tag tells.
        entry = mock.Mock(**{"is_symlink.return_value": False, "is_junction.return_value": False})

        def tagged(tag, attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT | stat.FILE_ATTRIBUTE_DIRECTORY):
            entry.stat.return_value = types.SimpleNamespace(st_file_attributes=attributes, st_reparse_tag=tag)
            return convert.redirects(entry)

        # A mount point, a symlink, and a name surrogate of another kind (a container's link).
        for tag in (0xA0000003, 0xA000000C, 0xA0000027):
            with self.subTest(tag=hex(tag)):
                self.assertTrue(tagged(tag))
        # OneDrive's cloud placeholders, a deduplicated folder and an app link stay where they are.
        for tag in (0x9000001A, 0x9000601A, 0x80000013, 0x8000001B):
            with self.subTest(tag=hex(tag)):
                self.assertFalse(tagged(tag))
        self.assertFalse(tagged(0xA0000003, stat.FILE_ATTRIBUTE_DIRECTORY))
        entry.stat.return_value = types.SimpleNamespace(st_file_attributes=stat.FILE_ATTRIBUTE_DIRECTORY)
        self.assertFalse(convert.redirects(entry))
        entry.is_junction.return_value = True
        self.assertTrue(convert.redirects(entry))

    def test_a_cloud_folder_is_used(self):
        cloud = self.folder("OneDrive")
        attributes = types.SimpleNamespace(st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT | stat.FILE_ATTRIBUTE_DIRECTORY,
                                           st_reparse_tag=0x9000001A)
        placeholder = mock.Mock(**{"is_symlink.return_value": False, "is_junction.return_value": False,
                                   "is_dir.return_value": True, "stat.return_value": attributes})
        placeholder.name = "OneDrive"
        with mock.patch.object(convert, "child_named", side_effect=[placeholder, None]):
            self.assertEqual(convert.resolve_dest(self.out, "onedrive/Chopin"), os.path.join(cloud, "Chopin"))

    @unittest.skipUnless(_winapi, "junctions are Windows only")
    def test_containment_backs_up_the_reparse_check(self):
        self.junction("Link", self.outside)
        self.junction("Inner", self.folder("Real"))
        with mock.patch.object(convert, "redirects", return_value=False):
            self.refused("Link")
            self.refused("Link/Chopin")
            self.assertEqual(convert.resolve_dest(self.out, "Inner"), os.path.join(self.out, "Inner"))

    def test_inside(self):
        out = os.path.realpath(self.out)
        self.assertTrue(convert.inside(out, out))
        self.assertTrue(convert.inside(os.path.join(out, "A", "B"), out))
        self.assertTrue(convert.inside(os.path.join(out, "A").upper(), out))
        self.assertFalse(convert.inside(out + "x", out))
        self.assertFalse(convert.inside(os.path.dirname(out), out))
        self.assertFalse(convert.inside(os.path.realpath(self.outside), out))
        if os.name == "nt":
            other = "Z:\\MIDI" if not out.upper().startswith("Z:") else "Y:\\MIDI"
            self.assertFalse(convert.inside(other, out))

    def test_destination(self):
        self.assertEqual(convert.destination(self.out), self.out)
        self.assertEqual(convert.destination(self.out, None, "Chopin: Best"), os.path.join(self.out, "Chopin Best"))
        self.assertEqual(convert.destination(self.out, "Anime", "Chopin"), os.path.join(self.out, "Anime"))
        self.assertEqual(convert.destination(self.out, "", "Chopin"), self.out)
        # Room for a one-letter file name, and no more.
        fits, too_long = deep_dest(self.out, 0), deep_dest(self.out, 1)
        if too_long is None:
            self.skipTest("the temporary folder is too deep")
        self.assertEqual(len(os.path.join(convert.destination(self.out, fits), "_.mid")), convert.LONGEST)
        with self.assertRaises(FolderFailure) as caught:
            convert.destination(self.out, too_long)
        self.assertEqual(str(caught.exception), "Path too long.")
        self.assertEqual(os.listdir(self.out), [])

    @unittest.skipUnless(_winapi, "junctions are Windows only")
    def test_a_name_is_resolved_as_a_dest_is(self):
        # A --subfolder, a list's folder or a playlist's title.
        self.junction("Link", self.outside)
        for name in ("Link", "link", ".Link", "Link?"):
            with self.subTest(name=name):
                with self.assertRaises(FolderFailure) as caught:
                    convert.destination(self.out, name=name)
                self.assertEqual(str(caught.exception), "Folder not allowed.")
        chopin = self.folder("Chopin  Nocturnes")
        self.assertEqual(convert.destination(self.out, name="chopin nocturnes"), chopin)
        # A MIDI folder not made yet holds nothing.
        missing = os.path.join(self.root, "Missing")
        self.assertEqual(convert.destination(missing, name="Link"), os.path.join(missing, "Link"))
        self.assertEqual(os.listdir(self.outside), [])


def deep_dest(out, over):
    """A dest of new folders in out that leaves room for a file path of exactly
    LONGEST characters with a one-letter name, plus over; None when out is too deep."""
    left = convert.LONGEST - len(os.sep + "_.mid") + over - len(out)  # each name costs a separator too
    count = -(-left // 81)
    if count > convert.DEST_DEPTH or left < 2 * count:
        return None
    size, extra = divmod(left - count, count)
    return "/".join("d" * (size + (number < extra)) for number in range(count))


class Ledger(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.ledger = os.path.join(self.root, "converted.jsonl")
        self.enterContext(mock.patch.object(convert, "LEDGER", self.ledger))

    def midi(self, name):
        path = os.path.join(self.root, name)
        with open(path, "wb") as file:
            file.write(b"MThd")
        return path

    def lines(self):
        with open(self.ledger, encoding="utf-8", errors="replace") as file:
            return file.read().splitlines()

    def test_missing_ledger(self):
        self.assertEqual(convert.ledger_records(), {})

    def test_write(self):
        path = self.midi("Nocturne.mid")
        record = convert.remember(V1, path, "Nocturne ピアノ")
        self.assertEqual(json.loads(self.lines()[0]), record)
        self.assertEqual({key: record[key] for key in ("id", "path", "title")},
                         {"id": V1, "path": path, "title": "Nocturne ピアノ"})
        self.assertIsNotNone(datetime.fromisoformat(record["at"]).tzinfo)
        self.assertEqual(convert.converted(convert.ledger_records(), V1), path)
        self.assertIsNone(convert.converted(convert.ledger_records(), V2))
        self.assertIsNone(convert.converted(convert.ledger_records(), None))
        self.assertEqual(sorted(record), ["at", "id", "path", "title"])

    def test_channel_dest_and_artist(self):
        path = self.midi("Nocturne.mid")
        record = convert.remember(V1, path, "Nocturne", "Chopin Fan", UC1, "Classical/Chopin", UC1)
        self.assertEqual({key: value for key, value in record.items() if key != "at"},
                         {"id": V1, "path": path, "title": "Nocturne", "channel": "Chopin Fan", "channelId": UC1,
                          "dest": "Classical/Chopin", "artist": True})
        self.assertEqual(json.loads(self.lines()[0]), record)
        # The MIDI folder is a dest too; another channel's artist is not this one's.
        record = convert.remember(V2, path, "Etude", "Someone", UC2, "", UC1)
        self.assertEqual((record["dest"], "artist" in record), ("", False))
        record = convert.remember(V3, path, "Waltz", None, None, None, UC1)
        self.assertEqual(sorted(record), ["at", "id", "path", "title"])
        self.assertEqual(sorted(convert.ledger_records()), sorted([V1, V2, V3]))

    def test_last_record_wins(self):
        first, second = self.midi("a.mid"), self.midi("b.mid")
        convert.remember(V1, first, "A")
        convert.remember(V2, first, "A")
        convert.remember(V1, second, "B")
        records = convert.ledger_records()
        self.assertEqual(records[V1]["path"], second)
        self.assertEqual(records[V2]["path"], first)

    def test_missing_path_is_not_converted(self):
        path = self.midi("gone.mid")
        convert.remember(V1, path, "Gone")
        os.remove(path)
        self.assertIsNone(convert.converted(convert.ledger_records(), V1))

    def test_bad_and_partial_lines_are_passed_over(self):
        path = self.midi("a.mid")
        with open(self.ledger, "wb") as file:
            file.write(b"not json\n[1, 2]\n{\"id\": 5, \"path\": \"x\"}\n\xff\xfe\n")
            file.write(json.dumps({"id": V2, "path": path, "title": "B", "at": ""}).encode() + b"\n")
            file.write(b"{\"id\": \"" + V1.encode() + b"\", \"pa")
        self.assertEqual(list(convert.ledger_records()), [V2])
        convert.remember(V1, path, "A")
        self.assertEqual(sorted(convert.ledger_records()), sorted([V1, V2]))
        self.assertEqual(len(self.lines()), 7)

    def test_a_last_line_without_a_newline_is_kept(self):
        path = self.midi("a.mid")
        with open(self.ledger, "w", encoding="utf-8") as file:
            file.write(json.dumps({"id": V2, "path": path, "title": "B", "at": ""}))
        convert.remember(V1, path, "A")
        self.assertEqual(sorted(convert.ledger_records()), sorted([V1, V2]))

    def test_a_write_that_fails_is_logged(self):
        convert.LEDGER = self.root  # a folder, which open() refuses
        out = io.StringIO()
        with redirect_stdout(out):
            record = convert.remember(V1, "x.mid", "A")
        self.assertEqual(record["id"], V1)
        self.assertTrue(out.getvalue().startswith("The list of converted videos could not be written: "))


class FakeDownloadError(Exception):
    pass


class Runs(unittest.TestCase):
    """main() as the app runs it, with yt-dlp and transcribe faked."""

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.addCleanup(os.chdir, os.getcwd())
        self.out = os.path.join(self.root, "out")
        os.mkdir(self.out)
        self.ledger = os.path.join(self.root, "converted.jsonl")
        self.titles = {}  # link -> the title its download gets
        self.channels = {}  # link -> yt-dlp's channel, uploader and channel_id for it
        self.failures = {}  # link -> yt-dlp's error text
        self.fail_once = {}  # link -> error text for the first try only
        self.bad_audio = set()  # audio stems transcribe fails on
        self.on_transcribe = None
        self.playlist = None  # what extract_info(download=False) returns
        self.listed, self.downloads, self.headers = [], [], []

        yt_dlp = types.ModuleType("yt_dlp")
        yt_dlp.utils = types.ModuleType("yt_dlp.utils")
        yt_dlp.utils.DownloadError = FakeDownloadError
        yt_dlp.YoutubeDL = self.youtube_dl()
        transcribe = types.ModuleType("transcribe")
        transcribe.transcribe = self.transcribe
        transcribe.write_midi = self.write_midi
        self.enterContext(mock.patch.dict(sys.modules, {"yt_dlp": yt_dlp, "yt_dlp.utils": yt_dlp.utils,
                                                       "transcribe": transcribe}))
        self.enterContext(mock.patch.dict(os.environ))
        self.enterContext(mock.patch.object(convert, "LEDGER", self.ledger))
        # Leaves cookies.txt and the JS runtimes alone.
        self.enterContext(mock.patch.object(convert, "ydl_options", dict))
        self.enterContext(mock.patch("shutil.which", return_value="C:\\tools\\ffmpeg.exe"))
        self.enterContext(mock.patch.object(convert, "CPU_SHARE", 100))

    def youtube_dl(self):
        test = self

        class YoutubeDL:
            def __init__(self, options):
                self.options = options

            def __enter__(self):
                return self

            def __exit__(self, *failure):
                return False

            def extract_info(self, link, download=True):
                if not download:
                    test.listed.append(link)
                    if isinstance(test.playlist, str):  # yt-dlp's error text
                        raise FakeDownloadError("ERROR: " + test.playlist)
                    return test.playlist
                test.downloads.append(link)
                test.headers.append(self.options.get("http_headers"))
                if link in test.fail_once:
                    raise FakeDownloadError("ERROR: " + test.fail_once.pop(link))
                if link in test.failures:
                    raise FakeDownloadError("ERROR: " + test.failures[link])
                title = test.titles.get(link, "Song " + link[-11:])
                with open(os.path.join(os.path.dirname(self.options["outtmpl"]), title + ".mp3"), "wb") as file:
                    file.write(b"ID3")
                return {"title": title, **test.channels.get(link, {})}

            def prepare_filename(self, info):
                return os.path.join(os.path.dirname(self.options["outtmpl"]), info["title"] + ".webm")

        return YoutubeDL

    def transcribe(self, audio, choice, threads, progress):
        if self.on_transcribe:
            self.on_transcribe(audio)
        if os.path.splitext(os.path.basename(audio))[0] in self.bad_audio:
            raise RuntimeError("The model could not be loaded.")
        progress(1, 2, "")
        progress(2, 2, "CUDA")
        return [audio]

    def write_midi(self, notes, path):
        with open(path, "wb") as file:
            file.write(b"MThd")

    def run_main(self, *args):
        out = io.StringIO()
        with mock.patch.object(sys, "argv", ["convert.py", *args]), redirect_stdout(out), \
                redirect_stderr(io.StringIO()):
            try:
                code = convert.main()
            except SystemExit as stop:
                code = stop.code
        return code, out.getvalue().splitlines()

    def convert(self, source, *args):
        return self.run_main(source, "--out-dir", self.out, *args)

    def convert_list(self, data, *args, raw=None):
        path = os.path.join(self.root, "list.json")
        with open(path, "w", encoding="utf-8") as file:
            file.write(raw if raw is not None else json.dumps(data, ensure_ascii=False))
        return self.run_main("--list", path, "--out-dir", self.out, *args)

    def tagged(self, lines, *kinds):
        return [line for line in lines if line.split(": ", 1)[0] in kinds]

    def midi(self, *parts):
        return os.path.join(self.out, *parts)

    def records(self):
        return convert.ledger_records()

    # A single file or link.

    def test_single_link(self):
        self.titles[WATCH + V1] = "Nocturne"
        code, lines = self.convert(WATCH + V1)
        self.assertEqual(code, 0)
        self.assertEqual(lines, ["step: Downloading the audio", "step: Transcribing",
                                 "step: Transcribing on the CPU, 50%", "step: Transcribing with CUDA, 100%",
                                 "done: " + self.midi("Nocturne.mid")])
        with open(self.midi("Nocturne.mid"), "rb") as file:
            self.assertEqual(file.read(), b"MThd")
        record = self.records()[V1]
        self.assertEqual((record["path"], record["title"]), (self.midi("Nocturne.mid"), "Nocturne"))

    def test_single_link_already_converted(self):
        self.titles[WATCH + V1] = "Nocturne"
        self.convert(WATCH + V1)
        code, lines = self.convert("https://youtu.be/" + V1 + "?si=abc")
        self.assertEqual(code, 0)
        self.assertEqual(lines, [f"exists: {V1}\t{self.midi('Nocturne.mid')}",
                                 "finished: Nocturne is already converted."])
        self.assertEqual(len(self.downloads), 1)

    def test_single_link_again(self):
        self.titles[WATCH + V1] = "Nocturne"
        self.convert(WATCH + V1)
        code, lines = self.convert(WATCH + V1, "--again")
        self.assertEqual((code, lines[-1]), (0, "done: " + self.midi("Nocturne (2).mid")))
        self.assertEqual(self.records()[V1]["path"], self.midi("Nocturne (2).mid"))

    def test_single_link_whose_file_is_gone(self):
        self.titles[WATCH + V1] = "Nocturne"
        self.convert(WATCH + V1)
        os.remove(self.midi("Nocturne.mid"))
        code, lines = self.convert(WATCH + V1)
        self.assertEqual((code, lines[-1]), (0, "done: " + self.midi("Nocturne.mid")))

    def test_single_link_fails(self):
        self.failures[WATCH + V1] = PRIVATE.format(V1)
        code, lines = self.convert(WATCH + V1)
        self.assertEqual(code, 1)
        self.assertEqual(lines[-2:], [f"failed: {V1}\tPrivate video.", "error: " + PRIVATE.format(V1)])
        self.assertEqual(self.records(), {})

    def test_single_link_transcription_fails(self):
        self.titles[WATCH + V1] = "Nocturne"
        self.bad_audio.add("Nocturne")
        code, lines = self.convert(WATCH + V1)
        self.assertEqual(code, 1)
        self.assertEqual(lines[-2:], [f"failed: {V1}\tTranscription failed.", "error: The model could not be loaded."])

    def test_single_link_blocked(self):
        self.failures[WATCH + V1] = BOT.format(V1)
        code, lines = self.convert(WATCH + V1)
        self.assertEqual(code, 1)
        self.assertEqual(lines[-2], "blocked: signin")
        self.assertTrue(lines[-1].startswith("error: YouTube refused this connection as a bot."))
        self.assertEqual(self.tagged(lines, "failed"), [])

    def test_single_link_subfolder(self):
        self.titles[WATCH + V1] = "Nocturne"
        code, lines = self.convert(WATCH + V1, "--subfolder", "Chopin: Nocturnes.")
        self.assertEqual((code, lines[-1]), (0, "done: " + self.midi("Chopin Nocturnes", "Nocturne.mid")))

    def test_subfolder_starting_with_a_dash(self):
        self.titles[WATCH + V1] = "Nocturne"
        code, lines = self.convert(WATCH + V1, "--subfolder=-Piano-")
        self.assertEqual((code, lines[-1]), (0, "done: " + self.midi("-Piano-", "Nocturne.mid")))

    def test_subfolder_is_not_made_for_a_failure(self):
        self.failures[WATCH + V1] = PRIVATE.format(V1)
        self.assertEqual(self.convert(WATCH + V1, "--subfolder", "Chopin")[0], 1)
        self.titles[WATCH + V2] = "Etude"
        self.bad_audio.add("Etude")
        self.assertEqual(self.convert(WATCH + V2, "--subfolder", "Chopin")[0], 1)
        self.assertEqual(os.listdir(self.out), [])

    def test_single_link_retries_a_403_as_quartzmidi(self):
        link = "https://upload.example.org/Nocturne.ogg"
        self.titles[link] = "Nocturne"
        self.fail_once[link] = "HTTP Error 403: Forbidden"
        code, lines = self.convert(link)
        self.assertEqual(code, 0)
        self.assertIn("step: The site refused the download. Trying again as QuartzMIDI", lines)
        self.assertEqual(self.headers, [None, {"User-Agent": convert.USER_AGENT}])
        self.assertEqual(self.records(), {})  # not a YouTube video
        self.assertEqual(self.tagged(lines, "item", "exists", "failed", "blocked"), [])

    def test_single_file(self):
        audio = os.path.join(self.root, "Etude.mp3")
        with open(audio, "wb") as file:
            file.write(b"ID3")
        code, lines = self.convert(audio)
        self.assertEqual(lines, ["step: Transcribing", "step: Transcribing on the CPU, 50%",
                                 "step: Transcribing with CUDA, 100%", "done: " + self.midi("Etude.mid")])
        self.assertEqual((code, self.records(), self.downloads), (0, {}, []))

    def test_single_file_missing(self):
        code, lines = self.convert(os.path.join(self.root, "missing.mp3"))
        self.assertEqual(code, 1)
        self.assertEqual(lines, ["error: The file does not exist: " + os.path.join(self.root, "missing.mp3")])

    def test_source_or_list(self):
        self.assertEqual(self.run_main("--out-dir", self.out)[0], 2)
        path = os.path.join(self.root, "list.json")
        self.assertEqual(self.run_main(WATCH + V1, "--list", path, "--out-dir", self.out)[0], 2)

    def test_missing_output_folder(self):
        missing = os.path.join(self.root, "missing")
        self.assertEqual(self.run_main(WATCH + V1, "--out-dir", missing),
                         (2, ["error: The output folder does not exist: " + missing]))

    # A --list run.

    def test_list_into_its_folder(self):
        self.titles.update({WATCH + V1: "Nocturne Op. 9", WATCH + V2: "Etude"})
        code, lines = self.convert_list({"title": "Chopin", "folder": "Chopin: Best",
                                         "items": [{"id": V1, "title": "Nocturne"}, {"id": V2, "title": "Etude"}]})
        self.assertEqual(code, 0)
        folder = self.midi("Chopin Best")
        self.assertEqual(self.tagged(lines, "item", "saved", "finished"), [
            f"item: 1\t2\t{V1}\tNocturne", "saved: " + os.path.join(folder, "Nocturne Op. 9.mid"),
            f"item: 2\t2\t{V2}\tEtude", "saved: " + os.path.join(folder, "Etude.mid"),
            "finished: Converted 2 of 2 videos from Chopin."])
        self.assertIn("step: 1 of 2, Nocturne: Downloading the audio", lines)
        self.assertIn("step: 2 of 2, Etude: Transcribing on the CPU, 50%", lines)
        self.assertEqual(self.records()[V1]["path"], os.path.join(folder, "Nocturne Op. 9.mid"))
        self.assertEqual(self.downloads, [WATCH + V1, WATCH + V2])

    def test_list_selection_without_a_folder(self):
        self.titles[WATCH + V1] = "Nocturne"
        code, lines = self.convert_list({"title": "", "folder": None, "items": [{"id": V1, "title": "Nocturne"}]})
        self.assertEqual((code, lines[-2:]), (0, ["saved: " + self.midi("Nocturne.mid"),
                                                  "finished: Converted 1 of 1 videos."]))
        code, lines = self.convert_list({"items": [{"id": V2}]})
        self.assertEqual(self.tagged(lines, "item", "finished"), [
            f"item: 1\t1\t{V2}\t{V2}", "finished: Converted 1 of 1 videos."])

    def test_list_already_converted(self):
        self.titles[WATCH + V1] = "Nocturne"
        self.convert(WATCH + V1)
        self.downloads.clear()
        items = [{"id": V1, "title": "Nocturne"}, {"id": V2, "title": "Etude"}]
        code, lines = self.convert_list({"title": "Mix", "folder": None, "items": items})
        self.assertEqual(code, 0)
        self.assertEqual(self.tagged(lines, "item", "exists", "saved", "finished"), [
            f"item: 1\t2\t{V1}\tNocturne", f"exists: {V1}\t{self.midi('Nocturne.mid')}",
            f"item: 2\t2\t{V2}\tEtude", "saved: " + self.midi(f"Song {V2}.mid"),
            "finished: Converted 1 of 2 videos from Mix. 1 already converted."])
        self.assertEqual(self.downloads, [WATCH + V2])
        code, lines = self.convert_list({"title": "Mix", "folder": None, "items": items})
        self.assertEqual((code, lines[-1]), (0, "finished: Converted 0 of 2 videos from Mix. 2 already converted."))
        code, lines = self.convert_list({"title": "Mix", "folder": None, "items": items}, "--again")
        self.assertEqual((code, self.tagged(lines, "exists")), (0, []))
        self.assertEqual(self.records()[V1]["path"], self.midi("Nocturne (2).mid"))

    def test_list_skip_file_is_read_before_each_item(self):
        path = os.path.join(self.root, "list.json")
        with open(path + ".skip", "w", encoding="utf-8-sig") as file:
            file.write(V1 + "\n")

        def skip_third(audio):
            with open(path + ".skip", "a", encoding="utf-8") as file:
                file.write(V3 + "\n")

        self.on_transcribe = skip_third
        items = [{"id": video, "title": video} for video in (V1, V2, V3, V4)]
        code, lines = self.convert_list({"title": "Mix", "folder": "Mix", "items": items})
        self.assertEqual(code, 0)
        self.assertEqual(self.tagged(lines, "item", "finished"), [
            f"item: 2\t4\t{V2}\t{V2}", f"item: 4\t4\t{V4}\t{V4}", "finished: Converted 2 of 2 videos from Mix."])
        self.assertEqual(self.downloads, [WATCH + V2, WATCH + V4])

    def test_list_failure_goes_on(self):
        self.failures[WATCH + V1] = PRIVATE.format(V1)
        code, lines = self.convert_list({"title": "Chopin", "folder": None, "items": [
            {"id": V1, "title": "Nocturne"}, {"id": V2, "title": "Etude"}]})
        self.assertEqual(code, 0)
        self.assertIn(f"failed: {V1}\tPrivate video.", lines)
        self.assertIn("Skipped Nocturne: " + PRIVATE.format(V1), lines)
        self.assertLess(lines.index(f"failed: {V1}\tPrivate video."), lines.index(f"item: 2\t2\t{V2}\tEtude"))
        self.assertEqual(lines[-1], "finished: Converted 1 of 2 videos from Chopin. 1 failed; the log says why.")

    def test_list_blocked_stops_the_run(self):
        self.failures[WATCH + V1] = BOT.format(V1)
        code, lines = self.convert_list({"title": "Chopin", "folder": None, "items": [
            {"id": V1, "title": "Nocturne"}, {"id": V2, "title": "Etude"}]})
        self.assertEqual(code, 1)
        self.assertEqual(lines[-2], "blocked: signin")
        self.assertTrue(lines[-1].startswith("error: YouTube refused this connection as a bot."))
        self.assertEqual(self.tagged(lines, "failed", "finished", "saved"), [])
        self.assertEqual(self.downloads, [WATCH + V1])

    def test_list_line_formats(self):
        self.failures[WATCH + V1] = REMOVED.format(V1)
        code, lines = self.convert_list({"title": "A\tB", "folder": "A", "items": [
            {"id": V1, "title": "Tab\there\nand\r\nthere"}]})
        self.assertEqual(code, 1)
        self.assertEqual(self.tagged(lines, "item", "failed", "error"), [
            f"item: 1\t1\t{V1}\tTab here and  there", f"failed: {V1}\tRemoved video.", "error: No videos converted."])
        self.assertEqual(os.listdir(self.out), [])  # no empty folder for a list that saved nothing

    def test_list_that_cannot_be_read(self):
        missing = os.path.join(self.root, "missing.json")
        code, lines = self.run_main("--list", missing, "--out-dir", self.out)
        self.assertEqual(code, 2)
        self.assertTrue(lines[0].startswith("error: The list of videos could not be read: "))
        for raw in ("{", "[]", "\"items\""):
            with self.subTest(raw=raw):
                code, lines = self.convert_list(None, raw=raw)
                self.assertEqual(code, 2)
                self.assertTrue(lines[-1].startswith("error: The list of videos could not be read: "))

    def test_list_passes_over_bad_items(self):
        path = os.path.join(self.root, "list.json")
        with open(path, "w", encoding="utf-8-sig") as file:  # a BOM, as PowerShell writes
            json.dump({"title": "Mix", "folder": 5, "dest": ["Anime"], "artist": "UCshort", "items": [
                {"id": "short"}, {"id": 5}, {"title": "no id"}, "text", None, {"id": V1, "title": ""}]}, file)
        self.assertEqual(convert.read_list(path), ("Mix", None, None, None, [(V1, V1)]))
        with open(path, "w", encoding="utf-8") as file:
            json.dump({"folder": "Old", "dest": "", "artist": UC1, "items": []}, file)
        self.assertEqual(convert.read_list(path), ("", "Old", "", UC1, []))

    # A --playlist run.

    def playlist_of(self, title, *entries):
        self.playlist = {"title": title, "entries": list(entries)}

    def test_playlist_filters_piano_and_streams(self):
        self.playlist_of("Chopin Favourites",
                         {"url": WATCH + V1, "title": "Nocturne Op. 9 No. 2", "live_status": "not_live"},
                         {"url": WATCH + V2, "title": "Canon - Violin & Piano"},
                         {"id": V3, "title": "Piano Livestream", "live_status": "is_live"},
                         {"id": V4, "title": "Piano Premiere", "live_status": "is_upcoming"},
                         {"id": V5, "title": "Etude (Live)", "live_status": "was_live"},
                         None)
        link = WATCH + V1 + "&list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG"
        code, lines = self.convert(link, "--playlist")
        self.assertEqual(code, 0)
        self.assertEqual(self.listed, [link])
        folder = self.midi("Chopin Favourites")
        self.assertEqual(self.tagged(lines, "step", "item", "saved", "finished")[0], "step: Reading the playlist")
        self.assertEqual(self.tagged(lines, "item", "saved", "finished"), [
            f"item: 1\t2\t{V1}\tNocturne Op. 9 No. 2", "saved: " + os.path.join(folder, f"Song {V1}.mid"),
            f"item: 2\t2\t{V5}\tEtude (Live)", "saved: " + os.path.join(folder, f"Song {V5}.mid"),
            "finished: Converted 2 of 2 videos from Chopin Favourites."])
        self.assertEqual(self.downloads, [WATCH + V1, WATCH + V5])

    def test_playlist_exists_and_failed(self):
        self.titles[WATCH + V1] = "Nocturne"
        self.convert(WATCH + V1)
        self.failures[WATCH + V2] = "[youtube] Dd4: This video is not available in your country"
        self.playlist_of("Chopin", {"url": WATCH + V1, "title": "Nocturne"}, {"url": WATCH + V2, "title": "Etude"})
        code, lines = self.convert(WATCH + V1 + "&list=PL1234", "--playlist")
        self.assertEqual(code, 0)
        self.assertEqual(self.tagged(lines, "exists", "failed", "finished"), [
            f"exists: {V1}\t{self.midi('Nocturne.mid')}", f"failed: {V2}\tNot available in your country.",
            "finished: Converted 0 of 2 videos from Chopin. 1 already converted. 1 failed; the log says why."])

    def test_playlist_that_converts_nothing(self):
        self.failures.update({WATCH + V1: REMOVED.format(V1), WATCH + V2: REMOVED.format(V2)})
        self.playlist_of("Chopin", {"url": WATCH + V1, "title": "Nocturne"}, {"url": WATCH + V2, "title": "Etude"})
        code, lines = self.convert(WATCH + V1 + "&list=PL1234", "--playlist")
        self.assertEqual((code, lines[-1]), (1, "error: None of the 2 videos in Chopin converted. The log says why."))
        self.playlist_of("", {"url": WATCH + V1, "title": "Nocturne"})
        code, lines = self.convert(WATCH + V1 + "&list=PL1234", "--playlist")
        self.assertEqual(lines[-1], "error: None of the 1 videos in the playlist converted. The log says why.")

    def test_playlist_without_a_title_saves_into_youtube(self):
        self.playlist_of("", {"url": WATCH + V1, "title": "Nocturne"})
        code, lines = self.convert(WATCH + V1 + "&list=PL1234", "--playlist")
        self.assertEqual((code, lines[-2:]), (0, ["saved: " + self.midi("YouTube", f"Song {V1}.mid"),
                                                  "finished: Converted 1 of 1 videos from the playlist."]))

    def test_playlist_with_no_piano_or_no_videos(self):
        self.playlist_of("Strings", {"url": WATCH + V1, "title": "Canon - Violin"})
        self.assertEqual(self.convert(WATCH + V1 + "&list=PL1234", "--playlist"),
                         (1, ["step: Reading the playlist", "error: The playlist has no piano videos that can be read."]))
        self.playlist_of("Empty")
        self.assertEqual(self.convert(WATCH + V1 + "&list=PL1234", "--playlist")[1][-1],
                         "error: The playlist has no videos that can be read.")
        self.assertEqual(self.downloads, [])

    def test_playlist_read_blocked(self):
        self.playlist = BOT.format("PL1234")
        code, lines = self.convert(WATCH + V1 + "&list=PL1234", "--playlist")
        self.assertEqual(code, 1)
        self.assertEqual(lines[:2], ["step: Reading the playlist", "blocked: signin"])
        self.assertTrue(lines[2].startswith("error: YouTube refused this connection as a bot."))
        self.assertEqual((len(lines), self.downloads), (3, []))
        self.playlist = "[youtube:tab] PL1234: The playlist does not exist."
        self.assertEqual(self.convert(WATCH + V1 + "&list=PL1234", "--playlist")[1][1:],
                         ["error: [youtube:tab] PL1234: The playlist does not exist."])

    def test_playlist_entry_without_a_video_id(self):
        self.playlist_of("Mixed", {"url": "https://soundcloud.com/a/nocturne", "title": "Nocturne"},
                         {"url": "https://soundcloud.com/a/etude", "title": "Etude"})
        self.titles["https://soundcloud.com/a/nocturne"] = "Nocturne"
        self.failures["https://soundcloud.com/a/etude"] = "HTTP Error 404: Not Found"
        code, lines = self.convert("https://soundcloud.com/a/sets/mixed", "--playlist")
        self.assertEqual(code, 0)
        self.assertEqual(self.tagged(lines, "item", "failed", "exists"), [])
        self.assertIn("saved: " + self.midi("Mixed", "Nocturne.mid"), lines)
        self.assertIn("Skipped Etude: HTTP Error 404: Not Found", lines)
        self.assertEqual(lines[-1], "finished: Converted 1 of 2 videos from Mixed. 1 failed; the log says why.")
        self.assertEqual(self.records(), {})

    def test_channel(self):
        self.playlist_of("Chopin Fan - Videos", {"id": V1, "title": "Nocturne"},
                         {"id": V2, "title": "Nocturne with Orchestra"})
        code, lines = self.convert("https://www.youtube.com/@ChopinFan/featured", "--playlist")
        self.assertEqual(code, 0)
        self.assertEqual(self.listed, ["https://www.youtube.com/@ChopinFan/videos"])
        self.assertEqual(lines[0], "step: Reading the channel")
        self.assertEqual(self.tagged(lines, "item", "saved", "finished"), [
            f"item: 1\t1\t{V1}\tNocturne", "saved: " + self.midi("Chopin Fan", f"Song {V1}.mid"),
            "finished: Converted 1 of 1 videos from Chopin Fan."])

    def test_channel_with_no_piano(self):
        self.playlist_of("Strings - Videos", {"id": V1, "title": "Canon - Violin"})
        code, lines = self.convert("https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw", "--playlist")
        self.assertEqual((code, lines[-1]), (1, "error: The channel has no piano videos that can be read."))

    # A dest: --dest, --artist, and a list's "dest" and "artist".

    def folder(self, *parts):
        path = self.midi(*parts)
        os.makedirs(path)
        return path

    def test_ledger_channel_fields(self):
        self.channels.update({WATCH + V1: {"channel": "Chopin Fan", "uploader": "Uploader", "channel_id": UC1},
                              WATCH + V2: {"uploader": "Uploader", "channel_id": UC2}})
        for video in (V1, V2, V3):
            self.assertEqual(self.convert(WATCH + video)[0], 0)
        records = self.records()
        self.assertEqual((records[V1]["channel"], records[V1]["channelId"]), ("Chopin Fan", UC1))
        self.assertEqual((records[V2]["channel"], records[V2]["channelId"]), ("Uploader", UC2))
        self.assertEqual(sorted(records[V3]), ["at", "id", "path", "title"])
        self.assertFalse(any("dest" in record or "artist" in record for record in records.values()))

    def test_single_link_into_an_existing_folder(self):
        self.folder("Classical", "Chopin")
        self.titles[WATCH + V1] = "Nocturne"
        self.channels[WATCH + V1] = {"channel": "Chopin Fan", "channel_id": UC1}
        code, lines = self.convert(WATCH + V1, "--dest=classical/CHOPIN", "--subfolder", "Ignored",
                                   "--artist=" + UC1)
        self.assertEqual((code, lines[-1]), (0, "done: " + self.midi("Classical", "Chopin", "Nocturne.mid")))
        self.assertEqual(os.listdir(self.out), ["Classical"])
        record = self.records()[V1]
        self.assertEqual((record["dest"], record["artist"]), ("Classical/Chopin", True))

    def test_single_link_into_the_midi_folder(self):
        self.channels[WATCH + V1] = {"channel": "Chopin Fan", "channel_id": UC1}
        code, lines = self.convert(WATCH + V1, "--dest=", "--artist=" + UC2)
        self.assertEqual((code, lines[-1]), (0, "done: " + self.midi(f"Song {V1}.mid")))
        self.assertEqual(self.records()[V1]["dest"], "")
        self.assertNotIn("artist", self.records()[V1])
        # An --artist that is not a channel id marks nothing.
        self.channels[WATCH + V2] = {"channel": "Odd", "channel_id": "nope"}
        self.assertEqual(self.convert(WATCH + V2, "--dest=", "--artist=nope")[0], 0)
        self.assertNotIn("artist", self.records()[V2])

    def test_dest_folders_are_made_for_the_first_file(self):
        dest = "--dest=Anime/Animenz  Piano Sheets"
        self.failures[WATCH + V1] = PRIVATE.format(V1)
        self.assertEqual(self.convert(WATCH + V1, dest)[0], 1)
        self.titles[WATCH + V2] = "Unravel"
        self.bad_audio.add("Unravel")
        self.assertEqual(self.convert(WATCH + V2, dest)[0], 1)
        self.assertEqual(os.listdir(self.out), [])
        self.titles[WATCH + V3] = "Kaikai Kitan"
        code, lines = self.convert(WATCH + V3, dest)
        self.assertEqual((code, lines[-1]),
                         (0, "done: " + self.midi("Anime", "Animenz Piano Sheets", "Kaikai Kitan.mid")))
        self.assertEqual(self.records()[V3]["dest"], "Anime/Animenz Piano Sheets")

    def test_dest_refused(self):
        for dest in ("..", "../Elsewhere", "Anime/..", "A//B", "A\\B", "CON", "Chopin​", "A/B/C/D/E", " Chopin"):
            with self.subTest(dest=ascii(dest)):
                code, lines = self.convert(WATCH + V1, "--dest=" + dest)
                self.assertEqual((code, lines), (1, [f"failed: {V1}\tFolder not allowed.", "error: Folder not allowed."]))
        self.assertEqual((self.downloads, os.listdir(self.out)), ([], []))

    @unittest.skipUnless(_winapi, "junctions are Windows only")
    def test_dest_through_a_junction_refused(self):
        elsewhere = os.path.join(self.root, "Elsewhere")
        os.mkdir(elsewhere)
        _winapi.CreateJunction(elsewhere, self.midi("Link"))
        self.addCleanup(os.rmdir, self.midi("Link"))
        self.assertEqual(self.convert(WATCH + V1, "--dest=Link")[1][-1], "error: Folder not allowed.")
        self.assertEqual(self.convert(WATCH + V1 + "&list=PL1234", "--playlist", "--dest=link/Chopin")[1],
                         ["error: Folder not allowed."])
        self.assertEqual((self.downloads, self.listed, os.listdir(elsewhere)), ([], [], []))

    @unittest.skipUnless(_winapi, "junctions are Windows only")
    def test_a_name_through_a_junction_refused(self):
        elsewhere = os.path.join(self.root, "Elsewhere")
        os.mkdir(elsewhere)
        _winapi.CreateJunction(elsewhere, self.midi("Link"))
        self.addCleanup(os.rmdir, self.midi("Link"))
        self.assertEqual(self.convert(WATCH + V1, "--subfolder", "link")[1][-1], "error: Folder not allowed.")
        self.assertEqual(self.convert_list({"title": "Mix", "folder": ".Link", "items": [{"id": V2}]}),
                         (1, ["error: Folder not allowed."]))
        self.playlist_of("Link", {"url": WATCH + V3, "title": "Nocturne"})
        self.assertEqual(self.convert(WATCH + V3 + "&list=PL1234", "--playlist")[1][-1], "error: Folder not allowed.")
        self.assertEqual((self.downloads, os.listdir(elsewhere)), ([], []))

    def test_path_too_long(self):
        # Windows takes a file path of up to 259 characters.
        fits, too_long = deep_dest(self.out, 0), deep_dest(self.out, 1)
        if too_long is None:
            self.skipTest("the temporary folder is too deep")
        self.titles.update({WATCH + V1: "Nocturne", WATCH + V2: "N"})
        code, lines = self.convert(WATCH + V1, "--dest=" + fits)
        self.assertEqual((code, lines[-2:]), (1, [f"failed: {V1}\tPath too long.", "error: Path too long."]))
        self.assertNotIn("step: Transcribing", lines)
        self.assertEqual(os.listdir(self.out), [])
        code, lines = self.convert(WATCH + V2, "--dest=" + fits)
        self.assertEqual(code, 0)
        self.assertEqual(len(lines[-1].removeprefix("done: ")), convert.LONGEST)
        self.assertEqual(self.convert(WATCH + V3, "--dest=" + too_long),
                         (1, [f"failed: {V3}\tPath too long.", "error: Path too long."]))
        self.assertEqual(self.convert_list({"items": [{"id": V4}]}, "--dest=" + too_long),
                         (1, ["error: Path too long."]))
        self.assertEqual(self.downloads, [WATCH + V1, WATCH + V2])

    def test_a_list_file_name_too_long_goes_on(self):
        fits = deep_dest(self.out, 0)
        if fits is None:
            self.skipTest("the temporary folder is too deep")
        self.titles.update({WATCH + V1: "Nocturne", WATCH + V2: "N"})
        code, lines = self.convert_list({"title": "Mix", "dest": fits, "items": [{"id": V1}, {"id": V2}]})
        self.assertEqual(code, 0)
        self.assertIn(f"failed: {V1}\tPath too long.", lines)
        self.assertIn(f"Skipped {V1}: Path too long.", lines)
        self.assertEqual(lines[-1], "finished: Converted 1 of 2 videos from Mix. 1 failed; the log says why.")

    def test_list_dest_beats_its_folder_and_keeps_the_artist(self):
        self.folder("anime")
        self.titles.update({WATCH + V1: "Unravel", WATCH + V2: "Gurenge"})
        self.channels.update({WATCH + V1: {"channel": "Animenz Piano Sheets", "channel_id": UC1},
                              WATCH + V2: {"channel": "Someone Else", "channel_id": UC2}})
        code, lines = self.convert_list({"title": "Animenz", "folder": "Animenz", "dest": "Anime/Animenz Piano Sheets",
                                         "artist": UC1, "items": [{"id": V1, "title": "Unravel"},
                                                                  {"id": V2, "title": "Gurenge"}]})
        folder = self.midi("anime", "Animenz Piano Sheets")
        self.assertEqual((code, self.tagged(lines, "saved", "finished")), (0, [
            "saved: " + os.path.join(folder, "Unravel.mid"), "saved: " + os.path.join(folder, "Gurenge.mid"),
            "finished: Converted 2 of 2 videos from Animenz."]))
        self.assertEqual(os.listdir(self.out), ["anime"])
        records = self.records()
        self.assertEqual({key: records[V1].get(key) for key in ("title", "channel", "channelId", "dest", "artist")},
                         {"title": "Unravel", "channel": "Animenz Piano Sheets", "channelId": UC1,
                          "dest": "anime/Animenz Piano Sheets", "artist": True})
        self.assertEqual((records[V2]["dest"], "artist" in records[V2]), ("anime/Animenz Piano Sheets", False))

    def test_list_dest_and_the_command_line(self):
        # --dest beats a list's folder, and a list's dest beats --dest; a list's artist beats --artist.
        self.channels.update({WATCH + V1: {"channel_id": UC1}, WATCH + V2: {"channel_id": UC2}})
        code, lines = self.convert_list({"folder": "Old", "items": [{"id": V1}]}, "--dest=New", "--artist=" + UC1)
        self.assertEqual((code, lines[-2]), (0, "saved: " + self.midi("New", f"Song {V1}.mid")))
        code, lines = self.convert_list({"folder": "Old", "dest": "", "artist": UC2, "items": [{"id": V2}]},
                                        "--dest=New", "--artist=" + UC1)
        self.assertEqual((code, lines[-2]), (0, "saved: " + self.midi(f"Song {V2}.mid")))
        records = self.records()
        self.assertEqual([(records[video]["dest"], records[video].get("artist")) for video in (V1, V2)],
                         [("New", True), ("", True)])
        self.assertEqual(sorted(os.listdir(self.out)), ["New", f"Song {V2}.mid"])

    def test_list_dest_refused(self):
        for dest in ("Mix/../..", "A\\B", "CON"):
            with self.subTest(dest=dest):
                code, lines = self.convert_list({"title": "Mix", "folder": "Mix", "dest": dest, "items": [{"id": V1}]})
                self.assertEqual((code, lines), (1, ["error: Folder not allowed."]))
        self.assertEqual((self.downloads, os.listdir(self.out)), ([], []))

    def test_list_dest_made_only_for_a_file(self):
        self.failures[WATCH + V1] = REMOVED.format(V1)
        code, lines = self.convert_list({"title": "Mix", "dest": "Mixes/New", "items": [{"id": V1}]})
        self.assertEqual((code, lines[-1]), (1, "error: No videos converted."))
        self.assertEqual(os.listdir(self.out), [])

    def test_playlist_dest_and_subfolder(self):
        self.folder("Classical")
        link = WATCH + V1 + "&list=PL1234"
        self.playlist_of("Chopin Favourites", {"url": WATCH + V1, "title": "Nocturne"})
        code, lines = self.convert(link, "--playlist", "--dest=CLASSICAL/Chopin", "--subfolder", "Ignored")
        self.assertEqual((code, lines[-2]), (0, "saved: " + self.midi("Classical", "Chopin", f"Song {V1}.mid")))
        self.assertEqual(self.records()[V1]["dest"], "Classical/Chopin")
        self.playlist_of("Chopin Favourites", {"url": WATCH + V2, "title": "Etude"})
        code, lines = self.convert(link, "--playlist", "--subfolder", "Mine: Chopin")
        self.assertEqual((code, lines[-2]), (0, "saved: " + self.midi("Mine Chopin", f"Song {V2}.mid")))
        self.assertNotIn("dest", self.records()[V2])
        self.playlist_of("Chopin Favourites", {"url": WATCH + V3, "title": "Waltz"})
        code, lines = self.convert(link, "--playlist", "--dest=")
        self.assertEqual((code, lines[-2]), (0, "saved: " + self.midi(f"Song {V3}.mid")))
        self.assertEqual(sorted(os.listdir(self.out)), ["Classical", "Mine Chopin", f"Song {V3}.mid"])

    def test_playlist_dest_refused_before_it_is_read(self):
        self.playlist_of("Chopin", {"url": WATCH + V1, "title": "Nocturne"})
        self.assertEqual(self.convert(WATCH + V1 + "&list=PL1234", "--playlist", "--dest=.."),
                         (1, ["error: Folder not allowed."]))
        self.assertEqual((self.listed, self.downloads), ([], []))

    def test_channel_into_the_artists_folder(self):
        self.folder("Chopin Fan")
        self.playlist_of("Chopin Fan - Videos", {"id": V1, "title": "Nocturne"}, {"id": V2, "title": "Etude"})
        self.channels.update({WATCH + V1: {"channel": "Chopin Fan", "channel_id": UC1},
                              WATCH + V2: {"channel": "Chopin Fan", "channel_id": UC1}})
        code, lines = self.convert("https://www.youtube.com/channel/" + UC1, "--playlist", "--dest=chopin fan",
                                   "--artist=" + UC1)
        self.assertEqual((code, lines[-1]), (0, "finished: Converted 2 of 2 videos from Chopin Fan."))
        self.assertEqual(sorted(os.listdir(self.midi("Chopin Fan"))), [f"Song {V1}.mid", f"Song {V2}.mid"])
        self.assertTrue(all(record["dest"] == "Chopin Fan" and record["artist"] is True
                            for record in self.records().values()))


class Script(unittest.TestCase):
    def test_help(self):
        done = subprocess.run([sys.executable, os.path.join(HERE, "convert.py"), "--help"], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        for option in ("--out-dir", "--playlist", "--list", "--again", "--subfolder", "--dest", "--artist"):
            self.assertIn(option, done.stdout)

    def test_import_needs_neither_yt_dlp_nor_transcribe(self):
        # browse.py imports convert for FolderName and the ledger.
        code = ("import sys; sys.path.insert(0, sys.argv[1]); import convert; "
                "print(sorted({'yt_dlp', 'transcribe', 'onnxruntime'} & set(sys.modules)))")
        done = subprocess.run([sys.executable, "-c", code, HERE], capture_output=True, text=True, timeout=60)
        self.assertEqual((done.returncode, done.stdout.strip()), (0, "[]"), done.stderr)


if __name__ == "__main__":
    unittest.main()
