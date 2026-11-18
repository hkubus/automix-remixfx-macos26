// rxenv.m — load macOS 27's REMIX_FX automation lanes (TransitionStyles.json format) into rxdrive's envelopes.
#import <Foundation/Foundation.h>
#include "rxdrive.h"

static double Rel(id t, BOOL* hadSeconds) {
    NSDictionary* d = [t isKindOfClass:NSDictionary.class] ? t : nil;
    NSDictionary* def = [d[@"default"] isKindOfClass:NSDictionary.class] ? d[@"default"] : d;
    if (def[@"offsetInSeconds"] && hadSeconds) *hadSeconds = YES;
    NSNumber* r = def[@"relative"];
    return [r isKindOfClass:NSNumber.class] ? r.doubleValue : 0;
}
static double Val(id v, int* bpm, NSString** other) {
    NSDictionary* d = [v isKindOfClass:NSDictionary.class] ? v : nil;
    if ([d[@"default"] isKindOfClass:NSNumber.class]) return [d[@"default"] doubleValue];
    NSString* pn = d[@"parameterName"];
    if ([pn isEqual:@"bpm"]) { *bpm = 1; return 120; }
    if (pn && other) *other = pn;
    return 0;
}
static void Curve(NSString* s, int* c, float* k) {
    *c = RX_CURVE_LINEAR; *k = 1;
    if ([s hasPrefix:@"ease-in-"])  { *c = RX_CURVE_IN;  *k = [[s substringFromIndex:8] floatValue]; }
    else if ([s hasPrefix:@"ease-out-"]) { *c = RX_CURVE_OUT; *k = [[s substringFromIndex:9] floatValue]; }
    else if (![s isEqual:@"linear"]) rx_log("envelope: unknown interpolation '%s' -> linear", s.UTF8String);
    if (*k <= 0) *k = 1;
}

// Returns the number of (style, side) envelopes loaded, or -1 on error.
int rx_load_envelopes(NSString* path) {
    NSData* data = [NSData dataWithContentsOfFile:path];
    if (!data) { rx_log("envelopes: cannot read %s", path.UTF8String); return -1; }
    NSError* err = nil;
    NSArray* styles = [NSJSONSerialization JSONObjectWithData:data options:0 error:&err];
    if (![styles isKindOfClass:NSArray.class]) { rx_log("envelopes: bad JSON in %s: %s", path.UTF8String, err.description.UTF8String); return -1; }
    int nenv = 0;
    for (NSDictionary* st in styles) {
        if (![st isKindOfClass:NSDictionary.class]) continue;
        int sid = [st[@"id"] intValue];
        NSString* name = st[@"name"] ?: @"";
        float bars = [st[@"duration"] isKindOfClass:NSNumber.class] ? [st[@"duration"] floatValue] : 8;
        NSArray* sides = @[@"outgoing", @"incoming"];
        for (int side = 0; side < 2; side++) {
            NSArray* insts = st[@"instructions"][sides[side]];
            BOOL defined = NO;
            for (NSDictionary* in in insts) {
                NSArray* autos = in[@"automations"];
                BOOL remix = [in[@"name"] isEqual:@"REMIX_FX"];
                for (NSDictionary* a in autos) if ([a[@"parameterId"] hasPrefix:@"remixfx_"]) remix = YES;
                if (!remix) continue;
                if (!defined) { rx_env_define(sid, side, name.UTF8String, bars); defined = YES; nenv++; }
                BOOL secs = NO;
                double pl0 = in[@"placement"] ? Rel(in[@"placement"][@"start"], &secs) : 0;
                double pl1 = in[@"placement"] ? Rel(in[@"placement"][@"end"], &secs) : 1;
                if (!in[@"placement"][@"end"]) pl1 = 1;
                for (NSDictionary* a in autos) {
                    NSString* pid = a[@"parameterId"];
                    int p = rx_param_index(pid.UTF8String);
                    if (p < 0) { rx_log("envelopes: style %d: unknown parameter %s (skipped)", sid, pid.UTF8String); continue; }
                    RXSeg s = { 0 };
                    double t0 = Rel(a[@"startTime"], &secs), t1 = Rel(a[@"endTime"], &secs);
                    s.t0 = (float)(pl0 + t0 * (pl1 - pl0)); s.t1 = (float)(pl0 + t1 * (pl1 - pl0));
                    NSString* other = nil;
                    s.v0 = (float)Val(a[@"startValue"], &s.bpm, &other);
                    s.v1 = (float)Val(a[@"endValue"], &s.bpm, &other);
                    if (other) rx_log("envelopes: style %d %s: value source '%s' unsupported (0 used)", sid, pid.UTF8String, other.UTF8String);
                    Curve(a[@"interpolation"] ?: @"linear", &s.curve, &s.k);
                    int r = rx_env_add(sid, side, p, s);
                    if (r) rx_log("envelopes: style %d %s: add failed (%d)", sid, pid.UTF8String, r);
                }
                if (secs) rx_log("envelopes: style %d %s: offsetInSeconds present (ignored)", sid, [sides[side] UTF8String]);
            }
        }
    }
    rx_log("envelopes: loaded %d RemixFX envelopes from %s", nenv, path.UTF8String);
    return nenv;
}
