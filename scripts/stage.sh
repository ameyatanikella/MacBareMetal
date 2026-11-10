#!/bin/sh
# Copy the built image to a place recovery (1TR) can read, together with a
# small install script, so the only thing typed in the recovery Terminal is
# one short command.
#
# This script does NOT install anything. Installing a boot object is only
# possible from 1TR, by you, with your password.
set -eu

DEST="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$ROOT/out/m5rt.bin"

[ -f "$IMG" ] || { echo "out/m5rt.bin not found: run 'make image' first"; exit 1; }
[ -d "$DEST" ] || { echo "$DEST is not a mounted folder"; exit 1; }

# The volume this macOS booted from. The install script refuses to target it.
MAIN_VOL="$(diskutil info / | awk -F': *' '/Volume Name/ {print $2; exit}')"
[ -n "$MAIN_VOL" ] || { echo "could not determine the running system's volume name"; exit 1; }

OUT="$DEST/m5rt"
mkdir -p "$OUT"
cp "$IMG" "$OUT/m5rt.bin"
cp "$ROOT/out/m5rt.build-id" "$OUT/build-id.txt" 2>/dev/null || true
shasum -a 256 "$OUT/m5rt.bin" | awk '{print $1}' > "$OUT/sha256.txt"

cat > "$OUT/install.sh" <<EOF
#!/bin/sh
# Run from the 1TR recovery Terminal ONLY:   sh install.sh /Volumes/<test volume>
# Installs m5rt.bin as the custom boot object of the TEST volume.
set -eu

HERE="\$(cd "\$(dirname "\$0")" && pwd)"
VOL="\${1:-}"
MAIN_VOL="$MAIN_VOL"

[ -n "\$VOL" ] || { echo "usage: sh install.sh /Volumes/<test volume>"; exit 1; }
[ -d "\$VOL" ] || { echo "\$VOL is not mounted"; exit 1; }

case "\$VOL" in
    "/Volumes/\$MAIN_VOL"|"/Volumes/\$MAIN_VOL/"|"/")
        echo "REFUSING: \$VOL is your main macOS volume."
        echo "A boot object installed there replaces macOS's kernel for that volume."
        exit 1;;
esac

echo "image:  \$HERE/m5rt.bin"
echo "build:  \$(cat "\$HERE/build-id.txt" 2>/dev/null || echo unknown)"
echo "target: \$VOL"
printf "Type the target volume's name to confirm: "
read -r ANSWER
[ "/Volumes/\$ANSWER" = "\${VOL%/}" ] || { echo "name does not match; nothing done"; exit 1; }

kmutil configure-boot -c "\$HERE/m5rt.bin" --raw --entry-point 2048 \\
    --lowest-virtual-address 0 -v "\$VOL"
EOF
chmod +x "$OUT/install.sh"

echo "staged in $OUT:"
ls -l "$OUT"
echo
echo "build id: $(cat "$OUT/build-id.txt" 2>/dev/null || echo unknown)"
echo "Next: docs/TESTING.md, step 6 (install the boot object from recovery)."
