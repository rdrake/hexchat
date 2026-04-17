"""Compatibility alias: ``import xchat`` maps to :mod:`hexchat`.

HexChat was forked from XChat and a long tail of scripts still import
the old name. The alias is deliberately thin — no shims, just a
re-export — so anything that works against ``hexchat`` works against
``xchat`` and vice versa.
"""

from hexchat import *  # noqa: F401,F403
