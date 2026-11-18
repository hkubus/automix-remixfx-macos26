// rxdrive.c — Path A: drive AURemixFX from inside its own dispatch during AutoMix transitions.
// See rxdrive.h for the carrier protocol. Everything on the render path is lock-free and allocation-free;
// diagnostics go through a lock-free ring drained by a writer thread into the trace file.
#include "rxdrive.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <os/log.h>

#define CHUNK 64          // sub-block size (frames) for envelope evaluation while a transition is active
#define TAIL_SECONDS 3.0  // keep the AU processing after a transition so reverb/delay tails ring out

// ======================================================================================= trace
enum { EV_NEW = 1, EV_OPEN, EV_CLOSE, EV_INIT, EV_SETPROP, EV_GETPROP, EV_SETPARAM, EV_SCHED, EV_RENDER,
       EV_PROCESS, EV_PROCMULTI, EV_RESET, EV_CARRIER, EV_ACTIVATE, EV_DEACTIVATE, EV_APPLY, EV_STATUS,
       EV_NOTE, EV_EXT_NEW, EV_EXT_SETPARAM, EV_BPM, EV_LOST, EV_AUDIO };
typedef struct { _Atomic uint64_t seq; uint64_t t; int kind, a, b, c, d; double v, w, x; const void* p; const char* s; } RXEv;
#define RING (1u << 15)
static RXEv gRing[RING];
static _Atomic uint64_t gHead;
static uint64_t gTail;
static FILE* gTF;
static int gVerbose, gTraceOn;
static os_log_t gOS;                       // mirror of the trace into the unified log (when enabled)
static pthread_mutex_t gTFm = PTHREAD_MUTEX_INITIALIZER;
static uint64_t gT0; static double gNsPerTick;

static inline void EV(int kind, const void* p, const char* s, int a, int b, int c, int d, double v, double w, double x) {
    if (!gTraceOn) return;
    uint64_t i = atomic_fetch_add_explicit(&gHead, 1, memory_order_relaxed);
    RXEv* e = &gRing[i & (RING - 1)];
    atomic_store_explicit(&e->seq, 0, memory_order_relaxed);
    e->t = mach_absolute_time(); e->kind = kind; e->p = p; e->s = s;
    e->a = a; e->b = b; e->c = c; e->d = d; e->v = v; e->w = w; e->x = x;
    atomic_store_explicit(&e->seq, i + 1, memory_order_release);
}
static inline int tid(void) { return (int)pthread_mach_thread_np(pthread_self()); }
static double secs(uint64_t t) { return (double)(t - gT0) * gNsPerTick * 1e-9; }

static const char* propName(int id) {
    switch (id) {
        case 0: return "ClassInfo"; case 1: return "MakeConnection"; case 2: return "SampleRate";
        case 3: return "ParameterList"; case 4: return "ParameterInfo"; case 8: return "StreamFormat";
        case 11: return "ElementCount"; case 12: return "Latency"; case 13: return "SupportedNumChannels";
        case 14: return "MaxFramesPerSlice"; case 19: return "AudioChannelLayout"; case 20: return "TailTime";
        case 21: return "BypassEffect"; case 22: return "LastRenderError"; case 23: return "SetRenderCallback";
        case 25: return "FactoryPresets"; case 27: return "HostCallbacks"; case 29: return "InPlaceProcessing";
        case 30: return "ElementName"; case 32: return "CocoaUI"; case 33: return "SupportedChannelLayoutTags";
        case 35: return "PresentPreset"; case 40: return "ShouldAllocateBuffer"; case 52: return "ParameterHistoryInfo";
        case 3020: return "RenderQuality"; case 19920: return "NickName"; case 56: return "ParameterStringFromValue";
        default: return "?";
    }
}
static const char* kParamName[RX_NPARAM] = {
    "bpm", "filterA_on", "filterA_cutoff", "filterA_res", "filterB_on", "filterB_cutoff", "filterB_res",
    "repeater_on", "repeater_rate", "repeater_mix", "reverb_on", "reverb_time", "reverb_send", "delay_on",
    "delay_rate", "delay_feedback", "delay_level", "gater_on", "gater_rate", "gater_depth", "gater_pw",
    "gater_noise_level", "gater_noise_cutoff", "flanger_on", "flanger_rate", "flanger_mix", "flanger_feedback",
    "phaser_on", "phaser_rate", "phaser_feedback", "phaser_mix", "lfo_phase_sync", "filterA_type", "filterB_type",
    "flanger_depth", "flanger_center", "reverb_dry", "reverb_wet", "tape_stop", "tape_stop_rate" };
// .dspg defaults (27's SmartTransitions graph) — the AU's idle state between transitions
static const float kDefault[RX_NPARAM] = {
    120, 0, 1000, 0, 0, 1000, 0, 0, 1, 1, 0, 1, 0, 0, 10, 0, 0, 0, 1, 1, 0.5f, 0.5f, 0.5f, 0, 1, 1, 0,
    0, 1, 0, 0.5f, 0, 0, 0, 0, 0, 1, 1, 0, 4 };
// discrete params whose changes are logged individually
static int isDiscrete(int p) {
    switch (p) { case 1: case 4: case 7: case 8: case 10: case 13: case 14: case 17: case 18: case 23: case 24:
        case 27: case 28: case 31: case 32: case 33: case 38: case 39: return 1; }
    return 0;
}

static void fmtEv(FILE* f, const RXEv* e) {
    fprintf(f, "%9.3f ", secs(e->t));
    switch (e->kind) {
    case EV_NEW:      fprintf(f, "rx#%d NEW self=%p\n", e->a, e->p); break;
    case EV_OPEN:     fprintf(f, "rx#%d OPEN unit=%p st=%d\n", e->a, e->p, e->b); break;
    case EV_CLOSE:    fprintf(f, "rx#%d CLOSE (renders=%d processes=%d)\n", e->a, e->b, e->c); break;
    case EV_INIT:     fprintf(f, "rx#%d INIT st=%d sr=%.0f ch=%d bytesPerFrame=%d nonInterleaved=%d maxFrames=%d\n", e->a, e->b, e->v, e->c, e->d, (int)e->w, (int)e->x); break;
    case EV_SETPROP:  fprintf(f, "rx#%d SETPROP %s(%d) scope=%d el=%d size=%d u32=%#x st=%d tid=%d\n", e->a, propName(e->b), e->b, e->c, e->d, (int)e->v, (unsigned)(uint32_t)e->w, (int)e->x, (int)(intptr_t)e->p); break;
    case EV_GETPROP:  fprintf(f, "rx#%d GETPROP %s(%d) scope=%d el=%d st=%d\n", e->a, propName(e->b), e->b, e->c, e->d, (int)e->v); break;
    case EV_SETPARAM: fprintf(f, "rx#%d SETPARAM %d(%s) = %g off=%d tid=%d\n", e->a, e->b, (e->b >= 0 && e->b < RX_NPARAM) ? kParamName[e->b] : "pseudo", e->v, e->c, e->d); break;
    case EV_SCHED:    fprintf(f, "rx#%d SCHED n=%d first: id=%d type=%d v0=%g v1=%g dur/off=%g tid=%d\n", e->a, e->b, e->c, e->d, e->v, e->w, e->x, (int)(intptr_t)e->p); break;
    case EV_RENDER:   fprintf(f, "rx#%d RENDER #%d frames=%d nbuf=%d nullData=%d sampleTime=%.0f tid=%d\n", e->a, e->b, e->c, (int)e->w, (int)e->x, e->v, e->d); break;
    case EV_PROCESS:  fprintf(f, "rx#%d PROCESS #%d frames=%d nbuf=%d sampleTime=%.0f tid=%d\n", e->a, e->b, e->c, (int)e->w, e->v, e->d); break;
    case EV_PROCMULTI:fprintf(f, "rx#%d PROCESSMULTIPLE #%d frames=%d nin=%d nout=%d\n", e->a, e->b, e->c, e->d, (int)e->v); break;
    case EV_RESET:    fprintf(f, "rx#%d RESET scope=%d el=%d\n", e->a, e->b, e->c); break;
    case EV_CARRIER:  fprintf(f, "rx#%d CARRIER %s=%.4f code=%d frac=%.5f frame=%.0f tid=%d\n", e->a, e->s, e->v, e->b, e->w, e->x, e->c); break;
    case EV_ACTIVATE: fprintf(f, "rx#%d ACTIVATE style=%d (%s) side=%s slot=%d p=%.4f bpm=%.2f beatLen=%.4f\n", e->a, e->b, e->s ? e->s : "?", e->c ? "incoming" : "outgoing", e->d, e->v, e->w, e->x); break;
    case EV_DEACTIVATE: fprintf(f, "rx#%d DEACTIVATE reason=%s p=%.4f activeSecs=%.2f path=%s\n", e->a, e->s, e->v, e->w, (const char*)e->p); break;
    case EV_APPLY:    fprintf(f, "rx#%d   %-18s = %-8g @p=%.4f\n", e->a, kParamName[e->b], e->v, e->w); break;
    case EV_AUDIO:    fprintf(f, "rx#%d AUDIO in=%.1f out=%.1f diff=%.1f dBFS  (diff vs in %+.1f dB) p=%.4f\n", e->a, e->v, e->w, e->x, e->x - e->v, *(const double*)&e->p); break;
    case EV_STATUS:   fprintf(f, "rx#%d STATUS p=%.4f bpm=%.2f slopeBPM=%.2f carrierUpdates=%d path=%s\n", e->a, e->v, e->w, e->x, e->b, (const char*)e->p); break;
    case EV_BPM:      fprintf(f, "rx#%d BPM from %s: bpm=%.2f (beatLen=%.4f, ramp spans %d beats) at p=%.4f\n", e->a, e->s ? e->s : "?", e->v, e->w, e->b, e->x); break;
    case EV_NOTE:     fprintf(f, "rx#%d NOTE %s %d %d %g %g %g\n", e->a, e->s, e->b, e->c, e->v, e->w, e->x); break;
    case EV_EXT_NEW:  fprintf(f, "EXT InstanceNew %.4s/%.4s/%.4s -> %p st=%d tid=%d\n", (char*)&e->a, (char*)&e->b, (char*)&e->c, e->p, e->d, (int)e->v); break;
    case EV_EXT_SETPARAM: fprintf(f, "EXT SetParameter unit=%p id=%d scope=%d = %g off=%d\n", e->p, e->a, e->b, e->v, e->c); break;
    case EV_LOST:     fprintf(f, "... %d trace events lost\n", e->a); break;
    default:          fprintf(f, "?? kind %d\n", e->kind);
    }
}
static void fixFourCC(RXEv* e) { // EXT_NEW stores big-endian 4ccs in a,b,c; make them printable
    if (e->kind != EV_EXT_NEW) return;
    int* v[3] = { &e->a, &e->b, &e->c };
    for (int i = 0; i < 3; i++) { uint32_t x = (uint32_t)*v[i]; *v[i] = (int)__builtin_bswap32(x); }
}
static void* writer(void* _) {
    (void)_;
    for (;;) {
        int wrote = 0;
        for (;;) {
            RXEv* e = &gRing[gTail & (RING - 1)];
            uint64_t s = atomic_load_explicit(&e->seq, memory_order_acquire);
            if (s == gTail + 1) {
                RXEv c = *e;
                if (atomic_load_explicit(&e->seq, memory_order_acquire) != s) continue;
                fixFourCC(&c);
                pthread_mutex_lock(&gTFm);
                if (gTF) fmtEv(gTF, &c);
                if (gOS) { char* buf = NULL; size_t len = 0; FILE* m = open_memstream(&buf, &len);
                           if (m) { fmtEv(m, &c); fclose(m); os_log(gOS, "%{public}s", buf); free(buf); } }
                pthread_mutex_unlock(&gTFm);
                gTail++; wrote = 1;
            } else if (s > gTail + 1) {
                RXEv l = { 0 }; l.kind = EV_LOST; l.t = mach_absolute_time(); l.a = (int)(s - 1 - gTail);
                pthread_mutex_lock(&gTFm); if (gTF) fmtEv(gTF, &l); pthread_mutex_unlock(&gTFm);
                gTail = s - 1;
            } else break;
        }
        if (wrote && gTF) { pthread_mutex_lock(&gTFm); fflush(gTF); pthread_mutex_unlock(&gTFm); }
        usleep(20000);
    }
    return NULL;
}
void rx_trace_open(const char* path, int verbose) {
    rx_trace_open2(path, verbose, 0);
}
void rx_trace_open2(const char* path, int verbose, int mirrorToOSLog) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); gNsPerTick = (double)tb.numer / tb.denom;
    gT0 = mach_absolute_time(); gVerbose = verbose;
    if (path) gTF = fopen(path, "w");
    if (mirrorToOSLog) gOS = os_log_create("dev.remixfx", "engine");
    if (!gTF && !gOS) return;
    gTraceOn = 1;
    pthread_t th; pthread_create(&th, NULL, writer, NULL); pthread_detach(th);
}
void rx_log(const char* fmt, ...) {
    if (!gTraceOn) return;
    va_list ap; va_start(ap, fmt);
    char msg[1024]; vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&gTFm);
    if (gTF) { fprintf(gTF, "%9.3f %s\n", secs(mach_absolute_time()), msg); fflush(gTF); }
    if (gOS) os_log(gOS, "%{public}s", msg);
    pthread_mutex_unlock(&gTFm);
}

// ======================================================================================= envelopes
#define MAXSEG 24
#define MAXLANE 48
#define MAXENV 24
typedef struct { int param, nseg; RXSeg seg[MAXSEG]; } RXLane;
typedef struct { int used, styleId, side, nlanes; float bars; char name[64]; RXLane lanes[MAXLANE]; } RXEnv;
static RXEnv gEnv[MAXENV];

static RXEnv* findEnv(int styleId, int side) {
    for (int i = 0; i < MAXENV; i++) if (gEnv[i].used && gEnv[i].styleId == styleId && gEnv[i].side == side) return &gEnv[i];
    return NULL;
}
void rx_env_define(int styleId, int side, const char* name, float bars) {
    RXEnv* e = findEnv(styleId, side);
    if (!e) for (int i = 0; i < MAXENV; i++) if (!gEnv[i].used) { e = &gEnv[i]; break; }
    if (!e) return;
    memset(e, 0, sizeof *e);
    e->used = 1; e->styleId = styleId; e->side = side; e->bars = bars > 0 ? bars : 8;
    snprintf(e->name, sizeof e->name, "%s", name ? name : "");
}
int rx_env_add(int styleId, int side, int param, RXSeg seg) {
    RXEnv* e = findEnv(styleId, side);
    if (!e || param < 0 || param >= RX_NPARAM) return -1;
    RXLane* L = NULL;
    for (int i = 0; i < e->nlanes; i++) if (e->lanes[i].param == param) { L = &e->lanes[i]; break; }
    if (!L) { if (e->nlanes >= MAXLANE) return -2; L = &e->lanes[e->nlanes++]; L->param = param; L->nseg = 0; }
    if (L->nseg >= MAXSEG) return -3;
    int i = L->nseg++;                                     // insertion sort by t0 (stable)
    while (i > 0 && L->seg[i - 1].t0 > seg.t0) { L->seg[i] = L->seg[i - 1]; i--; }
    L->seg[i] = seg;
    return 0;
}
int rx_param_index(const char* id) {
    static const struct { const char* n; int i; } m[] = {
        {"remixfx_beats_per_minute",0},{"remixfx_filter_a_on",1},{"remixfx_filter_a_cutoff",2},{"remixfx_filter_a_resonance",3},
        {"remixfx_filter_a_type",32},{"remixfx_filter_b_on",4},{"remixfx_filter_b_cutoff",5},{"remixfx_filter_b_resonance",6},
        {"remixfx_filter_b_type",33},{"remixfx_repeater_on",7},{"remixfx_repeater_rate",8},{"remixfx_repeater_mix",9},
        {"remixfx_reverb_on",10},{"remixfx_reverb_time",11},{"remixfx_reverb_send",12},{"remixfx_reverb_dry",36},
        {"remixfx_reverb_wet",37},{"remixfx_delay_on",13},{"remixfx_delay_rate",14},{"remixfx_delay_feedback",15},
        {"remixfx_delay_level",16},{"remixfx_gater_on",17},{"remixfx_gater_rate",18},{"remixfx_gater_depth",19},
        {"remixfx_gater_pw",20},{"remixfx_gater_noise_level",21},{"remixfx_gater_noise_cutoff",22},{"remixfx_flanger_on",23},
        {"remixfx_flanger_rate",24},{"remixfx_flanger_mix",25},{"remixfx_flanger_feedback",26},{"remixfx_flanger_depth",34},
        {"remixfx_flanger_center",35},{"remixfx_phaser_on",27},{"remixfx_phaser_rate",28},{"remixfx_phaser_feedback",29},
        {"remixfx_phaser_mix",30},{"remixfx_lfo_phase_sync",31},{"remixfx_tape_stop_on",38},{"remixfx_tape_stop",38},
        {"remixfx_tape_stop_rate",39} };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (!strcmp(m[i].n, id)) return m[i].i;
    return -1;
}
void rx_env_dump(void) {
    for (int i = 0; i < MAXENV; i++) {
        RXEnv* e = &gEnv[i]; if (!e->used) continue;
        rx_log("env style %d %s \"%s\" bars=%.0f lanes=%d", e->styleId, e->side ? "incoming" : "outgoing", e->name, e->bars, e->nlanes);
        if (gVerbose) for (int l = 0; l < e->nlanes; l++) for (int s = 0; s < e->lanes[l].nseg; s++) {
            RXSeg* g = &e->lanes[l].seg[s];
            rx_log("    %-18s t %.4f..%.4f  v %g..%g curve %d k %.2f%s", kParamName[e->lanes[l].param], g->t0, g->t1, g->v0, g->v1, g->curve, g->k, g->bpm ? " (bpm)" : "");
        }
    }
}
static inline float curveAt(int c, float k, float x) {
    if (x <= 0) return 0; if (x >= 1) return 1;
    switch (c) { case RX_CURVE_IN: return powf(x, k); case RX_CURVE_OUT: return 1.f - powf(1.f - x, k); default: return x; }
}
static inline float laneValue(const RXLane* L, double t, float bpm) {
    const RXSeg* s = NULL;
    for (int i = 0; i < L->nseg; i++) { if (L->seg[i].t0 <= t) s = &L->seg[i]; else break; }
    if (!s) { s = &L->seg[0]; return s->bpm ? bpm : s->v0; }        // before the first segment: its start value
    if (s->bpm) return bpm;
    if (t >= s->t1 || s->t1 <= s->t0) return s->v1;                  // after the segment (or a step): end value
    return s->v0 + (s->v1 - s->v0) * curveAt(s->curve, s->k, (float)((t - s->t0) / (s->t1 - s->t0)));
}

// ======================================================================================= instances
typedef OSStatus (*FnInit)(void*);
typedef OSStatus (*FnReset)(void*, AudioUnitScope, AudioUnitElement);
typedef OSStatus (*FnGetPI)(void*, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, UInt32*, Boolean*);
typedef OSStatus (*FnGetP)(void*, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, void*, UInt32*);
typedef OSStatus (*FnSetP)(void*, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void*, UInt32);
typedef OSStatus (*FnGetPar)(void*, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue*);
typedef OSStatus (*FnSetPar)(void*, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue, UInt32);
typedef OSStatus (*FnSched)(void*, const AudioUnitParameterEvent*, UInt32);
typedef OSStatus (*FnRender)(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
typedef OSStatus (*FnProcess)(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, AudioBufferList*);
typedef OSStatus (*FnProcMulti)(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, const AudioBufferList**, UInt32, AudioBufferList**);

static AudioComponent (*gRealRegister)(const AudioComponentDescription*, CFStringRef, UInt32, AudioComponentFactoryFunction);
static AudioComponentFactoryFunction gRealFactory;
static AudioComponentMethod (*gRealLookup)(SInt16);
static OSStatus (*gRealOpen)(void*, AudioComponentInstance);
static OSStatus (*gRealClose)(void*);
static FnInit rInit; static FnReset rReset; static FnGetPI rGetPI; static FnGetP rGetP; static FnSetP rSetP;
static FnGetPar rGetPar; static FnSetPar rSetPar; static FnSched rSched; static FnRender rRender; static FnProcess rProcess;
static FnProcMulti rProcMulti;

enum { ST_IDLE = 0, ST_ACTIVE, ST_TAIL };
typedef struct {
    _Atomic(void*) self;
    int serial;
    AudioComponentInstance unit;
    // ---- carrier (written by whichever thread sets the graph params) ----
    _Atomic uint32_t cVer;            // seqlock
    atomic_flag cLock;
    double cVal; int64_t cFrm; double cEndVal; int64_t cEndFrm; int cHasEnd;
    _Atomic double cSlot, cBeat;
    _Atomic int cUpdates;
    // ---- render-thread state ----
    _Atomic int64_t frames;
    double sr; int bpf; int maxFrames;
    AURenderCallbackStruct inCB; int hasCB;
    AudioUnitConnection conn; int hasConn;
    int state; int code; RXEnv* env; uint32_t lastVer;
    double pA; int64_t fA; double slope; int haveSlope; int64_t fLast, fLastChange, fActive; double lastVal;
    int moving;                             // carrier seen changing (before that: armed, progress held)
    double prevVal;                         // carrier value of the previous snapshot (any state)
    double pFirst; int64_t fFirst;          // first *moving* carrier sample: tempo baseline
    float beats;                            // beats spanned by this side's carrier ramp (bars*4 unless truncated)
    // published for the other side of the same transition (read from another graph's render thread)
    _Atomic int pubCode; _Atomic double pubP; _Atomic uint64_t pubT;
    float bpm; int bpmSrc; float applied[RX_NPARAM];      // bpmSrc: BPM_DEFAULT / BPM_SLOPE / BPM_BEATLEN
    int64_t tailUntil, nextStatus; const char* path; int lastIgnored;
    int nRender, nProcess, nMulti, nSetProp, nGetProp, nSetPar;
    int isFloat32; double eIn, eOut, eDiff; int64_t eN;   // live meter while active: input / output / difference energy
} RXInst;
#define MAXI 48
static RXInst gI[MAXI];
static pthread_mutex_t gIm = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int gSerial;

static RXInst* findInst(void* self) {
    for (int i = 0; i < MAXI; i++) if (atomic_load_explicit(&gI[i].self, memory_order_acquire) == self) return &gI[i];
    return NULL;
}
static RXInst* allocInst(void* self) {
    pthread_mutex_lock(&gIm);
    RXInst* r = NULL;
    for (int i = 0; i < MAXI; i++) if (!atomic_load(&gI[i].self)) {
        r = &gI[i];
        memset((char*)r + sizeof(r->self), 0, sizeof *r - sizeof(r->self));
        atomic_flag_clear(&r->cLock);
        r->serial = atomic_fetch_add(&gSerial, 1) + 1; r->sr = 44100; r->bpf = 4; r->lastIgnored = -1;
        atomic_store(&r->pubCode, -1);
        r->path = "-";
        atomic_store_explicit(&r->self, self, memory_order_release);
        break;
    }
    pthread_mutex_unlock(&gIm);
    return r;
}

// ---- carrier write/read (seqlock) ----
static void carrierWrite(RXInst* I, double val, int64_t frm, int hasEnd, double endVal, int64_t endFrm) {
    while (atomic_flag_test_and_set_explicit(&I->cLock, memory_order_acquire)) {}
    atomic_fetch_add_explicit(&I->cVer, 1, memory_order_acq_rel);    // odd: writing
    I->cVal = val; I->cFrm = frm; I->cHasEnd = hasEnd; I->cEndVal = endVal; I->cEndFrm = endFrm;
    atomic_fetch_add_explicit(&I->cVer, 1, memory_order_release);    // even: stable
    atomic_flag_clear_explicit(&I->cLock, memory_order_release);
    atomic_fetch_add_explicit(&I->cUpdates, 1, memory_order_relaxed);
}
typedef struct { uint32_t ver; double val, endVal; int64_t frm, endFrm; int hasEnd; } CSnap;
static int carrierRead(RXInst* I, CSnap* s) {
    for (int tries = 0; tries < 8; tries++) {
        uint32_t v1 = atomic_load_explicit(&I->cVer, memory_order_acquire);
        if (v1 & 1) continue;
        s->val = I->cVal; s->frm = I->cFrm; s->hasEnd = I->cHasEnd; s->endVal = I->cEndVal; s->endFrm = I->cEndFrm;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&I->cVer, memory_order_relaxed) == v1) { s->ver = v1; return 1; }
    }
    return 0;
}
// carrier value -> code (styleId*2 + side) and transition progress; code -1 when the value isn't a carrier
// (graph default 0, SonicKit's idle aufilter default 2500, ...)
static inline void decode(double v, int* code, double* frac) {
    double r = v - RX_CARRIER_BASE;
    if (!(r >= 0 && r < 1000.0 * RX_CARRIER_NIDX)) { *code = -1; *frac = 0; return; }
    int idx = (int)floor(r / 1000.0);
    *code = RX_CARRIER_STYLE0 * 2 + idx; *frac = (r - idx * 1000.0 - 100.0) / 800.0;
}

// ---- engine (render thread) ----
static inline void setParam(RXInst* I, int p, float v, int force) {
    float old = I->applied[p];
    if (!force && fabsf(v - old) <= 1e-6f * fmaxf(1.f, fabsf(old))) return;
    rSetPar(atomic_load_explicit(&I->self, memory_order_relaxed), (AudioUnitParameterID)p, kAudioUnitScope_Global, 0, v, 0);
    I->applied[p] = v;
}
static void applyAt(RXInst* I, double p) {
    const RXEnv* e = I->env; if (!e) return;
    for (int l = 0; l < e->nlanes; l++) {
        const RXLane* L = &e->lanes[l];
        float v = laneValue(L, p, I->bpm);
        float old = I->applied[L->param];
        if (fabsf(v - old) <= 1e-6f * fmaxf(1.f, fabsf(old))) continue;
        setParam(I, L->param, v, 1);
        if (isDiscrete(L->param) || gVerbose > 1) EV(EV_APPLY, NULL, NULL, I->serial, L->param, 0, 0, v, p, 0);
    }
}
static void resetDefaults(RXInst* I, int force) {
    for (int p = 0; p < RX_NPARAM; p++) setParam(I, p, kDefault[p], force);
}
static float beatLenNow(RXInst* I) {
    double b = atomic_load_explicit(&I->cBeat, memory_order_relaxed);
    return (b > 0.15 && b < 3.0) ? (float)b : 0.f;
}
enum { BPM_DEFAULT = 0, BPM_SLOPE = 1, BPM_BEATLEN = 2 };
static inline float clampBPM(double b) { return (float)fmin(300.0, fmax(20.0, b)); }
static double nsPerTick(void) {
    static double ns;
    if (!ns) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); ns = (double)tb.numer / tb.denom; }
    return ns;
}
// Progress now. Held until the carrier moves (an incoming graph is prerolled long before its transition with
// the start value parked on the carrier), then extrapolated from the last sample — at most two slices, so a
// paused or abandoned transition freezes instead of running on.
static inline double progressNow(RXInst* I, int64_t now) {
    if (!I->moving || !I->haveSlope) return I->pA;
    int64_t dt = now - I->fA, cap = 2 * (int64_t)(I->maxFrames > 0 ? I->maxFrames : 8192);
    return I->pA + I->slope * (double)(dt < cap ? dt : cap);
}
static inline double slopeNow(RXInst* I) { return I->moving && I->haveSlope ? I->slope : 0; }
static void publish(RXInst* I, double p) {
    atomic_store_explicit(&I->pubP, p, memory_order_relaxed);
    atomic_store_explicit(&I->pubT, mach_absolute_time(), memory_order_relaxed);
    atomic_store_explicit(&I->pubCode, I->state == ST_ACTIVE && I->moving ? I->code : -1, memory_order_release);
}
// The other side of the same transition (same style, other side), if it is running in this process.
static RXInst* findPartner(RXInst* I) {
    RXInst* best = NULL; uint64_t bt = 0;
    for (int i = 0; i < MAXI; i++) {
        RXInst* J = &gI[i];
        if (J == I || !atomic_load_explicit(&J->self, memory_order_acquire)) continue;
        if (atomic_load_explicit(&J->pubCode, memory_order_acquire) != (I->code ^ 1)) continue;
        uint64_t t = atomic_load_explicit(&J->pubT, memory_order_relaxed);
        if (t > bt) { bt = t; best = J; }
    }
    if (best && (double)(mach_absolute_time() - bt) * nsPerTick() > 1.5e9) best = NULL;    // stale
    return best;
}
static void activate(RXInst* I, int code, double frac, int64_t frm) {
    RXEnv* e = findEnv(code / 2, code % 2);
    if (!e) {
        if (I->lastIgnored != code) { EV(EV_NOTE, NULL, "no-envelope-for-code", I->serial, code, 0, 0, frac, 0, 0); I->lastIgnored = code; }
        return;
    }
    void* self = atomic_load_explicit(&I->self, memory_order_relaxed);
    float bl = beatLenNow(I);
    I->bpmSrc = bl > 0 ? BPM_BEATLEN : BPM_DEFAULT;
    I->bpm = bl > 0 ? clampBPM(60.0 / bl) : 120.f;
    I->env = e; I->code = code; I->state = ST_ACTIVE;
    I->pA = frac; I->fA = frm; I->fLast = frm; I->fActive = atomic_load_explicit(&I->frames, memory_order_relaxed);
    I->moving = 0; I->haveSlope = 0; I->slope = 0; I->beats = e->bars * 4.f;
    I->nextStatus = I->fActive;
    rReset(self, kAudioUnitScope_Global, 0);
    resetDefaults(I, 1);
    publish(I, frac);
    EV(EV_ACTIVATE, NULL, e->name, I->serial, e->styleId, e->side, (int)lrint(atomic_load(&I->cSlot)), frac, I->bpm, bl);
}
static inline double dbfs(double e, int64_t n) { return n > 0 ? 10.0 * log10(e / (double)n + 1e-20) : -200; }
static void meterFlush(RXInst* I, double p) {     // proof that the live graph's audio is being changed (or not)
    if (I->eN <= 0) return;
    union { double d; const void* ptr; } u = { .d = p };
    EV(EV_AUDIO, u.ptr, NULL, I->serial, 0, 0, 0, dbfs(I->eIn, I->eN), dbfs(I->eOut, I->eN), dbfs(I->eDiff, I->eN));
    I->eIn = I->eOut = I->eDiff = 0; I->eN = 0;
}
static void deactivate(RXInst* I, const char* why, double p) {
    int64_t now = atomic_load_explicit(&I->frames, memory_order_relaxed);
    if (!strcmp(why, "end")) {                                    // diagnostics: did the other side end early?
        RXInst* P = findPartner(I);
        if (P) { double pp = atomic_load(&P->pubP); if (pp < 0.9) EV(EV_NOTE, NULL, "ended-before-partner", I->serial, P->serial, 0, 0, p, pp, 0); }
    }
    meterFlush(I, p);
    EV(EV_DEACTIVATE, I->path, why, I->serial, 0, 0, 0, p, (now - I->fActive) / I->sr, 0);
    resetDefaults(I, 0);
    I->state = ST_TAIL; I->tailUntil = now + (int64_t)(TAIL_SECONDS * I->sr); I->env = NULL; I->moving = 0;
    atomic_store_explicit(&I->pubCode, -1, memory_order_release);
}
// First movement of the carrier: fix the tempo baseline and how many beats this side's ramp spans. SonicKit
// compresses a side's automation into that side's region; when the incoming song can't supply the whole
// region (it starts right at 0:00) the region is truncated and the incoming ramp only covers the tail of the
// transition. That side then starts while its partner is already part-way through: span = the rest.
static void startMoving(RXInst* I, double frac, int64_t frm) {
    I->moving = 1; I->pFirst = frac; I->fFirst = frm;
    I->beats = I->env->bars * 4.f;
    RXInst* P = findPartner(I);
    if (P) {
        double pp = atomic_load_explicit(&P->pubP, memory_order_relaxed);
        if (pp > frac + 0.05 && frac < 0.9) {
            double f = (1.0 - pp) / (1.0 - frac);
            float bars = fmaxf(1.f, rintf(I->env->bars * (float)f));
            I->beats = bars * 4.f;
            EV(EV_NOTE, NULL, "late-start-span-bars", I->serial, (int)bars, P->serial, 0, f, pp, frac);
        }
    }
}
static void updateTempo(RXInst* I) {
    if (I->bpmSrc == BPM_BEATLEN || !I->moving || I->fA <= I->fFirst) return;
    // the carrier is linear in this graph's (source) frames: beats over frames since the baseline = native tempo
    double sb = I->beats * 60.0 * I->sr * (I->pA - I->pFirst) / (double)(I->fA - I->fFirst);
    if (sb < 20 || sb > 300) return;
    if (I->bpmSrc == BPM_DEFAULT || fabs(sb - I->bpm) > 0.003 * I->bpm) {   // follow the converging estimate
        I->bpm = clampBPM(sb); I->bpmSrc = BPM_SLOPE;
        EV(EV_BPM, NULL, "slope", I->serial, (int)I->beats, 0, 0, I->bpm, 60.0 / I->bpm, I->pA);
    }
}
static void onCarrier(RXInst* I, const CSnap* s) {
    int code; double frac; decode(s->val, &code, &frac);
    double prev = I->prevVal; I->prevVal = s->val;
    int valid = code >= 0 && frac > -0.02 && frac < 1.02;
    if (!valid) { if (I->state == ST_ACTIVE) deactivate(I, "carrier-cleared", I->pA); return; }
    if (I->state != ST_ACTIVE || code != I->code) {
        if (I->state != ST_ACTIVE && code == I->code && s->val == I->lastVal) return;   // still parked after a timeout
        if (I->state == ST_ACTIVE) deactivate(I, "code-changed", I->pA);
        if (frac < 0.97) {
            activate(I, code, frac, s->frm); I->lastVal = s->val; I->fLastChange = s->frm;
            if (I->state == ST_ACTIVE && s->hasEnd && s->endFrm > s->frm) {          // ramp event: moving already
                int c2; double f2; decode(s->endVal, &c2, &f2);
                if (c2 == code && f2 > frac) { startMoving(I, frac, s->frm); I->slope = (f2 - frac) / (double)(s->endFrm - s->frm); I->haveSlope = 1; }
            } else if (I->state == ST_ACTIVE) {          // re-activation after an armed timeout: already moving?
                int pc; double pf; decode(prev, &pc, &pf);
                if (pc == code && frac > pf) { startMoving(I, frac, s->frm); publish(I, frac); }
            }
        }
        return;
    }
    if (frac >= 0.9995) { deactivate(I, "end", frac); return; }
    if (s->val != I->lastVal) {
        I->lastVal = s->val; I->fLastChange = s->frm;
        if (!I->moving) { startMoving(I, frac, s->frm); I->pA = frac; I->fA = s->frm; I->fLast = s->frm; publish(I, frac); return; }
    }
    if (s->hasEnd && s->endFrm > s->frm) {                         // ramp event: exact slope
        int c2; double f2; decode(s->endVal, &c2, &f2);
        if (c2 == code) { I->slope = (f2 - frac) / (double)(s->endFrm - s->frm); I->haveSlope = 1; }
        I->pA = frac; I->fA = s->frm;
    } else if (I->moving && s->frm > I->fFirst) {
        // anchored slope over everything since the first moving sample (per-slice deltas jitter by ~2%)
        I->slope = (frac - I->pFirst) / (double)(s->frm - I->fFirst); I->haveSlope = I->slope > 0;
        I->pA = frac; I->fA = s->frm;
    } else { I->pA = frac; I->fA = s->frm; }                        // same frame / not moving: latest value wins
    I->fLast = s->frm;
    updateTempo(I);
    publish(I, frac);
}
// Called once per render/process call before audio is touched. Returns 1 if a transition is active.
static int engineUpdate(RXInst* I) {
    CSnap s;
    if (carrierRead(I, &s) && s.ver != I->lastVer) { I->lastVer = s.ver; onCarrier(I, &s); }
    int64_t now = atomic_load_explicit(&I->frames, memory_order_relaxed);
    if (I->state == ST_ACTIVE) {
        if (I->bpmSrc != BPM_BEATLEN) {
            float bl = beatLenNow(I);
            if (bl > 0) {                                              // an explicit tempo always wins
                I->bpm = clampBPM(60.0 / bl); I->bpmSrc = BPM_BEATLEN;
                EV(EV_BPM, NULL, "beatLen", I->serial, (int)I->beats, 0, 0, I->bpm, bl, I->pA);
            }
        }
        double p0 = progressNow(I, now);
        if (p0 >= 1.0005) deactivate(I, "end", p0);
        else if (now - I->fLastChange > (int64_t)(2.5 * I->sr)) deactivate(I, I->moving ? "carrier-stalled" : "armed-timeout", p0);
        else if (now >= I->nextStatus) {
            double slopeBPM = I->haveSlope ? I->slope * I->sr * 60.0 * I->beats : 0;
            EV(EV_STATUS, I->path, NULL, I->serial, atomic_load(&I->cUpdates), 0, 0, p0, I->bpm, slopeBPM);
            meterFlush(I, p0);
            I->nextStatus = now + (int64_t)(0.5 * I->sr);
        }
    }
    if (I->state == ST_TAIL && now >= I->tailUntil) I->state = ST_IDLE;
    return I->state == ST_ACTIVE;
}
// In-place processing of an active slice in ≤CHUNK-frame sub-blocks, re-evaluating the envelope per block.
static OSStatus runChunks(RXInst* I, void* self, AudioUnitRenderActionFlags* fl, const AudioTimeStamp* ts, UInt32 n, AudioBufferList* io) {
    struct { AudioBufferList l; AudioBuffer more[15]; } sub;
    UInt32 nb = io->mNumberBuffers;
    int64_t now = atomic_load_explicit(&I->frames, memory_order_relaxed);
    double p0 = progressNow(I, now), sl = slopeNow(I);
    int meter = I->isFloat32 && nb <= 16;
    float pre[16][CHUNK * 2];
    for (UInt32 off = 0; off < n; off += CHUNK) {
        UInt32 len = n - off < CHUNK ? n - off : CHUNK;
        applyAt(I, p0 + sl * off);
        sub.l.mNumberBuffers = nb;
        for (UInt32 b = 0; b < nb; b++) {
            sub.l.mBuffers[b].mNumberChannels = io->mBuffers[b].mNumberChannels;
            // bytes per frame of this buffer: from the buffer itself when consistent, else the stream format's
            // (non-interleaved: one channel per buffer; interleaved: one buffer, mBytesPerFrame covers all channels)
            UInt32 bs = io->mBuffers[b].mDataByteSize;
            UInt32 bpf = (bs && bs % n == 0) ? bs / n : (UInt32)I->bpf;
            sub.l.mBuffers[b].mDataByteSize = len * bpf;
            sub.l.mBuffers[b].mData = (char*)io->mBuffers[b].mData + (size_t)off * bpf;
        }
        AudioTimeStamp t2 = *ts;
        if (t2.mFlags & kAudioTimeStampSampleTimeValid) t2.mSampleTime += off;
        UInt32 nf[16];
        if (meter) for (UInt32 b = 0; b < nb; b++) {
            nf[b] = sub.l.mBuffers[b].mDataByteSize / 4; if (nf[b] > CHUNK * 2) nf[b] = CHUNK * 2;
            memcpy(pre[b], sub.l.mBuffers[b].mData, nf[b] * 4);
        }
        OSStatus r = rProcess(self, fl, &t2, len, &sub.l);
        if (r) return r;
        if (meter) for (UInt32 b = 0; b < nb; b++) {
            const float* o = (const float*)sub.l.mBuffers[b].mData;
            for (UInt32 i = 0; i < nf[b]; i++) { double x = pre[b][i], y = o[i]; I->eIn += x * x; I->eOut += y * y; I->eDiff += (y - x) * (y - x); }
            I->eN += nf[b];
        }
    }
    return noErr;
}

// ---- wrapped selectors ----
static OSStatus RX_Initialize(void* self) {
    OSStatus r = rInit(self);
    RXInst* I = findInst(self);
    if (I) {
        AudioStreamBasicDescription f = { 0 }; UInt32 sz = sizeof f;
        if (!rGetP(self, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, &sz) && f.mSampleRate > 0) {
            I->sr = f.mSampleRate; I->bpf = (int)f.mBytesPerFrame;
            I->isFloat32 = (f.mFormatFlags & kAudioFormatFlagIsFloat) && f.mBitsPerChannel == 32;
        }
        UInt32 mf = 0; sz = 4; rGetP(self, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mf, &sz); I->maxFrames = (int)mf;
        EV(EV_INIT, NULL, NULL, I->serial, (int)r, (int)f.mChannelsPerFrame, (int)f.mBytesPerFrame, f.mSampleRate,
           (f.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 1 : 0, mf);
        for (int p = 0; p < RX_NPARAM; p++) I->applied[p] = kDefault[p];
    }
    return r;
}
static OSStatus RX_Reset(void* self, AudioUnitScope sc, AudioUnitElement el) {
    RXInst* I = findInst(self);
    if (I && gVerbose) EV(EV_RESET, NULL, NULL, I->serial, (int)sc, (int)el, 0, 0, 0, 0);
    return rReset(self, sc, el);
}
static void fillPseudoInfo(AudioUnitParameterInfo* pi, UInt32 id) {
    memset(pi, 0, sizeof *pi);
    snprintf(pi->name, sizeof pi->name, "RemixFX carrier %u", (unsigned)id);
    pi->unit = kAudioUnitParameterUnit_Generic; pi->minValue = -1e7f; pi->maxValue = 1e7f; pi->defaultValue = 0;
    pi->flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
}
static inline int isPseudo(UInt32 id) { return id >= RX_PSEUDO_PROGRESS && id <= RX_PSEUDO_BEATLEN; }
static OSStatus RX_GetPropertyInfo(void* self, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, UInt32* sz, Boolean* w) {
    if (id == kAudioUnitProperty_ParameterInfo && sc == kAudioUnitScope_Global && isPseudo(el)) {
        if (sz) *sz = sizeof(AudioUnitParameterInfo); if (w) *w = false; return noErr;
    }
    OSStatus r = rGetPI(self, id, sc, el, sz, w);
    if (!r && id == kAudioUnitProperty_ParameterList && sc == kAudioUnitScope_Global && sz) *sz += 3 * sizeof(AudioUnitParameterID);
    return r;
}
static OSStatus RX_GetProperty(void* self, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, void* data, UInt32* sz) {
    RXInst* I = findInst(self);
    OSStatus r;
    if (id == kAudioUnitProperty_ParameterInfo && sc == kAudioUnitScope_Global && isPseudo(el)) {
        if (!data || !sz || *sz < sizeof(AudioUnitParameterInfo)) r = kAudioUnitErr_InvalidPropertyValue;
        else { fillPseudoInfo((AudioUnitParameterInfo*)data, el); *sz = sizeof(AudioUnitParameterInfo); r = noErr; }
    } else if (id == kAudioUnitProperty_ParameterList && sc == kAudioUnitScope_Global && data && sz) {
        UInt32 real = 0; Boolean wr;
        r = rGetPI(self, id, sc, el, &real, &wr);
        if (!r && *sz >= real + 3 * sizeof(AudioUnitParameterID)) {
            UInt32 got = real; r = rGetP(self, id, sc, el, data, &got);
            if (!r) { AudioUnitParameterID* ids = (AudioUnitParameterID*)((char*)data + got);
                      ids[0] = RX_PSEUDO_PROGRESS; ids[1] = RX_PSEUDO_SLOT; ids[2] = RX_PSEUDO_BEATLEN; *sz = got + 3 * sizeof(AudioUnitParameterID); }
        } else r = rGetP(self, id, sc, el, data, sz);
    } else r = rGetP(self, id, sc, el, data, sz);
    if (I && I->nGetProp++ < 300 && gVerbose) EV(EV_GETPROP, NULL, NULL, I->serial, (int)id, (int)sc, (int)el, r, 0, 0);
    return r;
}
static OSStatus RX_SetProperty(void* self, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, const void* data, UInt32 sz) {
    RXInst* I = findInst(self);
    OSStatus r;
    uint32_t u = (data && sz >= 4) ? *(const uint32_t*)data : 0;
    if (id == kAudioUnitProperty_BypassEffect) {
        r = noErr;                                       // the engine owns RemixFX's bypass; SonicKit's `bypa` is ignored
    } else {
        r = rSetP(self, id, sc, el, data, sz);
        if (I && !r) {
            if (id == kAudioUnitProperty_SetRenderCallback && sc == kAudioUnitScope_Input && el == 0) {
                if (data && sz >= sizeof(AURenderCallbackStruct)) { I->inCB = *(const AURenderCallbackStruct*)data; I->hasCB = I->inCB.inputProc != NULL; }
                else I->hasCB = 0;
            } else if (id == kAudioUnitProperty_MakeConnection && sc == kAudioUnitScope_Input && el == 0) {
                if (data && sz >= sizeof(AudioUnitConnection)) { I->conn = *(const AudioUnitConnection*)data; I->hasConn = I->conn.sourceAudioUnit != NULL; }
                else I->hasConn = 0;
            }
        }
    }
    if (I && I->nSetProp++ < 400) EV(EV_SETPROP, (void*)(intptr_t)tid(), NULL, I->serial, (int)id, (int)sc, (int)el, sz, u, r);
    return r;
}
static OSStatus RX_GetParameter(void* self, AudioUnitParameterID id, AudioUnitScope sc, AudioUnitElement el, AudioUnitParameterValue* v) {
    if (isPseudo(id)) {
        RXInst* I = findInst(self);
        if (!I || !v) return kAudioUnitErr_InvalidParameter;
        if (id == RX_PSEUDO_PROGRESS) { CSnap s; *v = carrierRead(I, &s) ? (float)s.val : 0; }
        else if (id == RX_PSEUDO_SLOT) *v = (float)atomic_load(&I->cSlot);
        else *v = (float)atomic_load(&I->cBeat);
        return noErr;
    }
    return rGetPar(self, id, sc, el, v);
}
static void noteCarrier(RXInst* I, UInt32 id, double v, int force) {
    int n = atomic_load_explicit(&I->cUpdates, memory_order_relaxed);
    if (!force && gVerbose < 2 && (n % 50) != 1) return;
    int code; double frac; decode(v, &code, &frac);
    EV(EV_CARRIER, NULL, id == RX_PSEUDO_PROGRESS ? "progress" : id == RX_PSEUDO_SLOT ? "slot" : "beatLen",
       I->serial, code, tid(), 0, v, frac, (double)atomic_load_explicit(&I->frames, memory_order_relaxed));
}
static void pseudoSet(RXInst* I, UInt32 id, double v) {
    if (id == RX_PSEUDO_PROGRESS) {
        CSnap s; int had = carrierRead(I, &s);
        carrierWrite(I, v, atomic_load_explicit(&I->frames, memory_order_relaxed), 0, 0, 0);
        int c0, c1; double f0, f1; decode(had ? s.val : 0, &c0, &f0); decode(v, &c1, &f1);
        noteCarrier(I, id, v, c0 != c1);
    } else if (id == RX_PSEUDO_SLOT) {
        if (atomic_exchange(&I->cSlot, v) != v) noteCarrier(I, id, v, 1);
    } else {
        if (atomic_exchange(&I->cBeat, v) != v) noteCarrier(I, id, v, 1);
    }
}
static OSStatus RX_SetParameter(void* self, AudioUnitParameterID id, AudioUnitScope sc, AudioUnitElement el, AudioUnitParameterValue v, UInt32 off) {
    RXInst* I = findInst(self);
    if (isPseudo(id)) { if (I) pseudoSet(I, id, v); return noErr; }
    if (I && I->nSetPar++ < 200) EV(EV_SETPARAM, NULL, NULL, I->serial, (int)id, (int)off, tid(), v, 0, 0);
    if (I && I->state == ST_ACTIVE) return noErr;          // the engine owns the 40 params during a transition
    OSStatus r = rSetPar(self, id, sc, el, v, off);
    if (I && !r && id < RX_NPARAM) I->applied[id] = v;
    return r;
}
static OSStatus RX_ScheduleParameters(void* self, const AudioUnitParameterEvent* ev, UInt32 n) {
    RXInst* I = findInst(self);
    if (!I) return rSched(self, ev, n);
    AudioUnitParameterEvent keep[64]; UInt32 nk = 0;
    for (UInt32 i = 0; i < n; i++) {
        const AudioUnitParameterEvent* e = &ev[i];
        if (isPseudo(e->parameter)) {
            int64_t f = atomic_load_explicit(&I->frames, memory_order_relaxed);
            if (e->eventType == kParameterEvent_Ramped && e->parameter == RX_PSEUDO_PROGRESS) {
                int64_t f0 = f + e->eventValues.ramp.startBufferOffset;
                carrierWrite(I, e->eventValues.ramp.startValue, f0, 1, e->eventValues.ramp.endValue, f0 + e->eventValues.ramp.durationInFrames);
                noteCarrier(I, e->parameter, e->eventValues.ramp.startValue, 0);
            } else pseudoSet(I, e->parameter, e->eventType == kParameterEvent_Ramped ? e->eventValues.ramp.endValue : e->eventValues.immediate.value);
        } else if (nk < 64) keep[nk++] = *e;
    }
    if (n && I->nSetPar++ < 200)
        EV(EV_SCHED, (void*)(intptr_t)tid(), NULL, I->serial, (int)n, (int)ev[0].parameter, (int)ev[0].eventType,
           ev[0].eventType == kParameterEvent_Ramped ? ev[0].eventValues.ramp.startValue : ev[0].eventValues.immediate.value,
           ev[0].eventType == kParameterEvent_Ramped ? ev[0].eventValues.ramp.endValue : 0,
           ev[0].eventType == kParameterEvent_Ramped ? ev[0].eventValues.ramp.durationInFrames : ev[0].eventValues.immediate.bufferOffset);
    if (I->state == ST_ACTIVE) return noErr;
    return nk ? rSched(self, keep, nk) : noErr;
}
static OSStatus pullInput(RXInst* I, const AudioTimeStamp* ts, UInt32 n, AudioBufferList* io) {
    AudioUnitRenderActionFlags f2 = 0;
    if (I->hasCB) {
        void* saved[16]; UInt32 nb = io->mNumberBuffers > 16 ? 16 : io->mNumberBuffers;
        for (UInt32 b = 0; b < nb; b++) saved[b] = io->mBuffers[b].mData;
        OSStatus r = I->inCB.inputProc(I->inCB.inputProcRefCon, &f2, ts, 0, n, io);
        for (UInt32 b = 0; b < nb; b++) if (io->mBuffers[b].mData != saved[b]) {
            if (io->mBuffers[b].mData) memcpy(saved[b], io->mBuffers[b].mData, io->mBuffers[b].mDataByteSize);
            io->mBuffers[b].mData = saved[b];
        }
        return r;
    }
    if (I->hasConn) return AudioUnitRender(I->conn.sourceAudioUnit, &f2, ts, I->conn.sourceOutputNumber, n, io);
    return kAudioUnitErr_NoConnection;
}
static OSStatus RX_Render(void* self, AudioUnitRenderActionFlags* fl, const AudioTimeStamp* ts, UInt32 bus, UInt32 n, AudioBufferList* io) {
    RXInst* I = findInst(self);
    if (!I) return rRender(self, fl, ts, bus, n, io);
    int k = ++I->nRender;
    if (k == 1 || (gVerbose && k % 4000 == 0)) {
        int nullData = 0; if (io) for (UInt32 b = 0; b < io->mNumberBuffers; b++) if (!io->mBuffers[b].mData) nullData = 1;
        EV(EV_RENDER, NULL, NULL, I->serial, k, (int)n, tid(), ts ? ts->mSampleTime : -1, io ? io->mNumberBuffers : 0, nullData);
    }
    OSStatus r;
    if (engineUpdate(I)) {
        int canSplit = rProcess && io && io->mNumberBuffers <= 16 && (I->hasCB || I->hasConn) && bus == 0;
        if (canSplit) for (UInt32 b = 0; b < io->mNumberBuffers; b++) if (!io->mBuffers[b].mData) canSplit = 0;
        if (canSplit && !(r = pullInput(I, ts, n, io))) { I->path = "render->pull+process(sub-block)"; r = runChunks(I, self, fl, ts, n, io); }
        else {                                                // slice-resolution fallback
            I->path = "render(slice)";
            int64_t now = atomic_load_explicit(&I->frames, memory_order_relaxed);
            applyAt(I, progressNow(I, now));
            r = rRender(self, fl, ts, bus, n, io);
        }
    } else r = rRender(self, fl, ts, bus, n, io);
    atomic_fetch_add_explicit(&I->frames, n, memory_order_relaxed);
    return r;
}
static OSStatus RX_Process(void* self, AudioUnitRenderActionFlags* fl, const AudioTimeStamp* ts, UInt32 n, AudioBufferList* io) {
    RXInst* I = findInst(self);
    if (!I) return rProcess(self, fl, ts, n, io);
    int k = ++I->nProcess;
    if (k == 1 || (gVerbose && k % 4000 == 0))
        EV(EV_PROCESS, NULL, NULL, I->serial, k, (int)n, tid(), ts ? ts->mSampleTime : -1, io ? io->mNumberBuffers : 0, 0);
    OSStatus r;
    if (engineUpdate(I) && io && io->mNumberBuffers <= 16) { I->path = "process(sub-block)"; r = runChunks(I, self, fl, ts, n, io); }
    else r = rProcess(self, fl, ts, n, io);
    atomic_fetch_add_explicit(&I->frames, n, memory_order_relaxed);
    return r;
}
static OSStatus RX_ProcessMultiple(void* self, AudioUnitRenderActionFlags* fl, const AudioTimeStamp* ts, UInt32 n,
                                   UInt32 nin, const AudioBufferList** in, UInt32 nout, AudioBufferList** out) {
    RXInst* I = findInst(self);
    if (!I) return rProcMulti(self, fl, ts, n, nin, in, nout, out);
    int k = ++I->nMulti;
    if (k == 1) EV(EV_PROCMULTI, NULL, NULL, I->serial, k, (int)n, (int)nin, nout, 0, 0);
    if (engineUpdate(I)) {                                    // slice-resolution (not expected to be used)
        I->path = "processMultiple(slice)";
        int64_t now = atomic_load_explicit(&I->frames, memory_order_relaxed);
        applyAt(I, progressNow(I, now));
    }
    OSStatus r = rProcMulti(self, fl, ts, n, nin, in, nout, out);
    atomic_fetch_add_explicit(&I->frames, n, memory_order_relaxed);
    return r;
}
static AudioComponentMethod RX_Lookup(SInt16 sel) {
    AudioComponentMethod real = gRealLookup(sel);
    if (!real) return NULL;
    switch (sel) {
        case kAudioUnitInitializeSelect:         rInit = (FnInit)real;          return (AudioComponentMethod)RX_Initialize;
        case kAudioUnitResetSelect:              rReset = (FnReset)real;        return (AudioComponentMethod)RX_Reset;
        case kAudioUnitGetPropertyInfoSelect:    rGetPI = (FnGetPI)real;        return (AudioComponentMethod)RX_GetPropertyInfo;
        case kAudioUnitGetPropertySelect:        rGetP = (FnGetP)real;          return (AudioComponentMethod)RX_GetProperty;
        case kAudioUnitSetPropertySelect:        rSetP = (FnSetP)real;          return (AudioComponentMethod)RX_SetProperty;
        case kAudioUnitGetParameterSelect:       rGetPar = (FnGetPar)real;      return (AudioComponentMethod)RX_GetParameter;
        case kAudioUnitSetParameterSelect:       rSetPar = (FnSetPar)real;      return (AudioComponentMethod)RX_SetParameter;
        case kAudioUnitScheduleParametersSelect: rSched = (FnSched)real;        return (AudioComponentMethod)RX_ScheduleParameters;
        case kAudioUnitRenderSelect:             rRender = (FnRender)real;      return (AudioComponentMethod)RX_Render;
        case kAudioUnitProcessSelect:            rProcess = (FnProcess)real;    return (AudioComponentMethod)RX_Process;
        case kAudioUnitProcessMultipleSelect:    rProcMulti = (FnProcMulti)real; return (AudioComponentMethod)RX_ProcessMultiple;
        default: return real;
    }
}
static void resolveAll(void) {   // make sure every wrapped selector's real implementation is known up front
    if (!gRealLookup) return;
    rInit = (FnInit)gRealLookup(kAudioUnitInitializeSelect);       rReset = (FnReset)gRealLookup(kAudioUnitResetSelect);
    rGetPI = (FnGetPI)gRealLookup(kAudioUnitGetPropertyInfoSelect); rGetP = (FnGetP)gRealLookup(kAudioUnitGetPropertySelect);
    rSetP = (FnSetP)gRealLookup(kAudioUnitSetPropertySelect);       rGetPar = (FnGetPar)gRealLookup(kAudioUnitGetParameterSelect);
    rSetPar = (FnSetPar)gRealLookup(kAudioUnitSetParameterSelect);  rSched = (FnSched)gRealLookup(kAudioUnitScheduleParametersSelect);
    rRender = (FnRender)gRealLookup(kAudioUnitRenderSelect);        rProcess = (FnProcess)gRealLookup(kAudioUnitProcessSelect);
    rProcMulti = (FnProcMulti)gRealLookup(kAudioUnitProcessMultipleSelect);
}
static OSStatus RX_Open(void* self, AudioComponentInstance inst) {
    OSStatus r = gRealOpen(self, inst);
    RXInst* I = findInst(self);
    if (I) { I->unit = inst; EV(EV_OPEN, inst, NULL, I->serial, (int)r, 0, 0, 0, 0, 0); }
    return r;
}
static OSStatus RX_Close(void* self) {
    RXInst* I = findInst(self);
    if (I) {
        EV(EV_CLOSE, NULL, NULL, I->serial, I->nRender, I->nProcess, 0, 0, 0, 0);
        pthread_mutex_lock(&gIm); atomic_store(&I->self, NULL); pthread_mutex_unlock(&gIm);
    }
    return gRealClose(self);
}
static AudioComponentPlugInInterface* RX_Factory(const AudioComponentDescription* d) {
    AudioComponentPlugInInterface* p = gRealFactory(d);
    if (!p) return p;
    if (!gRealLookup) { gRealLookup = p->Lookup; gRealOpen = p->Open; gRealClose = p->Close; resolveAll(); }
    if (p->Lookup != gRealLookup) { rx_log("RemixFX factory: unexpected Lookup %p (expected %p) — instance left unwrapped", (void*)p->Lookup, (void*)gRealLookup); return p; }
    RXInst* I = allocInst(p);
    if (!I) { rx_log("RemixFX factory: instance table full — instance left unwrapped"); return p; }
    p->Lookup = RX_Lookup; p->Open = RX_Open; p->Close = RX_Close;
    EV(EV_NEW, p, NULL, I->serial, 0, 0, 0, 0, 0, 0);
    return p;
}
static int gCaptureOnly;
void rx_set_capture_only(int on) { gCaptureOnly = on; }
int  rx_has_factory(void) { return gRealFactory != NULL; }
AudioComponentPlugInInterface* rx_factory(const AudioComponentDescription* d) { return gRealFactory ? RX_Factory(d) : NULL; }
static AudioComponent RX_Register(const AudioComponentDescription* d, CFStringRef name, UInt32 ver, AudioComponentFactoryFunction f) {
    if (d && d->componentSubType == 'remx' && d->componentManufacturer == kAudioUnitManufacturer_Apple && f) {
        gRealFactory = f;
        rx_log("wrapping factory of aufx/remx/appl (real factory %p)%s", (void*)f, gCaptureOnly ? " [component bundle: not re-registering]" : "");
        if (gCaptureOnly) return NULL;
        return gRealRegister(d, name, ver, RX_Factory);
    }
    if (d) { uint32_t t = __builtin_bswap32(d->componentType), s = __builtin_bswap32(d->componentSubType);
             rx_log("AudioComponentRegister passthrough %.4s/%.4s", (char*)&t, (char*)&s); }
    return gRealRegister(d, name, ver, f);
}
void* rx_bind_hook(const char* name, void* resolved) {
    if (!strcmp(name, "AudioComponentRegister") && resolved) { gRealRegister = resolved; return (void*)RX_Register; }
    return NULL;
}

// ---- external diagnostics ----
static _Atomic int gExtNew, gExtSet;
void rx_note_instance_new(AudioComponent c, AudioComponentInstance inst, OSStatus st) {
    if (atomic_fetch_add(&gExtNew, 1) > 400) return;
    AudioComponentDescription d = { 0 }; if (c) AudioComponentGetDescription(c, &d);
    EV(EV_EXT_NEW, inst, NULL, (int)d.componentType, (int)d.componentSubType, (int)d.componentManufacturer, (int)st, tid(), 0, 0);
}
void rx_note_set_parameter(AudioUnit u, AudioUnitParameterID id, AudioUnitScope sc, AudioUnitElement el, AudioUnitParameterValue v, UInt32 off) {
    (void)el;
    if (!gVerbose || atomic_fetch_add(&gExtSet, 1) > 3000) return;
    EV(EV_EXT_SETPARAM, u, NULL, (int)id, (int)sc, (int)off, 0, v, 0, 0);
}
