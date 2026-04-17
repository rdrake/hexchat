"""HexChat scripting interface.

User-facing Python package. Most names come straight from the
:mod:`_hexchat` C extension; the ``hexchat`` package layer exists to
add the ergonomic wrappers that are clearer to express in Python
than in C (iterator wrappers over :func:`get_list`,
:func:`emit_print` keyword handling, and so on — added as the rest
of the API lands).
"""

from _hexchat import *  # noqa: F401,F403 — intentional re-export.

__version__ = "3.0"
