#!/bin/sh
#
# HexChat macOS bundle launcher.
#
# Resolves $0 to the bundle root and exports the environment GTK4 needs so
# that pixbuf loaders, GIO modules (TLS), and GSettings schemas are all
# read from inside the bundle rather than /opt/homebrew.

if [ -n "$GTK_DEBUG_LAUNCHER" ]; then
    set -x
fi

if [ -n "$GTK_DEBUG_GDB" ]; then
    EXEC="gdb --args"
else
    EXEC=exec
fi

name=$(basename "$0")
tmp=$(dirname "$0")
tmp=$(dirname "$tmp")
bundle=$(dirname "$tmp")
bundle_contents="$bundle/Contents"
bundle_res="$bundle_contents/Resources"
bundle_lib="$bundle_res/lib"
bundle_data="$bundle_res/share"
bundle_etc="$bundle_res/etc"

export XDG_CONFIG_DIRS="$bundle_etc/xdg"
export XDG_DATA_DIRS="$bundle_data"
export GTK_DATA_PREFIX="$bundle_res"
export GTK_EXE_PREFIX="$bundle_res"
export GTK_PATH="$bundle_res"

# Pixbuf loaders live under lib/gdk-pixbuf-2.0/<ver>/.
PIXBUF_LOADERS_CACHE=$(ls "$bundle_lib"/gdk-pixbuf-2.0/*/loaders.cache 2>/dev/null | head -1)
if [ -n "$PIXBUF_LOADERS_CACHE" ]; then
    export GDK_PIXBUF_MODULE_FILE="$PIXBUF_LOADERS_CACHE"
fi

# GIO modules (glib-networking for TLS).
export GIO_MODULE_DIR="$bundle_lib/gio/modules"

# GSettings schemas.
export GSETTINGS_SCHEMA_DIR="$bundle_data/glib-2.0/schemas"

# HexChat plugins (loadable .so). Matches HEXCHATLIBDIR in meson config.
export HEXCHAT_LIBDIR="$bundle_lib/hexchat/plugins"

APP=$name
I18NDIR="$bundle_data/locale"
# Set the locale-related variables appropriately:
unset LANG LC_MESSAGES LC_MONETARY LC_COLLATE

# Has a language ordering been set?
APPLELANGUAGES=$(defaults read .GlobalPreferences AppleLanguages 2>/dev/null | \
    sed -En -e 's/\-/_/' -e 's/Hant/TW/' -e 's/Hans/CN/' \
            -e 's/[[:space:]]*\"?([[:alnum:]_]+)\"?,?/\1/p')
if [ -n "$APPLELANGUAGES" ]; then
    for L in $APPLELANGUAGES; do
        if [ -f "$I18NDIR/${L}/LC_MESSAGES/$APP.mo" ]; then
            export LANG=$L
            break
        fi
        if [ "x$L" = "xen_US" ]; then
            export LANG=$L
            break
        fi
        if [ -f "$I18NDIR/${L%%_*}/LC_MESSAGES/$APP.mo" ]; then
            export LANG=${L%%_*}
            break
        fi
        case "$L" in
            en*)
                export LANG=$L
                break
                ;;
        esac
    done
fi
unset APPLELANGUAGES L

APPLECOLLATION=$(defaults read .GlobalPreferences AppleCollationOrder 2>/dev/null)
if [ -z "$LANG" ] && [ -n "$APPLECOLLATION" ]; then
    SHORT=${APPLECOLLATION%%_*}
    if [ -f "$I18NDIR/${SHORT}/LC_MESSAGES/$APP.mo" ]; then
        export LANG=$SHORT
    fi
fi
if [ -n "$APPLECOLLATION" ]; then
    export LC_COLLATE=$APPLECOLLATION
fi
unset APPLECOLLATION SHORT

APPLELOCALE=$(defaults read .GlobalPreferences AppleLocale 2>/dev/null)
SHORT5=${APPLELOCALE:0:5}
SHORT2=${APPLELOCALE:0:2}

if [ -f "$I18NDIR/$SHORT5/LC_MESSAGES/$APP.mo" ]; then
    if [ -z "$LANG" ]; then
        export LANG=$SHORT5
    fi
elif [ -z "$LANG" ] && [ -f "$I18NDIR/$SHORT2/LC_MESSAGES/$APP.mo" ]; then
    export LANG=$SHORT2
fi

if [ -n "$LANG" ]; then
    LANG_SHORT=${LANG%%_*}
    if [ "$LANG" = "$SHORT5" ] || [ "$LANG" != "$LANG_SHORT" ]; then
        export LC_MESSAGES=$LANG
    elif [ "$LANG" = "$SHORT2" ] && [ -n "$APPLELOCALE" ] && [ "$APPLELOCALE" != "$SHORT2" ]; then
        export LC_MESSAGES=$SHORT5
    elif [ "$LANG" = "en" ]; then
        export LC_MESSAGES="en_US"
    else
        export LC_MESSAGES=$LANG
    fi
else
    export LANG="en_US"
    export LC_MESSAGES="en_US"
fi

if [ -z "$LC_MONETARY" ]; then
    LC_MONETARY=$SHORT5
fi

# GTK reads LC_ALL only.
export LC_ALL=$LC_MESSAGES

unset APPLELOCALE SHORT5 SHORT2 LANG_SHORT

if [ -f "$bundle_lib/charset.alias" ]; then
    export CHARSETALIASDIR="$bundle_lib"
fi

# Extra arguments can be added in environment.sh.
EXTRA_ARGS=
if [ -f "$bundle_res/environment.sh" ]; then
    . "$bundle_res/environment.sh"
fi

# Strip out the -psn_ argument macOS adds when launching from Finder.
if /bin/expr "x$1" : '^x-psn_' > /dev/null; then
    shift 1
fi

$EXEC "$bundle_contents/MacOS/$name-bin" "$@" $EXTRA_ARGS
