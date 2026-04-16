# macOS App Bundle Plan

Status: in progress on `macos-gtk4-cleanup`
Target: arm64-only HexChat.app built from Homebrew GTK4 via
[`gtk-mac-bundler`](https://github.com/GNOME/gtk-mac-bundler).

## Goals

- One command from a clean tree produces `builddir/osx/HexChat.app`,
  ad-hoc signed, that launches by double-click.
- The same command with env vars set produces a Developer ID–signed and
  notarized `HexChat-<ver>.dmg`. No code changes needed to flip that on.
- Bundle is GTK4-correct: libraries relocated under
  `Contents/Resources/lib`, GIO modules, GDK-pixbuf loaders, GTK IM
  modules, print backends, GLib schemas, Adwaita icon theme, and
  translations all shipped.
- Heavy relocation work is delegated to `gtk-mac-bundler`. We do not
  reimplement `install_name_tool` gymnastics.

## Non-goals

- No Intel or universal binary. arm64 only.
- No scripting-runtime bundling in v1. `lua`, `perl`, and `python`
  plugins are out. `checksum`, `fishlim`, `sasl`, and `sysinfo` are in.
- No App Store distribution, no sandboxing. Developer ID only.
- No jhbuild. `/opt/homebrew` is assumed.
- No Sparkle or auto-update.

## Shape

```
osx/
├── hexchat.bundle          # gtk-mac-bundler manifest, GTK4 + Homebrew
├── launcher.sh             # runtime env setup, execs the real binary
├── makebundle.sh           # pipeline: stage → bundler → sign → dmg
├── Info.plist.in           # existing, Meson-processed for @VERSION@
├── entitlements.plist      # hardened runtime entitlements
└── hexchat.icns            # existing

builddir/osx/               # all outputs, gitignored
├── Info.plist              # configured from Info.plist.in
├── staging/                # `meson install --destdir` target
├── HexChat.app/            # gtk-mac-bundler output
└── HexChat-<ver>.dmg       # optional final artifact
```

## Bundle manifest

`osx/hexchat.bundle` declares two prefixes:

- `default` → `/opt/homebrew` (GTK4, GLib, pango, cairo, gdk-pixbuf,
  glib-networking, and transitive deps).
- `hexchat` → `builddir/osx/staging/opt/homebrew` (our freshly installed
  binary, plugins, and data).

Bundler walks the main binary's dependency graph, copies each dylib
into `Contents/Resources/lib`, and rewrites install names to
`@executable_path/../Resources/lib/...`.

Gone from the GTK2-era manifest: `gtk+-2.0`, theme engines
(`libquartz.so`, `libxamarin.so`), enchant-applespell, pango modules
(pango ≥ 1.44 is stateless), GTK2 `gtkrc`.

Added for GTK4:

- `gdk-pixbuf-2.0/*/loaders/*.so` and `loaders.cache`
- `gio/modules/*.so` (glib-networking for TLS)
- `gtk-4.0/*/immodules/*.so`
- `gtk-4.0/*/printbackends/*.so`
- `share/glib-2.0/schemas/gschemas.compiled`
- `share/icons/Adwaita` (GTK4 icon fallback)

## Pipeline

`osx/makebundle.sh`:

1. `rm -rf builddir/osx/{staging,HexChat.app,*.dmg}`
2. `meson install -C builddir --destdir "$PWD/builddir/osx/staging"`
3. `gtk-mac-bundler osx/hexchat.bundle`
4. `codesign --force --deep --options runtime
   --entitlements osx/entitlements.plist --sign "$CODESIGN_IDENTITY"
   builddir/osx/HexChat.app`
5. If `NOTARY_PROFILE` is set: `xcrun notarytool submit --wait` then
   `xcrun stapler staple`.
6. Unless `BUNDLE_SKIP_DMG` is set: `hdiutil create -format UDZO` a
   `HexChat-<ver>.dmg` with an Applications symlink.

Environment variables:

- `CODESIGN_IDENTITY` — defaults to `-` (ad-hoc). Set to the
  `Developer ID Application: …` string for real signing.
- `NOTARY_PROFILE` — keychain profile name for `notarytool`. Unset
  skips notarization.
- `BUNDLE_SKIP_DMG` — any non-empty value skips the DMG step.

## Meson integration

Gated on `host_machine.system() == 'darwin'`:

- `configure_file` processes `osx/Info.plist.in` → `builddir/osx/Info.plist`.
- `custom_target('bundle')` invokes `osx/makebundle.sh`. Not built by
  default. Depends on the hexchat executable target so a stale binary
  isn't bundled.

Invocation: `meson compile -C builddir bundle`.

## Hardened runtime entitlements

`osx/entitlements.plist` carries two entitlements:

- `com.apple.security.cs.allow-jit` — pango/cairo take JIT code paths
  in some glyph caches.
- `com.apple.security.cs.disable-library-validation` — lets the
  embedded GTK dylibs load under hardened runtime even though they
  weren't signed by the same team at build time.

## Lessons learned (implementation notes)

### Homebrew opt/ vs Cellar/ duplication

Homebrew ships some dylibs with install names referencing
`/opt/homebrew/opt/<pkg>/lib/...` (stable symlink) and others
referencing `/opt/homebrew/Cellar/<pkg>/<ver>/lib/...` (versioned). The
bundler dutifully follows both chains and copies the same library
twice. When pango or glib load twice, GObject type registration panics
with "cannot register existing type 'PangoFontFamily'".

`makebundle.sh` runs a dedupe pass after the bundler: for every
`Cellar/<pkg>/<ver>/lib/<X>.dylib` that also exists as
`opt/<pkg>/lib/<X>.dylib`, rewrite every Mach-O consumer with
`install_name_tool -change` and delete the Cellar copy.

### Plugin suffix

`src/common/plugin.h` defines `PLUGIN_SUFFIX = "dylib"` on macOS, so
the manifest globs `plugins/*.dylib`, not `*.so`.

### Scripting runtimes skipped in v1

Meson builds `lua.dylib`, `perl.dylib`, `python.dylib` by default.
LuaJIT is self-contained so `lua.dylib` ships. Python and Perl need
their interpreter stdlibs (PYTHONHOME, `@INC`) to be useful, so
`makebundle.sh` deletes those plugins from the staging prefix before
the bundler runs, keeping the bundle small and avoiding broken plugin
loads.

### GSettings schemas

Point `<data>` at the schemas *directory* (`share/glib-2.0/schemas`),
not the single `gschemas.compiled` file. Bundler's `<data>` tag has a
subtle bug on lone files. After the directory copy, run
`glib-compile-schemas` inside the bundle to recompile against the
subset that actually shipped.

### Translations from non-default prefix

`<translations>` only understands the default prefix. HexChat's own
`.mo` files live under the staging prefix, so `makebundle.sh` walks
them in by hand after the bundler finishes.

## Plugin follow-ups (out of v1)

- `python` plugin: bundle Homebrew's `python@3.x` framework, set
  `PYTHONHOME` inside launcher.sh, codesign the embedded framework.
- `perl` plugin: relocate `@INC` into the bundle, bundle the Perl
  runtime + CORE, resign.
