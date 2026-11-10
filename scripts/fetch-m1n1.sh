#!/bin/sh
# Clone m1n1 at the pinned commit and apply our hook patch. Safe to re-run.
set -eu

URL="$1"
COMMIT="$2"
DIR="$3"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCH="$ROOT/patches/0001-m1n1-runtime-hooks.patch"

if [ ! -d "$DIR/.git" ]; then
    mkdir -p "$(dirname "$DIR")"
    git clone "$URL" "$DIR"
fi

cd "$DIR"

if [ "$(git rev-parse HEAD)" != "$COMMIT" ]; then
    if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
        echo "vendor/m1n1 has local changes and is not at $COMMIT; not touching it."
        exit 1
    fi
    git fetch origin
    git checkout --quiet "$COMMIT"
fi

if git apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "m1n1 $COMMIT: hooks already applied"
elif git apply --check "$PATCH" 2>/dev/null; then
    git apply "$PATCH"
    echo "m1n1 $COMMIT: hooks applied"
else
    echo "The hook patch does not apply to this m1n1 tree. If you moved the pin,"
    echo "re-create the three hooks by hand (see patches/) and regenerate the patch."
    exit 1
fi
