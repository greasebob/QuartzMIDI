"""Generate ui/IconData.hpp from the Lucide SVGs.

SVG geometry is flattened to polylines so the UI only has to stroke them with
ImDrawList; unlike a raster atlas, this stays crisp at any DPI scale.

Run after changing third_party/lucide/icons:
  python tools/gen-icons.py
"""
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "third_party" / "lucide" / "icons"
# Brand marks come from Simple Icons (CC0, same 24-unit grid, filled); Lucide has none.
OWN = ROOT / "third_party" / "simple-icons" / "icons"
OUT = ROOT / "ui" / "IconData.hpp"

# (Icon enumerator, SVG file name), in ui/Panels.cpp enum order.
ICONS = [
    ("Folder", "folder"), ("Open", "file-music"), ("Refresh", "refresh-cw"),
    ("Settings", "settings"), ("Sun", "sun"), ("Moon", "moon"),
    ("Play", "play"), ("Pause", "pause"),
    ("Back", "chevrons-left"), ("Forward", "chevrons-right"),
    ("Minus", "minus"), ("Plus", "plus"),
    ("Left", "chevron-left"), ("Right", "chevron-right"),
    ("Down", "chevron-down"), ("Up", "chevron-up"),
    ("Close", "x"), ("Keyboard", "keyboard-music"),
    ("Speaker", "volume-2"), ("Muted", "volume-x"), ("Solo", "headphones"),
    ("Piano", "piano"), ("Mini", "minimize-2"), ("Expand", "maximize-2"),
    ("Copy", "copy"), ("Rename", "pencil"), ("Check", "check"),
    ("SortDown", "arrow-down"), ("SortUp", "arrow-up"),
    ("Undo", "undo-2"), ("Redo", "redo-2"), ("Anchor", "git-commit-horizontal"),
    ("Clear", "eraser"), ("Hold", "hand"), ("Tap", "pointer"),
    ("Audio", "audio-lines"), ("Discord", "discord"), ("Roblox", "roblox"),
    ("Help", "circle-question-mark"),
]

CURVE_STEPS = 10   # enough for a 24-unit glyph at 32 px
ARC_STEPS = 16
UNIT = 16.0        # stored coordinates are sixteenths of an SVG unit

NUMBER = r"[-+]?(?:\d*\.\d+|\d+)(?:[eE][-+]?\d+)?"


def numbers(text):
    return [float(n) for n in re.findall(NUMBER, text)]


def cubic(p0, p1, p2, p3):
    out = []
    for i in range(1, CURVE_STEPS + 1):
        t = i / CURVE_STEPS
        u = 1 - t
        out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                    u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))
    return out


def signed_angle(ux, uy, vx, vy):
    return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)


def arc(p0, rx, ry, rotation, large, sweep, p1):
    """SVG endpoint arc to points, following the F.6.5 implementation notes."""
    if rx == 0 or ry == 0 or p0 == p1:
        return [p1]
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(rotation)
    cos_p, sin_p = math.cos(phi), math.sin(phi)
    dx, dy = (p0[0] - p1[0]) / 2, (p0[1] - p1[1]) / 2
    x1 = cos_p * dx + sin_p * dy
    y1 = -sin_p * dx + cos_p * dy
    oversize = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry)
    if oversize > 1:
        rx *= math.sqrt(oversize)
        ry *= math.sqrt(oversize)
    denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1
    numerator = max(0.0, rx * rx * ry * ry - denominator)
    coefficient = 0.0 if denominator == 0 else math.sqrt(numerator / denominator)
    if large == sweep:
        coefficient = -coefficient
    cx1 = coefficient * rx * y1 / ry
    cy1 = -coefficient * ry * x1 / rx
    cx = cos_p * cx1 - sin_p * cy1 + (p0[0] + p1[0]) / 2
    cy = sin_p * cx1 + cos_p * cy1 + (p0[1] + p1[1]) / 2
    start = signed_angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    span = signed_angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and span > 0:
        span -= 2 * math.pi
    elif sweep and span < 0:
        span += 2 * math.pi
    out = []
    for i in range(1, ARC_STEPS + 1):
        a = start + span * i / ARC_STEPS
        px, py = rx * math.cos(a), ry * math.sin(a)
        out.append((cos_p * px - sin_p * py + cx, sin_p * px + cos_p * py + cy))
    return out


def parse_path(d):
    """Return a list of (points, closed) for one path's d attribute.

    Arc flags are single digits that SVG allows packed against the next number
    ("a3.5 3.5 0 0018.5 8" is large=0, sweep=0, x=18.5), so they are read one
    character at a time instead of with the number regex.
    """
    position = 0
    length = len(d)

    def skip():
        nonlocal position
        while position < length and d[position] in ", \t\r\n":
            position += 1

    def read_number():
        nonlocal position
        skip()
        found = re.compile(NUMBER).match(d, position)
        if not found:
            raise SystemExit("expected a number at " + repr(d[position:position + 12]))
        position = found.end()
        return float(found.group())

    def read_flag():
        nonlocal position
        skip()
        if position >= length or d[position] not in "01":
            raise SystemExit("expected an arc flag at " + repr(d[position:position + 12]))
        position += 1
        return int(d[position - 1])

    def more_numbers():
        skip()
        return position < length and (d[position].isdigit() or d[position] in "+-.")

    subpaths, points = [], []
    cursor = start = (0.0, 0.0)
    command = None
    control = None
    while True:
        skip()
        if position >= length:
            break
        if d[position].isalpha():
            command = d[position]
            position += 1
        elif command is None:
            raise SystemExit("path data starts without a command: " + d)
        elif command in "Mm":
            command = "l" if command == "m" else "L"   # repeats after a moveto are linetos
        if command in "Zz":
            if points:
                subpaths.append((points, True))
                points = []
            cursor = start
            control = None
            continue
        relative = command.islower()
        letter = command.upper()
        if letter == "M":
            x, y = read_number(), read_number()
            if relative:
                x, y = cursor[0] + x, cursor[1] + y
            if points:
                subpaths.append((points, False))
            points = [(x, y)]
            cursor = start = (x, y)
        elif letter == "L":
            x, y = read_number(), read_number()
            if relative:
                x, y = cursor[0] + x, cursor[1] + y
            points.append((x, y))
            cursor = (x, y)
        elif letter == "H":
            x = read_number()
            if relative:
                x += cursor[0]
            points.append((x, cursor[1]))
            cursor = (x, cursor[1])
        elif letter == "V":
            y = read_number()
            if relative:
                y += cursor[1]
            points.append((cursor[0], y))
            cursor = (cursor[0], y)
        elif letter in ("C", "S"):
            if letter == "C":
                x1, y1 = read_number(), read_number()
            else:
                x1, y1 = ((2 * cursor[0] - control[0], 2 * cursor[1] - control[1])
                          if control else cursor)
            x2, y2 = read_number(), read_number()
            x, y = read_number(), read_number()
            if relative:
                if letter == "C":
                    x1, y1 = cursor[0] + x1, cursor[1] + y1
                x2, y2 = cursor[0] + x2, cursor[1] + y2
                x, y = cursor[0] + x, cursor[1] + y
            points.extend(cubic(cursor, (x1, y1), (x2, y2), (x, y)))
            control = (x2, y2)
            cursor = (x, y)
            continue
        elif letter == "A":
            rx, ry, rotation = read_number(), read_number(), read_number()
            large, sweep = read_flag(), read_flag()
            x, y = read_number(), read_number()
            if relative:
                x, y = cursor[0] + x, cursor[1] + y
            points.extend(arc(cursor, rx, ry, rotation, large, sweep, (x, y)))
            cursor = (x, y)
        else:
            raise SystemExit("unsupported path command " + repr(command) + " in " + d)
        control = None
        if not more_numbers():
            continue
    if points:
        subpaths.append((points, False))
    return subpaths


def ellipse(cx, cy, rx, ry, steps=24):
    return [(cx + rx * math.cos(2 * math.pi * i / steps),
             cy + ry * math.sin(2 * math.pi * i / steps)) for i in range(steps)]


def rounded_rect(x, y, w, h, rx, ry, steps=6):
    rx = rx or ry
    ry = ry or rx
    if rx <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    rx, ry = min(rx, w / 2), min(ry, h / 2)
    corners = ((x + w - rx, y + ry, -math.pi / 2), (x + w - rx, y + h - ry, 0.0),
               (x + rx, y + h - ry, math.pi / 2), (x + rx, y + ry, math.pi))
    points = []
    for cx, cy, a0 in corners:
        for i in range(steps + 1):
            a = a0 + (math.pi / 2) * i / steps
            points.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return points


def attribute(tag, name, default=None):
    found = re.search(name + r'="([^"]+)"', tag)
    if found:
        return float(found.group(1))
    if default is None:
        raise SystemExit("missing " + name + " in " + tag)
    return default


def shapes(svg):
    out = []
    for d in re.findall(r'<path[^>]*\sd="([^"]+)"', svg):
        out.extend(parse_path(d))
    for tag in re.findall(r"<circle[^>]*>", svg):
        r = attribute(tag, "r")
        out.append((ellipse(attribute(tag, "cx"), attribute(tag, "cy"), r, r), True))
    for tag in re.findall(r"<ellipse[^>]*>", svg):
        out.append((ellipse(attribute(tag, "cx"), attribute(tag, "cy"),
                            attribute(tag, "rx"), attribute(tag, "ry")), True))
    for tag in re.findall(r"<rect[^>]*>", svg):
        out.append((rounded_rect(attribute(tag, "x", 0.0), attribute(tag, "y", 0.0),
                                 attribute(tag, "width"), attribute(tag, "height"),
                                 attribute(tag, "rx", 0.0), attribute(tag, "ry", 0.0)), True))
    for tag in re.findall(r"<line[^>]*>", svg):
        out.append(([(attribute(tag, "x1"), attribute(tag, "y1")),
                     (attribute(tag, "x2"), attribute(tag, "y2"))], False))
    for kind, body in re.findall(r'<(polyline|polygon)[^>]*points="([^"]+)"', svg):
        values = numbers(body)
        out.append((list(zip(values[0::2], values[1::2])), kind == "polygon"))
    return out


def main():
    coordinates, paths, glyphs, missing = [], [], [], []
    for name, file in ICONS:
        source = SRC / (file + ".svg")
        if not source.exists():
            source = OWN / (file + ".svg")
        if not source.exists():
            missing.append(file)
            continue
        subpaths = shapes(source.read_text(encoding="utf-8"))
        if not subpaths:
            raise SystemExit(file + ": no geometry parsed")
        first = len(paths)
        for points, closed in subpaths:
            # Rounding can collapse short arc segments to zero length, and ImGui
            # widens the stroke at a zero-length segment. Drop repeated points,
            # including a closed path's final return to its start.
            rounded = []
            for x, y in points:
                point = (round(x * UNIT), round(y * UNIT))
                if not rounded or rounded[-1] != point:
                    rounded.append(point)
            if closed and len(rounded) > 1 and rounded[0] == rounded[-1]:
                rounded.pop()
            paths.append((len(coordinates) // 2, len(rounded), closed))
            for x, y in rounded:
                coordinates.extend((x, y))
        glyphs.append((name, file, first, len(paths) - first))
    if missing:
        raise SystemExit("missing SVGs: " + ", ".join(missing))

    out = []
    add = out.append
    add("#pragma once")
    add("")
    add("// Generated by tools/gen-icons.py from third_party/lucide/icons. Do not edit.")
    add("// Lucide v1.41.0, ISC, see third_party/lucide/LICENSE.")
    add("//")
    add("// Geometry is flattened to polylines on Lucide's 24 x 24 grid, in sixteenths")
    add("// of a unit, so the shell strokes points rather than interpreting SVG.")
    add("")
    add("namespace icon_data {")
    add("")
    add("struct Path { unsigned short first; unsigned short count; bool closed; };")
    add("struct Glyph { unsigned short first; unsigned short count; };")
    add("")
    add("inline constexpr float kGrid = 24.f;          // Lucide's viewBox")
    add("inline constexpr float kUnit = " + ("%.1f" % UNIT) + ";           // coordinate divisor")
    add("inline constexpr float kStrokeWidth = 2.f;    // Lucide's, in grid units")
    add("")
    add("inline constexpr short kPoints[] = {")
    for i in range(0, len(coordinates), 16):
        add("    " + ", ".join(str(v) for v in coordinates[i:i + 16]) + ",")
    add("};")
    add("")
    add("inline constexpr Path kPaths[] = {")
    for first, count, closed in paths:
        add("    {%d, %d, %s}," % (first, count, "true" if closed else "false"))
    add("};")
    add("")
    add("// Indexed by the Icon enum in ui/Panels.cpp, in its declared order.")
    add("inline constexpr Glyph kGlyphs[] = {")
    for name, file, first, count in glyphs:
        add("    {%d, %d},  // %s: lucide %s" % (first, count, name, file))
    add("};")
    add("")
    add("} // namespace icon_data")
    add("")
    OUT.write_text("\n".join(out), encoding="utf-8")
    print("%d glyphs, %d paths, %d points -> %s"
          % (len(glyphs), len(paths), len(coordinates) // 2, OUT.relative_to(ROOT)))


main()
