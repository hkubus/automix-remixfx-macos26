// rxdrive.h — drives Apple's AURemixFX during AutoMix transitions on macOS 26 ("Path A").
//
// 26's SonicKit can't route macOS 27's `remixfx_*` automation and its beat-matched planner only picks
// style slots 8/9/12. So RemixFX content is authored into those slots (make_catalog.py), minus the REMIX_FX
// lanes, plus a "carrier" lane on the AUFilter params that 26 *does* route (aufilter_center_*). Our
// DSPGraph.dspg rewires those graph params (Fcf1/Fcg1/Fbw1) onto pseudo-parameters of the AURemixFX node.
// This module wraps every AURemixFX instance's dispatch (Lookup), receives the carrier, and replays
// 27's REMIX_FX envelopes onto the real AU sample-accurately (≤64-frame sub-blocks) inside its render call.
//
// Carrier encoding (aufilter_center_freq → Fcf1 → pseudo 1000), ramped linearly over the transition and kept
// inside a plausible center-frequency range, clear of SonicKit's idle default (2500 Hz):
//     value = RX_CARRIER_BASE + 1000*idx + 100 + 800*progress,   idx = (styleId - 17)*2 + side (0 out, 1 in)
// aufilter_center_gain → Fcg1 → pseudo 1001: the slot id (diagnostics only)
// Tempo: 26's beat-matched planner can't resolve `"parameterName": "beat_length"` ("Mapped automation value
// missing" → the whole transition fails), so the carrier only uses literal values and the engine derives the
// tempo from the measured ramp slope: bpm = slope[progress/frame] * sr * 60 * bars * 4.
// Pseudo 1002 still accepts seconds/beat if a graph ever wires one in (it overrides the slope estimate).
#pragma once
#include <AudioToolbox/AudioToolbox.h>

#define RX_NPARAM          40
#define RX_PSEUDO_PROGRESS 1000
#define RX_PSEUDO_SLOT     1001
#define RX_PSEUDO_BEATLEN  1002

#define RX_CARRIER_BASE    10000.0
#define RX_CARRIER_STYLE0  17          // first RemixFX style id
#define RX_CARRIER_NIDX    8           // styles 17..20 × 2 sides
static inline double rx_carrier_value(int styleId, int side, double progress) {
    return RX_CARRIER_BASE + 1000.0 * ((styleId - RX_CARRIER_STYLE0) * 2 + side) + 100.0 + 800.0 * progress;
}

enum { RX_CURVE_LINEAR = 0, RX_CURVE_IN = 1, RX_CURVE_OUT = 2 };

typedef struct {
    float t0, t1;     // transition-relative times (0..1)
    float v0, v1;     // values
    float k;          // curve exponent
    int   curve;      // RX_CURVE_*
    int   bpm;        // value comes from the song tempo ("parameterName": "bpm")
} RXSeg;

// ---- setup (call before any instance exists; not real-time safe) ----
void  rx_trace_open(const char* path, int verbose);
void  rx_trace_open2(const char* path, int verbose, int mirrorToOSLog);   // path may be NULL
void  rx_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void  rx_env_define(int styleId, int side, const char* name, float durationBars);
int   rx_env_add(int styleId, int side, int auParam, RXSeg seg);   // returns 0 ok
int   rx_param_index(const char* catalogId);                        // remixfx_* -> AU param index, -1 if unknown
void  rx_env_dump(void);

// sau_set_bind_hook() target: wraps AudioComponentRegister so aufx/remx/appl instances get our dispatch.
void* rx_bind_hook(const char* name, void* resolved);
// component-bundle mode: capture Apple's factory instead of registering it in-process; the bundle's own
// AudioComponents entry points at a factory that calls rx_factory().
void  rx_set_capture_only(int on);
int   rx_has_factory(void);
AudioComponentPlugInInterface* rx_factory(const AudioComponentDescription* d);

// diagnostics from outside the AU (interposers)
void  rx_note_instance_new(AudioComponent c, AudioComponentInstance inst, OSStatus st);
void  rx_note_set_parameter(AudioUnit u, AudioUnitParameterID id, AudioUnitScope sc, AudioUnitElement el, AudioUnitParameterValue v, UInt32 off);
