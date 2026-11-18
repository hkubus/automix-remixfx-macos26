#!/bin/bash
# run_test.sh N_TRANSITIONS [PLAYLIST] — relaunch Music with RemixFXEnabler, force N AutoMix transitions at low
# volume, then collect the RemixFX engine trace + SonicKit planner logs into logs/<time>/.
#   REMIXFX_VERBOSE=0|1|2 (default 1)   SEEK_BEFORE_END=seconds (default 38)   WAIT=seconds per transition (default 44)
#   REMIXFX_PLANNER27=0|1|2|3 (default 1): macOS 27 selection rules off / decisions / +inputs / +every candidate score
#   START_INDEX=n|last  play the playlist in order from track n (shuffle is turned off for the run, then restored)
# Music's volume is lowered for the test and restored afterwards; playback is paused at the end.
# On this machine library-to-library transitions fail before planning ("NOT NULL constraint failed:
# cloud_library_source.cloud_revision", Apple's content DB). Start on the playlist's last track
# (START_INDEX=last) so the queue runs into Autoplay, whose store tracks plan normally.
# SEEK_BEFORE_END must leave room for the styles under test: the planner only places a transition after the
# current position, so 38 s fits 4-bar styles (18, 20) but not 16-bar ones (12, 17); use ~80 s for those.
N=${1:-3}; PL=${2:-Playlist}
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$HOME/Library/Application Support/RemixFX"
OUT="$HERE/logs/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$OUT"
START=$(date '+%Y-%m-%d %H:%M:%S')
SEEK=${SEEK_BEFORE_END:-38}; WAIT=${WAIT:-44}

"$HERE/install.sh" --stage >/dev/null || { echo "staging failed"; exit 1; }
cp "$BASE/Resources/TransitionStyles.json" "$OUT/served-TransitionStyles.json"

osascript -e 'tell application "Music" to quit' >/dev/null 2>&1
for _ in $(seq 1 60); do pgrep -xq Music || break; sleep 0.5; done
pkill -9 -x Music 2>/dev/null; sleep 1
rm -f /tmp/remixfx-trace.log /tmp/remixfx-trace-*.log
/usr/bin/open -a Music --env "DYLD_INSERT_LIBRARIES=$BASE/RemixFXEnabler.dylib" \
  --env "REMIXFX_VERBOSE=${REMIXFX_VERBOSE:-1}" --env "REMIXFX_PLANNER27=${REMIXFX_PLANNER27:-1}" --stdout /tmp/remixfx-music.log --stderr /tmp/remixfx-music.log
sleep 10
pgrep -xq Music || { echo "Music did not start"; exit 1; }
VOL=$(osascript -e 'tell application "Music" to get sound volume' 2>/dev/null); VOL=${VOL:-100}
osascript -e 'tell application "Music" to set sound volume to 12' >/dev/null 2>&1
SHUF=$(osascript -e 'tell application "Music" to get shuffle enabled' 2>/dev/null)
[ -n "${START_INDEX:-}" ] && osascript -e 'tell application "Music" to set shuffle enabled to false' >/dev/null 2>&1
if [ "${START_INDEX:-}" = "last" ]; then
  for _ in 1 2 3 4 5 6; do
    START_INDEX=$(osascript -e "tell application \"Music\" to get count of tracks of playlist \"$PL\"" 2>/dev/null)
    [ -n "$START_INDEX" ] && break; sleep 3
  done
  echo "starting at track $START_INDEX (last) of $PL"
fi
for attempt in 1 2 3 4 5; do                     # Music may still be loading its library right after launch
  if [ -n "${START_INDEX:-}" ]; then
    osascript -e "tell application \"Music\" to play track $START_INDEX of playlist \"$PL\"" 2>&1
  else
    osascript -e "tell application \"Music\" to play playlist \"$PL\"" 2>&1
  fi
  sleep 6
  [ "$(osascript -e 'tell application "Music" to get player state' 2>/dev/null)" = "playing" ] && break
  echo "   (playback not started yet, retry $attempt)"; sleep 4
done
for i in $(seq 1 "$N"); do
  dur=$(osascript -e 'tell application "Music" to get duration of current track' 2>/dev/null)
  name=$(osascript -e 'tell application "Music" to get (name of current track) & " | " & (artist of current track) & " | " & (genre of current track)' 2>/dev/null)
  echo "[$i] $(date +%H:%M:%S) now: $name ($dur s)" | tee -a "$OUT/run.txt"
  [ -z "$dur" ] && { sleep 5; continue; }
  pos=$(python3 -c "print(max(0, float('$dur') - $SEEK))")
  osascript -e "tell application \"Music\" to set player position to $pos" >/dev/null 2>&1
  sleep "$WAIT"
done
osascript -e 'tell application "Music" to pause' >/dev/null 2>&1
osascript -e "tell application \"Music\" to set sound volume to $VOL" >/dev/null 2>&1
if [ -n "$SHUF" ]; then
  osascript -e "tell application \"Music\" to set shuffle enabled to $SHUF" >/dev/null 2>&1
  # A run that ran into Autoplay (radio) leaves no current playlist, and shuffle can't be set there: reload the
  # playlist muted, set shuffle, stop, then put the volume back.
  if [ "$(osascript -e 'tell application "Music" to get shuffle enabled' 2>/dev/null)" != "$SHUF" ]; then
    osascript -e 'tell application "Music"' -e 'set sound volume to 0' -e "play playlist \"$PL\"" -e 'delay 1.5' \
      -e "set shuffle enabled to $SHUF" -e 'delay 0.5' -e 'stop' -e "set sound volume to $VOL" -e 'end tell' >/dev/null 2>&1
  fi
fi
sleep 2
cp /tmp/remixfx-trace.log "$OUT/trace-music.log" 2>/dev/null
for f in /tmp/remixfx-trace-*.log; do [ -f "$f" ] && cp "$f" "$OUT/"; done
cp /tmp/remixfx-music.log "$OUT/music-stderr.log" 2>/dev/null
/usr/bin/log show --start "$START" --info --debug --style compact --predicate '(process == "Music" AND (subsystem == "com.apple.smarttransitions" OR subsystem BEGINSWITH "com.apple.Sonic" OR eventMessage CONTAINS[c] "DSPGraph" OR eventMessage CONTAINS[c] "AURemixFX" OR eventMessage CONTAINS[c] "Transition setup failed" OR eventMessage CONTAINS[c] "TransitionStyles" OR eventMessage CONTAINS "AudioUnitRender error" OR eventMessage CONTAINS "couldn'"'"'t initialize processor" OR eventMessage CONTAINS "FailedTransitionState")) OR (subsystem == "dev.remixfx" AND category != "engine") OR (process == "AUHostingServiceXPC" AND category == "AUDSPGraph")' > "$OUT/log.txt" 2>/dev/null
/usr/bin/log show --start "$START" --info --debug --style compact --predicate 'subsystem == "dev.remixfx" AND category == "engine"' > "$OUT/engine.log" 2>/dev/null
echo "== results in $OUT"
echo "   transitions set up (strategy - style id, duration):"
grep -E 'Smart transition setup complete' "$OUT/log.txt" | sed -E 's/^[0-9-]+ ([0-9:.]+) .*"strategy": "([^"]*)".*/\1 \2/; s/^([0-9:.]+ [^"]*)$/\1/' | cut -c1-80 | sed 's/^/     /'
grep -E 'Smart transition setup complete' "$OUT/log.txt" | grep -oE '"duration": [0-9.]+' | sed 's/^/       /' | paste -sd' ' -
echo "   beat-matched picks: $(grep -c 'Beat-Matched Filtered Cross-Fade: Styling result identified' "$OUT/log.txt")   mapping errors: $(grep -c 'Mapped automation value missing' "$OUT/log.txt")   setup failures: $(grep -c 'Transition setup failed' "$OUT/log.txt")"
echo "   planner27 (macOS 27 selection rules):"
grep -E '\[planner27\] (installed|genre gates|pick|not installed|.*failed|.*disabled|.*unexpected)' "$OUT/trace-music.log" 2>/dev/null | sed -E 's/^ *[0-9.]+ \[planner27\] //' | cut -c1-230 | sed 's/^/     /' | head -40
echo "   graph:"; grep -E 'failed to create graph|couldn.t initialize processor' "$OUT/log.txt" | sed -E 's/^.*\] //' | sort | uniq -c | head -5
echo "   render errors: $(grep -c 'AudioUnitRender error' "$OUT/log.txt")   failed transitions: $(grep -c 'STATE ENTERED           FailedTransitionState' "$OUT/log.txt")"
echo "   engine (all processes):"; grep -E 'loaded in|ACTIVATE|DEACTIVATE|BPM from|NOTE|   [a-z]+_[a-z]+ +=' "$OUT/engine.log" | sed -E 's/^[0-9-]+ //; s/ [A-Za-z]+\[[0-9]+:[0-9a-f]+\] \[dev.remixfx:engine\]//' | cut -c1-200 | sed 's/^/     /' | head -80
