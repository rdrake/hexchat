#!/bin/sh
#
# Build a HexChat.app bundle from a Meson build dir.
#
# Pipeline:
#   1. meson install --destdir=<stage>      (fresh hexchat binary + data)
#   2. gtk-mac-bundler osx/hexchat.bundle   (relocate GTK4 + deps)
#   3. codesign                             (ad-hoc by default)
#   4. xcrun notarytool + stapler           (if NOTARY_PROFILE set)
#   5. hdiutil create                       (unless BUNDLE_SKIP_DMG set)
#
# Environment:
#   HEXCHAT_SOURCE_DIR   (required) absolute path to repo root
#   HEXCHAT_BUILD_DIR    (required) absolute path to meson build dir
#   HEXCHAT_VERSION      (required) version string for DMG filename
#   CODESIGN_IDENTITY    optional, defaults to "-" (ad-hoc)
#   NOTARY_PROFILE       optional, keychain profile name for notarytool
#   BUNDLE_SKIP_DMG      optional, non-empty skips DMG step
#   GTK_MAC_BUNDLER      optional, path to gtk-mac-bundler binary

set -eu

: "${HEXCHAT_SOURCE_DIR:?must be set}"
: "${HEXCHAT_BUILD_DIR:?must be set}"
: "${HEXCHAT_VERSION:?must be set}"

CODESIGN_IDENTITY=${CODESIGN_IDENTITY:--}
GTK_MAC_BUNDLER=${GTK_MAC_BUNDLER:-$HOME/.local/bin/gtk-mac-bundler}

if [ ! -x "$GTK_MAC_BUNDLER" ]; then
    echo "error: gtk-mac-bundler not found at $GTK_MAC_BUNDLER" >&2
    echo "       install from https://gitlab.gnome.org/GNOME/gtk-mac-bundler" >&2
    exit 1
fi

BUNDLE_DEST="$HEXCHAT_BUILD_DIR/osx"
STAGE_ROOT="$BUNDLE_DEST/staging"
STAGE_PREFIX="$STAGE_ROOT/opt/homebrew"
BUNDLE_PLIST="$BUNDLE_DEST/Info.plist"
APP_PATH="$BUNDLE_DEST/HexChat.app"
DMG_PATH="$BUNDLE_DEST/HexChat-$HEXCHAT_VERSION.dmg"

echo "==> Cleaning previous bundle outputs"
rm -rf "$STAGE_ROOT" "$APP_PATH" "$DMG_PATH"
mkdir -p "$BUNDLE_DEST"

echo "==> Staging meson install into $STAGE_ROOT"
# Use --destdir with a prefix of /opt/homebrew (Meson was configured against
# that prefix). Everything lands at $STAGE_ROOT/opt/homebrew/...
DESTDIR="$STAGE_ROOT" meson install -C "$HEXCHAT_BUILD_DIR" --skip-subprojects --no-rebuild --quiet

if [ ! -x "$STAGE_PREFIX/bin/hexchat" ]; then
    echo "error: $STAGE_PREFIX/bin/hexchat not found after staging install" >&2
    echo "       reconfigure meson with --prefix=/opt/homebrew" >&2
    exit 1
fi

echo "==> Pruning unbundled scripting plugins from staging"
# Keep: checksum, fishlim, sasl, sysinfo, lua (luajit is self-contained).
# Drop: python, perl — their .dylib alone doesn't carry the interpreter's
# stdlib, so they fail on first import. Revisit when we bundle the full
# framework + PYTHONHOME/@INC redirection.
for plug in perl python; do
    rm -f "$STAGE_PREFIX/lib/hexchat/plugins/$plug.dylib"
done
rm -rf "$STAGE_PREFIX/lib/hexchat/python"

if [ ! -f "$BUNDLE_PLIST" ]; then
    echo "error: $BUNDLE_PLIST missing; Meson should have generated it" >&2
    exit 1
fi

echo "==> Running gtk-mac-bundler"
HEXCHAT_STAGE_PREFIX="$STAGE_PREFIX" \
HEXCHAT_BUNDLE_DEST="$BUNDLE_DEST" \
HEXCHAT_BUNDLE_PLIST="$BUNDLE_PLIST" \
    "$GTK_MAC_BUNDLER" "$HEXCHAT_SOURCE_DIR/osx/hexchat.bundle"

if [ ! -d "$APP_PATH" ]; then
    echo "error: gtk-mac-bundler did not produce $APP_PATH" >&2
    exit 1
fi

echo "==> Deduplicating opt/ vs Cellar/ copies of Homebrew dylibs"
# Homebrew install names mix opt/<name>/... (stable symlink) and
# Cellar/<name>/<ver>/... (versioned). gtk-mac-bundler follows both and
# ends up with two copies of e.g. libpango, which crashes GObject type
# registration. Canonicalize every Cellar/ path to its opt/ equivalent.
RES="$APP_PATH/Contents/Resources"
if [ -d "$RES/Cellar" ]; then
    # Build a list of (cellar_rel -> opt_rel) rewrites for each dylib that
    # exists under Cellar but also has a matching file under opt/.
    find "$RES/Cellar" -type f -name "*.dylib" | while read -r cellar_path; do
        rel=${cellar_path#"$RES/"}                                  # Cellar/pango/1.57.1/lib/libpango-1.0.0.dylib
        pkg=$(echo "$rel" | awk -F/ '{print $2}')                    # pango
        tail=$(echo "$rel" | cut -d/ -f4-)                           # lib/libpango-1.0.0.dylib
        opt_path="$RES/opt/$pkg/$tail"
        if [ -f "$opt_path" ]; then
            echo "$rel"
        fi
    done > "$BUNDLE_DEST/cellar-dupes.txt"

    while read -r rel; do
        pkg=$(echo "$rel" | awk -F/ '{print $2}')
        tail=$(echo "$rel" | cut -d/ -f4-)
        old_name="@executable_path/../Resources/$rel"
        new_name="@executable_path/../Resources/opt/$pkg/$tail"
        # Rewrite every Mach-O consumer in the bundle.
        find "$RES" "$APP_PATH/Contents/MacOS" -type f \
            \( -name "*.dylib" -o -name "*.so" -o -perm -u+x \) \
            -exec install_name_tool -change "$old_name" "$new_name" {} \; 2>/dev/null
        # Drop the duplicate file.
        rm -f "$RES/$rel"
    done < "$BUNDLE_DEST/cellar-dupes.txt"
    rm -f "$BUNDLE_DEST/cellar-dupes.txt"

    # Purge any now-empty Cellar subtree.
    find "$RES/Cellar" -type d -empty -delete 2>/dev/null || true
fi

echo "==> Copying HexChat translations into bundle"
# gtk-mac-bundler's <translations> tag only understands the default prefix,
# so we copy HexChat's .mo files in by hand.
HEXCHAT_LOCALE_SRC="$STAGE_PREFIX/share/locale"
BUNDLE_LOCALE_DEST="$APP_PATH/Contents/Resources/share/locale"
if [ -d "$HEXCHAT_LOCALE_SRC" ]; then
    find "$HEXCHAT_LOCALE_SRC" -name hexchat.mo | while read -r mo; do
        rel=${mo#"$HEXCHAT_LOCALE_SRC/"}
        mkdir -p "$BUNDLE_LOCALE_DEST/$(dirname "$rel")"
        cp "$mo" "$BUNDLE_LOCALE_DEST/$rel"
    done
fi

echo "==> Compiling GSettings schema cache inside bundle"
SCHEMA_DIR="$APP_PATH/Contents/Resources/share/glib-2.0/schemas"
if [ -d "$SCHEMA_DIR" ] && command -v glib-compile-schemas >/dev/null 2>&1; then
    glib-compile-schemas "$SCHEMA_DIR"
fi

echo "==> Codesigning with identity: $CODESIGN_IDENTITY"
# Sign inside-out: dylibs first, then the main binary, then the .app wrapper.
# --deep walks the bundle and signs everything with the provided identity.
ENTITLEMENTS="$HEXCHAT_SOURCE_DIR/osx/entitlements.plist"
CODESIGN_ARGS="--force --timestamp --options runtime --entitlements $ENTITLEMENTS --sign $CODESIGN_IDENTITY"
if [ "$CODESIGN_IDENTITY" = "-" ]; then
    # Ad-hoc signing can't use --timestamp.
    CODESIGN_ARGS="--force --options runtime --entitlements $ENTITLEMENTS --sign -"
fi

# shellcheck disable=SC2086
find "$APP_PATH/Contents" -type f \( -name "*.dylib" -o -name "*.so" \) \
    -exec codesign $CODESIGN_ARGS {} \;
# shellcheck disable=SC2086
codesign $CODESIGN_ARGS "$APP_PATH/Contents/MacOS/HexChat-bin"
# shellcheck disable=SC2086
codesign $CODESIGN_ARGS "$APP_PATH"

echo "==> Verifying signature"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

if [ -n "${NOTARY_PROFILE:-}" ]; then
    echo "==> Notarizing with profile: $NOTARY_PROFILE"
    NOTARY_ZIP="$BUNDLE_DEST/HexChat-notary.zip"
    rm -f "$NOTARY_ZIP"
    ditto -c -k --keepParent "$APP_PATH" "$NOTARY_ZIP"
    xcrun notarytool submit "$NOTARY_ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$APP_PATH"
    rm -f "$NOTARY_ZIP"
fi

if [ -n "${BUNDLE_SKIP_DMG:-}" ]; then
    echo "==> BUNDLE_SKIP_DMG set, skipping DMG"
else
    echo "==> Building DMG: $DMG_PATH"
    DMG_STAGE="$BUNDLE_DEST/dmg-stage"
    rm -rf "$DMG_STAGE"
    mkdir -p "$DMG_STAGE"
    cp -R "$APP_PATH" "$DMG_STAGE/"
    ln -s /Applications "$DMG_STAGE/Applications"
    hdiutil create -format UDZO -srcfolder "$DMG_STAGE" \
        -volname "HexChat $HEXCHAT_VERSION" -quiet -ov "$DMG_PATH"
    rm -rf "$DMG_STAGE"

    if [ "$CODESIGN_IDENTITY" != "-" ]; then
        codesign --force --timestamp --sign "$CODESIGN_IDENTITY" "$DMG_PATH"
    fi
    if [ -n "${NOTARY_PROFILE:-}" ]; then
        echo "==> Notarizing DMG"
        xcrun notarytool submit "$DMG_PATH" --keychain-profile "$NOTARY_PROFILE" --wait
        xcrun stapler staple "$DMG_PATH"
    fi
fi

echo ""
echo "Bundle: $APP_PATH"
if [ -f "$DMG_PATH" ]; then
    echo "DMG:    $DMG_PATH"
fi
