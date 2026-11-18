// EnhancedAutoMix — backports macOS 27 "Enhanced AutoMix" pieces into Music on macOS 26 (x86_64).
//
// Builds on AutoMixEnabler (same 5 method overrides, so this dylib replaces it) and adds:
//   * Resource redirection: the SonicKit planner loads TransitionStyles.json and DSPGraph.dspg via
//     NSBundle. We serve our own copies from ~/Library/Application Support/EnhancedAutoMix/Resources
//     (27's style catalog / DSP graph, adapted for 26) without touching the sealed system volume.
//   * Optionally loads a relinked copy of 27's SonicAudioUnits (AURemixFX, aufx/remx/appl) and calls
//     registerSonicAudioUnits() so the 27 DSP graph can instantiate it in-process.
//
// Loaded into Music via DYLD_INSERT_LIBRARIES (needs AMFI relaxed). Only acts inside com.apple.Music.

#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <os/log.h>
#import <dlfcn.h>

static os_log_t gLog;
static NSString *gResDir;

static void Override(Class cls, SEL sel, BOOL isClassMethod, id block) {
    if (!cls) { os_log_error(gLog, "class missing for %{public}s", sel_getName(sel)); return; }
    Method m = isClassMethod ? class_getClassMethod(cls, sel) : class_getInstanceMethod(cls, sel);
    if (!m) { os_log_error(gLog, "method missing: %{public}c[%{public}s %{public}s]",
                           isClassMethod ? '+' : '-', class_getName(cls), sel_getName(sel)); return; }
    method_setImplementation(m, imp_implementationWithBlock(block));
    os_log(gLog, "patched %{public}c[%{public}s %{public}s]",
           isClassMethod ? '+' : '-', class_getName(cls), sel_getName(sel));
}

// ---- resource redirection ----------------------------------------------------------------------

static NSString *RedirectPath(NSString *name, NSString *ext) {
    if (!name) return nil;
    NSString *base = name, *e = ext;
    if (!e.length && name.pathExtension.length) { base = name.stringByDeletingPathExtension; e = name.pathExtension; }
    if (!([base isEqualToString:@"TransitionStyles"] && [e isEqualToString:@"json"]) &&
        !([base isEqualToString:@"DSPGraph"] && [e isEqualToString:@"dspg"])) return nil;
    NSString *p = [gResDir stringByAppendingPathComponent:[base stringByAppendingPathExtension:e]];
    return [NSFileManager.defaultManager fileExistsAtPath:p] ? p : nil;
}

static IMP gOrigURLExt, gOrigURLExtSub, gOrigPathType, gOrigPathTypeDir;

static NSURL *H_URLExt(NSBundle *self, SEL _cmd, NSString *name, NSString *ext) {
    NSString *p = RedirectPath(name, ext);
    if (p) { os_log(gLog, "redirect %{public}@.%{public}@ (%{public}@) -> %{public}@", name, ext, self.bundleIdentifier, p);
             return [NSURL fileURLWithPath:p]; }
    return ((NSURL *(*)(id, SEL, id, id))gOrigURLExt)(self, _cmd, name, ext);
}
static NSURL *H_URLExtSub(NSBundle *self, SEL _cmd, NSString *name, NSString *ext, NSString *sub) {
    NSString *p = RedirectPath(name, ext);
    if (p) { os_log(gLog, "redirect(sub) %{public}@.%{public}@ (%{public}@) -> %{public}@", name, ext, self.bundleIdentifier, p);
             return [NSURL fileURLWithPath:p]; }
    return ((NSURL *(*)(id, SEL, id, id, id))gOrigURLExtSub)(self, _cmd, name, ext, sub);
}
static NSString *H_PathType(NSBundle *self, SEL _cmd, NSString *name, NSString *ext) {
    NSString *p = RedirectPath(name, ext);
    if (p) { os_log(gLog, "redirect(path) %{public}@.%{public}@ -> %{public}@", name, ext, p); return p; }
    return ((NSString *(*)(id, SEL, id, id))gOrigPathType)(self, _cmd, name, ext);
}
static NSString *H_PathTypeDir(NSBundle *self, SEL _cmd, NSString *name, NSString *ext, NSString *dir) {
    NSString *p = RedirectPath(name, ext);
    if (p) { os_log(gLog, "redirect(pathdir) %{public}@.%{public}@ -> %{public}@", name, ext, p); return p; }
    return ((NSString *(*)(id, SEL, id, id, id))gOrigPathTypeDir)(self, _cmd, name, ext, dir);
}

static void Hook(SEL sel, IMP repl, IMP *orig) {
    Method m = class_getInstanceMethod(NSBundle.class, sel);
    if (!m) { os_log_error(gLog, "NSBundle %{public}s missing", sel_getName(sel)); return; }
    *orig = method_setImplementation(m, repl);
    os_log(gLog, "hooked -[NSBundle %{public}s]", sel_getName(sel));
}

// ---- 27 SonicAudioUnits (relinked) --------------------------------------------------------------

static void LoadSonicAudioUnits(void) {
    NSString *path = [[gResDir stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"SonicAudioUnits27.dylib"];
    if (![NSFileManager.defaultManager fileExistsAtPath:path]) { os_log(gLog, "no SonicAudioUnits27.dylib; REMIX_FX styles unavailable"); return; }
    void *h = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
    if (!h) { os_log_error(gLog, "dlopen SonicAudioUnits27 failed: %{public}s", dlerror()); return; }
    void (*reg)(void) = (void (*)(void))dlsym(h, "registerSonicAudioUnits");
    if (!reg) { os_log_error(gLog, "registerSonicAudioUnits missing"); return; }
    reg();
    AudioComponentDescription d = { kAudioUnitType_Effect, 'remx', kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    os_log(gLog, "registerSonicAudioUnits() done; aufx/remx/appl %{public}s", c ? "FOUND" : "NOT found");
}

__attribute__((constructor))
static void EnhancedAutoMixInit(void) {
    NSString *bundleID = NSBundle.mainBundle.bundleIdentifier;
    if (![bundleID isEqualToString:@"com.apple.Music"]) return;
    gLog = os_log_create("dev.enhancedautomix", "init");
    gResDir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/EnhancedAutoMix/Resources"];

    // --- AutoMix unlock (same as AutoMixEnabler) ---
    Class engine = NSClassFromString(@"MPCPlaybackEngine");
    Override(engine, @selector(deviceSupportsSmartTransitions), YES, ^BOOL(id _self) { return YES; });
    Override(engine, @selector(deviceSupportsTransitions), YES, ^BOOL(id _self) { return YES; });
    Override(NSClassFromString(@"MusicPlaybackPrefsModel"), @selector(hasAlchemy), NO, ^BOOL(id _self) { return YES; });
    Class queue = NSClassFromString(@"ITPlayQueueModel");
    Override(queue, @selector(mixingAvailable), NO, ^BOOL(id _self) { return YES; });
    Override(queue, @selector(mixingEnabled), NO, ^BOOL(id _self) {
        id player = ((id (*)(id, SEL))objc_msgSend)(_self, @selector(player));
        id state = ((id (*)(id, SEL))objc_msgSend)(player, @selector(playerControlsState));
        return state ? ((BOOL (*)(id, SEL))objc_msgSend)(state, @selector(isTransitionsSupported)) : NO;
    });

    // --- Enhanced AutoMix ---
    Hook(@selector(URLForResource:withExtension:), (IMP)H_URLExt, &gOrigURLExt);
    Hook(@selector(URLForResource:withExtension:subdirectory:), (IMP)H_URLExtSub, &gOrigURLExtSub);
    Hook(@selector(pathForResource:ofType:), (IMP)H_PathType, &gOrigPathType);
    Hook(@selector(pathForResource:ofType:inDirectory:), (IMP)H_PathTypeDir, &gOrigPathTypeDir);
    LoadSonicAudioUnits();
    os_log(gLog, "EnhancedAutoMix loaded; resources from %{public}@", gResDir);
}
