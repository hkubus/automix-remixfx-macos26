#!/bin/bash
# Opens Music with AutoMixEnabler loaded. If Music is already running without it, offer a restart.
DYLIB="$HOME/Library/Application Support/AutoMixEnabler/AutoMixEnabler.dylib"
PID=$(pgrep -x Music | head -1)
if [[ -n "$PID" ]]; then
  if lsof -p "$PID" 2>/dev/null | grep -q AutoMixEnabler; then open -a Music; exit 0; fi
  ANSWER=$(osascript -e 'button returned of (display dialog "Music is running without AutoMix. Restart it?" buttons {"Not Now","Restart"} default button "Restart" with title "Music (AutoMix)")' 2>/dev/null)
  [[ "$ANSWER" == "Restart" ]] || { open -a Music; exit 0; }
  osascript -e 'tell application "Music" to quit' >/dev/null 2>&1
  for _ in $(seq 1 40); do pgrep -xq Music || break; sleep 0.25; done
fi
exec /usr/bin/open -a Music --env "DYLD_INSERT_LIBRARIES=$DYLIB"
