#!/usr/bin/env bash
# Re-pull the vendored GameTankEmulator sources from a pinned upstream commit.
#
# vendor/ is an in-tree COPY of a modified subset of upstream (we drop the SDL
# app shell + imgui/devtools and patch SDL_inc.h to a shim) — a git submodule
# can't express "8 of ~40 files, some edited", which is why this exists. This
# script makes updating that copy a reproducible one-command operation instead of
# hand-copying, and shows a diff so intentional local edits aren't clobbered blind.
#
# Usage:
#   scripts/update-vendor.sh                 # pull the pinned UPSTREAM_COMMIT, show diff
#   scripts/update-vendor.sh <commit-ish>    # pull a different commit (to bump the pin)
#   scripts/update-vendor.sh --apply         # actually overwrite vendor/ (default: dry-run)
#
# After bumping: review the diff for any of OUR edits that need re-applying
# (currently: vendor/SDL_inc.h routes to sdl_shim.h under LIBRETRO_BUILD), update
# UPSTREAM_COMMIT below + vendor/PROVENANCE.txt, rebuild, run the harness.
set -euo pipefail

UPSTREAM_REPO="https://github.com/clydeshaffer/GameTankEmulator"
UPSTREAM_COMMIT="e7e25e2daf5da5d041ae8dc48f740a362c1e66ff"

# The exact files we vendor, as <upstream-src-path> -> <our-vendor-path>.
# This list IS the definition of what vendor/ contains; keep it in sync with
# vendor/PROVENANCE.txt. NOTE: SDL_inc.h is intentionally NOT pulled here — ours
# is a local shim-routing edit, not an upstream copy (see vendor/sdl_shim.h).
FILES=(
  "src/mos6502/mos6502.cpp:vendor/mos6502/mos6502.cpp"
  "src/mos6502/mos6502.h:vendor/mos6502/mos6502.h"
  "src/mos6502/LICENSE.txt:vendor/mos6502/LICENSE.txt"
  "src/blitter.cpp:vendor/blitter.cpp"
  "src/blitter.h:vendor/blitter.h"
  "src/audio_coprocessor.cpp:vendor/audio_coprocessor.cpp"
  "src/audio_coprocessor.h:vendor/audio_coprocessor.h"
  "src/palette.cpp:vendor/palette.cpp"
  "src/palette.h:vendor/palette.h"
  "src/gametank_palette.h:vendor/gametank_palette.h"
  "src/timekeeper.cpp:vendor/timekeeper.cpp"
  "src/timekeeper.h:vendor/timekeeper.h"
  "src/system_state.h:vendor/system_state.h"
  "src/emulator_config.cpp:vendor/emulator_config.cpp"
  "src/emulator_config.h:vendor/emulator_config.h"
  "src/game_config.cpp:vendor/game_config.cpp"
  "src/game_config.h:vendor/game_config.h"
)

COMMIT="${1:-$UPSTREAM_COMMIT}"
APPLY=0
for a in "$@"; do [ "$a" = "--apply" ] && APPLY=1; done
[ "${1:-}" = "--apply" ] && COMMIT="$UPSTREAM_COMMIT"   # --apply alone keeps the pin

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> Fetching $UPSTREAM_REPO @ $COMMIT"
git clone -q --filter=blob:none --no-checkout "$UPSTREAM_REPO" "$TMP/up"
git -C "$TMP/up" checkout -q "$COMMIT"

changed=0
for pair in "${FILES[@]}"; do
  src="${pair%%:*}"; dst="${pair##*:}"
  up="$TMP/up/$src"; ours="$PROJ_DIR/$dst"
  if [ ! -f "$up" ]; then echo "  ! upstream missing: $src (renamed/removed upstream?)"; continue; fi
  if [ ! -f "$ours" ] || ! diff -q "$up" "$ours" >/dev/null 2>&1; then
    changed=$((changed+1))
    echo "  ~ $dst differs from upstream $src"
    if [ "$APPLY" = 1 ]; then mkdir -p "$(dirname "$ours")"; cp "$up" "$ours"; fi
  fi
done

echo ""
if [ "$changed" = 0 ]; then
  echo "==> vendor/ is identical to upstream @ $COMMIT — nothing to do."
elif [ "$APPLY" = 1 ]; then
  echo "==> Applied $changed file update(s) from $COMMIT."
  echo "    NEXT: re-check our local edits (vendor/SDL_inc.h shim routing), update"
  echo "    UPSTREAM_COMMIT in this script + vendor/PROVENANCE.txt, rebuild + run harness."
else
  echo "==> $changed file(s) differ. This was a DRY RUN — re-run with --apply to overwrite,"
  echo "    or with 'git -C $TMP/up show $COMMIT:<path>' to inspect a specific upstream file."
fi
