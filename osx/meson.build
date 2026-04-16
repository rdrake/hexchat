# macOS app bundle assembly.
#
# This subdir is only entered on Darwin. It does two things:
#   1. Configures Info.plist.in → builddir/osx/Info.plist with the project
#      version.
#   2. Registers a `bundle` custom target that invokes osx/makebundle.sh,
#      which stages a meson install, runs gtk-mac-bundler, codesigns, and
#      optionally notarizes + builds a DMG.
#
# Build the bundle with:
#   meson compile -C builddir bundle

plist_data = configuration_data()
plist_data.set('VERSION', meson.project_version())

configure_file(
  input: 'Info.plist.in',
  output: 'Info.plist',
  configuration: plist_data,
)

bundle_script = files('makebundle.sh')

custom_target('bundle',
  output: 'HexChat.app',
  command: [
    'env',
    'HEXCHAT_SOURCE_DIR=' + meson.project_source_root(),
    'HEXCHAT_BUILD_DIR=' + meson.project_build_root(),
    'HEXCHAT_VERSION=' + meson.project_version(),
    bundle_script,
  ],
  console: true,
  build_by_default: false,
  build_always_stale: true,
)
