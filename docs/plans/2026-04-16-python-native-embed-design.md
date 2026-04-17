# Python Plugin Runtime: Native CPython Embedding

**Status:** Design
**Date:** 2026-04-16
**Branch target:** `macos-gtk4-cleanup` (implementation in worktree `python-native-embed`)

## Summary

Replace the current CFFI-based Python plugin with a direct CPython C API
embedding, modeled on Blender's runtime structure. Same user-facing `hexchat`
module, new internals. Drops a build-time dependency on the `cffi` Python
package, removes the `generate_plugin.py` codegen step, and unblocks shipping
Python plugins in the macOS app bundle — which this branch currently cannot do.

The Python plugin becomes the reference implementation for a **shared
structural pattern** that Lua and Perl rewrites will follow: same file layout,
same hook dispatch discipline, same lifecycle conventions. No shared C runtime
— each language owns its own code — but the pattern is consistent enough that
a reader who knows one knows the shape of the others.

## Goals

- Replace CFFI embedding with hand-written CPython C API code.
- Preserve existing Python plugin scripts with no edits (subject to the
  compatibility bar below).
- Fix Python plugin support in the macOS `.app` bundle.
- Remove `cffi` and `pycparser` from the Flatpak manifest.
- Establish a structural pattern Lua and Perl rewrites can follow.

## Non-goals

- Shared C runtime across Python/Lua/Perl. Factoring three languages with
  different impedance into one "script host" layer has a poor history; we
  share structure, not code.
- API surface redesign. `hexchat.hook_command` et al. keep their signatures.
  Obvious typos (`prnt`) get cleaned up; anything ambiguous gets called out.
- Python 2 support. Target is CPython 3.8+ (the version where the `-embed`
  pkg-config variant landed and where `PyConfig` / `Py_InitializeFromConfig`
  became the recommended init path).

## Compatibility bar

- Documented API preserved exactly.
- Undocumented internals not preserved. In particular, anything that
  `import _hexchat_embedded` or reaches into CFFI will break. Scripts doing
  this were using implementation details.
- Obvious typos in the public API (`prnt` for `print`) are fixed. A compat
  alias costs nothing if a real-world script turns out to depend on it — we'll
  add it back when we find one, not speculatively.
- Migration note listing intentional breakage ships with the release.

## Reference: Blender

Blender's embedded Python runtime is the cleanest open-source example of
CPython-in-a-desktop-app done right. The structural choices we adopt from it:

- Ship (or use system) Python deliberately. Explicit `PYTHONHOME`, never let
  the interpreter guess.
- `PyConfig` + `Py_InitializeFromConfig` for interpreter startup, not the
  deprecated `Py_SetPythonHome` / `Py_Initialize` path.
- C extension registered via `PyImport_AppendInittab` *before*
  `Py_InitializeFromConfig`.
- `_hexchat` is the C module; `hexchat` is a Python package that wraps it.
  Work that is easier in Python (addon discovery, stdout buffering, error
  formatting) lives in Python.
- C sources split by subsystem, not one monolithic file.

Where we depart from Blender: Blender uses `PyConfig_InitPythonConfig` as the
base and then overrides specific fields. We use `PyConfig_InitIsolatedConfig`,
which is strictly stricter — no environment variables, no user site-packages,
no implicit path injection. HexChat is for running user scripts, not authoring
a platform; reproducibility matters more than flexibility, so the tighter
default is right for us.

Blender's surface API (class-based registration, `register()`/`unregister()`
conventions, property system) is *not* adopted. HexChat scripts have always
been flat modules with `__module_name__` globals and top-level hook calls.
That stays.

## Architecture

### File layout

```
plugins/python/
  meson.build                  # no more codegen custom_target
  python.def                   # unchanged, exports plugin_init/deinit
  src/
    hc_python.c                # hexchat_plugin_init/deinit, ph handle
    hc_python_interp.c         # PyConfig + interpreter lifecycle
    hc_python_interp.h
    hc_python_module.c         # PyInit__hexchat + method tables
    hc_python_module.h
    hc_python_hooks.c          # hook trampolines, owner bookkeeping
    hc_python_hooks.h
    hc_python_context.c        # hexchat_context PyObject wrapper
    hc_python_attrs.c          # hexchat_event_attrs PyObject wrapper
    hc_python_plugin.c         # per-script Plugin object
    hc_python_console.c        # /py command handler
    hc_python_compat.c         # xchat alias module, legacy shims
  py/
    hexchat/__init__.py        # re-exports from _hexchat, __version__
    hexchat/_loader.py         # addon discovery + import machinery
    hexchat/_stdio.py           # stdout/stderr buffered writer
    xchat/__init__.py          # `from hexchat import *`
```

Rough size: ~1,850 lines of C replacing the cffi-generated `python.c` plus
the 566-line `python.py` runtime. Line count goes up because we own every
line; readability and maintenance improve.

### C file responsibilities

**`hc_python.c`** — plugin entry point. Implements `hexchat_plugin_init` /
`hexchat_plugin_deinit`. Holds the `hexchat_plugin *ph` used by the rest of
the module. Registers the `/py` command. ~100 lines.

**`hc_python_interp.c`** — interpreter lifecycle. Populates `PyConfig`, calls
`PyImport_AppendInittab("_hexchat", PyInit__hexchat)`, calls
`Py_InitializeFromConfig`. Imports `hexchat` Python-side so the loader is
ready. Platform-specific home/path logic lives here. Provides `start()` and
`stop()`. ~200 lines.

**`hc_python_module.c`** — `PyInit__hexchat` plus the `PyMethodDef methods[]`
table. Every public function the user calls is a static `py_xxx` in this
file: `prnt`, `command`, `emit_print`, `get_info`, `get_prefs`, `nickcmp`,
`strip`, `find_context`, `set_context`, `pluginpref_*`, and the `hook_*`
entry points. Mechanically similar code throughout: `PyArg_ParseTuple`, call
HexChat C API, wrap result. Largest file, ~600 lines.

**`hc_python_hooks.c`** — hook infrastructure. Owns a list of
`struct py_hook { hexchat_hook *hh; PyObject *callable; PyObject *userdata;
plugin_t *owner; enum py_hook_kind kind; }`. Generic C trampolines convert
HexChat's `word[]` / `word_eol[]` / `hexchat_event_attrs` into Python tuples,
invoke the callable, map return value to `HEXCHAT_EAT_*`. Exposes
`py_hooks_release_for_plugin(plugin_t *)` for unload-time cleanup.

Unload ordering matters: `py_hooks_release_for_plugin` must call
`hexchat_unhook` + `Py_DECREF` on each owned hook's callable and userdata
*before* the loader removes the script's module from `sys.modules`. Hook
callables almost always close over module-level names, so the module stays
alive via the callable until the refcount actually drops. Getting this wrong
produces dangling hooks that fire against a half-torn-down module. ~400
lines.

**`hc_python_context.c`, `hc_python_attrs.c`** — custom `PyTypeObject`
wrappers for `hexchat_context *` and `hexchat_event_attrs *`. Context objects
compare by pointer identity; attrs exposes field getters. ~100 lines each.

**`hc_python_plugin.c`** — per-loaded-script object. Tracks filename, module
object, and the hooks the script owns. Exposes `plugin_load`,
`plugin_unload`, `plugin_reload`. On load, calls `hexchat_plugingui_add` so
the script appears in HexChat's Plugins and Scripts menu with its declared
name/version/description; on unload, calls `hexchat_plugingui_remove`.
~200 lines.

**`hc_python_console.c`** — `/py load | unload | reload | exec | console |
about` handler. Same surface as today. ~200 lines.

**`hc_python_compat.c`** — `xchat` alias registration, any intentional
compat shims. ~50 lines.

### Python-side package

**`hexchat/__init__.py`** — the user-facing module. Imports everything from
`_hexchat`; adds convenience helpers that are easier to express in Python
(e.g., `emit_print` kwargs handling, iterator wrappers over `get_list`).
Exports the `EAT_*` and `PRI_*` constants. ~100 lines.

**`hexchat/_loader.py`** — addon discovery. Scans
`~/.config/hexchat/addons/*.py` on startup. For each script: creates a module
via `importlib.util.spec_from_file_location`, execs it, reads
`__module_name__` / `__module_version__` / `__module_description__` /
`__module_author__`, registers with the C side via
`_hexchat._register_plugin(...)`. On unload: calls the optional
`__module_deinit__`, releases hooks the plugin owned (C side), removes the
module from `sys.modules`, drops the reference. Doing this in Python instead
of C is the single largest readability win in the design. ~150 lines.

**`hexchat/_stdio.py`** — buffered writer that flushes on newline via
`_hexchat.prnt`, assigned to `sys.stdout` / `sys.stderr`. ~50 lines.

**`xchat/__init__.py`** — `from hexchat import *`. Two lines.

**`_hexchat_embedded` is gone.** If a script imports it, the script breaks.
This is the single intentional compatibility hole. Anyone doing
`from _hexchat_embedded import ffi, lib` was using cffi's internals.

## Interpreter initialization

All platforms share:

```c
PyConfig config;
PyConfig_InitIsolatedConfig(&config);
config.isolated = 1;
config.use_environment = 0;
config.user_site_directory = 0;
config.site_import = 1;
config.utf8_mode = 1;
config.install_signal_handlers = 0;   /* HexChat owns SIGINT/SIGTERM */
config.filesystem_encoding = L"utf-8";
config.stdio_encoding = L"utf-8";
config.program_name = L"hexchat";
PyImport_AppendInittab("_hexchat", PyInit__hexchat);
/* per-platform: config.home, config.module_search_paths */
Py_InitializeFromConfig(&config);
```

`PyConfig_InitIsolatedConfig` is the key choice: default to ignoring the
environment and user site-packages. Opt back in only to what we want. Plugin
behavior becomes reproducible — the user's `PYTHONPATH` does not affect
whether their HexChat script loads.

### Per-platform specifics

**Linux.** `config.home` left unset; the interpreter discovers its prefix
from the linked `libpython3.x.so` location. `module_search_paths` is
`<prefix>/lib/python3.x`, `<prefix>/lib/python3.x/lib-dynload`, and
`~/.config/hexchat/addons`. Uses system Python. No change from today.

**Windows.** `config.home` computed relative to the plugin DLL's location at
load time via `GetModuleFileNameW` and parent-directory traversal. If Python
is bundled with the HexChat installer (recommended), a reduced `python313/`
tree ships next to `hexchat.exe`. If not bundled, fall back to the
`python313.dll` the linker resolved. `module_search_paths` is always
explicit. Removes the need for `cffi` at build time.

**macOS.** Bundle layout:

```
HexChat.app/Contents/
  MacOS/hexchat
  Frameworks/Python.framework/Versions/3.x/...
  Resources/lib/hexchat/plugins/python.so
  Resources/lib/hexchat/python/hexchat/__init__.py
```

At init, `hc_python_interp.c` uses `_NSGetExecutablePath` + `realpath` to
find the bundle root, then sets
`config.home = <bundle>/Contents/Frameworks/Python.framework/Versions/3.x`
and lists `module_search_paths` explicitly. `osx/makebundle.sh` changes from
"strip the python plugin" to "copy `Python.framework` into `Frameworks/`".
First time this branch ships a working Python plugin on macOS.

**Flatpak.** Uses the sandbox's system Python. Same code path as Linux.
`flatpak/python3-cffi.json` is deleted and its reference removed from
`flatpak/io.github.Hexchat.json`.

### User extensions and third-party packages

`~/.config/hexchat/addons/` is always on `sys.path`. Users can drop
`.py` scripts or package directories (`requests/`, `yaml/`, etc.) alongside
their addons. `pip install --target ~/.config/hexchat/addons/` works for
people who want pip-managed deps. System `site-packages` is not implicitly
on `sys.path` — matches Blender's "add-ons bring their own deps" discipline.

## Implementation plan

Work proceeds in a git worktree (`python-native-embed`) off
`macos-gtk4-cleanup`. Each step is a separate commit; each commit leaves the
tree buildable.

1. **Build system + empty C module.** New `plugins/python/src/` tree. Meson
   switches from `generate_plugin.py` custom_target to a normal
   `shared_module` of hand-written sources. `PyInit__hexchat` returns an
   empty module. `/py` does not work yet. Verification: `meson compile`
   green on Linux, plugin loads, `import _hexchat` succeeds in a debug
   interpreter.

2. **Interpreter lifecycle + exec harness.** `hc_python_interp.c` with the
   `PyConfig` dance, Linux-only paths. `hc_python_console.c` with `/py about`
   and a minimal `/py exec <code>` that compiles + evals code in a throwaway
   module against the (still mostly empty) `_hexchat` module. This harness
   is what makes step 3 commits independently verifiable without the full
   loader. Verification: loading the plugin starts the interpreter, `/py
   about` prints a version, `/py exec "1+1"` round-trips, unload/reload is
   clean under `valgrind --leak-check=full`.

3. **Core API surface.** Methods land in dependency order: `prnt` →
   `command` → `get_info` → `nickcmp` / `strip` → pluginpref family. Then
   `hook_command` + the `hc_python_hooks.c` trampoline (the most intricate
   piece). Then `hook_print`, `hook_server`, `hook_timer`, `hook_unload`,
   `unhook`. Then attrs variants. Each lands as its own commit. Per-commit
   verification uses `/py exec` from step 2: e.g., after the `hook_command`
   commit, `/py exec "hexchat.hook_command('foo', lambda w, we, ud: ...)"`
   registers, fires, and unloads cleanly. End-to-end plugin loading is
   deferred to step 4.

4. **Plugin lifecycle + loader.** `hc_python_plugin.c` + `hexchat/_loader.py`.
   Full `/py load | unload | reload | exec | console`. Plugin objects register
   with `hexchat_plugingui_add` on load. Verification: two or three
   nontrivial third-party plugins from the HexChat addons wiki load, appear
   in the Plugins menu, run end-to-end, and unload without leaks.

5. **Windows port.** `PyConfig.home` via `GetModuleFileNameW`, vcxproj
   updates, remove cffi from the build. Verification: build on Windows,
   load on Windows, run the test corpus.

6. **macOS port.** `_NSGetExecutablePath`-based home. `osx/makebundle.sh`
   changes from strip-the-plugin to copy-the-Framework. Verification: `.app`
   launches, Python plugin loads, test corpus runs. First time this has
   worked on this branch.

7. **Flatpak cleanup.** Delete `flatpak/python3-cffi.json`, drop the
   reference. Verification: Flatpak build green.

8. **Old tree removal.** Delete `plugins/python/python.py`, `_hexchat.py`,
   `generate_plugin.py`, `hexchat.py`, `xchat.py`. They have been replaced
   piece by piece.

## Compatibility testing

Before step 8, pick 4-6 real-world Python plugins from the HexChat addons
ecosystem and record results in a migration note: which work drop-in, which
need trivial edits, which hit intentional breakage. Candidates: an
away-status handler, a logger, a URL highlighter, a notifier. Ship the note
with the release.

## Rollback

Through step 7 the old tree still exists; `with-python=false` disables the
plugin. After step 8 we are committed — but the test corpus has run and a
release has shipped.

## Blueprint for Lua and Perl

Each language gets the same structural treatment, not shared code:

- `plugins/<lang>/src/` with files split by subsystem:
  `hc_<lang>.c`, `hc_<lang>_interp.c`, `hc_<lang>_module.c`,
  `hc_<lang>_hooks.c`, `hc_<lang>_plugin.c`, `hc_<lang>_console.c`.
- Hook ownership tracked per loaded script; unload releases cleanly.
- Language-specific stdlib shims in `<lang>/` data files where it makes
  sense (less in Lua, more in Perl).
- Same `/<lang> load | unload | reload | exec | console` command surface.

Lua and Perl rewrites are out of scope for this plan but inherit the
structure.

## Small decisions worth recording

- **Script compilation uses `optimize=0`** (the default). Today's runtime
  compiles user scripts with `optimize=2` (`python.py:114,119`), which
  strips docstrings and elides `assert` statements. Scripts that rely on
  side effects inside `assert` will silently not execute them. Matching the
  old behavior perpetuates a footgun; the new runtime defaults to
  unoptimized compilation, so `assert` works as scripts expect.
- **Environment-independent by design.** `use_environment = 0` disables
  `PYTHONPATH` / `PYTHONHOME` pickup. Users with conda or pyenv Pythons
  activated will find that HexChat does not pick up their environment —
  this is intentional (reproducibility > flexibility for a scripting host).
  Document in the migration note.

## Open questions

- **Python version pinning on Windows.** Blender ships a specific Python
  point release with each Blender release. We could do the same, or stick
  with whatever `hexchat.props` names (`python313` today). Defer; decide at
  step 5 based on what the Windows installer pipeline looks like then.
- **Subinterpreters.** CPython's PEP 684 per-interpreter GIL is interesting
  long-term for plugin isolation, but the API is unstable across 3.12/3.13
  and Blender does not use it. Not in scope.
