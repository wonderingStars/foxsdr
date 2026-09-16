#!/usr/bin/env bash
# make-icons.sh - the launcher icon, from the one the desktop already ships.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# WHY A SCRIPT RATHER THAN SIX CHECKED-IN PNGs BY HAND. resources/icon/
# foxsdr-256.png is the product's mark and it is regenerated from
# resources/icon/generate_icon.py when the brand changes. Six hand-made
# downscales would drift from it silently, and the search-icon lesson in this
# project's history is exactly what that costs. Run this after the mark changes.
#
# The outputs ARE committed - a Gradle build must not depend on ImageMagick
# being installed on the machine doing the building - so this is a generator,
# not a build step.
#
# TOOLING, recorded because it differs per machine:
#   Linux/WSL   ImageMagick, which this script uses. `magick` (IM 7) is
#               preferred and `convert` (IM 6) is accepted; on this development
#               box /usr/bin/magick is ImageMagick 6.9 and both work.
#   Windows     Pillow via `py -3.14`, which resources/icon/generate_icon.py
#               already uses. NOTE that C:\Windows\system32\convert.exe is the
#               FAT-to-NTFS utility and has nothing to do with ImageMagick -
#               running it by accident is a documented trap in this project.
#
# Usage:  bash android/tools/make-icons.sh

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${here}/../.." && pwd)"
src="${repo}/resources/icon/foxsdr-256.png"
res="${repo}/android/app/src/main/res"

if [ ! -f "${src}" ]; then
    echo "source mark not found: ${src}" >&2
    exit 1
fi

if command -v magick >/dev/null 2>&1; then
    im=(magick)
elif command -v convert >/dev/null 2>&1; then
    im=(convert)
else
    echo "neither magick nor convert found - install ImageMagick" >&2
    exit 1
fi

# The five density buckets Android asks for, at the launcher's 48 dp.
#   mdpi 1x = 48   hdpi 1.5x = 72   xhdpi 2x = 96
#   xxhdpi 3x = 144   xxxhdpi 4x = 192
#
# The source is 256 px, so every one of these is a DOWNSCALE. Lanczos rather
# than the default, because the mark has fine strokes and a box filter turns
# them to mud at 48 px.
for pair in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    bucket="${pair%%:*}"
    size="${pair##*:}"
    dir="${res}/mipmap-${bucket}"
    mkdir -p "${dir}"
    "${im[@]}" "${src}" -filter Lanczos -resize "${size}x${size}" \
        -strip -define png:color-type=6 "${dir}/ic_launcher.png"
    # NO ic_launcher_round. It would be a byte-for-byte copy of this file -
    # the mark is already circular within its square - and lint's IconDuplicates
    # check is right to call that out. A launcher with no roundIcon falls back
    # to icon, which is the same bitmap by a shorter route.
    echo "wrote ${dir}/ic_launcher.png (${size}x${size})"
done
