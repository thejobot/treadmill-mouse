#!/usr/bin/env bash
# Fetch joypad-os at the pinned commit and lay this overlay on top of it.
#
#   ./setup.sh [target-dir]        default: ./build-tree
#
# Nothing here is installed system-wide and nothing outside the target
# directory is touched. Re-running it is safe: the checkout is reset to the
# pinned commit first, so a half-finished attempt cannot leave stale files.

set -euo pipefail

# Overridable so a fork or a newer upstream can be tried without editing this.
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/joypad-ai/joypad-os.git}"
UPSTREAM_REF="${UPSTREAM_REF:-027326397a88fe43d60deb37948ffc9d8f1d8c30}"  # joypad-os 2.2.0 + a few commits

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${1:-$HERE/build-tree}"

if [ ! -d "$DEST/.git" ]; then
  echo "Cloning joypad-os into $DEST"
  git clone "$UPSTREAM_URL" "$DEST"
fi

echo "Checking out $UPSTREAM_REF"
git -C "$DEST" fetch --quiet origin
git -C "$DEST" checkout --quiet --force "$UPSTREAM_REF"
git -C "$DEST" submodule update --init --recursive --quiet

echo "Applying overlay"
# -a keeps mode bits; the overlay mirrors upstream's paths exactly.
( cd "$HERE/overlay" && find . -type f -print0 ) | while IFS= read -r -d '' f; do
  mkdir -p "$DEST/$(dirname "$f")"
  cp -a "$HERE/overlay/$f" "$DEST/$f"
  echo "  $f"
done

cat <<MSG

Done. The tree in $DEST is upstream joypad-os with this firmware's changes in
place. To see exactly what changed:

    git -C "$DEST" diff

To build it, see docs/BUILDING.md.
MSG
