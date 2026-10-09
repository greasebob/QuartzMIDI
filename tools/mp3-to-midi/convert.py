"""Audio to MIDI for QuartzMIDI: a file, a link, a playlist, a channel or a list of videos in, .mid files out.

The app runs this as a child process. Transcription uses the Transkun model,
as LioK251's mp3converter does (MIT, see README.md in this folder), on ONNX
Runtime with CUDA or DirectML (transcribe.py); links are downloaded with yt-dlp and
FFmpeg first.

Status protocol, one line per status on stdout:

    step: <current activity>
    saved: <path of a .mid written during a playlist or list run>
    done: <path of the .mid written for a single file or link>
    finished: <summary of a playlist or list run, or a link already converted>
    error: <reason for stopping>

Lines for the YouTube window, which the app reads as plain text (the fields
are separated by tabs):

    item: <number> <total> <video id> <title>   a playlist or list video starts
    exists: <video id> <path>                   converted before, so not again
    failed: <video id> <short reason>           this video failed; a run goes on
    blocked: signin                             YouTube refused the connection; error: follows

Other lines (skipped videos, a GPU that could not load the model) are plain
log text. Every video converted from YouTube is added to converted.jsonl
beside this script, one {"id", "path", "title", "at", "channel", "channelId"}
object per line, with "dest" when the run was given one and "artist": true
when the run's artist is the video's channel.
"""

import argparse
import json
import os
import re
import shutil
import stat
import sys
import tempfile
import unicodedata
from collections import namedtuple
from datetime import datetime
from urllib.parse import parse_qs, urlparse

# setup.ps1's Python runs isolated (its ._pth file), so it never puts this
# script's folder, where transcribe.py is, on the import path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from piano_match import is_other


def say(kind, text):
    print(f"{kind}: {text}", flush=True)


def one_line(text):
    """Text for a tab-separated line: tabs and line breaks become spaces."""
    return re.sub(r"[\t\n\r\v\f\x85\u2028\u2029]", " ", str(text))


def is_link(value):
    return value.lower().startswith(("http://", "https://"))


CHANNEL =re.compile(r"https?://(?:www\.|m\.)?youtube\.com/(?:@[^/?#]+|(?:channel|c|user)/[^/?#]+)", re.I)
YOUTUBE = {"youtube.com", "www.youtube.com", "m.youtube.com", "music.youtube.com"}
VIDEO = re.compile(r"[A-Za-z0-9_-]{11}")
WATCH = "https://www.youtube.com/watch?v="
LIVE = {"is_live", "is_upcoming"}  # yt-dlp's live_status values for a stream not yet over
LEDGER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "converted.jsonl")


def is_channel(link):
    return bool(CHANNEL.match(link))


def video_id(link):
    """The video id of a watch, youtu.be, Shorts, live or YouTube Music link, else None."""
    try:
        parts = urlparse(str(link))
    except ValueError:
        return None
    host = (parts.hostname or "").lower()
    if parts.scheme.lower() not in ("http", "https"):
        return None
    if host == "youtu.be":
        found = parts.path.strip("/")
    elif host in YOUTUBE and parts.path == "/watch":
        found = (parse_qs(parts.query).get("v") or [""])[0]
    elif host in YOUTUBE and parts.path.startswith(("/shorts/", "/live/")):
        found = parts.path.split("/")[2]
    else:
        return None
    return found if VIDEO.fullmatch(found) else None


# A playlist video. live is yt-dlp's live_status, None when it gives none.
Entry = namedtuple("Entry", "url title id live")


def piano_entries(entries):
    """The entries whose titles piano_match does not count as another instrument or voice."""
    return [entry for entry in entries if not is_other(entry.title)]


def unique_path(folder, stem):
    candidate = os.path.join(folder, f"{stem}.mid")
    number = 2
    while os.path.exists(candidate):
        candidate = os.path.join(folder, f"{stem} ({number}).mid")
        number += 1
    return candidate


def clean_stem(name):
    stem = "".join("_" if c in '<>:"/\\|?*' or ord(c) < 32 else c for c in name).strip(" .")
    return stem[:120] or "conversion"


# Windows' device names, which it opens in place of a file or folder so named;
# COM and LPT take a superscript 1, 2 or 3 too.
RESERVED = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
            *(f"{port}{n}" for port in ("COM", "LPT") for n in [*"123456789", "¹", "²", "³"])}
NOT_IN_NAMES = '<>:"/\\|?*'
SPACES = re.compile(r"\s+")


def FolderName(name):
    """The folder a playlist, channel, Mix, list or new dest name is saved into, as Windows accepts it.
    tests/fixtures/foldernames.json pins it for the page's folderName too."""
    name = "".join(c for c in str(name or "")
                   if c not in NOT_IN_NAMES and unicodedata.category(c) not in ("Cc", "Cf", "Cs"))
    name = SPACES.sub(" ", name).strip(" .")[:80].rstrip(" .")
    # Windows reserves a device name before any extension too ("CON.txt").
    stem, dot, rest = name.partition(".")
    if stem.rstrip(" ").upper() in RESERVED:
        name = (stem + "_" + dot + rest)[:80].rstrip(" .")
    return name or "YouTube"


# A dest is a folder in --out-dir: up to DEST_DEPTH names joined by "/", or "" for
# --out-dir itself. The page, browse.py and the app check it the same way.
DEST_DEPTH = 4
ARTIST = re.compile(r"UC[0-9A-Za-z_-]{22}")  # the channel id a dest was chosen for
LONGEST = 259  # characters in a file path: Windows' MAX_PATH less its closing null


class FolderFailure(Exception):
    """A dest that is refused, or a file path too long for Windows; the text is the reason."""


def units(text):
    """The length of text as Windows counts it, in UTF-16 units."""
    return len(text.encode("utf-16-le", "surrogatepass")) // 2


def valid_segment(name):
    """Whether name is a folder name a dest may hold, kept as written: 1 to 255
    units(), none of <>:"/\\|?* or a control, format or lone surrogate
    character, not . or .., no space first, no space or dot last, and no device
    name with or without an extension."""
    if not isinstance(name, str) or name in (".", "..") or name[:1] == " " or name[-1:] in (" ", "."):
        return False
    if not 1 <= units(name) <= 255:
        return False
    if any(c in NOT_IN_NAMES or unicodedata.category(c) in ("Cc", "Cf", "Cs") for c in name):
        return False
    return name.partition(".")[0].rstrip(" ").upper() not in RESERVED


def parse_dest(dest):
    """The folder names of a dest ([] for --out-dir itself), or None when it is not one."""
    if not isinstance(dest, str):
        return None
    names = dest.split("/") if dest else []
    return names if len(names) <= DEST_DEPTH and all(valid_segment(name) for name in names) else None


def folded(name):
    """A folder name as a dest matches it: without case, and with runs of whitespace as one space."""
    return SPACES.sub(" ", name).casefold()


NAME_SURROGATE = 0x20000000  # the tag bit of a reparse point that names another place


def redirects(entry):
    """Whether a folder entry leads somewhere else: a symlink, a junction or any
    other name-surrogate reparse point. A cloud placeholder (OneDrive, say) is a
    reparse point that stays where it is, so it is not one."""
    if entry.is_symlink() or entry.is_junction():
        return True
    found = entry.stat(follow_symlinks=False)
    return bool(getattr(found, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
                and getattr(found, "st_reparse_tag", 0) & NAME_SURROGATE)


def child_named(folder, name):
    """The entry of folder that name matches by folded(), its exact name first; else
    the one FolderName(name) matches, which making name would reach. None for neither."""
    with os.scandir(folder) as found:
        entries = list(found)
    for wanted in (name, FolderName(name)):
        matches = [entry for entry in entries if folded(entry.name) == folded(wanted)]
        if matches:
            return min(matches, key=lambda entry: (entry.name != wanted, entry.name))
    return None


def inside(path, root):
    """Whether the real path path is root or below it."""
    path, root = os.path.normcase(path), os.path.normcase(root)
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:  # another drive
        return False


def resolve_dest(out_dir, dest):
    """The folder a dest (text, or its list of names) names in out_dir; nothing is
    made. A name that matches an existing folder keeps that folder's own name; a
    new name goes through FolderName. Raises FolderFailure for a dest that is not
    valid, a name that matches a file or a folder that redirects(), or a folder
    that is not inside out_dir."""
    names = parse_dest(dest) if isinstance(dest, str) else dest
    if not isinstance(names, (list, tuple)) or len(names) > DEST_DEPTH or not all(map(valid_segment, names)):
        raise FolderFailure("Folder not allowed.")
    folder, new = out_dir, False
    for name in names:
        entry = None
        if not new:
            try:
                entry = child_named(folder, name)
                refused = entry is not None and (redirects(entry) or not entry.is_dir(follow_symlinks=False))
            except OSError:
                refused = True
            if refused:
                raise FolderFailure("Folder not allowed.")
        new = entry is None
        folder = os.path.join(folder, FolderName(name) if new else entry.name)
    if not inside(os.path.realpath(folder), os.path.realpath(out_dir)):
        raise FolderFailure("Folder not allowed.")
    return folder


def relative_dest(folder, out_dir):
    """A folder in out_dir as a dest: its names as they are on disk, joined by "/"."""
    names = os.path.relpath(folder, out_dir)
    return "" if names == os.curdir else names.replace(os.sep, "/")


def fits(path):
    return units(path) <= LONGEST


def destination(out_dir, dest=None, name=None):
    """The folder a run saves into: its dest resolved in out_dir, else the folder
    FolderName makes of name resolved the same way, else out_dir; nothing is
    made. Raises FolderFailure for a refused dest or name, or a folder too deep
    for even a one-letter file name."""
    if dest is not None:
        folder = resolve_dest(out_dir, dest)
    elif name is not None:
        # out_dir is made at the first save, and nothing is in it before then.
        folder = (resolve_dest(out_dir, [FolderName(name)]) if os.path.isdir(out_dir)
                  else os.path.join(out_dir, FolderName(name)))
    else:
        folder = out_dir
    if not fits(os.path.join(folder, "_.mid")):
        raise FolderFailure("Path too long.")
    return folder


def ledger_records():
    """The last ledger record for each video id. Lines that do not parse, such as
    a partial last line, are passed over."""
    records = {}
    try:
        with open(LEDGER, encoding="utf-8", errors="replace") as file:
            for line in file:
                try:
                    record = json.loads(line)
                except ValueError:
                    continue
                if isinstance(record, dict) and isinstance(record.get("id"), str) and isinstance(record.get("path"), str):
                    records[record["id"]] = record
    except OSError:
        pass
    return records


def converted(records, video):
    """The .mid the ledger records for this video, if it still exists."""
    path = records.get(video, {}).get("path")
    return path if path and os.path.isfile(path) else None


def remember(video, path, title, channel=None, channel_id=None, dest=None, artist=None):
    """Add a converted video to the ledger and return its record. dest is the run's
    dest, when it had one; artist is the channel id the run's dest was chosen for."""
    record = {"id": video, "path": path, "title": title, "at": datetime.now().astimezone().isoformat(timespec="seconds")}
    if channel:
        record["channel"] = channel
    if channel_id:
        record["channelId"] = channel_id
    if dest is not None:
        record["dest"] = dest
    if artist and artist == channel_id:
        record["artist"] = True
    try:
        with open(LEDGER, "a+b") as file:
            # After a partial last line, start a line of our own.
            end = file.seek(0, os.SEEK_END)
            if end:
                file.seek(end - 1)
                if file.read(1) != b"\n":
                    file.write(b"\n")
            file.write(json.dumps(record, ensure_ascii=False).encode("utf-8") + b"\n")
    except OSError as failure:
        print(f"The list of converted videos could not be written: {failure}", flush=True)
    return record


def ydl_options():
    options = {
        "noplaylist": True,
        "quiet": True,
        "no_warnings": True,
        "noprogress": True,
        # YouTube serves audio only to clients that solve its JavaScript
        # challenge, which needs a JS runtime plus yt-dlp's solver scripts
        # (fetched from yt-dlp's GitHub). Without them YouTube answers "Sign in
        # to confirm you're not a bot".
        "remote_components": ["ejs:github"],
    }
    for runtime in ("deno", "node", "bun"):
        found = shutil.which(runtime)
        if found:
            options["js_runtimes"] = {runtime: {"path": found}}
            break
    # Sign-in fallback: a Netscape cookies.txt next to this script (written by
    # signin.py or exported by the user). Browser cookie stores are never read.
    cookies = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cookies.txt")
    if os.path.isfile(cookies):
        options["cookiefile"] = cookies
    return options


USER_AGENT = "QuartzMIDI/1.0 (audio to MIDI converter)"


def fetch(link, options):
    import yt_dlp

    with yt_dlp.YoutubeDL(options) as ydl:
        info = ydl.extract_info(link, download=True)
        if info is None:
            raise RuntimeError("The link has no audio that can be downloaded.")
        return info, os.path.splitext(ydl.prepare_filename(info))[0] + ".mp3"


# What a source says of itself: its title, and for a download yt-dlp's channel
# name and id, None when the site gives none.
About = namedtuple("About", "title channel channel_id")


def download(link, folder, prefix=""):
    import yt_dlp

    say("step", f"{prefix}Downloading the audio")
    options = ydl_options()
    options.update({
        "format": "bestaudio/best",
        "outtmpl": os.path.join(folder, "%(title)s.%(ext)s"),
        "postprocessors": [{"key": "FFmpegExtractAudio", "preferredcodec": "mp3"}],
    })
    try:
        info, audio = fetch(link, options)
    except yt_dlp.utils.DownloadError as failure:
        # Some hosts reject yt-dlp's generic-extractor default headers with 403
        # (upload.wikimedia.org requires a descriptive User-Agent by policy).
        # Retry once with our own User-Agent. YouTube's extractor sets its own
        # headers and is unaffected.
        if "403" not in str(failure):
            raise
        say("step", f"{prefix}The site refused the download. Trying again as QuartzMIDI")
        options["http_headers"] = {"User-Agent": USER_AGENT}
        info, audio = fetch(link, options)
    if not os.path.exists(audio):
        raise RuntimeError("The download finished without an audio file.")
    return audio, About(info.get("title") or "conversion", info.get("channel") or info.get("uploader"),
                        info.get("channel_id"))


def playlist_entries(link):
    """Return the playlist title ("" when it has none) and its videos as Entry, without downloading."""
    import yt_dlp

    options = ydl_options()
    options.update({"noplaylist": False, "extract_flat": "in_playlist"})
    with yt_dlp.YoutubeDL(options) as ydl:
        info = ydl.extract_info(link, download=False)
    if info is None:
        raise RuntimeError("The playlist could not be read.")
    entries = []
    for entry in info.get("entries") or []:
        if not entry:
            continue
        url = entry.get("url") or entry.get("webpage_url")
        if not url and entry.get("id"):
            url = WATCH + entry["id"]
        if url:
            entries.append(Entry(url, one_line(entry.get("title") or url), video_id(url), entry.get("live_status")))
    return info.get("title") or "", entries


class DownloadFailure(Exception):
    """A download that failed; __cause__ is the exception that stopped it."""


class TranscribeFailure(Exception):
    """A transcription that failed; __cause__ is transcribe's exception."""


def reason_for(failure):
    if isinstance(failure, (DownloadFailure, TranscribeFailure)) and failure.__cause__:
        failure = failure.__cause__
    # yt-dlp has already printed its own "ERROR: " line; strip the prefix.
    reason = str(failure).removeprefix("ERROR: ") or type(failure).__name__
    if "confirm you" in reason and "not a bot" in reason:
        # The block applies to the connection, not the video; replace yt-dlp's
        # advice with the app's options.
        reason = ("YouTube refused this connection as a bot. Use Sign in to YouTube below, then try "
                  "the link again, or download the audio another way and use Choose audio file.")
    return reason


def is_blocked(reason):
    """A bot block affects every remaining video, so it stops a run."""
    return reason.startswith("YouTube refused this connection")


# The YouTube window's words for a failed video, the most specific first.
SHORT_REASONS = (
    (re.compile(r"private video", re.I), "Private video."),
    (re.compile(r"confirm your age|age-restricted", re.I), "Age-restricted video."),
    (re.compile(r"live event|is live|premieres", re.I), "Live stream."),
    (re.compile(r"your country", re.I), "Not available in your country."),
    (re.compile(r"removed|unavailable|terminated|deleted", re.I), "Removed video."),
)


def short_reason(failure):
    if isinstance(failure, FolderFailure):
        return str(failure)
    if isinstance(failure, TranscribeFailure):
        return "Transcription failed."
    for pattern, reason in SHORT_REASONS:
        if pattern.search(str(failure)):
            return reason
    return "Download failed." if isinstance(failure, DownloadFailure) else "Conversion failed."


def convert_one(source, out_dir, choice, work, prefix=""):
    """Convert one file or link; return the path of the .mid written and what the source says of itself (About)."""
    if is_link(source):
        try:
            audio, about = download(source, work, prefix)
        except Exception as failure:
            raise DownloadFailure(str(failure)) from failure
    else:
        if not os.path.isfile(source):
            raise RuntimeError(f"The file does not exist: {source}")
        audio, about = source, About(os.path.splitext(os.path.basename(source))[0], None, None)

    try:
        import transcribe
    except Exception as failure:
        raise TranscribeFailure(str(failure)) from failure

    target = unique_path(out_dir, clean_stem(about.title))
    if not fits(target):
        raise FolderFailure("Path too long.")
    partial = os.path.join(work, "transcribed.mid")
    say("step", f"{prefix}Transcribing")

    def progress(done, total, engine):
        say("step", f"{prefix}Transcribing {'with ' + engine if engine else 'on the CPU'}, {done * 100 // total}%")

    try:
        transcribe.write_midi(transcribe.transcribe(audio, choice, cpu_threads(), progress), partial)
    except Exception as failure:
        raise TranscribeFailure(str(failure)) from failure
    # A dest's, a list's or --subfolder's folders are made only once they have a file to hold.
    os.makedirs(out_dir, exist_ok=True)
    shutil.move(partial, target)
    return target, about


CPU_SHARE = 100  # percent of cores a transcription may use (--cpu)


def cpu_threads():
    """The threads a transcription may use. Below 100% this process also drops
    to below-normal priority."""
    if CPU_SHARE >= 100:
        return os.cpu_count() or 1
    if os.name == "nt":
        import ctypes

        kernel = ctypes.windll.kernel32
        kernel.SetPriorityClass(kernel.GetCurrentProcess(), 0x4000)  # BELOW_NORMAL_PRIORITY_CLASS
    return max(1, round((os.cpu_count() or 1) * CPU_SHARE / 100))


def read_skips(list_file):
    """The video ids the app has taken off a running list (FILE.skip, one per line)."""
    try:
        with open(list_file + ".skip", encoding="utf-8-sig") as file:
            return {line.strip() for line in file if line.strip()}
    except OSError:
        return set()


def convert_videos(videos, out_dir, choice, work, again=False, list_file=None, dest=None, artist=None):
    """Convert each (id, link, title) in turn into out_dir, made when first needed,
    skipping failures; dest and artist go to the ledger (remember). Returns the
    saved, already converted, failed and skipped counts, or None when a bot block
    stopped the run."""
    records = ledger_records()
    saved = existing = failed = skipped = 0
    total = len(videos)
    for number, (video, url, title) in enumerate(videos, 1):
        if list_file and video in read_skips(list_file):
            skipped += 1
            continue
        if video:
            say("item", f"{number}\t{total}\t{video}\t{title}")
            found = None if again else converted(records, video)
            if found:
                say("exists", f"{video}\t{found}")
                existing += 1
                continue
        folder = tempfile.mkdtemp(dir=work)
        try:
            path, about = convert_one(url, out_dir, choice, folder, f"{number} of {total}, {title}: ")
            if video:
                records[video] = remember(video, path, title, about.channel, about.channel_id, dest, artist)
            say("saved", path)
            saved += 1
        except Exception as failure:
            reason = reason_for(failure)
            if is_blocked(reason):
                say("blocked", "signin")
                say("error", reason)
                return None
            failed += 1
            if video:
                say("failed", f"{video}\t{short_reason(failure)}")
            print(f"Skipped {title}: {reason}", flush=True)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
    return saved, existing, failed, skipped


def summary(saved, total, name, existing, failed):
    text = f"Converted {saved} of {total} videos" + (f" from {name}." if name else ".")
    if existing:
        text += f" {existing} already converted."
    if failed:
        text += f" {failed} failed; the log says why."
    return text


def convert_playlist(link, out_dir, choice, work, again=False, dest=None, subfolder=None, artist=None):
    """Convert each piano video in turn into its dest, else --subfolder, else a
    folder named after the playlist, skipping failures and streams not yet over.
    A channel converts the piano videos of its Videos tab."""
    # A dest or --subfolder is checked before the playlist is read.
    folder = destination(out_dir, dest, subfolder) if dest is not None or subfolder is not None else None
    channel = is_channel(link)
    say("step", "Reading the channel" if channel else "Reading the playlist")
    if channel:
        link = CHANNEL.match(link).group(0) + "/videos"
    title, found = playlist_entries(link)
    if channel:
        title = title.removesuffix(" - Videos")
    if folder is None:
        folder = destination(out_dir, name=title)
    name = title or "the playlist"
    entries = [entry for entry in piano_entries(found) if entry.live not in LIVE]
    if not entries:
        say("error", "The channel has no piano videos that can be read." if channel
            else "The playlist has no piano videos that can be read." if found
            else "The playlist has no videos that can be read.")
        return 1
    counts = convert_videos([(entry.id, entry.url, entry.title) for entry in entries], folder, choice, work, again,
                            dest=relative_dest(folder, out_dir) if dest is not None else None, artist=artist)
    if counts is None:
        return 1
    saved, existing, failed, _ = counts
    if not saved and not existing:
        say("error", f"None of the {len(entries)} videos in {name} converted. The log says why.")
        return 1
    say("finished", summary(saved, len(entries), name, existing, failed))
    return 0


# A --list file. folder, dest and artist are None when it gives none that can be used.
ListFile = namedtuple("ListFile", "title folder dest artist videos")


def read_list(list_file):
    """The title, folder, dest, artist and (id, title) videos of a --list file."""
    with open(list_file, encoding="utf-8-sig") as file:
        data = json.load(file)
    if not isinstance(data, dict):
        raise ValueError("it is not a JSON object")
    videos = []
    for item in data.get("items") or []:
        video = item.get("id") if isinstance(item, dict) else None
        if isinstance(video, str) and VIDEO.fullmatch(video):
            videos.append((video, one_line(item.get("title") or video)))
    folder, dest, artist = data.get("folder"), data.get("dest"), data.get("artist")
    return ListFile(one_line(data.get("title") or ""), folder if isinstance(folder, str) else None,
                    dest if isinstance(dest, str) else None,
                    artist if isinstance(artist, str) and ARTIST.fullmatch(artist) else None, videos)


def convert_list(list_file, out_dir, choice, work, again=False, dest=None, artist=None):
    """Convert the videos of a --list file in turn, into its dest, else --dest,
    else its folder when it names one. A list without a title is a selection."""
    try:
        listed = read_list(list_file)
    except (OSError, ValueError) as failure:
        say("error", f"The list of videos could not be read: {failure}")
        return 2
    name, videos = listed.title, listed.videos
    dest = listed.dest if listed.dest is not None else dest
    folder = destination(out_dir, dest, listed.folder)
    counts = convert_videos([(video, WATCH + video, title) for video, title in videos], folder, choice, work,
                            again, list_file, relative_dest(folder, out_dir) if dest is not None else None,
                            listed.artist or artist)
    if counts is None:
        return 1
    saved, existing, failed, skipped = counts
    if not saved and not existing:
        say("error", "No videos converted.")
        return 1
    say("finished", summary(saved, len(videos) - skipped, name, existing, failed))
    return 0


def convert_single(source, out_dir, choice, work, again=False, subfolder=None, dest=None, artist=None):
    """Convert one file or link into its dest, else --subfolder, unless the ledger
    has the link's video already."""
    video = video_id(source) if is_link(source) else None
    found = converted(ledger_records(), video) if video and not again else None
    if found:
        say("exists", f"{video}\t{found}")
        say("finished", f"{os.path.splitext(os.path.basename(found))[0]} is already converted.")
        return 0
    try:
        folder = destination(out_dir, dest, subfolder)
        path, about = convert_one(source, folder, choice, work)
    except Exception as failure:
        reason = reason_for(failure)
        if is_blocked(reason):
            say("blocked", "signin")
        elif video:
            say("failed", f"{video}\t{short_reason(failure)}")
        say("error", reason)
        return 1
    if video:
        remember(video, path, about.title, about.channel, about.channel_id,
                 relative_dest(folder, out_dir) if dest is not None else None, artist)
    say("done", path)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", nargs="?", help="an audio file, or an http(s) link")
    parser.add_argument("--out-dir", required=True, help="the folder the .mid files are written to")
    parser.add_argument("--device", default="auto", help="auto (the GPU through CUDA or DirectML when there is one) or cpu")
    parser.add_argument("--playlist", action="store_true",
                        help="for a link inside a playlist, convert every video in it; for a channel, its piano videos")
    parser.add_argument("--cpu", type=int, default=100,
                        help="percent of the processor a transcription may use")
    parser.add_argument("--list", metavar="FILE",
                        help='convert the YouTube videos in a JSON file: {"title", "folder", "dest", "artist", '
                             '"items": [{"id", "title"}]}')
    parser.add_argument("--again", action="store_true", help="convert videos that converted.jsonl has already")
    parser.add_argument("--subfolder", metavar="NAME",
                        help="for a single file, a link or a --playlist link, the folder in --out-dir to save into")
    parser.add_argument("--dest", metavar="FOLDERS",
                        help='the folder in --out-dir to save into, up to 4 names joined by "/" ("" for --out-dir '
                             "itself); a name that matches a folder without case uses it, and --subfolder is ignored")
    parser.add_argument("--artist", metavar="CHANNEL",
                        help="the channel id the dest was chosen for, marked in converted.jsonl on its videos")
    args = parser.parse_args()
    if (args.source is None) == (args.list is None):
        parser.error("give either a source or --list")
    artist = args.artist if args.artist and ARTIST.fullmatch(args.artist) else None
    global CPU_SHARE
    CPU_SHARE = min(100, max(1, args.cpu))

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    # setup.ps1 installs FFmpeg and Deno in subfolders here; prepend them to PATH.
    # yt-dlp needs Deno for YouTube's JavaScript challenge.
    here = os.path.dirname(os.path.abspath(__file__))
    # Windows searches the current directory before PATH, so switch to this
    # folder to avoid running a stray ffmpeg.exe from the inherited one.
    args.out_dir = os.path.abspath(args.out_dir)
    if args.source and not is_link(args.source):
        args.source = os.path.abspath(args.source)
    if args.list:
        args.list = os.path.abspath(args.list)
    os.chdir(here)
    # Library deprecation warnings read as errors in the app's log.
    os.environ["PYTHONWARNINGS"] = "ignore"
    for tool in ("ffmpeg", "deno"):
        bundled = os.path.join(here, tool)
        if os.path.isdir(bundled):
            os.environ["PATH"] = bundled + os.pathsep + os.environ.get("PATH", "")
    if not os.path.isdir(args.out_dir):
        say("error", f"The output folder does not exist: {args.out_dir}")
        return 2
    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            say("error", f"{tool} is not installed, and the converter needs it to read audio.")
            return 2

    work = tempfile.mkdtemp(prefix="midipp-convert-")
    try:
        if args.list:
            return convert_list(args.list, args.out_dir, args.device, work, args.again, args.dest, artist)
        if args.playlist and is_link(args.source):
            return convert_playlist(args.source, args.out_dir, args.device, work, args.again, args.dest,
                                    args.subfolder, artist)
        return convert_single(args.source, args.out_dir, args.device, work, args.again, args.subfolder, args.dest,
                              artist)
    except Exception as failure:  # report a status line, not a traceback; a refused dest says so
        reason = reason_for(failure)
        if is_blocked(reason):
            say("blocked", "signin")  # reading a playlist or channel can be refused too
        say("error", reason)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
