// AutoMixEnabler — re-enables Apple Music AutoMix on Intel Macs (macOS 26).
//
// Why it's off on Intel:
//   * Music.app's x86_64 slice has the Sonic/Alchemy (AutoMix) gate compiled to
//     a constant NO: -[MusicPlaybackPrefsModel hasAlchemy],
//     -[ITPlayQueueModel mixingAvailable] and -[ITPlayQueueModel mixingEnabled].
//     (On arm64e these call os_feature_enabled(Sonic, Alchemy), which is ON.)
//   * MediaPlaybackCore's x86_64 +[MPCPlaybackEngine deviceSupportsSmartTransitions]
//     is `SupportsSmartTransitionsOverride && os_variant_has_internal_content("com.apple.amp")`,
//     i.e. Apple-internal builds only. When it's NO the AVPlayer is never opted into
//     speed ramps and every AutoMix transition fails with
//     SmartPlayerItemTransitionError.playerCannotSpeedRamp.
//
// This dylib is loaded into Music via DYLD_INSERT_LIBRARIES (needs AMFI relaxed)
// and overrides only those methods, only inside com.apple.Music.

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <os/log.h>

static os_log_t gLog;

static void Override(Class cls, SEL sel, BOOL isClassMethod, id block) {
    if (!cls) { os_log_error(gLog, "class missing for %{public}s", sel_getName(sel)); return; }
    Method m = isClassMethod ? class_getClassMethod(cls, sel) : class_getInstanceMethod(cls, sel);
    if (!m) { os_log_error(gLog, "method missing: %{public}c[%{public}s %{public}s]",
                           isClassMethod ? '+' : '-', class_getName(cls), sel_getName(sel)); return; }
    method_setImplementation(m, imp_implementationWithBlock(block));
    os_log(gLog, "patched %{public}c[%{public}s %{public}s]",
           isClassMethod ? '+' : '-', class_getName(cls), sel_getName(sel));
}

__attribute__((constructor))
static void AutoMixEnablerInit(void) {
    NSString *bundleID = NSBundle.mainBundle.bundleIdentifier;
    if (![bundleID isEqualToString:@"com.apple.Music"]) return;
    gLog = os_log_create("dev.automixenabler", "init");

    // Playback engine: let MediaPlaybackCore opt the AVPlayer into speed ramps.
    Class engine = NSClassFromString(@"MPCPlaybackEngine");
    Override(engine, @selector(deviceSupportsSmartTransitions), YES, ^BOOL(id _self) { return YES; });
    Override(engine, @selector(deviceSupportsTransitions), YES, ^BOOL(id _self) { return YES; });

    // Settings > Playback: show the Crossfade / AutoMix song-transition popup.
    Override(NSClassFromString(@"MusicPlaybackPrefsModel"), @selector(hasAlchemy), NO,
             ^BOOL(id _self) { return YES; });

    // Up Next / play queue AutoMix button (mirrors the arm64e logic).
    Class queue = NSClassFromString(@"ITPlayQueueModel");
    Override(queue, @selector(mixingAvailable), NO, ^BOOL(id _self) { return YES; });
    Override(queue, @selector(mixingEnabled), NO, ^BOOL(id _self) {
        id player = ((id (*)(id, SEL))objc_msgSend)(_self, @selector(player));
        id state = ((id (*)(id, SEL))objc_msgSend)(player, @selector(playerControlsState));
        return state ? ((BOOL (*)(id, SEL))objc_msgSend)(state, @selector(isTransitionsSupported)) : NO;
    });
}
