#!/bin/bash
# fetch_apple_files.sh — copy the Apple files this project needs from YOUR OWN macOS installs.
#
# None of Apple's binaries or resource files are included in this repository. This script takes them from the
# macOS 26 system you're running and from a macOS 27 system volume mounted on the same machine, and checks each
# one against the hash of the build this project was made against (the hook addresses and fixup tables only fit
# those builds). It writes:
#   ../reference/macos26/{TransitionStyles.json,DSPGraph.dspg}   26's catalog + graph (for diffing only)
#   ../reference/macos27/{TransitionStyles.json,DSPGraph.dspg}   27's catalog + graph (make_catalog.py's input)
#   payload/SonicAudioUnits                                       27's x86_64 SonicAudioUnits (holds AURemixFX),
#                                                                 rebuilt from 27's dyld shared cache
# payload/SonicAudioUnits.fixups / .ehbinds (this project's offset tables for that binary) are part of the repo.
#
#   ./fetch_apple_files.sh                     macOS 27 volume at "/Volumes/macOS 27"
#   ./fetch_apple_files.sh --vol27 /Volumes/X  another mount point
#   ./fetch_apple_files.sh --no-verify         skip the hash checks (other builds: expect the hooks to refuse to load)
#
# Built against: macOS 26.7.3 (25G342) x86_64, macOS 27.2 (26B123) x86_64 shared cache. Read-only on both systems.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
VOL27="/Volumes/macOS 27"; VERIFY=1
while [ $# -gt 0 ]; do
  case "$1" in
    --vol27) VOL27="$2"; shift 2 ;;
    --no-verify) VERIFY=0; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
say() { printf '\033[1m==>\033[0m %s\n' "$*"; }

R26=/System/Library/PrivateFrameworks/_SonicKit_MusicKit_Packages.framework/Versions/A/Resources
R27="$VOL27/System/Library/PrivateFrameworks/_SonicKit_MusicKit.framework/Versions/A/Resources"
CACHE27="$VOL27/System/Library/dyld/dyld_shared_cache_x86_64"
SAU_IMAGE=/System/Library/PrivateFrameworks/SonicAudioUnits.framework/Versions/A/SonicAudioUnits

[ "$(uname -m)" = x86_64 ] || die "this project targets x86_64 macOS 26"
[ -f "$R26/TransitionStyles.json" ] || die "macOS 26's SonicKit resources not found at $R26"
[ -d "$VOL27/System" ] || die "no macOS 27 system volume at \"$VOL27\" (mount it, or pass --vol27 PATH)"
[ -f "$CACHE27.map" ] || die "macOS 27's x86_64 shared cache not found at $CACHE27"

check() {   # check FILE SHA256
  [ $VERIFY = 1 ] || return 0
  local got; got=$(shasum -a 256 "$1" | cut -d' ' -f1)
  [ "$got" = "$2" ] || die "$1: hash $got doesn't match the build this project supports ($2). Use --no-verify to keep it anyway."
}

say "macOS 26 $(sw_vers -productVersion) ($(sw_vers -buildVersion)); macOS 27 $(/usr/libexec/PlistBuddy -c 'Print ProductVersion' "$VOL27/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null) ($(/usr/libexec/PlistBuddy -c 'Print ProductBuildVersion' "$VOL27/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null))"

mkdir -p "$ROOT/reference/macos26" "$ROOT/reference/macos27" "$HERE/payload"
copy() {    # copy SRC DST SHA256
  cp "$1" "$2.tmp"; check "$2.tmp" "$3"; mv -f "$2.tmp" "$2"; echo "    $2"
}
say "Copying SonicKit catalogs and DSP graphs"
copy "$R26/TransitionStyles.json" "$ROOT/reference/macos26/TransitionStyles.json" fe3d0a36625ccb043c519bcc5119c98190893a9911cfa58412ad60cbab04d120
copy "$R26/DSPGraph.dspg"         "$ROOT/reference/macos26/DSPGraph.dspg"         8326e890fd6905a0fba94176ed57ae200fe38c206dbac1c07d8355c278dafd2a
copy "$R27/TransitionStyles.json" "$ROOT/reference/macos27/TransitionStyles.json" 4f3ec26017d936294b88cd03144fe903ecd34bdca5847c28d8375affeaa46d83
copy "$R27/DSPGraph.dspg"         "$ROOT/reference/macos27/DSPGraph.dspg"         c6908d8833d1d5d0b36bb3876d3286bc4f8b3fa08e9b3740febd2bb11f787bfb

say "Rebuilding SonicAudioUnits from macOS 27's shared cache (takes a minute)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
python3 -I "$ROOT/tools/dsc_extract_one.py" "$CACHE27" "$SAU_IMAGE" "$TMP/SonicAudioUnits" >/dev/null
check "$TMP/SonicAudioUnits" 7e0f3708db239c95fdb2db53e0af1c50127ec25074b1356ccf5ec75b614e30af
mv -f "$TMP/SonicAudioUnits" "$HERE/payload/SonicAudioUnits"
echo "    $HERE/payload/SonicAudioUnits"

say "Done. Next: ./install.sh"
