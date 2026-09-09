"""
progress.py - a status panel that redraws in place instead of scrolling.

The long-running tools here print for hours. An SPRT streams two lines per game
and a seven-line report block every ten; a 1000-iteration SPSA run prints a
21-parameter line per iteration. Almost none of that is read as it goes past -
it is the same handful of numbers over and over, and the lines that DO matter
end up buried under tens of thousands that do not.

So the split this module exists to enforce:

  * TRANSIENT state - counts, rates, the current estimate - is a panel that
    overwrites itself and occupies a fixed number of rows forever.
  * DURABLE events - a crashed engine, a restarted match, a final verdict - go
    through `log()` and scroll above the panel exactly as they always have.

Getting that backwards is the failure mode to avoid: a crash notice erased by
the next redraw is worse than the scrolling this replaces, so anything
unexpected keeps its old behaviour and only the repetition is collapsed.

NOT A TERMINAL, NOT A PANEL. Redirected output, a pipe, or a Windows console
that will not take VT sequences all fall back to printing the panel in full
every PLAIN_INTERVAL seconds, so `make sprt > run.log` and CI produce something
readable rather than a file full of cursor-movement escapes.

ASCII ONLY in anything drawn. A fresh Windows console is cp1252 and one box
character raises UnicodeEncodeError from inside a redraw - which would end an
eight-hour run over decoration.

STDLIB ONLY, like the rest of tools/ - see common.py.
"""

from __future__ import annotations

import atexit
import os
import re
import shutil
import sys
import time

CSI = "\x1b["

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# How often the non-terminal fallback repeats the panel. Long enough that eight
# hours of it is a readable log rather than a second scrolling problem.
PLAIN_INTERVAL = 60.0

# Redraws per second. The panels here are driven by events that arrive far
# faster than anyone reads (a game finishing, a batch of lines from fastchess),
# and repainting on every one of them is flicker plus wasted syscalls.
MAX_FPS = 5.0


# ------------------------------------------------------------ capability ----


def _enable_vt(stream) -> bool:
    """Turn on ENABLE_VIRTUAL_TERMINAL_PROCESSING for a Windows console.

    Returns False when it cannot be had, which is the signal to fall back:
    writing escapes at a console that does not honour them prints them
    literally, which is worse than the plain output it replaced.
    """
    if os.name != "nt":
        return True
    try:
        import ctypes

        kernel32 = ctypes.windll.kernel32
        # -11 is STD_OUTPUT_HANDLE, -12 STD_ERROR_HANDLE.
        handle = kernel32.GetStdHandle(-12 if stream is sys.stderr else -11)
        mode = ctypes.c_uint32()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))
    except Exception:
        return False


def supported(stream=None) -> bool:
    """Whether an in-place panel can be drawn on `stream`."""
    stream = stream or sys.stdout
    if os.environ.get("NO_LIVE"):
        return False
    try:
        if not stream.isatty():
            return False
    except Exception:
        return False
    return _enable_vt(stream)


# --------------------------------------------------------------- helpers ----


def visible_len(text: str) -> int:
    return len(_ANSI_RE.sub("", text))


def terminal_size() -> os.terminal_size:
    try:
        return shutil.get_terminal_size((100, 30))
    except Exception:
        return os.terminal_size((100, 30))


def fit_width(line: str, width: int | None = None) -> str:
    """Truncate to the terminal width.

    A line that wraps occupies a row the redraw does not know about, so the
    cursor-up count comes out short and the panel walks up the scrollback one
    row per frame. Every drawn line goes through here.
    """
    if width is None:
        width = max(20, terminal_size().columns - 1)
    if visible_len(line) <= width:
        return line
    return line[: max(0, width - 3)] + "..."


def fit_height(lines, rows: int | None = None) -> list[str]:
    """Trim a panel that is taller than the window, with a line saying so.

    Silently dropping rows would be the wrong answer for a 21-parameter tuning
    table on a short terminal: the reader needs to know the table continues.
    """
    lines = list(lines)
    if rows is None:
        rows = max(6, terminal_size().lines - 2)
    if len(lines) <= rows:
        return lines
    keep = max(1, rows - 1)
    hidden = len(lines) - keep
    return lines[:keep] + [f"  ... {hidden} more row(s) hidden - widen the window"]


def grid(cells, rows: int, width: int | None = None, gap: int = 3) -> list[str]:
    """Lay `cells` out in as few rows as will hold them, column-major.

    For a table that must show every row it has - a tuner's parameter list is
    the case this exists for: dropping half of it because the window is short
    defeats the point of watching a tuning run at all. Column-major so reading
    down a column follows the original order.

    Returns as many rows as it takes when even one column will not fit; the
    caller's alternative is hiding data, and a wrapped panel is the lesser
    problem.
    """
    cells = [str(x) for x in cells]
    if not cells:
        return []
    if width is None:
        width = max(20, terminal_size().columns - 1)
    rows = max(1, rows)

    columns = max(1, -(-len(cells) // rows))  # ceil: how many columns that needs
    cell_width = max(visible_len(x) for x in cells)
    # Shrink the column count until the row actually fits the window.
    while columns > 1 and columns * cell_width + (columns - 1) * gap > width:
        columns -= 1
    per_column = -(-len(cells) // columns)

    out = []
    for r in range(per_column):
        parts = []
        for col in range(columns):
            i = col * per_column + r
            if i < len(cells):
                parts.append(cells[i].ljust(cell_width))
        out.append((" " * gap).join(parts).rstrip())
    return out


def bar(fraction: float, width: int = 20) -> str:
    """`[####------]` for a 0..1 progress fraction."""
    fraction = min(1.0, max(0.0, fraction))
    filled = int(round(fraction * width))
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def span_bar(value: float, lo: float, hi: float, width: int = 20) -> str:
    """`[----|-----]`: where `value` sits between two bounds.

    For a quantity that travels towards either end rather than filling up - an
    LLR between its two Wald boundaries is the case this exists for, and a
    fill-up bar would misrepresent which way it is going.
    """
    if hi <= lo:
        return "[" + "-" * width + "]"
    frac = min(1.0, max(0.0, (value - lo) / (hi - lo)))
    pos = min(width - 1, int(frac * width))
    return "[" + "-" * pos + "|" + "-" * (width - pos - 1) + "]"


def hms(seconds) -> str:
    """`1:04:12`, or `4:12` under an hour. `?` when there is nothing to say -
    an ETA nobody can compute is not a number to invent."""
    if seconds is None or seconds != seconds or seconds < 0:
        return "?"
    total = int(seconds)
    hours, rem = divmod(total, 3600)
    minutes, secs = divmod(rem, 60)
    if hours:
        return f"{hours}:{minutes:02d}:{secs:02d}"
    return f"{minutes}:{secs:02d}"


# ------------------------------------------------------------------ live ----


class Live:
    """A block of lines that overwrites itself.

    Used as a context manager so the cursor is restored on the way out of a
    KeyboardInterrupt, which is how most of these runs actually end.
    """

    def __init__(self, stream=None, enabled=None, plain_interval: float = PLAIN_INTERVAL):
        self.stream = stream or sys.stdout
        self.enabled = supported(self.stream) if enabled is None else bool(enabled)
        self.plain_interval = plain_interval
        self._drawn = 0
        self._frame: list[str] = []
        self._last_draw = 0.0
        self._last_plain = 0.0
        self._hidden_cursor = False
        self._closed = False
        atexit.register(self.close)

    # -- plumbing ---------------------------------------------------------

    def _write(self, text: str) -> None:
        try:
            self.stream.write(text)
            self.stream.flush()
        except Exception:
            # A stream that went away must not take an eight-hour run with it.
            self.enabled = False

    def _erase(self) -> None:
        if self._drawn:
            self._write(f"{CSI}{self._drawn}A{CSI}J")
            self._drawn = 0

    # -- drawing ----------------------------------------------------------

    def update(self, lines, force: bool = False) -> None:
        """Replace the panel with `lines`.

        `force` skips the redraw throttle. It does NOT skip the plain-mode
        interval: a caller that wants an immediate frame on a terminal must not
        thereby write a full panel per iteration into a redirected log, which
        would reproduce the scrolling this replaces in the one place nobody is
        watching it happen.
        """
        now = time.time()
        if not self.enabled:
            self._plain(lines, now)
            return
        if not force and now - self._last_draw < 1.0 / MAX_FPS:
            self._frame = list(lines)  # keep it for the next draw or a log()
            return
        self._last_draw = now

        frame = [fit_width(line) for line in fit_height(lines)]
        if not self._hidden_cursor:
            self._write(f"{CSI}?25l")
            self._hidden_cursor = True

        out = []
        if self._drawn:
            out.append(f"{CSI}{self._drawn}A")
        for line in frame:
            out.append(f"\r{CSI}2K{line}\n")
        # A frame shorter than the last leaves the tail of the old one on
        # screen; clear those rows and step back over them.
        extra = self._drawn - len(frame)
        for _ in range(max(0, extra)):
            out.append(f"\r{CSI}2K\n")
        if extra > 0:
            out.append(f"{CSI}{extra}A")

        self._drawn = len(frame)
        self._frame = list(lines)
        self._write("".join(out))

    def _plain(self, lines, now: float, final: bool = False) -> None:
        self._frame = list(lines)
        if not final and now - self._last_plain < self.plain_interval:
            return
        self._last_plain = now
        self._write("\n".join(str(line) for line in lines) + "\n\n")

    def log(self, text: str) -> None:
        """A line that must survive every later redraw."""
        if not self.enabled:
            self._write(text.rstrip("\n") + "\n")
            return
        self._erase()
        self._write(text.rstrip("\n") + "\n")
        if self._frame:
            self.update(self._frame, force=True)

    def close(self, clear: bool = False) -> None:
        """Leave the last frame on screen (or erase it) and restore the cursor."""
        if self._closed:
            return
        self._closed = True
        if not self.enabled:
            return
        if clear:
            self._erase()
        if self._hidden_cursor:
            self._write(f"{CSI}?25h")
            self._hidden_cursor = False

    def finish(self, lines=None) -> None:
        """Draw a last frame unthrottled, then release the terminal.

        The throttle in update() means the frame on screen can be up to a fifth
        of a second stale, which for a final tally is simply wrong. This is
        also the one plain-mode print that ignores the interval, so a redirected
        run still ends with the totals in its log.
        """
        frame = list(lines) if lines is not None else self._frame
        if frame:
            if self.enabled:
                self.update(frame, force=True)
            else:
                self._plain(frame, time.time(), final=True)
        self.close()

    def __enter__(self) -> "Live":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
