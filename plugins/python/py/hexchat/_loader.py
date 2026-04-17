"""Load, unload, and reload HexChat Python scripts.

The loader holds a registry of every script that's currently live
and is the single point through which the :mod:`_hexchat` C module
learns who owns what. Doing this in Python rather than C buys three
things: less C boilerplate, clearer error messages, and easy
introspection through familiar :mod:`importlib` machinery.

Lifecycle of a single load:

1. The loader receives a filesystem path (absolute, or relative to
   the addons directory).
2. An :mod:`importlib` spec is created from that path and a fresh
   module object materialises; the script's globals never pollute
   :data:`sys.modules` before the load completes.
3. A handle is obtained from ``_hexchat._register_plugin`` — the
   handle wraps the C-side plugin record and becomes the attribution
   target for every hook the script will register.
4. The handle is pushed onto the active-plugin stack. The module's
   code executes; any ``hexchat.hook_*`` calls it makes attribute
   their hooks to this plugin.
5. On exec success, :func:`_read_metadata` extracts
   ``__module_name__`` and friends and reports them to C. The module
   is cached under its reported name, then :data:`sys.modules` gets
   a stable key pointing at the same object.
6. If exec raises, every step 3+ side-effect is rolled back and the
   exception is re-raised so the caller can surface it.

Unload mirrors the sequence in reverse. Hook-owner attribution
(set up in C by the active-plugin stack) is the only reason
``_hexchat._release_plugin_hooks`` knows which hooks to tear down.

This module is not intended to be imported by user scripts; the
public face of addon management is the ``/py`` command.
"""

from __future__ import annotations

import importlib.util
import os
import sys
import traceback
from dataclasses import dataclass
from typing import Any, Iterable

import _hexchat

_MODULE_ATTRIBUTES: tuple[tuple[str, str], ...] = (
    ("name", "__module_name__"),
    ("version", "__module_version__"),
    ("description", "__module_description__"),
    ("author", "__module_author__"),
)


@dataclass
class _Entry:
    """Bookkeeping for a single loaded script."""

    handle: Any  # PyCapsule from _hexchat._register_plugin
    module: Any
    filename: str
    name: str  # the name we registered under sys.modules


# Key is the absolute canonical path to the script on disk. Keeping the
# path as the primary key means two scripts that happen to declare the
# same __module_name__ can still be told apart; user-visible commands
# look up by name first but fall back to path.
_loaded: dict[str, _Entry] = {}


def _absolute(path: str) -> str:
    return os.path.realpath(os.path.expanduser(path))


def _read_metadata(module: Any) -> dict[str, str | None]:
    """Pull __module_name__ / version / description / author off a module."""
    out: dict[str, str | None] = {}
    for key, dunder in _MODULE_ATTRIBUTES:
        value = getattr(module, dunder, None)
        out[key] = str(value) if value is not None else None
    return out


def _module_spec(filename: str) -> Any:
    # A unique synthetic module name so two scripts with the same
    # filename basename don't collide in sys.modules.
    mod_name = f"hexchat.addons.{os.path.splitext(os.path.basename(filename))[0]}"
    spec = importlib.util.spec_from_file_location(mod_name, filename)
    if spec is None:
        raise ImportError(f"could not build a spec for {filename!r}")
    return spec


def load(filename: str) -> str:
    """Load the script at ``filename``.

    Returns the registered name (``__module_name__`` if the script
    sets one, otherwise a sanitised filename stem). Raises on any
    error; the loader leaves no partial state behind.
    """
    abspath = _absolute(filename)
    if abspath in _loaded:
        raise ValueError(f"{abspath!r} is already loaded")

    spec = _module_spec(abspath)
    module = importlib.util.module_from_spec(spec)

    handle = _hexchat._register_plugin(abspath, module)
    registered_in_sysmodules: str | None = None

    try:
        _hexchat._push_active_plugin(handle)
        try:
            assert spec.loader is not None
            spec.loader.exec_module(module)
        finally:
            _hexchat._pop_active_plugin()

        metadata = _read_metadata(module)
        reported_name = metadata["name"] or spec.name
        _hexchat._set_metadata(
            handle,
            reported_name,
            metadata["version"],
            metadata["description"],
            metadata["author"],
        )

        # Cache under the spec's unique synthetic name so the module
        # object is reachable via the standard import machinery if
        # anything else wants it.
        sys.modules[spec.name] = module
        registered_in_sysmodules = spec.name

        _loaded[abspath] = _Entry(
            handle=handle,
            module=module,
            filename=abspath,
            name=reported_name,
        )
        return reported_name
    except BaseException:
        # Roll back everything we did up to this point.
        if registered_in_sysmodules is not None:
            sys.modules.pop(registered_in_sysmodules, None)
        _hexchat._release_plugin_hooks(handle)
        _hexchat._unregister_plugin(handle)
        raise


def _unload_entry(entry: _Entry) -> None:
    """Tear down a single loaded script."""
    try:
        _hexchat._push_active_plugin(entry.handle)
        try:
            _hexchat._fire_unload_for_plugin(entry.handle)

            deinit = getattr(entry.module, "__module_deinit__", None)
            if callable(deinit):
                try:
                    deinit()
                except BaseException:
                    # A script's __module_deinit__ blowing up must not
                    # stop us from releasing its hooks. Log, keep going.
                    traceback.print_exc()
        finally:
            _hexchat._pop_active_plugin()

        # Hooks first — DECREFing the callables typically drops the
        # last reference to the module's globals dict. Doing it while
        # the active-plugin stack is empty keeps attribution clean
        # for any hooks the DECREF itself might trigger.
        _hexchat._release_plugin_hooks(entry.handle)

        spec_name = f"hexchat.addons.{os.path.splitext(os.path.basename(entry.filename))[0]}"
        sys.modules.pop(spec_name, None)
    finally:
        _hexchat._unregister_plugin(entry.handle)


def unload(identifier: str) -> bool:
    """Unload a script by path, absolute or relative, or by registered name."""
    entry = _find(identifier)
    if entry is None:
        return False
    _unload_entry(entry)
    _loaded.pop(entry.filename, None)
    return True


def reload(identifier: str) -> bool:
    """Unload then load the same file. Uses the original path so the
    reload tracks script source moves in the obvious way."""
    entry = _find(identifier)
    if entry is None:
        return False
    path = entry.filename
    _unload_entry(entry)
    _loaded.pop(path, None)
    load(path)
    return True


def _find(identifier: str) -> _Entry | None:
    abspath = _absolute(identifier)
    entry = _loaded.get(abspath)
    if entry is not None:
        return entry
    for candidate in _loaded.values():
        if candidate.name == identifier:
            return candidate
    return None


def loaded() -> list[_Entry]:
    """Return a snapshot of currently loaded plugins."""
    return list(_loaded.values())


def unload_all() -> None:
    """Tear down every loaded script in reverse load order. Meant to
    run during interpreter shutdown, not as a user-facing command."""
    for entry in reversed(list(_loaded.values())):
        try:
            _unload_entry(entry)
        except BaseException:
            traceback.print_exc()
    _loaded.clear()


def autoload(directory: str) -> Iterable[str]:
    """Load every ``*.py`` file under ``directory``.

    Files that fail to load leave a traceback in the HexChat window
    but do not abort the rest of the autoload sweep — one bad addon
    shouldn't take down the others.
    """
    loaded_names: list[str] = []
    if not os.path.isdir(directory):
        return loaded_names
    for entry in sorted(os.listdir(directory)):
        if not entry.endswith(".py") or entry.startswith("_"):
            continue
        path = os.path.join(directory, entry)
        try:
            loaded_names.append(load(path))
        except BaseException:
            traceback.print_exc()
    return loaded_names
