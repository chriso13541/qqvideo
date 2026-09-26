#!/usr/bin/env bash
#
# lumenctl -- install/remove/list plugins for lumen-play, Gentoo-emerge-style.
#
# A "plugin package" is just a directory containing:
#   <name>.manifest.json   (required)
#   lib<name>.so / <name>.dll  (the shared library the manifest points at)
#
# Usage:
#   lumenctl install <package-dir> [--plugins <dir>]
#   lumenctl remove <plugin-name>  [--plugins <dir>]
#   lumenctl list                  [--plugins <dir>]
#
set -euo pipefail

PLUGIN_DIR="plugins"
ACTION="${1:-}"
shift || true

ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --plugins) PLUGIN_DIR="$2"; shift 2 ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

mkdir -p "$PLUGIN_DIR"

find_manifest_name() {
    # Extract the JSON "name" field without a real JSON parser -- fine for
    # this trusted, locally-authored CLI; the core's own minijson parser
    # is what actually matters for plugin trust boundaries.
    grep -o '"name"[[:space:]]*:[[:space:]]*"[^"]*"' "$1" | head -1 | sed -E 's/.*"name"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/'
}

case "$ACTION" in
    install)
        PKG_DIR="${ARGS[0]:?usage: lumenctl install <package-dir>}"
        MANIFEST=$(find "$PKG_DIR" -maxdepth 1 -name '*.manifest.json' | head -1)
        if [ -z "$MANIFEST" ]; then
            echo "lumenctl: no *.manifest.json found in $PKG_DIR" >&2
            exit 1
        fi
        NAME=$(find_manifest_name "$MANIFEST")
        LIBFILE=$(grep -o '"library"[[:space:]]*:[[:space:]]*"[^"]*"' "$MANIFEST" | sed -E 's/.*"([^"]*)"$/\1/')
        if [ ! -f "$PKG_DIR/$LIBFILE" ]; then
            echo "lumenctl: manifest references '$LIBFILE' but it isn't in $PKG_DIR" >&2
            exit 1
        fi
        cp "$MANIFEST" "$PLUGIN_DIR/$(basename "$MANIFEST")"
        cp "$PKG_DIR/$LIBFILE" "$PLUGIN_DIR/$LIBFILE"
        echo "lumenctl: installed '$NAME' -> $PLUGIN_DIR/"
        ;;

    remove)
        NAME="${ARGS[0]:?usage: lumenctl remove <plugin-name>}"
        FOUND=0
        for m in "$PLUGIN_DIR"/*.manifest.json; do
            [ -e "$m" ] || continue
            if [ "$(find_manifest_name "$m")" = "$NAME" ]; then
                LIBFILE=$(grep -o '"library"[[:space:]]*:[[:space:]]*"[^"]*"' "$m" | sed -E 's/.*"([^"]*)"$/\1/')
                rm -f "$m" "$PLUGIN_DIR/$LIBFILE"
                echo "lumenctl: removed '$NAME' ($LIBFILE)"
                FOUND=1
            fi
        done
        if [ "$FOUND" -eq 0 ]; then
            echo "lumenctl: no installed plugin named '$NAME'" >&2
            exit 1
        fi
        ;;

    list)
        for m in "$PLUGIN_DIR"/*.manifest.json; do
            [ -e "$m" ] || { echo "lumenctl: no plugins installed in $PLUGIN_DIR"; exit 0; }
            NAME=$(find_manifest_name "$m")
            KIND=$(grep -o '"kind"[[:space:]]*:[[:space:]]*"[^"]*"' "$m" | sed -E 's/.*"([^"]*)"$/\1/')
            echo "$NAME ($KIND)"
        done
        ;;

    *)
        echo "usage: lumenctl {install|remove|list} ... [--plugins <dir>]" >&2
        exit 1
        ;;
esac
