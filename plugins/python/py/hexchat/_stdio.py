"""Route ``sys.stdout`` / ``sys.stderr`` through ``_hexchat.print``.

Embedded interpreters have no default TTY: anything a script writes
to stdout would otherwise either go to the terminal hexchat was
launched from or be silently dropped. This module swaps both streams
for a line-buffered writer that forwards completed lines to
:func:`_hexchat.print`, so ``print('hi')`` lands in the user's
current HexChat window like every other plugin output.

Only whole lines are flushed automatically; partial writes buffer
until a newline arrives or ``flush()`` is called. That keeps
multi-write sequences like ``sys.stdout.write('foo'); sys.stdout.write('bar\\n')``
from rendering as two separate HexChat lines.
"""

from __future__ import annotations

import sys

import _hexchat


class _HexchatWriter:
    """File-like object that forwards whole lines to HexChat."""

    def __init__(self) -> None:
        self._buf = ""

    def write(self, text: str) -> int:
        if not text:
            return 0
        self._buf += text
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            _hexchat.print(line)
        return len(text)

    def flush(self) -> None:
        if self._buf:
            _hexchat.print(self._buf)
            self._buf = ""

    # The rest of the io.IOBase surface that Python expects.
    def isatty(self) -> bool:
        return False

    def writable(self) -> bool:
        return True

    def readable(self) -> bool:
        return False

    def seekable(self) -> bool:
        return False


def install() -> None:
    """Swap sys.stdout and sys.stderr for HexChat-routed writers.

    Called automatically from :mod:`hexchat` on first import; exposed
    as a top-level function so tests (and scripts that want to
    temporarily restore the original streams) can invoke it directly.
    """
    sys.stdout = _HexchatWriter()
    sys.stderr = _HexchatWriter()
