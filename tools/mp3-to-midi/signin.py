"""Sign in to YouTube so convert.py can download links YouTube flags as bots.

Opens a pywebview (WebView2) window with a persistent profile in ./browser.
The user signs in directly; this script never handles credentials. Once the
sign-in cookies appear, the session is written to ./cookies.txt for yt-dlp.

Status lines on stdout, same protocol as convert.py:

    step: <current activity>
    done: <path of cookies.txt>
    error: <reason for stopping>
"""

import ctypes
import email.utils
import os
import sys
import threading
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
COOKIES = os.path.join(HERE, "cookies.txt")
PROFILE = os.path.join(HERE, "browser")
START = "https://accounts.google.com/ServiceLogin?service=youtube&continue=https%3A%2F%2Fwww.youtube.com%2F"
# Cookies set only for a signed-in YouTube session.
SIGNED_IN = {"LOGIN_INFO", "SID", "__Secure-3PSID"}
# Logical px a window shrunk to fit keeps clear of the work area's edges.
MARGIN = 8
# The window class of QuartzMIDI's own window (ui/Shell.cpp); its popups are "ImGui Platform".
APP_CLASS = "QuartzMIDI"


def say(kind, text):
    print(f"{kind}: {text}", flush=True)


def physical(value, dpi):
    """pywebview's step from logical to physical px for a window's size and place
    (winforms.py BrowserForm.__init__): int(value * scale), scale = dpi / 96."""
    return int(value * (dpi / 96))


def fit_axis(low, high, want, least, centre, dpi, margin=MARGIN):
    """One axis of fit_window: the logical place, size and minimum."""
    room = high - low
    edge = -(-margin * dpi // 96)  # the margin in physical px, rounded up
    size = want
    if physical(want, dpi) > room:
        keep = edge if room > 4 * edge else 0  # no margin on a work area too small for one
        limit = room - 2 * keep
        # The largest logical size whose physical size is within the limit, never less.
        size = max(1, limit * 96 // dpi)
        while physical(size + 1, dpi) <= limit:
            size += 1
        while size > 1 and physical(size, dpi) > limit:
            size -= 1
    while True:
        span = physical(size, dpi)
        gap = min(edge, max(0, room - span) // 2)
        # Centred, then inside the work area; the left or top edge wins, so the title bar stays on it.
        target = max(low + gap, min(centre - span // 2, high - gap - span))
        # pywebview places a window at int(x * scale), which skips some physical px: the logical
        # place that lands nearest the target and keeps the window inside. Where none does, the
        # window fits too tightly to move, and a logical px less gives it room.
        start = target * 96 // dpi
        places = [place for place in range(start - 2, start + 4) if low <= physical(place, dpi) <= high - span]
        if places or size == 1:
            place = min(places, key=lambda p: abs(physical(p, dpi) - target)) if places else start
            return place, size, min(least, size)
        size -= 1


def fit_window(work, dpi, size, min_size=(200, 100), over=None, margin=MARGIN):
    """Where a pywebview window of size (logical px) opens so that it lies inside work.

    work is the monitor's work area (left, top, right, bottom) in the pixels of the process's
    windows, which pywebview makes system-DPI aware: physical px on a monitor at the system DPI,
    negative left of or above the primary monitor. dpi is the system DPI pywebview scales by
    (96 at 100%, 120 at 125%). over is a window rect in the same pixels to centre on, else the
    work area is centred on.

    Each axis keeps its size where it fits the work area. Where it does not, it shrinks to the
    work area less margin logical px on each side, and its minimum shrinks with it. The window
    is then moved inside the work area, keeping the margin where there is room, so its title
    bar and caption buttons are on the monitor.

    Returns the logical x, y, width, height and min_size for webview.create_window."""
    left, top, right, bottom = work
    around = over or work
    x, width, least_width = fit_axis(left, right, size[0], min_size[0], (around[0] + around[2]) // 2, dpi, margin)
    y, height, least_height = fit_axis(top, bottom, size[1], min_size[1], (around[1] + around[3]) // 2, dpi, margin)
    return x, y, width, height, (least_width, least_height)


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT),
                ("dwFlags", wintypes.DWORD)]


def app_window(pid):
    """QuartzMIDI's own window in process pid, else 0. The class and the process both have to
    match, so a pid that is not QuartzMIDI's, or no longer is, finds nothing."""
    if not pid:
        return 0
    user32 = ctypes.WinDLL("user32")
    user32.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.FindWindowExW.restype = wintypes.HWND
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    after = None
    while True:
        after = user32.FindWindowExW(None, after, APP_CLASS, None)  # top-level windows, front to back
        if not after:
            return 0
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(after, ctypes.byref(owner))
        if owner.value == pid:
            return after


def work_area(over=0, near=0):
    """The work area and system DPI of the monitor a window opens on, and over's rect. The monitor
    is over's (a window to centre on) while it is open and not minimised, else near's (QuartzMIDI's
    window, whose monitor alone is used, also while it is minimised), else the cursor's. Rects are
    (left, top, right, bottom) as pywebview's system-aware windows see them. None where Windows
    cannot say."""
    try:
        user32 = ctypes.WinDLL("user32")
        user32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        user32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
        user32.GetDpiForSystem.restype = wintypes.UINT
    except (AttributeError, OSError):
        return None  # before Windows 10 1607
    user32.IsWindow.argtypes = user32.IsIconic.argtypes = [wintypes.HWND]
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
    user32.MonitorFromWindow.restype = ctypes.c_void_p
    user32.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]
    user32.MonitorFromPoint.restype = ctypes.c_void_p
    user32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
    user32.GetMonitorInfoW.argtypes = [ctypes.c_void_p, ctypes.POINTER(MONITORINFO)]
    # This thread reads in system-aware px, as the windows use, even before pywebview sets the process.
    previous = user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-2))  # DPI_AWARENESS_CONTEXT_SYSTEM_AWARE
    if not previous:
        return None
    try:
        rect, box = None, wintypes.RECT()
        placed = over and user32.IsWindow(over) and not user32.IsIconic(over)
        if placed and user32.GetWindowRect(over, ctypes.byref(box)):
            rect = (box.left, box.top, box.right, box.bottom)
            monitor = user32.MonitorFromWindow(over, 2)  # MONITOR_DEFAULTTONEAREST
        elif near and user32.IsWindow(near):
            # A minimised window's monitor is the one it was on before.
            monitor = user32.MonitorFromWindow(near, 2)
        else:
            point = wintypes.POINT()
            user32.GetCursorPos(ctypes.byref(point))
            monitor = user32.MonitorFromPoint(point, 2)
        info = MONITORINFO()
        info.cbSize = ctypes.sizeof(info)
        if not monitor or not user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
            return None
        work = info.rcWork
        return (work.left, work.top, work.right, work.bottom), user32.GetDpiForSystem(), rect
    finally:
        user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(previous))


def placement(size, min_size=(200, 100), over=0, near=0):
    """webview.create_window's x, y, width, height and min_size for a window of size (logical
    px) fitted to the work area it opens on (fit_window): over's monitor, centred on over, else
    near's monitor or the cursor's, centred on its work area. Only the size, as asked, where
    Windows cannot say."""
    found = work_area(over, near)
    if not found:
        return {"width": size[0], "height": size[1], "min_size": min_size}
    work, dpi, rect = found
    x, y, width, height, least = fit_window(work, dpi, size, min_size, rect)
    return {"x": x, "y": y, "width": width, "height": height, "min_size": least}


def expiry(morsel):
    stamp = morsel["expires"]
    if not stamp:
        return 0
    try:
        return int(email.utils.parsedate_to_datetime(stamp).timestamp())
    except (TypeError, ValueError):
        return 0


def write_cookies(jars):
    lines = ["# Netscape HTTP Cookie File", "# Written by signin.py for yt-dlp. Do not share: it is a signed-in session.", ""]
    names = set()
    for jar in jars:
        for name, morsel in jar.items():
            domain = morsel["domain"] or ".youtube.com"
            names.add(name)
            lines.append("\t".join([
                domain,
                "TRUE" if domain.startswith(".") else "FALSE",
                morsel["path"] or "/",
                "TRUE" if morsel["secure"] else "FALSE",
                str(expiry(morsel)),
                name,
                morsel.value,
            ]))
    temporary = COOKIES + ".partial"
    with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    os.replace(temporary, COOKIES)
    return names


def watch(window, finished):
    # Poll every 2 s until the sign-in cookies appear.
    while not finished.is_set():
        time.sleep(2)
        try:
            url = window.get_current_url() or ""
            if "youtube.com" not in url:
                continue
            jars = window.get_cookies()
        except Exception:
            continue  # the window is closing or still loading
        names = {name for jar in jars for name in jar.keys()}
        if names & SIGNED_IN:
            write_cookies(jars)
            say("done", COOKIES)
            finished.set()
            window.destroy()
            return


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    try:
        import webview
    except ImportError:
        say("error", "pywebview is not installed for this Python.")
        return 2

    os.makedirs(PROFILE, exist_ok=True)
    say("step", "Sign in to YouTube in the window that opened")
    # On QuartzMIDI's monitor, which started this script, else the cursor's.
    window = webview.create_window("Sign in to YouTube for QuartzMIDI", START,
                                   **placement((520, 720), near=app_window(os.getppid())))
    finished = threading.Event()
    webview.start(watch, (window, finished), private_mode=False, storage_path=PROFILE)
    if not finished.is_set():
        say("error", "The window was closed before YouTube showed a signed-in account.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
