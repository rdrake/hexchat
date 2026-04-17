# macOS App Bundle and Python Bundling Plan

**Status:** Design
**Date:** 2026-04-17
**Primary baseline branch:** `macos-gtk4-cleanup`
**Python worktree:** `worktree-python-native-embed`

## Summary

Deliver a proper runnable macOS HexChat launch path first, then extend that
into a proper `HexChat.app` bundle, and only then add the Python plugin
runtime. The baseline app work should happen in a separate worktree based on
the main repository branch that already owns the macOS GTK4 modernization
work. The current `python-native-embed` worktree should stay focused on
Python-specific runtime and packaging changes.

This separates two concerns that have different failure modes:

- generic macOS app launch, bundling, codesigning, and GTK runtime setup
- Python interpreter and plugin packaging inside the macOS app bundle

## Goals

- Produce a macOS HexChat launch path that stays running outside the Codex
  sandbox, first as the plain frontend binary and then as a proper
  `HexChat.app`.
- Keep generic macOS bundle fixes isolated from Python runtime work.
- Reuse the existing `osx/` bundle machinery already present in the repo.
- Make the Python worktree consume a known-good app bundle foundation instead
  of solving macOS bundle and Python packaging at the same time.

## Non-goals

- Fixing sandbox-only GUI crashes inside the Codex sandbox. That environment is
  not the production launch target.
- Bundling Python in the baseline app phase.
- Large startup-sequence refactors unrelated to getting a proper app bundle
  launching cleanly.

## Current facts

- `./builddir/src/fe-gtk/hexchat` crashes immediately when launched inside the
  Codex sandbox because sandboxed macOS GUI execution yields an invalid GTK
  window frame before HexChat-specific logic matters.
- Outside the Codex sandbox, the same binary currently launches and then exits
  immediately before any visible window remains on screen.
- `/opt/homebrew/bin/gtk4-demo` shows the same sandbox-only failure, which
  isolates the immediate crash to sandboxed GTK/macOS execution.
- Outside the Codex sandbox, HexChat currently emits GTK criticals during
  application startup and exits cleanly rather than staying open.
- The strongest current hypothesis for the non-sandbox exit is an application
  lifecycle bug around `GtkApplication` usage. HexChat currently calls
  `gtk_init()` manually before constructing `GtkApplication`, even though GTK
  documentation says that when using `GtkApplication`, `gtk_init()` is normally
  performed by the default `GApplication::startup` handler.
- The repository already contains macOS bundling assets under `osx/`:
  `Info.plist.in`, `launcher.sh`, `hexchat.bundle`, `makebundle.sh`, and a
  Meson `bundle` target.
- The main repository checkout is currently on branch `macos-gtk4-cleanup`.
- The Python work is already isolated in a separate worktree,
  `worktree-python-native-embed`.

## Recommended approach

Use a two-phase workflow with separate worktrees.

### Phase 1: baseline macOS launch path and app bundle

Create a dedicated worktree from the main repository branch
`macos-gtk4-cleanup` and finish the proper macOS app bundle there. The target
for this phase is:

- `./builddir/src/fe-gtk/hexchat` stays running outside the Codex sandbox
- the initial UI can be shown without the process immediately exiting
- `meson compile -C builddir bundle` succeeds
- `builddir/osx/HexChat.app` launches via Finder or `open`
- the app can open its initial UI without requiring a terminal wrapper
- the bundled runtime resolves GTK resources, pixbuf loaders, GIO modules,
  schemas, and HexChat resources correctly

Any fixes made in this phase should be generic macOS app fixes only. Examples:

- bundle assembly problems
- launcher environment fixes
- missing resources in the app bundle
- codesign and verification issues
- branch-local startup issues that prevent a normal app launch

### Phase 2: Python-capable app bundle

Bring the Phase 1 commits into `worktree-python-native-embed` by rebase or
cherry-pick, then add the Python-specific bundle/runtime changes in that
worktree.

The target for this phase is:

- the same `HexChat.app` bundle still launches cleanly
- the Python plugin is present in the bundle
- the embedded or bundled Python runtime resolves its home, stdlib, and module
  search paths from inside the app
- Python addons can be loaded from the expected HexChat addons location

## Alternatives considered

### Option 1: do everything in the Python worktree

This is faster in the very short term but mixes generic macOS app work with
Python packaging. It raises the risk that useful baseline bundle fixes become
harder to upstream or review independently.

### Option 2: baseline app only, then rebase Python work later

This is close to the recommended approach, but phrased as a single-branch
sequence. It is workable, but the actual repository state already benefits from
keeping the Python work in its own worktree while the baseline app bundle is
stabilized elsewhere.

### Option 3: recommended, separate worktrees and phased integration

This keeps the history clear:

- baseline macOS app work belongs with the macOS modernization branch
- Python bundle work belongs with the Python embedding branch

It is slightly more coordination up front, but it produces the cleanest branch
history and lowest review friction.

## Architecture and data flow

### Build and bundle flow

Phase 1 uses the existing path, but only after stabilizing the raw binary
launch:

1. Meson builds the GTK frontend binary.
2. The raw binary launch path is stabilized so the process stays alive outside
   the Codex sandbox.
3. The `bundle` custom target stages `meson install` output.
4. `osx/makebundle.sh` runs `gtk-mac-bundler`, copies app resources, compiles
   schema caches, and codesigns the resulting app.
5. `osx/launcher.sh` provides the runtime environment the bundled app needs.
6. The result is `builddir/osx/HexChat.app`.

### Integration flow between phases

1. Stabilize `HexChat.app` on `macos-gtk4-cleanup`.
2. First verify the raw frontend binary stays running outside the sandbox.
3. Then verify the app bundle launches outside the sandbox.
4. Import those commits into `worktree-python-native-embed`.
5. Extend bundle contents and launcher/runtime logic for Python.
6. Re-run app bundle verification with Python enabled.

## Error handling and debugging strategy

Phase 1 debugging should focus on baseline app-launch failures only:

- raw binary startup and lifecycle failures
- bundle build failures
- missing dylibs or resources
- launcher environment mistakes
- codesign or notarization issues, if they block launch

Phase 2 debugging should focus on Python-specific failures only:

- plugin not found in bundle
- interpreter initialization failure
- incorrect `PYTHONHOME` or module search paths
- missing stdlib or extension modules
- plugin import failures at startup

This split is important because a broken baseline app bundle makes every Python
failure harder to reason about.

## Testing and verification

### Phase 1 verification

- `meson compile -C builddir`
- launch `./builddir/src/fe-gtk/hexchat` outside the sandbox
- confirm the process does not immediately exit
- confirm the main UI or server list appears
- `meson compile -C builddir bundle`
- launch `builddir/osx/HexChat.app` outside the sandbox
- confirm the main UI or server list appears and the app stays running
- confirm the app resolves bundled GTK resources and does not depend on a
  terminal launch environment

### Phase 2 verification

- repeat all Phase 1 verification
- confirm the Python plugin binary is bundled
- confirm the bundled app can initialize Python
- confirm a simple Python addon loads successfully
- confirm Python path resolution uses the app bundle, not ambient shell state

## Implementation boundaries

Files likely owned by Phase 1:

- `osx/meson.build`
- `osx/makebundle.sh`
- `osx/launcher.sh`
- `osx/Info.plist.in`
- other generic macOS bundle assets if required

Files likely owned by Phase 2:

- `plugins/python/meson.build`
- `plugins/python/src/*`
- Python-related bundle staging additions in `osx/`
- any launcher/runtime changes required specifically for Python home/path setup

## Open decisions

- Whether the baseline branch should remain `macos-gtk4-cleanup` or be renamed
  later to match broader macOS modernization work. This does not block Phase 1.
- Whether Phase 1 needs any startup-sequence cleanup for `GtkApplication`
  warnings seen during external launch. Those warnings are secondary unless they
  block a stable app launch.

## Success criteria

The work is successful when:

- a proper `HexChat.app` launches from the baseline macOS branch
- the Python worktree can adopt that baseline without re-solving generic macOS
  launch issues
- the final bundled app supports Python plugins without relying on external
  shell environment configuration
