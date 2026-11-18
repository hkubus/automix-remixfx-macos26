// planner27.c — make macOS 26's beat-matched planner choose transitions the way macOS 27's does.
// Rules and addresses: RULES27.md ("Option (b) on 26" and "Option (a) on 26").
//
// 26 has one beat-matched strategy, Beat-Matched Filtered Cross-Fade (BMFCF), which evaluates the catalog styles
// [8, 9, 12] through a per-style switch, keeps the best style per region pair and takes the overall maximum.
// 27 runs four beat-matched strategies — Dance {17, 19}, Hip-Hop {15, 18}, Pop {16, 20} and BMFCF {8, 9, 12} —
// each with its own genre gate, base scores and factor lists, keeps the best candidate per style and makes a
// seeded weighted random pick across all of them (RULES27.md §6).
//
// Option (b) (catalog serves 17-20 at their real IDs, make_catalog.py default): we hand BMFCF's catalog lookup a
// longer ID list, so 26 evaluates 27's styles next to its own 8/9/12. 26's switch only knows 8/9/12, so each new
// style runs through the branch whose checks are closest to its 27 strategy, and we prune that branch's factor
// list down to 27's:
//   17 Dance  -> branch 9: tempo 0.16 · tonality · matching bars · trailing loudness            (27: same 4)
//   19 Dance  -> branch 9: tempo 0.16 · matching bars · leading vocals                          (27: exactly these)
//   18 Hip-Hop-> branch 8: leading vocals · bar ratio ≥ 0.75 · bar score · loudness, no tempo    (27: exactly these)
//   20 Pop    -> branch 8: leading vocals · bar ratio ≥ 0.75 · loudness, no tempo  (27: + 2 outgoing-region checks)
// The results carry the real style (name, bars, instructions); we relabel their style ID to the real one, keep
// the best candidate per style across all region pairs and return 27's weighted pick.
// Option (a) (catalog carries 27 styles inside slots 8/9/12, "--slots"): same scoring/pick, no extra styles.
//
// In-process hooks on 26's _SonicKit_MusicKit_Packages (shared-cache code, patched copy-on-write in this process):
//   expectedDuration  0x7ffe071909b4  records each song's expected stereo duration (the seed input)
//   genreCompatible   0x7ffe071abdf0  BMFCF's genre check: re-run with 27's per-strategy filter lists
//   score             0x7ffe0719846e  base × Π factors (+0.001·x): 27's base (0 if the genre gate fails), 27's factors
//   best              0x7ffe071993d4  per region pair: relabel + collect the best per style; at the end: 27's pick
//   styleLookup       0x7ffe0719a1ee  BMFCF's [8, 9, 12] -> [8, 9, 12, <27 styles in the catalog>]
//   evaluate          0x7ffe0719da6a  route each new style through a 26 branch, remember its real ID
//   resolveMapped     0x7ffe071ba330  supply styles 15/16 their beat_length / dynamic_beat_length value
// Every hook site and call site is byte-verified first; on any mismatch nothing is patched.
//
// Styles 15 (Hip-Hop) and 16 (Pop) are "delay out": the outgoing track tails off into a beat-timed echo. Their
// delay-time automation reads a MappedAutomationValue by name (beat_length / dynamic_beat_length) that 26's
// beat-matched planner never fills in (it builds every result's mapped-value dictionary empty), so 26's value
// resolver throws "Mapped automation value missing" and the whole transition fails. 27 fills it with the outgoing
// song's beat length (60 / BPM, BPM snapped to 0.5). We reproduce that without building a Swift dictionary: when a
// 15/16 result is picked we stash the outgoing beat length (region length / (bars × 4), the same quantity to <0.5
// BPM), and the resolveMapped hook returns it for the by-name lookup (26's resolver takes an enum tag in r8b: 0/2 =
// literal double, 1 = look up by ID and throw if absent — only 15/16 use the by-name path, so tag 1 is always ours).
// 15's value is 27's tempo-scaled "dynamic" beat; we approximate it with the outgoing beat (tempos are compatible,
// so the echo stays on the outgoing track's beat as it tails out).

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <os/lock.h>
#include <os/log.h>
#include "rxdrive.h"
#include "planner27.h"

extern const void* _dyld_get_shared_cache_range(size_t* length);

// ---- macOS 26 _SonicKit_MusicKit_Packages (x86_64h shared cache), unslid addresses ----
#define A_EXPECTED_DURATION 0x7ffe071909b4ULL   // (ctx rdi, …, stereo xmm0, spatial xmm1) -> Double
#define A_SCORE             0x7ffe0719846eULL   // (factors [Double] rdi, x xmm0, base xmm1) -> Double
#define A_BEST              0x7ffe071993d4ULL   // (results [CandidateResult] rdi, out rax) -> Optional<CandidateResult>
#define A_GENRE             0x7ffe071abdf0ULL   // (outInfo*, inInfo*, kind, [GenreFilter], logArg) -> Bool
#define A_LOOKUP            0x7ffe0719a1eeULL   // (catalog rdi, ids [Int] rsi) throws -> [TransitionStyle]
#define A_EVAL              0x7ffe0719da6aULL   // per-style switch for one region pair; [TransitionStyle] = stack arg 3
#define A_STATIC_IDS        0x7ffe4141d798ULL   // BMFCF's static [Int] [8, 9, 12] (immortal)
#define A_RESULT_COPY       0x7ffe071a3a70ULL   // outlined initializeWithCopy(src, dst) of the 256-byte result
#define A_RESULT_DESTROY    0x7ffe07182928ULL   // outlined destroy(p)
#define R_SCORE_SLOT9       0x7ffe0719e05dULL   // return addresses into 0x7ffe0719da6a (per-style scoring switch)
#define R_SCORE_SLOT8       0x7ffe0719e267ULL
#define R_SCORE_SLOT12      0x7ffe0719e4ffULL
#define R_BEST_PER_PAIR     0x7ffe071997f2ULL   // inside 0x7ffe071995ab (best style for one region pair)
#define R_BEST_FINAL        0x7ffe0719939bULL   // inside 0x7ffe071991e1 (best over all region pairs)
#define R_LOOKUP            0x7ffe0719cc1dULL   // inside 0x7ffe0719bce8 (BMFCF, one region pair)
#define R_EVAL              0x7ffe0719cd3aULL
#define A_SECTION_START     0x7ffe0719752bULL   // Section.startTime   (self r13, out rax: Double)
#define A_REGION_TIMES      0x7ffe0717d290ULL   // Region.timeRange    (self r13, out rax: (start, end) Double)
#define A_RESOLVE           0x7ffe071ba330ULL   // MappedAutomationValue resolve: (dict rdi, …, id rdx, …, tag r8b) -> Double
// 0x7ffe0719da6a's args: rsi = outgoing song structure (borrowed from 0x7ffe0719bce8), sections array at +0x38
// (0x50-byte Section elements); during scoring the region pair being evaluated is at rbp-0x2c0, outgoing region first
#define SONG_SECTIONS       0x38
#define SECTION_SIZE        0x50
#define EVAL_REGION_PAIR    (-0x2c0)
// CandidateResult (256 bytes): +0x00 placement, +0x08..+0x3f the TransitionStyle (id, name, bars, instructions),
// +0x40 mapped values, +0x48 region pair, +0xF8 score. 26's branches write their own constant as the style id.
#define RES_SIZE            0x100
#define RES_STYLE           0x08
#define RES_BARS            0x20                 // the TransitionStyle's bar count (+0x08 style + 0x18)
#define RES_BARS_NIL        0x28                 // byte, 1 = catalog had no duration (26 uses 8 bars)
#define RES_OBJ1            0x30
#define RES_OBJ2            0x38
#define RES_REGION          0x48                 // the region pair, outgoing region first (Region.timeRange reads it)
#define RES_SCORE           0xF8
// TransitionStyle (0x38): +0x00 id, +0x08 name, +0x18 bars, +0x20 bars-is-nil, +0x28 / +0x30 instruction arrays
#define STY_SIZE            0x38
#define STY_OBJ1            0x28
#define STY_OBJ2            0x30
// 0x7ffe0719da6a's frame while branch 8 or 9 scores a style: its two instruction arrays and its bar count
#define EVAL_OBJ1           (-0x58)
#define EVAL_OBJ2           (-0x50)
#define EVAL_BARS           (-0xb0)
#define EVAL_BARS_NIL       (-0x48)             // byte, 1 = catalog has no duration (26 then uses 8 bars)

typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15, rsp;
    double   xmm[8][2];
} p27_ctx;
_Static_assert(sizeof(p27_ctx) == 0x100, "p27_ctx must match hook_x64.s");

typedef struct p27_hook {
    void*          resume;          // must stay first: hook_x64.s jumps to *(hook+0)
    int          (*fn)(p27_ctx*);
    const char*    name;
    uint64_t       addr;
    const uint8_t* expect;
    int            n;               // bytes displaced (whole instructions, none RIP-relative)
} p27_hook;

void p27_hook_common(void);
uint64_t p27_swiftcall(const void* fn, void* self, void* indirect, uint64_t a0, uint64_t a1);
int  p27_dispatch(p27_ctx* c, p27_hook* h) { return h->fn(c); }

static const uint8_t kExpDur[]  = { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58 };
static const uint8_t kScore[]   = { 0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x20, 0x66, 0x0f, 0x29, 0x45, 0xe0, 0x48, 0x8b, 0x47, 0x10 };
static const uint8_t kBest[]    = { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x08, 0x02, 0x00, 0x00 };
static const uint8_t kGenre[]   = { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x98, 0x01, 0x00, 0x00 };
static const uint8_t kLookup[]  = { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x53, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00 };
static const uint8_t kEval[]    = { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x53, 0x48, 0x81, 0xec, 0x00, 0x05, 0x00, 0x00 };
static const uint8_t kResolve[] = { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x53, 0x48, 0x83, 0xec, 0x40 };
static const uint8_t kOutlined[] = { 0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89 };   // copy & destroy helpers
static const uint8_t kSecStart[] = { 0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x3d };            // push rbp; mov rbp,rsp; lea rdi,[rip+…]
static const uint8_t kRegTimes[] = { 0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x41, 0x55, 0x53, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00,
                                     0x4c, 0x89, 0xef, 0x48, 0x89, 0xc3 };

static int h_dur(p27_ctx*), h_score(p27_ctx*), h_best(p27_ctx*), h_genre(p27_ctx*), h_lookup(p27_ctx*), h_eval(p27_ctx*), h_resolve(p27_ctx*);
static p27_hook gHooks[] = {
    { 0, h_dur,     "expectedDuration", A_EXPECTED_DURATION, kExpDur,  sizeof kExpDur  },
    { 0, h_genre,   "genreCompatible",  A_GENRE,             kGenre,   sizeof kGenre   },
    { 0, h_score,   "score",            A_SCORE,             kScore,   sizeof kScore   },
    { 0, h_best,    "best",             A_BEST,              kBest,    sizeof kBest    },
    { 0, h_lookup,  "styleLookup",      A_LOOKUP,            kLookup,  sizeof kLookup  },
    { 0, h_eval,    "evaluate",         A_EVAL,              kEval,    sizeof kEval    },
    { 0, h_resolve, "resolveMapped",    A_RESOLVE,           kResolve, sizeof kResolve },
};
#define NHOOK (int)(sizeof gHooks / sizeof gHooks[0])
static p27_hook* const hGenre = &gHooks[1];

static uintptr_t gSlide;
static int gVerbose = 1, gInstalled;
static os_log_t gLog;
static void* (*gResCopy)(const void* src, void* dst);
static void* (*gResDestroy)(void* p);

static void plog(int lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void plog(int lvl, const char* fmt, ...) {
    if (lvl > gVerbose) return;
    char m[1024]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    rx_log("[planner27] %s", m);
    if (gLog) os_log(gLog, "%{public}s", m);
}

// ================================================================================= 27's per-style rules
// option (a): slot -> 27 style carried in that slot (from the served catalog); 0 = the slot holds 26's own style
static int gSlotStyle[13];
void p27_set_slot_style(int slot, int style) { if (slot == 8 || slot == 9 || slot == 12) gSlotStyle[slot] = style; }

// option (b): 27 styles the served catalog has at their real IDs (in 27's strategy order)
static const int kOrder27[] = { 17, 19, 15, 18, 16, 20 };
static int gHave[21];
void p27_add_style(int id) { if (id >= 15 && id <= 20) gHave[id] = 1; }

static double base27(int style) {
    switch (style) {
    case 17: return 75;  case 19: return 17;                 // Beat-Matched Dance
    case 15: case 18: return 100;                            // Beat-Matched Hip-Hop
    case 16: case 20: return 50;                             // Beat-Matched Pop
    case 8: return 20;   case 9: case 12: return 50;         // BMFCF
    }
    return 0;
}
enum { G_NONE = 0, G_DANCE = 1, G_HIPHOP = 2, G_POP = 3, NGROUP = 4 };
static int group27(int style) {
    switch (style) { case 17: case 19: return G_DANCE; case 15: case 18: return G_HIPHOP; case 16: case 20: return G_POP; }
    return G_NONE;
}
// the 26 branch (8 or 9) that evaluates a 27 style whose ID 26's switch doesn't know
static int route27(int style) {
    switch (style) { case 17: case 19: return 9; case 15: case 16: case 18: case 20: return 8; }
    return 0;
}
// 15/16 need "parameterName" values (dynamic_beat_length / beat_length); the resolveMapped hook supplies them
static int routable27(int style) { return style >= 15 && style <= 20; }

// Factors 26's branch computes but 27 does not apply to the style (set to 1). 26's factor arrays:
//   branch 8 : [tempo 0.16, leading incoming vocals 0.75, incoming bar-count score, trailing incoming loudness]
//   branch 9 : [tempo 0.16, tonality compatible, matching bars ≥ min, vocal activity relationship,
//               leading incoming vocals 0.75, trailing incoming loudness]
//   branch 12: [tempo only in the expanded range, matching bars ≥ min, leading incoming vocals 0.75, trailing loudness]
// 27 (RULES27.md §5): Dance 17 = tempo, tonality, matching bars, trailing loudness; Dance 19 = tempo, matching bars,
// leading vocals; Hip-Hop 18 (0x7ffe0811d9da) = leading vocals, scaled-bar ratio ≥ 0.75, bar-count score,
// incoming loudness; Pop 20 (0x7ffe08120f83) = outgoing bars = style bars, scaled-bar ratio ≥ 0.75, leading vocals,
// outgoing loudness significant, incoming loudness. Neither Hip-Hop nor Pop checks the tempo per candidate.
// Bar factors: 26's branch-8 bar score is (incoming bars, tempo-scaled) / 8 (0x7ffe071a1a2d: bars × 0.125); 27's
// Hip-Hop/Pop use r = (incoming bars, tempo-scaled) / style bars, require r ≥ 0.75, and Hip-Hop multiplies by r.
enum { BAR_KEEP = 0, BAR_HIPHOP = 1, BAR_POP = 2 };
static const struct { int slot, style, nfac; unsigned drop; int bar; } kDrop[] = {
    {  9, 17, 6, 1u << 3 | 1u << 4,          BAR_KEEP   },
    {  9, 19, 6, 1u << 1 | 1u << 3 | 1u << 5, BAR_KEEP  },
    {  8, 18, 4, 1u << 0,                    BAR_HIPHOP },
    {  8, 15, 4, 1u << 0,                    BAR_HIPHOP },
    {  8, 20, 4, 1u << 0,                    BAR_POP    },
    {  8, 16, 4, 1u << 0,                    BAR_POP    },
    {  8, 19, 4, 1u << 3,                    BAR_KEEP   },   // option (a) catalogs that carry 19 in slot 8 or 12
    { 12, 19, 4, 1u << 3,                    BAR_KEEP   },
};
#define DUR_CAP_17 90.0              // 27 drops style 17 candidates longer than 90 s

// ================================================================================= shared state
static const int kBestIds[] = { 8, 9, 12, 15, 16, 17, 18, 19, 20 };
#define NBEST (int)(sizeof kBestIds / sizeof kBestIds[0])
typedef struct {
    int     valid, slot, style;
    double  score;
    uint8_t buf[RES_SIZE] __attribute__((aligned(16)));
} Best;
typedef struct { const void* o1; const void* o2; int id; } StyleKey;
#define NMAP 32
static struct {
    os_unfair_lock lock;
    double   dur[2];            // last two expected stereo durations: [0] outgoing, [1] incoming
    uint64_t ndur;
    int      gate[NGROUP], gateValid;
    double   pendBeat;          // outgoing beat length for the chosen 15/16 result (resolveMapped returns it)
    int      pendValid;
    uintptr_t run;              // frame of the BMFCF region-pair loop the stash belongs to
    Best     best[NBEST];       // per style, kBestIds order
    StyleKey map[NMAP];         // a style's instruction arrays -> its real ID (26 relabels results by branch)
    int      nmap, mapNext;
} S = { .lock = OS_UNFAIR_LOCK_INIT };
static int bestIndex(int64_t style) {
    for (int i = 0; i < NBEST; i++) if (kBestIds[i] == style) return i;
    return -1;
}
static void stash_clear_locked(void) {
    for (int i = 0; i < NBEST; i++) if (S.best[i].valid) { gResDestroy(S.best[i].buf); S.best[i].valid = 0; }
    S.run = 0;
}
static void map_put_locked(const void* o1, const void* o2, int id) {
    for (int i = 0; i < S.nmap; i++) if (S.map[i].o1 == o1 && S.map[i].o2 == o2) { S.map[i].id = id; return; }
    int k = S.nmap < NMAP ? S.nmap++ : (S.mapNext++ % NMAP);
    S.map[k] = (StyleKey){ o1, o2, id };
}
static int map_get_locked(const void* o1, const void* o2) {
    for (int i = 0; i < S.nmap; i++) if (S.map[i].o1 == o1 && S.map[i].o2 == o2) return S.map[i].id;
    return 0;
}
// the 27 style we routed through a branch (0 if the element/result is the branch's own style)
static int routed_of(int64_t branch, const void* o1, const void* o2) {
    os_unfair_lock_lock(&S.lock);
    int real = map_get_locked(o1, o2);
    os_unfair_lock_unlock(&S.lock);
    return real >= 15 && real <= 20 && route27(real) == branch ? real : 0;
}
// the style a branch is evaluating / a result carries: the routed 27 style, else the option (a) carried style,
// else the branch itself
static int style_of(int64_t branch, const void* o1, const void* o2) {
    int real = routed_of(branch, o1, o2);
    if (real) return real;
    if (branch >= 0 && branch <= 12 && gSlotStyle[branch]) return gSlotStyle[branch];
    return (int)branch;
}

// ================================================================================= hooks
static int h_dur(p27_ctx* c) {
    double stereo = c->xmm[0][0], spatial = c->xmm[1][0];
    os_unfair_lock_lock(&S.lock);
    S.dur[0] = S.dur[1]; S.dur[1] = stereo; S.ndur++;
    os_unfair_lock_unlock(&S.lock);
    plog(2, "expected duration: stereo %.3f s (spatial %.3f s)", stereo, spatial);
    return 0;
}

// --- genre: rerun 26's own check with 27's per-strategy filter lists ---
// GenreFilter array element (24 bytes): w0, w1, tag. 26's BMFCF list is [tag2(common allowed 52, common denied 89),
// tag0(common allowed 52)]; 27's Dance/Hip-Hop/Pop lists are the same with element 1 replaced by tag0([ids]).
typedef uint64_t (*genre_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void* gFilt[NGROUP];
static int   gFiltState;                 // 0 untried, 1 built, -1 unusable
static __thread int tInGenre;

static void* mk_strings(const uint64_t hdr[2], const char* const* ids, int n) {
    uint64_t* o = calloc(4 + 2 * n, 8);
    o[0] = hdr[0]; o[1] = hdr[1]; o[2] = n; o[3] = (uint64_t)n << 1;
    for (int i = 0; i < n; i++) {                                  // Swift small strings (≤15 UTF-8 bytes)
        uint8_t* s = (uint8_t*)&o[4 + 2 * i]; size_t L = strlen(ids[i]);
        memcpy(s, ids[i], L); s[15] = (uint8_t)(0xE0 | L);
    }
    return o;
}
static void* mk_filters(const uint64_t hdr[2], void* allow, void* deny, void* only) {
    uint8_t* o = calloc(1, 0x20 + 2 * 0x18);
    uint64_t* w = (uint64_t*)o;
    w[0] = hdr[0]; w[1] = hdr[1]; w[2] = 2; w[3] = 4;
    *(void**)(o + 0x20) = allow; *(void**)(o + 0x28) = deny; o[0x30] = 2;
    *(void**)(o + 0x38) = only;  *(void**)(o + 0x40) = NULL; o[0x48] = 0;
    return o;
}
static void build_filters(const uint8_t* f) {
    if (!f || *(const int64_t*)(f + 0x10) != 2 || f[0x30] != 2 || f[0x48] != 0) {
        plog(0, "genre filter list has an unexpected shape - per-style genre gates disabled");
        gFiltState = -1; return;
    }
    const uint64_t* allow = *(const uint64_t* const*)(f + 0x20);
    const uint64_t* deny  = *(const uint64_t* const*)(f + 0x28);
    if (!allow || !deny || allow[2] < 1 || allow[2] > 1000 || deny[2] > 1000) {
        plog(0, "genre id lists unreadable - per-style genre gates disabled"); gFiltState = -1; return;
    }
    const uint64_t fh[2] = { ((const uint64_t*)f)[0], ((const uint64_t*)f)[1] };
    const uint64_t sh[2] = { allow[0], allow[1] };
    static const char* const dance[] = { "17" }, * const hiphop[] = { "15", "18" }, * const pop[] = { "14" };
    gFilt[G_DANCE]  = mk_filters(fh, (void*)allow, (void*)deny, mk_strings(sh, dance, 1));
    gFilt[G_HIPHOP] = mk_filters(fh, (void*)allow, (void*)deny, mk_strings(sh, hiphop, 2));
    gFilt[G_POP]    = mk_filters(fh, (void*)allow, (void*)deny, mk_strings(sh, pop, 1));
    gFiltState = 1;
    plog(1, "genre filters built (common lists: %llu allowed, %llu denied)",
         (unsigned long long)allow[2], (unsigned long long)deny[2]);
}

static int h_genre(p27_ctx* c) {
    if (tInGenre) return 0;
    os_unfair_lock_lock(&S.lock);
    if (!gFiltState) build_filters((const uint8_t*)c->rcx);
    int ok = gFiltState == 1;
    os_unfair_lock_unlock(&S.lock);
    if (!ok) return 0;
    genre_fn G = (genre_fn)hGenre->resume;
    int g[NGROUP] = { 1, 0, 0, 0 };
    tInGenre = 1;
    for (int k = G_DANCE; k < NGROUP; k++) g[k] = (int)(G(c->rdi, c->rsi, c->rdx, (uint64_t)gFilt[k], c->r8) & 1);
    tInGenre = 0;
    os_unfair_lock_lock(&S.lock);
    memcpy(S.gate, g, sizeof g); S.gateValid = 1;
    os_unfair_lock_unlock(&S.lock);
    plog(1, "genre gates: Dance %s, Hip-Hop/R&B %s, Pop %s", g[1] ? "pass" : "fail", g[2] ? "pass" : "fail", g[3] ? "pass" : "fail");
    return 0;
}

// --- styleLookup: BMFCF asks the catalog for [8, 9, 12]; hand it [8, 9, 12, <27 styles we can route>] ---
static uintptr_t gStaticIds;     // slid address of 26's static [8, 9, 12]
static uint64_t* gExtIds;        // our immortal [Int] (same header as the static array)
static int       gNExt;

static void build_ext_ids(void) {
    const uint64_t* st = (const uint64_t*)gStaticIds;
    int ids[16], n = 0;
    for (int i = 0; i < 3; i++) ids[n++] = (int)st[4 + i];
    for (size_t k = 0; k < sizeof kOrder27 / sizeof kOrder27[0]; k++) {
        int s = kOrder27[k];
        if (!gHave[s]) continue;
        if (!routable27(s)) { plog(1, "style %d is in the catalog but needs named automation values 26 can't supply - not evaluated", s); continue; }
        ids[n++] = s;
    }
    if (n == 3) return;
    uint64_t* a = calloc(4 + n, 8);
    a[0] = st[0]; a[1] = st[1];                                    // isa + immortal refcount, as the static array
    a[2] = (uint64_t)n; a[3] = (uint64_t)n << 1;
    for (int i = 0; i < n; i++) a[4 + i] = (uint64_t)ids[i];
    gExtIds = a; gNExt = n;
}

static int h_lookup(p27_ctx* c) {
    if (!gExtIds || c->rsi != gStaticIds) return 0;
    if (*(const uint64_t*)c->rsp - gSlide != R_LOOKUP) return 0;
    c->rsi = (uint64_t)(uintptr_t)gExtIds;
    return 0;
}

// --- 27's Hip-Hop/Pop anchor: the outgoing region must end inside the outgoing song's last section ---
// (27 0x7ffe0811c179 / 0x7ffe0811f6c9: candidate outgoing end downbeats = downbeats at or after the last section's
// start.) We get the same from 26's own accessors: Section.startTime of sections.last, Region.timeRange.
static __thread double tLastStart;       // start of the outgoing song's last section for the current evaluation
static __thread int    tLastValid;
static double gLoggedLast = -1;

static void last_section_for(const uint8_t* song) {
    tLastValid = 0;
    if (!song) return;
    const uint8_t* secs = *(const uint8_t* const*)(song + SONG_SECTIONS);
    if (!secs) return;
    int64_t n = *(const int64_t*)(secs + 0x10);
    if (n < 1 || n > 1000) return;
    double t = -1;
    p27_swiftcall((const void*)(A_SECTION_START + gSlide), (void*)(secs + 0x20 + (n - 1) * SECTION_SIZE), &t, 0, 0);
    if (!(t >= 0 && t < 36000)) return;
    tLastStart = t; tLastValid = 1;
    if (t != gLoggedLast) { gLoggedLast = t; plog(2, "outgoing song: %lld sections, last starts at %.3f s", (long long)n, t); }
}
static int outgoing_times(uint8_t* pair, double* start, double* end) {
    double t[2] = { -1, -1 };
    p27_swiftcall((const void*)(A_REGION_TIMES + gSlide), pair, t, 0, 0);
    if (!(t[0] >= 0 && t[1] < 36000 && t[0] <= t[1])) return 0;
    *start = t[0]; *end = t[1];
    return 1;
}
// outgoing beat length (seconds) of a chosen result: its region length / (bars × 4 beats). This is 27's
// beat_length (60 / BPM) to within its 0.5-BPM snap, built only from accessors we already use.
static double result_beat_length(const uint8_t* buf) {
    int64_t bars = *(const uint8_t*)(buf + RES_BARS_NIL) == 1 ? 8 : *(const int64_t*)(buf + RES_BARS);
    if (bars < 1 || bars > 64) return 0;
    double s, e;
    if (!outgoing_times((uint8_t*)(buf + RES_REGION), &s, &e) || !(e > s)) return 0;
    double bl = (e - s) / ((double)bars * 4.0);
    return (bl > 0.02 && bl < 2.5) ? bl : 0;    // ~30 to ~3000 BPM; outside this, decline and let 26 throw
}

// --- evaluate: route each 27 style through a 26 branch, remember which real style each element is ---
static int h_eval(p27_ctx* c) {
    if (*(const uint64_t*)c->rsp - gSlide != R_EVAL) return 0;
    uint8_t* arr = *(uint8_t**)(c->rsp + 0x18);                    // stack arg 3: [TransitionStyle]
    if (!arr) return 0;
    int64_t n = *(const int64_t*)(arr + 0x10);
    if (n <= 3 || n > 32) return 0;                                 // 26's own list: nothing to route
    last_section_for((const uint8_t*)c->rsi);
    char desc[256] = ""; size_t o = 0;
    os_unfair_lock_lock(&S.lock);
    for (int64_t i = 0; i < n; i++) {
        uint8_t* e = arr + 0x20 + i * STY_SIZE;
        int64_t id = *(const int64_t*)e;
        const void* o1 = *(void* const*)(e + STY_OBJ1);
        const void* o2 = *(void* const*)(e + STY_OBJ2);
        int r = (id >= 15 && id <= 20) ? route27((int)id) : 0;
        if (!r) {                                                    // 8/9/12, or an element we routed before
            int was = map_get_locked(o1, o2);
            if (!(was >= 15 && was <= 20 && route27(was) == id)) map_put_locked(o1, o2, (int)id);
            continue;
        }
        map_put_locked(o1, o2, (int)id);
        *(int64_t*)e = r;
        if (o < sizeof desc - 16) o += snprintf(desc + o, sizeof desc - o, "%s%lld->%d", o ? " " : "", (long long)id, r);
    }
    os_unfair_lock_unlock(&S.lock);
    if (o) plog(3, "evaluating %lld styles, routed %s", (long long)n, desc);
    return 0;
}

// --- resolveMapped: give styles 15/16 their by-name delay value instead of 26's "missing" throw ---
// 26's MappedAutomationValue resolver gets the mapped-values dictionary in rdi, the value's enum tag in r8b
// (0/2 = an inline literal, already in place; 1 = look up by ID, throw if the dictionary lacks it) and returns a
// Double in xmm0. Only 15/16 carry by-name values, so tag 1 is always one of ours and the dictionary is always the
// empty one 26 built (count at +0x10 is 0). We return the stashed outgoing beat length and clear the Swift error.
static int h_resolve(p27_ctx* c) {
    if ((c->r8 & 0xff) != 1) return 0;                     // literal value: 26 already has it
    const void* dict = (const void*)c->rdi;
    if (!dict || *(const int64_t*)((const uint8_t*)dict + 0x10) != 0) return 0;  // a real, non-empty map: leave it
    double v = 0; int ok = 0;
    os_unfair_lock_lock(&S.lock);
    if (S.pendValid) { v = S.pendBeat; ok = 1; }
    os_unfair_lock_unlock(&S.lock);
    if (!ok || !(v > 0)) return 0;                         // nothing stashed: let 26 throw as before (safe)
    c->xmm[0][0] = v; c->xmm[0][1] = 0;
    c->r12 = 0;                                            // no thrown error
    c->rax = 0;
    plog(2, "resolved by-name delay value: %.4f s (%.1f BPM)", v, 60.0 / v);
    return 1;                                              // value produced; skip 26's resolver body
}

// --- score: 27's base per style, genre gate, factor drops, 17's 90 s cap, Hip-Hop/Pop last-section anchor ---
static int h_score(p27_ctx* c) {
    uint64_t ret = *(const uint64_t*)c->rsp - gSlide;
    int slot = ret == R_SCORE_SLOT8 ? 8 : ret == R_SCORE_SLOT9 ? 9 : ret == R_SCORE_SLOT12 ? 12 : 0;
    if (!slot) return 0;                                   // another strategy's scoring: untouched
    const void* o1 = *(void* const*)(c->rbp + EVAL_OBJ1);  // rbp is still 0x7ffe0719da6a's frame
    const void* o2 = *(void* const*)(c->rbp + EVAL_OBJ2);
    int style = style_of(slot, o1, o2);
    int grp = group27(style), pass = 1;
    os_unfair_lock_lock(&S.lock);
    if (grp && S.gateValid) pass = S.gate[grp];
    os_unfair_lock_unlock(&S.lock);
    uint8_t* arr = (uint8_t*)c->rdi;
    int64_t nf = arr ? *(int64_t*)(arr + 0x10) : 0;
    for (size_t k = 0; k < sizeof kDrop / sizeof kDrop[0]; k++) {
        if (kDrop[k].slot != slot || kDrop[k].style != style || kDrop[k].nfac != nf) continue;
        double* f = (double*)(arr + 0x20);
        for (int j = 0; j < nf; j++) if (kDrop[k].drop & (1u << j)) f[j] = 1.0;
        if (kDrop[k].bar != BAR_KEEP) {
            int64_t bars = *(const uint8_t*)(c->rbp + EVAL_BARS_NIL) == 1 ? 8 : *(const int64_t*)(c->rbp + EVAL_BARS);
            if (bars >= 1 && bars <= 64) {
                double r = f[2] * 8.0 / (double)bars;
                f[2] = r >= 0.75 ? (kDrop[k].bar == BAR_HIPHOP ? r : 1.0) : 0.0;
            }
        }
    }
    double x = c->xmm[0][0];                               // 26's tie-breaker input (a time in s), not a duration
    double outStart = 0, outEnd = 0;
    int haveTimes = (style == 17 || grp == G_HIPHOP || grp == G_POP) && (slot == 8 || slot == 9) &&
                    outgoing_times((uint8_t*)(c->rbp + EVAL_REGION_PAIR), &outStart, &outEnd);
    int capped = style == 17 && haveTimes && outEnd - outStart > DUR_CAP_17;
    int early = (grp == G_HIPHOP || grp == G_POP) && haveTimes && tLastValid && outEnd < tLastStart - 0.05;
    double base26 = c->xmm[1][0], base = pass && !capped && !early ? base27(style) : 0.0;
    c->xmm[1][0] = base;
    if (gVerbose >= 3) {
        char fs[160] = ""; size_t o = 0;
        for (int64_t j = 0; j < nf && j < 8 && o < sizeof fs - 12; j++)
            o += snprintf(fs + o, sizeof fs - o, "%s%.3g", j ? " " : "", ((double*)(arr + 0x20))[j]);
        char why[80] = "";
        if (!pass) snprintf(why, sizeof why, " (genre gate)");
        else if (capped) snprintf(why, sizeof why, " (outgoing region %.1f s, over 90 s)", outEnd - outStart);
        else if (early) snprintf(why, sizeof why, " (outgoing ends %.2f s, before last section %.2f s)", outEnd, tLastStart);
        plog(3, "score style %d (branch %d): base %g -> %g%s, factors [%s], x %.3f", style, slot, base26, base, why, fs, x);
    }
    return 0;
}

// --- best: relabel + collect per-style bests across region pairs, then 27's seeded weighted pick ---
static uint64_t seed27(double outDur, double inDur, char* s, size_t ns) {
    long long a = (long long)rint(outDur * 1000.0), b = (long long)rint(inDur * 1000.0);   // .toNearestOrEven
    snprintf(s, ns, "%lld%lld", a, b);
    uint64_t h = 5381;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) h = h * 33 + *p;        // DJB2, wrapping
    return h;
}

static int h_best(p27_ctx* c) {
    uint64_t ret = *(const uint64_t*)c->rsp - gSlide;
    if (ret == R_BEST_PER_PAIR) {
        uintptr_t run = *(const uintptr_t*)c->rbp;         // saved rbp of 0x7ffe071995ab = the pair loop's frame
        uint8_t* arr = (uint8_t*)c->rdi;
        int64_t n = *(const int64_t*)(arr + 0x10);
        for (int64_t i = 0; i < n; i++) {                  // relabel first: 26's max() then returns the real style too
            uint8_t* e = arr + 0x20 + i * RES_SIZE;
            int64_t branch = *(const int64_t*)(e + RES_STYLE);
            int real = routed_of(branch, *(void* const*)(e + RES_OBJ1), *(void* const*)(e + RES_OBJ2));
            if (real) *(int64_t*)(e + RES_STYLE) = real;
        }
        os_unfair_lock_lock(&S.lock);
        if (S.run != run) { stash_clear_locked(); S.run = run; }
        for (int64_t i = 0; i < n; i++) {
            const uint8_t* e = arr + 0x20 + i * RES_SIZE;
            int64_t id = *(const int64_t*)(e + RES_STYLE);
            double score = *(const double*)(e + RES_SCORE);
            int style = id >= 15 ? (int)id : (id <= 12 && gSlotStyle[id]) ? gSlotStyle[id] : (int)id;
            int k = bestIndex(style);
            if (k < 0 || !(score > 0)) continue;
            Best* b = &S.best[k];
            if (b->valid && !(score > b->score)) continue; // strict: the first maximum wins, as in 27
            if (b->valid) gResDestroy(b->buf);
            gResCopy(e, b->buf);
            b->valid = 1; b->score = score; b->style = style;
            b->slot = id >= 15 ? route27((int)id) : (int)id;
        }
        os_unfair_lock_unlock(&S.lock);
        return 0;
    }
    if (ret != R_BEST_FINAL) return 0;

    uintptr_t run = c->rbp;
    os_unfair_lock_lock(&S.lock);
    Best* list[NBEST]; int n = 0;
    if (S.run == run) for (int k = 0; k < NBEST; k++) if (S.best[k].valid) list[n++] = &S.best[k];
    if (!n) {                                              // nothing scored > 0: let 26 return "none"
        stash_clear_locked();
        os_unfair_lock_unlock(&S.lock);
        plog(1, "pick: no beat-matched candidate (all styles scored 0 or were gated)");
        return 0;
    }
    for (int i = 1; i < n; i++)                            // 27's order: score desc, then style id asc
        for (int j = i; j > 0; j--) {
            Best* x = list[j]; Best* y = list[j - 1];
            int before = x->score == y->score ? x->style < y->style : x->score > y->score;
            if (!before) break;
            list[j] = y; list[j - 1] = x;
        }
    int pick = 0; char why[192] = "single result", seedstr[48] = "";
    long long w[NBEST], total = 0;
    for (int i = 0; i < n; i++) { w[i] = (long long)(list[i]->score * 1000.0); total += w[i]; }
    if (n > 1) {
        if (total <= 0) snprintf(why, sizeof why, "weights too low, first result");
        else if (S.ndur < 2) snprintf(why, sizeof why, "song durations unknown, first result");
        else {
            uint64_t seed = seed27(S.dur[0], S.dur[1], seedstr, sizeof seedstr);
            uint64_t r = seed % (uint64_t)total; long long cum = 0;
            for (int i = 0; i < n; i++) { cum += w[i]; if ((long long)r < cum) { pick = i; break; } }
            snprintf(why, sizeof why, "durations %.3f/%.3f s, seed DJB2(\"%s\")=%llu, r=%llu of %lld",
                     S.dur[0], S.dur[1], seedstr, (unsigned long long)seed, (unsigned long long)r, total);
        }
    }
    char desc[512] = ""; size_t o = 0;
    for (int i = 0; i < n && o < sizeof desc - 48; i++)
        o += snprintf(desc + o, sizeof desc - o, "%s%sstyle %d%s score %.3f w %lld", i ? ", " : "",
                      i == pick ? "*" : "", list[i]->style,
                      list[i]->slot != list[i]->style ? (list[i]->slot == 8 ? " (b8)" : list[i]->slot == 9 ? " (b9)" : " (b12)") : "",
                      list[i]->score, w[i]);
    Best* chosen = list[pick];
    memcpy((void*)c->rax, chosen->buf, RES_SIZE);          // hand our +1 copy to the caller
    // styles 15/16 resolve a by-name delay value at build time; stash its beat length for resolveMapped
    S.pendValid = 0;
    if (chosen->style == 15 || chosen->style == 16) {
        double bl = result_beat_length((const uint8_t*)c->rax);
        if (bl > 0) { S.pendBeat = bl; S.pendValid = 1; }
    }
    double pend = S.pendValid ? S.pendBeat : 0;
    chosen->valid = 0;
    stash_clear_locked();
    os_unfair_lock_unlock(&S.lock);
    if (pend > 0) plog(1, "pick: style %d | %s | %s | delay beat %.4f s", chosen->style, desc, why, pend);
    else plog(1, "pick: style %d | %s | %s", chosen->style, desc, why);
    return 1;                                              // result written; skip 26's max()
}

// ================================================================================= install
static int write_code(uint8_t* t, const uint8_t* bytes, size_t n) {
    mach_vm_address_t pg = (uintptr_t)t & ~(uintptr_t)(vm_page_size - 1);
    mach_vm_size_t sz = (((uintptr_t)t + n) - pg + vm_page_size - 1) & ~(uintptr_t)(vm_page_size - 1);
    kern_return_t kr = mach_vm_protect(mach_task_self(), pg, sz, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) return kr;
    memcpy(t, bytes, n);
    return mach_vm_protect(mach_task_self(), pg, sz, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
}
static int call_site_ok(uint64_t retAddr, uint64_t callee) {
    const uint8_t* p = (const uint8_t*)(retAddr + gSlide - 5);
    if (p[0] != 0xE8) return 0;
    int32_t rel; memcpy(&rel, p + 1, 4);
    return retAddr + (int64_t)rel == callee;
}

int p27_install(int verbose) {
    gVerbose = verbose;
    if (!gLog) gLog = os_log_create("dev.remixfx", "planner");
    if (gInstalled) return gInstalled;
    size_t len = 0;
    const uint8_t* cache = _dyld_get_shared_cache_range(&len);
    if (!cache) { plog(0, "no shared cache"); return -1; }
    uint32_t mo = *(const uint32_t*)(cache + 0x10);
    gSlide = (uintptr_t)cache - *(const uint64_t*)(cache + mo);

    for (int i = 0; i < NHOOK; i++)
        if (memcmp((const void*)(gHooks[i].addr + gSlide), gHooks[i].expect, gHooks[i].n)) {
            plog(0, "%s at 0x%llx: unexpected bytes - not this macOS 26 build; planner hooks not installed",
                 gHooks[i].name, (unsigned long long)gHooks[i].addr);
            return -2;
        }
    gStaticIds = A_STATIC_IDS + gSlide;
    const uint64_t* st = (const uint64_t*)gStaticIds;
    if (memcmp((const void*)(A_RESULT_COPY + gSlide), kOutlined, sizeof kOutlined) ||
        memcmp((const void*)(A_RESULT_DESTROY + gSlide), kOutlined, sizeof kOutlined) ||
        memcmp((const void*)(A_SECTION_START + gSlide), kSecStart, sizeof kSecStart) ||
        memcmp((const void*)(A_REGION_TIMES + gSlide), kRegTimes, sizeof kRegTimes) ||
        !call_site_ok(R_SCORE_SLOT8, A_SCORE) || !call_site_ok(R_SCORE_SLOT9, A_SCORE) ||
        !call_site_ok(R_SCORE_SLOT12, A_SCORE) || !call_site_ok(R_BEST_PER_PAIR, A_BEST) ||
        !call_site_ok(R_BEST_FINAL, A_BEST) || !call_site_ok(R_LOOKUP, A_LOOKUP) || !call_site_ok(R_EVAL, A_EVAL) ||
        st[1] != 0x80000004ffffffffULL || st[2] != 3 || st[3] != 6 || st[4] != 8 || st[5] != 9 || st[6] != 12) {
        plog(0, "call sites / helpers / style list don't match - planner hooks not installed");
        return -3;
    }
    gResCopy = (void* (*)(const void*, void*))(A_RESULT_COPY + gSlide);
    gResDestroy = (void* (*)(void*))(A_RESULT_DESTROY + gSlide);
    build_ext_ids();

    // stubs: per hook an entry stub (movabs r11, hook; jmp [rip] -> common) and a resume stub (prologue; jmp back)
    uint8_t* page = mmap(NULL, vm_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) { plog(0, "mmap failed"); return -4; }
    _Static_assert(NHOOK * 96 <= 4096, "stub page");
    uint8_t* entry[NHOOK];
    for (int i = 0; i < NHOOK; i++) {
        p27_hook* h = &gHooks[i];
        uint8_t* e = page + i * 96;
        uint64_t hv = (uint64_t)(uintptr_t)h, common = (uint64_t)(uintptr_t)p27_hook_common;
        e[0] = 0x49; e[1] = 0xBB; memcpy(e + 2, &hv, 8);                       // movabs r11, h
        e[10] = 0xFF; e[11] = 0x25; memset(e + 12, 0, 4); memcpy(e + 16, &common, 8);  // jmp [rip+0]
        uint8_t* r = e + 32;
        uint64_t back = h->addr + gSlide + h->n;
        memcpy(r, h->expect, h->n);
        r[h->n] = 0xFF; r[h->n + 1] = 0x25; memset(r + h->n + 2, 0, 4); memcpy(r + h->n + 6, &back, 8);
        h->resume = r; entry[i] = e;
    }
    if (mprotect(page, vm_page_size, PROT_READ | PROT_EXEC)) { plog(0, "mprotect stubs failed"); return -5; }

    for (int i = 0; i < NHOOK; i++) {
        p27_hook* h = &gHooks[i];
        uint8_t patch[32]; uint64_t ev = (uint64_t)(uintptr_t)entry[i];
        patch[0] = 0xFF; patch[1] = 0x25; memset(patch + 2, 0, 4); memcpy(patch + 6, &ev, 8);
        memset(patch + 14, 0x90, h->n - 14);
        int kr = write_code((uint8_t*)(h->addr + gSlide), patch, h->n);
        if (kr) {
            plog(0, "patching %s failed (kr %d)%s", h->name, kr, i ? " - earlier hooks stay installed" : "");
            return -6;
        }
    }
    gInstalled = NHOOK;
    char ext[96] = ""; size_t o = 0;
    for (int i = 3; i < gNExt && o < sizeof ext - 8; i++)
        o += snprintf(ext + o, sizeof ext - o, "%s%llu->b%d", i > 3 ? " " : "", (unsigned long long)gExtIds[4 + i],
                      route27((int)gExtIds[4 + i]));
    plog(1, "installed %d hooks (cache slide 0x%lx); BMFCF styles 8 9 12%s%s; slots: 8=%d 9=%d 12=%d", NHOOK,
         (unsigned long)gSlide, o ? " + 27's " : "", ext,
         gSlotStyle[8] ? gSlotStyle[8] : 8, gSlotStyle[9] ? gSlotStyle[9] : 9, gSlotStyle[12] ? gSlotStyle[12] : 12);
    return gInstalled;
}
