// component.m — RemixFX.component: an AU v2 component bundle that serves aufx/remx/appl in *any* process.
//
// Why a bundle: on macOS 26, AudioQueue opens the SmartTransitions DSPGraph AU (aufx/dspg/appl) out-of-process,
// in AUHostingServiceXPC. A component registered only inside Music (AudioComponentRegister) is invisible there,
// so the graph fails with `box component "('aufx' 'remx' 'appl')" not found`. Installed in
// ~/Library/Audio/Plug-Ins/Components, this bundle is discovered by the AudioComponent registrar and loaded by
// whichever process builds the graph.
//
// On first instantiation it maps Apple's transplanted x86_64 SonicAudioUnits image from its Resources (sauload.c),
// captures AURemixFX's real factory, and hands out instances wrapped by the Path A engine (rxdrive.c), which
// replays macOS 27's REMIX_FX envelopes (Resources/RemixFXStyles.json) when the transition carrier arrives.
#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include <libgen.h>
#include <unistd.h>
#include "rxdrive.h"

void* sau_load(const char* dylib, const char* fixups);
void  sau_set_bind_hook(void* (*hook)(const char* name, void* resolved));
int   rx_load_envelopes(NSString* path);

static int gReady;

static void Setup(void) {
    @autoreleasepool {
        Dl_info di = { 0 };
        dladdr((const void*)Setup, &di);
        NSString* exe = di.dli_fname ? [NSString stringWithUTF8String:di.dli_fname] : @"";
        NSString* res = [[[exe stringByDeletingLastPathComponent] stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Resources"];
        NSString* proc = NSProcessInfo.processInfo.processName ?: @"proc";
        char path[512];
        snprintf(path, sizeof path, "/tmp/remixfx-trace-%s-%d.log", proc.UTF8String, getpid());
        const char* vb = getenv("REMIXFX_VERBOSE");
        rx_trace_open2(path, vb ? atoi(vb) : 1, 1);           // file if writable, plus the unified log
        rx_log("RemixFX.component loaded in %s (pid %d) from %s", proc.UTF8String, getpid(), exe.UTF8String);
        int n = rx_load_envelopes([res stringByAppendingPathComponent:@"RemixFXStyles.json"]);
        if (n <= 0) rx_log("no RemixFX envelopes — the AU will work but transitions won't be driven");
        rx_set_capture_only(1);
        sau_set_bind_hook(rx_bind_hook);
        void (*reg)(void) = (void (*)(void))sau_load([res stringByAppendingPathComponent:@"SonicAudioUnits"].fileSystemRepresentation,
                                                     [res stringByAppendingPathComponent:@"SonicAudioUnits.fixups"].fileSystemRepresentation);
        if (!reg) { rx_log("sau_load failed"); return; }
        reg();
        gReady = rx_has_factory();
        rx_log("AURemixFX factory %s", gReady ? "captured" : "NOT captured");
    }
}

__attribute__((visibility("default")))
void* RemixFXFactory(const AudioComponentDescription* desc) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ Setup(); });
    return gReady ? rx_factory(desc) : NULL;
}
