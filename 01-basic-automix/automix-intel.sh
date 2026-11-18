#!/bin/bash
# automix-intel.sh — enable Apple Music AutoMix on Intel Macs (macOS 26+).
#
#   ./automix-intel.sh              install (or update), then (re)launch Music
#   ./automix-intel.sh --no-launch  install only; don't touch a running Music
#   ./automix-intel.sh uninstall    remove everything (quits Music)
#
# Requirements: Intel Mac, macOS 26+, Xcode Command Line Tools, and AMFI relaxed
# (boot-arg amfi_get_out_of_my_way=1, as on most OpenCore Hackintoshes). Nothing on
# the system volume is modified; Music is only patched when opened via the launcher.
set -euo pipefail

SUPPORT="$HOME/Library/Application Support/AutoMixEnabler"
DYLIB="$SUPPORT/AutoMixEnabler.dylib"
APP="$HOME/Applications/Music (AutoMix).app"
MUSIC=/System/Applications/Music.app

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

quit_music() {
  pgrep -xq Music || return 0
  osascript -e 'tell application "Music" to quit' >/dev/null 2>&1 || true
  for _ in $(seq 1 40); do pgrep -xq Music || return 0; sleep 0.25; done
  die "Music didn't quit; quit it manually and re-run."
}

if [[ "${1:-}" == "uninstall" ]]; then
  quit_music
  rm -rf "$SUPPORT" "$APP"
  defaults delete com.apple.Music TransitionStyle 2>/dev/null || true
  say "Removed. Music will open normally (Crossfade/AutoMix settings reset)."
  exit 0
fi

# --- checks ------------------------------------------------------------------
[[ "$(uname -m)" == "x86_64" ]] || die "This Mac isn't Intel — AutoMix is already supported natively."
[[ "$(sw_vers -productVersion | cut -d. -f1)" -ge 26 ]] || die "AutoMix needs macOS 26 or later."
xcode-select -p >/dev/null 2>&1 || die "Install the Command Line Tools first: xcode-select --install"
if ! sysctl -n kern.bootargs 2>/dev/null | grep -Eq 'amfi_get_out_of_my_way=(1|0x1)|amfi=0x80'; then
  warn "AMFI doesn't look relaxed (no amfi_get_out_of_my_way=1 boot-arg)."
  warn "macOS will likely refuse to load the patch into Music; continuing anyway."
fi

# --- build the patch -----------------------------------------------------------
say "Building AutoMixEnabler.dylib"
mkdir -p "$SUPPORT"
SRC="$(mktemp -d)/AutoMixEnabler.m"
cat > "$SRC" <<'OBJC'
// Re-enables AutoMix in Music.app on x86_64. The Intel build of Music hard-codes the
// AutoMix ("Alchemy") gate to NO, and MediaPlaybackCore only allows smart transitions
// (speed-ramped AVPlayer) on Apple-internal installs. Override just those methods.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <os/log.h>

static os_log_t gLog;
static void Override(Class cls, SEL sel, BOOL meta, id block) {
    Method m = !cls ? NULL : meta ? class_getClassMethod(cls, sel) : class_getInstanceMethod(cls, sel);
    if (!m) { os_log_error(gLog, "missing %{public}s", sel_getName(sel)); return; }
    method_setImplementation(m, imp_implementationWithBlock(block));
    os_log(gLog, "patched %{public}c[%{public}s %{public}s]", meta ? '+' : '-', class_getName(cls), sel_getName(sel));
}

__attribute__((constructor)) static void Init(void) {
    if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.Music"]) return;
    gLog = os_log_create("dev.automixenabler", "init");
    Class engine = NSClassFromString(@"MPCPlaybackEngine");
    Override(engine, @selector(deviceSupportsSmartTransitions), YES, ^BOOL(id s) { return YES; });
    Override(engine, @selector(deviceSupportsTransitions), YES, ^BOOL(id s) { return YES; });
    Override(NSClassFromString(@"MusicPlaybackPrefsModel"), @selector(hasAlchemy), NO, ^BOOL(id s) { return YES; });
    Class queue = NSClassFromString(@"ITPlayQueueModel");
    Override(queue, @selector(mixingAvailable), NO, ^BOOL(id s) { return YES; });
    Override(queue, @selector(mixingEnabled), NO, ^BOOL(id s) {
        id player = ((id (*)(id, SEL))objc_msgSend)(s, @selector(player));
        id state = ((id (*)(id, SEL))objc_msgSend)(player, @selector(playerControlsState));
        return state ? ((BOOL (*)(id, SEL))objc_msgSend)(state, @selector(isTransitionsSupported)) : NO;
    });
}
OBJC
clang -arch x86_64 -dynamiclib -fobjc-arc -O2 -framework Foundation -o "$SRC.dylib" "$SRC"
codesign -f -s - "$SRC.dylib" 2>/dev/null
mv -f "$SRC.dylib" "$DYLIB"   # atomic replace: safe even if a running Music has the old one loaded
rm -rf "$(dirname "$SRC")"

# --- launcher app --------------------------------------------------------------
say "Creating launcher: $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cat > "$APP/Contents/MacOS/launcher" <<'SH'
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
SH
chmod +x "$APP/Contents/MacOS/launcher"
cp "$MUSIC/Contents/Resources/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null || true
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>local.automixenabler.launcher</string>
  <key>CFBundleName</key><string>Music (AutoMix)</string>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSUIElement</key><true/>
</dict></plist>
PLIST
codesign -f -s - "$APP" 2>/dev/null

# --- turn AutoMix on -------------------------------------------------------------
say "Setting Song Transitions to AutoMix"
defaults write com.apple.Music TransitionsEnabled -bool true
defaults write com.apple.Music TransitionStyle -int 1

# --- launch & verify -------------------------------------------------------------
if [[ "${1:-}" == "--no-launch" ]]; then
  say "Installed. Quit Music when convenient, then open \"Music (AutoMix)\"."
  exit 0
fi
if pgrep -xq Music; then
  read -r -p "Music is running and must restart to load the patch. Restart now? [Y/n] " REPLY </dev/tty || REPLY=n
  [[ "$REPLY" =~ ^[Nn] ]] && { say "Done. Quit Music, then open \"Music (AutoMix)\"."; exit 0; }
  quit_music
fi
say "Launching Music via the launcher"
open "$APP"
sleep 8
PATCHED=$(/usr/bin/log show --last 30s --predicate 'subsystem == "dev.automixenabler"' 2>/dev/null | grep -c patched || true)
if [[ "$PATCHED" -ge 5 ]]; then
  say "AutoMix enabled ($PATCHED/5 patches applied)."
  say "Always open Music via \"Music (AutoMix)\" (drag it to your Dock in place of Music)."
else
  die "Music started but the patch didn't load ($PATCHED/5). AMFI is probably still enforcing; see the note at the top of this script."
fi
