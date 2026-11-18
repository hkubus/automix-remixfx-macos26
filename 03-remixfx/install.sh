#!/bin/bash
# install.sh — stage the RemixFX transplant and launch Music with it.
#   ./install.sh             build (if needed), stage payload, launch Music with the dylib
#   ./install.sh --stage     stage payload only (no launch)
#   ./install.sh --launcher  stage, then create ~/Applications/Music (RemixFX).app (open Music through it)
#   ./install.sh uninstall   remove the payload, the AU component bundle and the launcher app
#
# Nothing here touches the sealed system volume. RemixFX is Apple's own x86_64 AURemixFX,
# transplanted from macOS 27's SonicAudioUnits.framework and loaded in-process by RemixFXEnabler.dylib.
# Apple's files aren't in this repo: the first run calls fetch_apple_files.sh, which copies them from your own
# macOS 26 system and a mounted macOS 27 volume (set REMIXFX_VOL27 if it isn't at "/Volumes/macOS 27").
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$HOME/Library/Application Support/RemixFX"
APP="$HOME/Applications/Music (RemixFX).app"
COMPDIR="$HOME/Library/Audio/Plug-Ins/Components"
COMP="$COMPDIR/RemixFX.component"

uninstall() {
  rm -rf "$BASE" "$APP" "$COMP"
  killall -9 AudioComponentRegistrar 2>/dev/null || true
  echo "removed $BASE, $COMP and $APP"
  echo "(basic AutoMix setup, if any, is separate and untouched)"
  exit 0
}
[ "${1:-}" = "uninstall" ] && uninstall

# 0. Apple's files (binary + catalogs) come from your own installs, never from the repo
if [ ! -f "$HERE/payload/SonicAudioUnits" ] || [ ! -f "$HERE/../reference/macos27/TransitionStyles.json" ]; then
  "$HERE/fetch_apple_files.sh" ${REMIXFX_VOL27:+--vol27 "$REMIXFX_VOL27"}
fi

# 1. build the dylib if missing or older than sources
SRCS=("$HERE/RemixFXEnabler.m" "$HERE/sauload.c" "$HERE/rxdrive.c" "$HERE/rxenv.m" "$HERE/planner27.c" "$HERE/hook_x64.s")
stale=0; [ -f "$HERE/RemixFXEnabler.dylib" ] || stale=1
for f in "${SRCS[@]}" "$HERE/rxdrive.h" "$HERE/planner27.h"; do [ "$f" -nt "$HERE/RemixFXEnabler.dylib" ] && stale=1; done
if [ $stale = 1 ]; then
  echo "building RemixFXEnabler.dylib ..."
  clang -arch x86_64 -dynamiclib -fobjc-arc -O2 "${SRCS[@]}" \
    -o "$HERE/RemixFXEnabler.dylib" -framework Foundation -framework AudioToolbox
  codesign -f -s - "$HERE/RemixFXEnabler.dylib"
fi
# catalog + graph for 26 (make_catalog.py; files are rewritten only when their content changes):
#   default             RemixFX styles 17-20 at their real IDs, planned by planner27.c (option b)
#   REMIXFX_SLOTS=8:18,9:17,12:20   RemixFX styles inside 26's beat-matched slots (option a)
python3 -I "$HERE/make_catalog.py" ${REMIXFX_SLOTS:+--slots "$REMIXFX_SLOTS"} >/dev/null

# 2. stage payload
mkdir -p "$BASE/Resources"
cp "$HERE/payload/SonicAudioUnits"         "$BASE/SonicAudioUnits"
cp "$HERE/payload/SonicAudioUnits.fixups"  "$BASE/SonicAudioUnits.fixups"
cp "$HERE/payload/SonicAudioUnits.ehbinds" "$BASE/SonicAudioUnits.ehbinds"
codesign -f -s - "$BASE/SonicAudioUnits" 2>/dev/null || true   # ad-hoc (AMFI permissive); harmless if it fails
cp "$HERE/RemixFXEnabler.dylib"            "$BASE/RemixFXEnabler.dylib"
# transition catalog + DSP graph (27's graph carries the AURemixFX node). Supply yours in payload/Resources
# to override; otherwise fall back to the repo reference copies.
for f in TransitionStyles.json DSPGraph.dspg RemixFXStyles.json; do
  if   [ -f "$HERE/payload/Resources/$f" ]; then cp "$HERE/payload/Resources/$f" "$BASE/Resources/$f"
  elif [ -f "$HERE/../reference/macos27/$f" ]; then cp "$HERE/../reference/macos27/$f" "$BASE/Resources/$f"; fi
done
echo "staged payload under $BASE"

# 3. AU component bundle: makes aufx/remx/appl available in every process — notably AUHostingServiceXPC, where
#    AudioQueue runs the SmartTransitions DSPGraph out-of-process (see component.m).
CB="$HERE/build/RemixFX.component"
CSRCS=("$HERE/component.m" "$HERE/sauload.c" "$HERE/rxdrive.c" "$HERE/rxenv.m")
cstale=0; [ -f "$CB/Contents/MacOS/RemixFX" ] || cstale=1
for f in "${CSRCS[@]}" "$HERE/rxdrive.h" "$HERE/component/Info.plist" "$HERE/payload/Resources/RemixFXStyles.json" \
         "$HERE/payload/SonicAudioUnits" "$HERE/payload/SonicAudioUnits.fixups" "$HERE/payload/SonicAudioUnits.ehbinds"; do
  [ "$f" -nt "$CB/Contents/MacOS/RemixFX" ] && cstale=1; done
if [ $cstale = 1 ]; then
  echo "building RemixFX.component ..."
  rm -rf "$CB"; mkdir -p "$CB/Contents/MacOS" "$CB/Contents/Resources"
  cp "$HERE/component/Info.plist" "$CB/Contents/Info.plist"
  clang -arch x86_64 -bundle -fobjc-arc -O2 "${CSRCS[@]}" -o "$CB/Contents/MacOS/RemixFX" \
    -framework Foundation -framework AudioToolbox
  cp "$HERE/payload/SonicAudioUnits" "$HERE/payload/SonicAudioUnits.fixups" "$HERE/payload/SonicAudioUnits.ehbinds" \
     "$HERE/payload/Resources/RemixFXStyles.json" \
     "$CB/Contents/Resources/"
  codesign -f -s - "$CB"
fi
mkdir -p "$COMPDIR"
if ! diff -rq "$CB" "$COMP" >/dev/null 2>&1; then
  rm -rf "$COMP"; cp -R "$CB" "$COMP"
  killall -9 AudioComponentRegistrar 2>/dev/null || true      # rescan components
  echo "installed $COMP"
fi
[ "${1:-}" = "--stage" ] && { echo "done (no launch)"; exit 0; }

# 3b. optional launcher app: opens Music with the dylib (offers a restart if Music runs without it)
if [ "${1:-}" = "--launcher" ]; then
  rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
  cat > "$APP/Contents/MacOS/launcher" <<'SH'
#!/bin/bash
# Opens Music with RemixFXEnabler loaded. If Music is already running without it, offer a restart.
DYLIB="$HOME/Library/Application Support/RemixFX/RemixFXEnabler.dylib"
PID=$(pgrep -x Music | head -1)
if [[ -n "$PID" ]]; then
  if lsof -p "$PID" 2>/dev/null | grep -q RemixFXEnabler; then open -a Music; exit 0; fi
  ANSWER=$(osascript -e 'button returned of (display dialog "Music is running without RemixFX. Restart it?" buttons {"Not Now","Restart"} default button "Restart" with title "Music (RemixFX)")' 2>/dev/null)
  [[ "$ANSWER" == "Restart" ]] || { open -a Music; exit 0; }
  osascript -e 'tell application "Music" to quit' >/dev/null 2>&1
  for _ in $(seq 1 40); do pgrep -xq Music || break; sleep 0.25; done
fi
exec /usr/bin/open -a Music --env "DYLD_INSERT_LIBRARIES=$DYLIB"
SH
  chmod +x "$APP/Contents/MacOS/launcher"
  cp /System/Applications/Music.app/Contents/Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null || true
  cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>local.remixfx.launcher</string>
  <key>CFBundleName</key><string>Music (RemixFX)</string>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSUIElement</key><true/>
</dict></plist>
PLIST
  codesign -f -s - "$APP" 2>/dev/null || true
  echo "created $APP - open Music through it (e.g. drag it to the Dock in place of Music)"
  exit 0
fi

# 4. launch Music with the dylib injected
MUSIC="/System/Applications/Music.app/Contents/MacOS/Music"
echo "quitting any running Music ..."
osascript -e 'tell application "Music" to quit' 2>/dev/null || true
sleep 2; pkill -x Music 2>/dev/null || true; sleep 1
echo "launching Music with RemixFX ..."
DYLD_INSERT_LIBRARIES="$BASE/RemixFXEnabler.dylib" "$MUSIC" >/tmp/remixfx-music.log 2>&1 &
echo "Music launched (pid $!). Log: /tmp/remixfx-music.log"
echo "check:  log stream --predicate 'subsystem == \"dev.remixfx\"' --info"
