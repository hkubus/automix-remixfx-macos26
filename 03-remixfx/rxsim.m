// rxsim.m — offline validation of the Path A engine (rxdrive) without Music.
// Plays the role of SonicKit/DSPGraph: sets the carrier pseudo-params per slice exactly as the generated
// catalog would, renders a synthetic 120 BPM drum loop through Apple's AURemixFX, and writes a WAV.
//
//   rxsim <SonicAudioUnits> <fixups> <RemixFXStyles.json> <style> <side 0|1> <process|render> <out.wav> [bpm] [slice]
//   RXSIM_BEATLEN=1  also send seconds/beat on pseudo 1002 (the shipped catalog doesn't: tempo comes from the ramp slope)
//   RXSIM_TRUNC=f    also run the other side in a second instance whose region is truncated to the last f of the
//                    transition (its carrier parks at its start value, like a prerolled incoming graph, then ramps
//                    over the tail) — exercises the partner-based span detection (expected tempo: the same bpm)
#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include "rxdrive.h"

void* sau_load(const char* dylib, const char* fixups);
void  sau_set_bind_hook(void* (*hook)(const char*, void*));
int   rx_load_envelopes(NSString* path);

#define SR 44100.0
static double gBPM = 120;
static double noise(long i, int k) {             // deterministic white noise in [-1,1] (repeatable A/B renders)
    uint64_t x = (uint64_t)i * 0x9E3779B97F4A7C15ull + (uint64_t)k * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 31; x *= 0x94D049BB133111EBull; x ^= x >> 29;
    return (double)(x >> 11) / (double)(1ull << 53) * 2 - 1;
}
static float drum(long i, int ch) {
    double t = i / SR, beat = 60.0 / gBPM, bt = fmod(t, beat), et = fmod(t, beat / 2);
    long b = (long)(t / beat);
    double s = 0;
    s += 0.8 * sin(2 * M_PI * (50 + 80 * exp(-bt * 30)) * bt) * exp(-bt * 8);                    // kick every beat
    if (b % 2 == 1) s += 0.35 * noise(i, 2 * ch) * exp(-bt * 18);      // snare 2 & 4
    s += 0.12 * noise(i, 2 * ch + 1) * exp(-et * 60);                       // hats 8ths
    double f = (b / 4) % 2 ? 220.0 : 196.0;                                                         // 2-chord pad, per bar
    s += 0.08 * (sin(2 * M_PI * f * t) + sin(2 * M_PI * f * 1.26 * t + ch) + sin(2 * M_PI * f * 1.5 * t));
    long e8 = (long)(t / (beat / 2));                                                               // 8th-note lead, pitch changes every 8th
    static const double scale[8] = { 440, 494, 523, 587, 659, 698, 784, 880 };
    s += 0.12 * sin(2 * M_PI * scale[(e8 * 5 + e8 / 3) % 8] * t) * exp(-et * 6);
    return (float)(0.6 * s);
}
static float* gIn[2];
static OSStatus feed(void* r, AudioUnitRenderActionFlags* fl, const AudioTimeStamp* ts, UInt32 bus, UInt32 n, AudioBufferList* io) {
    long at = (long)ts->mSampleTime;
    for (UInt32 b = 0; b < io->mNumberBuffers; b++) memcpy(io->mBuffers[b].mData, gIn[b] + at, n * 4);
    return noErr;
}
int main(int argc, char** argv) {
    @autoreleasepool {
    if (argc < 8) { fprintf(stderr, "usage: rxsim SAU FIXUPS STYLES.json STYLE SIDE process|render OUT.wav [bpm] [slice]\n"); return 2; }
    int style = atoi(argv[4]), side = atoi(argv[5]); BOOL useProcess = !strcmp(argv[6], "process");
    if (argc > 8) gBPM = atof(argv[8]);
    UInt32 SL = argc > 9 ? (UInt32)atoi(argv[9]) : 512;
    rx_trace_open("/dev/stdout", 1);
    if (rx_load_envelopes([NSString stringWithUTF8String:argv[3]]) <= 0) return 1;
    rx_env_dump();
    sau_set_bind_hook(rx_bind_hook);
    void (*reg)(void) = sau_load(argv[1], argv[2]); if (!reg) return 1; reg();

    AudioComponentDescription d = { kAudioUnitType_Effect, 'remx', 'appl', 0, 0 };
    AudioUnit au; OSStatus st = AudioComponentInstanceNew(AudioComponentFindNext(NULL, &d), &au);
    double trunc = getenv("RXSIM_TRUNC") ? atof(getenv("RXSIM_TRUNC")) : 0;
    AudioUnit au2 = NULL;
    if (trunc > 0 && useProcess) AudioComponentInstanceNew(AudioComponentFindNext(NULL, &d), &au2);
    AudioStreamBasicDescription f = { 0 }; f.mSampleRate = SR; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4; f.mBytesPerPacket = 4;
    AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
    UInt32 mf = SL > 4096 ? SL : 4096; AudioUnitSetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mf, 4);
    if (!useProcess) { AURenderCallbackStruct cb = { feed, NULL }; AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb); }
    UInt32 byp = 1; AudioUnitSetProperty(au, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &byp, 4);   // like the graph's `bypa` default
    st = AudioUnitInitialize(au); printf("init=%d\n", (int)st);
    if (au2) {
        AudioUnitSetProperty(au2, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
        AudioUnitSetProperty(au2, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
        AudioUnitSetProperty(au2, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mf, 4);
        printf("partner init=%d (side %d, region = last %.2f of the transition)\n", (int)AudioUnitInitialize(au2), side ^ 1, trunc);
    }
    // carrier params must be visible in the parameter list (in case DSPGraph validates wiring against it)
    UInt32 sz = 0; AudioUnitGetPropertyInfo(au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &sz, NULL);
    printf("ParameterList size=%u (%u ids)\n", sz, sz / 4);

    // timeline: 2 bars pre-roll, transition of N bars, 3 bars after
    double beatLen = 60.0 / gBPM;
    float bars = style == 17 ? 16 : (style == 18 || style == 20) ? 4 : 8;
    long pre = (long)(8 * beatLen * SR), len = (long)(bars * 4 * beatLen * SR), post = (long)(12 * beatLen * SR);
    long total = pre + len + post;
    for (int c = 0; c < 2; c++) { gIn[c] = malloc(total * 4 + 8192 * 4); for (long i = 0; i < total + 8192; i++) gIn[c][i] = drum(i, c); }
    float* out[2] = { malloc(total * 4), malloc(total * 4) }, *out2[2] = { malloc(total * 4), malloc(total * 4) };
    int sendBeat = getenv("RXSIM_BEATLEN") && atoi(getenv("RXSIM_BEATLEN"));
    AudioBufferList* abl = malloc(sizeof(AudioBufferList) + sizeof(AudioBuffer));
    for (long pos = 0; pos < total; pos += SL) {
        UInt32 n = (UInt32)((total - pos) < SL ? (total - pos) : SL);
        // what SonicKit would set before rendering this slice
        // idle: SonicKit's aufilter_center_freq default (2500) — must not trigger anything
        double carrier = 2500;
        if (pos >= pre) { double p = (double)(pos - pre) / len; if (p > 1) p = 1; carrier = rx_carrier_value(style, side, p); }
        if (pos >= pre) { if (sendBeat) AudioUnitSetParameter(au, RX_PSEUDO_BEATLEN, kAudioUnitScope_Global, 0, beatLen, 0);
                          AudioUnitSetParameter(au, RX_PSEUDO_SLOT, kAudioUnitScope_Global, 0, 8, 0); }
        AudioUnitSetParameter(au, RX_PSEUDO_PROGRESS, kAudioUnitScope_Global, 0, (float)carrier, 0);
        abl->mNumberBuffers = 2;
        for (int c = 0; c < 2; c++) { abl->mBuffers[c].mNumberChannels = 1; abl->mBuffers[c].mDataByteSize = n * 4; abl->mBuffers[c].mData = out[c] + pos; }
        AudioUnitRenderActionFlags fl = 0; AudioTimeStamp ts = { 0 }; ts.mFlags = kAudioTimeStampSampleTimeValid; ts.mSampleTime = pos;
        if (useProcess) { for (int c = 0; c < 2; c++) memcpy(out[c] + pos, gIn[c] + pos, n * 4); st = AudioUnitProcess(au, &fl, &ts, n, abl); }
        else st = AudioUnitRender(au, &fl, &ts, 0, n, abl);
        if (au2) {          // partner: parked at its start value from the pre-roll on, ramps over the last `trunc`
            double c2 = 2500;
            if (pos >= pre) { double p = (double)(pos - pre) / len, q = (p - (1 - trunc)) / trunc;
                              c2 = rx_carrier_value(style, side ^ 1, q < 0 ? 0 : q > 1 ? 1 : q); }
            AudioUnitSetParameter(au2, RX_PSEUDO_PROGRESS, kAudioUnitScope_Global, 0, (float)c2, 0);
            AudioBufferList* b2 = malloc(sizeof(AudioBufferList) + sizeof(AudioBuffer)); b2->mNumberBuffers = 2;
            for (int c = 0; c < 2; c++) { memcpy(out2[c] + pos, gIn[c] + pos, n * 4); b2->mBuffers[c].mNumberChannels = 1;
                                          b2->mBuffers[c].mDataByteSize = n * 4; b2->mBuffers[c].mData = out2[c] + pos; }
            AudioUnitRenderActionFlags f2 = 0; AudioUnitProcess(au2, &f2, &ts, n, b2); free(b2);
        }
        if (st) { printf("render error %d at %ld\n", (int)st, pos); break; }
    }
    usleep(100000);
    // per-beat difference report
    printf("\nbeat  phase       rms_in  rms_out  |out-in|/rms_in\n");
    long bl = (long)(beatLen * SR);
    for (long b = 0; b * bl < total; b++) {
        double si = 0, so = 0, sd = 0; long s0 = b * bl, s1 = s0 + bl > total ? total : s0 + bl;
        for (long i = s0; i < s1; i++) { si += gIn[0][i] * gIn[0][i]; so += out[0][i] * out[0][i]; sd += (out[0][i] - gIn[0][i]) * (out[0][i] - gIn[0][i]); }
        double ri = sqrt(si / (s1 - s0)), ro = sqrt(so / (s1 - s0)), rd = sqrt(sd / (s1 - s0));
        const char* ph = s0 < pre ? "pre" : s0 < pre + len ? "TRANSITION" : "post";
        double p = s0 < pre ? 0 : (double)(s0 - pre) / len;
        printf("%4ld  %-10s p=%.3f %6.3f  %6.3f   %6.3f\n", b, ph, p, ri, ro, ri > 0 ? rd / ri : 0);
    }
    // WAV out (stereo float -> 16-bit)
    AudioStreamBasicDescription w = { 0 }; w.mSampleRate = SR; w.mFormatID = kAudioFormatLinearPCM;
    w.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked; w.mChannelsPerFrame = 2; w.mBitsPerChannel = 16;
    w.mFramesPerPacket = 1; w.mBytesPerFrame = 4; w.mBytesPerPacket = 4;
    ExtAudioFileRef ef; NSURL* u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[7]]];
    if (!ExtAudioFileCreateWithURL((__bridge CFURLRef)u, kAudioFileWAVEType, &w, NULL, kAudioFileFlags_EraseFile, &ef)) {
        ExtAudioFileSetProperty(ef, kExtAudioFileProperty_ClientDataFormat, sizeof f, &f);
        AudioBufferList* wl = malloc(sizeof(AudioBufferList) + sizeof(AudioBuffer)); wl->mNumberBuffers = 2;
        for (int c = 0; c < 2; c++) { wl->mBuffers[c].mNumberChannels = 1; wl->mBuffers[c].mDataByteSize = (UInt32)(total * 4); wl->mBuffers[c].mData = out[c]; }
        ExtAudioFileWrite(ef, (UInt32)total, wl); ExtAudioFileDispose(ef);
        printf("wrote %s (%.1f s)\n", argv[7], total / SR);
    }
    AudioUnitUninitialize(au); AudioComponentInstanceDispose(au);
    usleep(100000);
    }
    return 0;
}
