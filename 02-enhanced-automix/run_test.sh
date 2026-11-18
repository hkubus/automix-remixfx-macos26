#!/bin/bash
# run_test.sh N_TRANSITIONS [PLAYLIST] — relaunch Music with EnhancedAutoMix, force N transitions, dump logs.
# Music volume is kept low and restored to 100 afterwards; playback is stopped at the end.
N=${1:-3}; PL=${2:-Playlist}
D="$(cd "$(dirname "$0")" && pwd)"
DYLIB="$D/EnhancedAutoMix.dylib"
if [ ! -f "$DYLIB" ] || [ "$D/EnhancedAutoMix.m" -nt "$DYLIB" ]; then
  clang -arch x86_64 -dynamiclib -fobjc-arc -O2 "$D/EnhancedAutoMix.m" -o "$DYLIB" \
    -framework Foundation -framework AudioToolbox && codesign -f -s - "$DYLIB" || exit 1
fi
OUT="$D/logs/$(date +%H%M%S)"; mkdir -p "$OUT"
START=$(date '+%Y-%m-%d %H:%M:%S')

osascript -e 'tell application "Music" to quit' >/dev/null 2>&1
for _ in $(seq 1 60); do pgrep -xq Music || break; sleep 0.5; done
pkill -9 -x Music 2>/dev/null; sleep 1
/usr/bin/open -a Music --env "DYLD_INSERT_LIBRARIES=$DYLIB"
sleep 10
osascript -e 'tell application "Music" to set sound volume to 12' >/dev/null 2>&1
osascript -e "tell application \"Music\" to play playlist \"$PL\"" 2>&1
sleep 6
for i in $(seq 1 "$N"); do
  dur=$(osascript -e 'tell application "Music" to get duration of current track' 2>/dev/null)
  name=$(osascript -e 'tell application "Music" to get (name of current track) & " | " & (genre of current track)' 2>/dev/null)
  echo "[$i] now: $name ($dur s)" | tee -a "$OUT/run.txt"
  [ -z "$dur" ] && { sleep 5; continue; }
  pos=$(python3 -c "print(max(0, float('$dur') - 38))")
  osascript -e "tell application \"Music\" to set player position to $pos" >/dev/null 2>&1
  sleep 44
done
osascript -e 'tell application "Music" to pause' >/dev/null 2>&1
osascript -e 'tell application "Music" to set sound volume to 100' >/dev/null 2>&1
sleep 2
/usr/bin/log show --start "$START" --predicate 'process == "Music" AND (subsystem == "dev.enhancedautomix" OR subsystem == "com.apple.Sonic" OR subsystem == "com.apple.smarttransitions" OR eventMessage CONTAINS[c] "transition" OR eventMessage CONTAINS[c] "DSPGraph" OR eventMessage CONTAINS[c] "AudioUnit")' --info --debug --style compact > "$OUT/log.txt" 2>/dev/null
echo "logs: $OUT/log.txt ($(wc -l < "$OUT/log.txt") lines)"
