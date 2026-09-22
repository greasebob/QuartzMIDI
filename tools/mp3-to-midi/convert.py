"""Audio to MIDI for QuartzMIDI: a file, a link or a playlist in, .mid files out.

The app runs this as a child process. Transcription uses the Transkun model,
as LioK251's mp3converter does (MIT, see README.md in this folder); links are
downloaded with yt-dlp and FFmpeg first.

Status protocol, one line per status on stdout:

    step: <current activity>
    saved: <path of a .mid written during a playlist run>
    done: <path of the .mid written for a single file or link>
    finished: <summary of a playlist run>
    error: <reason for stopping>

Other lines (Transkun progress, skipped videos) are plain log text.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def say(kind, text):
    print(f"{kind}: {text}", flush=True)


def is_link(value):
    return value.lower().startswith(("http://", "https://"))


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
    return audio, info.get("title") or "conversion"


def playlist_entries(link):
    """Return the playlist title and its videos as (link, title), without downloading."""
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
            url = "https://www.youtube.com/watch?v=" + entry["id"]
        if url:
            entries.append((url, entry.get("title") or url))
    return info.get("title") or "the playlist", entries


def device(choice):
    if choice != "auto":
        return choice
    import torch

    return "cuda" if torch.cuda.is_available() else "cpu"


def reason_for(failure):
    # yt-dlp has already printed its own "ERROR: " line; strip the prefix.
    reason = str(failure).removeprefix("ERROR: ") or type(failure).__name__
    if "confirm you" in reason and "not a bot" in reason:
        # The block applies to the connection, not the video; replace yt-dlp's
        # advice with the app's options.
        reason = ("YouTube refused this connection as a bot. Use Sign in to YouTube below, then try "
                  "the link again, or download the audio another way and use Choose audio file.")
    return reason


def convert_one(source, out_dir, choice, work, prefix=""):
    """Convert one file or link and return the path of the .mid written."""
    if is_link(source):
        audio, title = download(source, work, prefix)
    else:
        if not os.path.isfile(source):
            raise RuntimeError(f"The file does not exist: {source}")
        audio, title = source, os.path.splitext(os.path.basename(source))[0]

    target = unique_path(out_dir, clean_stem(title))
    partial = os.path.join(work, "transcribed.mid")
    chosen = device(choice)
    say("step", f"{prefix}Transcribing on the {'GPU' if chosen == 'cuda' else chosen.upper()}; a song takes a few minutes")
    # Transkun reports progress on stderr; merge it into stdout as log text.
    result = subprocess.run(
        [sys.executable, "-m", "transkun.transcribe", audio, partial, "--device", chosen],
        stdout=sys.stdout,
        stderr=subprocess.STDOUT,
        **cpu_limits(),
    )
    if result.returncode != 0 or not os.path.exists(partial):
        raise RuntimeError(f"Transkun stopped with exit code {result.returncode}.")
    shutil.move(partial, target)
    return target


CPU_SHARE = 100  # percent of cores a transcription may use (--cpu)


def cpu_limits():
    """subprocess.run kwargs that cap PyTorch's thread pools (OMP/MKL) and, below
    100%, run the child at below-normal priority."""
    if CPU_SHARE >= 100:
        return {}
    threads = max(1, round((os.cpu_count() or 1) * CPU_SHARE / 100))
    env = dict(os.environ, OMP_NUM_THREADS=str(threads), MKL_NUM_THREADS=str(threads))
    limits = {"env": env}
    if os.name == "nt":
        limits["creationflags"] = subprocess.BELOW_NORMAL_PRIORITY_CLASS
    return limits


def convert_playlist(link, out_dir, choice, work):
    """Convert each video in turn, skipping failures."""
    say("step", "Reading the playlist")
    name, entries = playlist_entries(link)
    if not entries:
        say("error", "The playlist has no videos that can be read.")
        return 1
    saved = failed = 0
    for number, (url, title) in enumerate(entries, 1):
        folder = tempfile.mkdtemp(dir=work)
        try:
            say("saved", convert_one(url, out_dir, choice, folder, f"{number} of {len(entries)}, {title}: "))
            saved += 1
        except Exception as failure:
            reason = reason_for(failure)
            if reason.startswith("YouTube refused this connection"):
                # A bot block affects every remaining video; stop.
                say("error", reason)
                return 1
            failed += 1
            print(f"Skipped {title}: {reason}", flush=True)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
    if not saved:
        say("error", f"None of the {len(entries)} videos in {name} converted. The log says why.")
        return 1
    summary = f"Converted {saved} of {len(entries)} videos from {name}."
    if failed:
        summary += f" {failed} failed; the log says why."
    say("finished", summary)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", help="an audio file, or an http(s) link")
    parser.add_argument("--out-dir", required=True, help="the folder the .mid files are written to")
    parser.add_argument("--device", default="auto", help="auto, cpu or cuda")
    parser.add_argument("--playlist", action="store_true",
                        help="for a link inside a playlist, convert every video in it")
    parser.add_argument("--cpu", type=int, default=100,
                        help="percent of the processor a transcription may use")
    args = parser.parse_args()
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
    if not is_link(args.source):
        args.source = os.path.abspath(args.source)
    os.chdir(here)
    for tool in ("ffmpeg", "deno"):
        bundled = os.path.join(here, tool)
        if os.path.isdir(bundled):
            os.environ["PATH"] = bundled + os.pathsep + os.environ.get("PATH", "")
    if not os.path.isdir(args.out_dir):
        say("error", f"The output folder does not exist: {args.out_dir}")
        return 2
    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            say("error", f"{tool} is not installed, and Transkun needs it to read audio.")
            return 2

    work = tempfile.mkdtemp(prefix="midipp-convert-")
    try:
        if args.playlist and is_link(args.source):
            return convert_playlist(args.source, args.out_dir, args.device, work)
        say("done", convert_one(args.source, args.out_dir, args.device, work))
        return 0
    except Exception as failure:  # report a single status line, not a traceback
        say("error", reason_for(failure))
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
