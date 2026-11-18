# RemixFX on macOS 26 (Intel and other x86_64 Macs)

Reverse-engineering **RemixFX** — the effect behind macOS 27's *Enhanced AutoMix* looping/
repeater/reverb transitions — and getting it running natively on macOS 26.

## What RemixFX is

`aufx remx appl` — **"Apple: AURemixFX"** — is a C++ Audio Unit
(`AURemixFX : ausdk::AUBaseProcessAndMultipleLookup`) in **`SonicAudioUnits.framework`**.
It is a DJ-style "LiveRemixer" multi-effect. Internal modules (from the binary's embedded
`.../RemixFXAudioUnit/Source/AURemixExtension/DSP/LiveRemixer/` source paths):
Filter A/B + AutoFilter, Repeater (beat-repeat), FreezeReverb, Flanger, Phaser, Reverse,
Scratcher (tape stop), a Gater, and an LFO bank with phase-sync.

macOS 27's `SmartTransitions` DSP graph (`DSPGraph.dspg`) inserts an `AURemixFX` node, and
transition styles **17–20** (Dance-Repeater, Hiphop-Looping, Dance-Any-Key, Pop-Reverb) drive
its ~40 parameters. On macOS 26 the whole framework is **absent** — every *other* AU in the
graph (`filt/hpas/lpas/dely/rvb2`) already exists; RemixFX is the one missing piece.

## Status

| Piece | State |
|---|---|
| **Apple's native x86_64 AURemixFX running on 26** | **Working & validated.** Registers as `aufx/remx/appl`, instantiates, initializes, and **processes audio** (verified in a standalone harness *and* inside the live Music process). |
| AU available where the graph runs | **Working.** `RemixFX.component` (AU bundle in `~/Library/Audio/Plug-Ins/Components`) serves it in every process — notably **AUHostingServiceXPC**, where AudioQueue runs the SmartTransitions graph out-of-process. |
| AutoMix unlock + catalog/graph redirect | Working (carried over from the Enhanced AutoMix work). |
| Styles 17–20 *chosen & driven* in live AutoMix | **Working (Path A).** Beat-matched transitions pick the RemixFX style authored into slot 8/12; both sides' AURemixFX run 27's envelopes; live meter confirms the audio changes on cue (2026-11-16, see *Driving the transitions*). |
| Chosen **by macOS 27's rules** | **Working (option b, 2026-11-17).** `planner27.c` hooks 26's planner in Music. 26 evaluates 27's styles 17–20 at their real IDs next to its own 8/9/12, with 27's genre gates, base scores and factor lists, and the last-section anchor for Hip-Hop/Pop. The pick is 27's per-style best + seeded weighted random. Live: Hip-Hop pairs weigh 15 and 18 against 26's 8/12; both play through RemixFX when picked (see *Choosing like macOS 27*). Styles 15/16 ("delay out") are evaluated too — a 7th hook on 26's value resolver supplies their beat-timed delay value (26 can't), with 15 verified live. |

## How the transplant works

macOS 27's system volume ships a **native x86_64 shared cache** (the "x86legacy" variant), so
RemixFX exists as real x86_64 code — no arm64e / a2x needed. The pipeline:

1. **Extract** the x86_64 `SonicAudioUnits` image from 27's cache, with the cache slide-info applied so every
   pointer holds its real cache-absolute target. The sidecars below were made from an `ipsw dyld extract --slide`
   extract; `fetch_apple_files.sh` now rebuilds the image with `../tools/dsc_extract_one.py`, whose code and data
   segments are byte-identical to it (only the load commands' page rounding differs), so the same sidecars apply.
   Checked offline: both binaries load, instantiate and render the same output through `sautest`.
2. **Reconstruct fixups** the cache strips out. A sidecar (`SonicAudioUnits.fixups`) records:
   - **1779 rebases** (in-image pointers), found by diffing the `--slide` vs non-slide extracts;
   - **173 data binds** to 25 stable runtime symbols (C++ ABI type-info vtables, `__NSCFConstantString`,
     `NSObject`, `__cxa_*`, typed `operator new/delete`, `__NSConcreteGlobalBlock`), resolved by owner image;
   - **185 shared-GOT imports** — the cache routes code *and* data imports through a cache-wide GOT that
     lives outside the image; every slot referenced by `__TEXT` was enumerated from the disassembly and
     symbolicated (objc/CF/libc++/pthread/mach/`AudioComponentRegister`/`AudioUnitRender`/vDSP/vForce/math/mem…).
     All 185 resolve to symbols that exist on macOS 26.
3. **Load in-process** (`sauload.c`): reserve the image's VM span at a fresh base, map the three segments,
   anon-map the shared-GOT gap pages, apply the sidecar (rebases `+= slide`; binds/GOT via `dlsym` against
   the running 26 process), set segment protections, then call the one exported entry
   `registerSonicAudioUnits()`. That `AudioComponentRegister`s `aufx/remx/appl` with its factory.

No arm64e, no a2x, no second extractor binary at runtime — just Apple's own x86_64 code with its
imports re-bound to macOS 26's libraries. The one ObjC class in the image (an AUv3-extension/timing
helper, unused by the AUv2 path) is left unregistered; its 7 selector/string refs are the only fixups skipped.

## Driving the transitions on 26 ("Path A")

26's SonicKit can't route `remixfx_*` automation, so:

1. **Catalog** (`make_catalog.py` → `payload/Resources/`): a RemixFX style keeps its routable content
   (`ts_rate`, `out_gain`), its `REMIX_FX` lanes are dropped, and a **carrier** lane is added on the
   unused-but-routable `aufilter_center_*` params. By default styles 17–20 keep their **real IDs**, which 26
   evaluates only through `planner27.c` (option b). `REMIXFX_SLOTS=8:18,9:17,12:20` instead authors them into
   26's beat-matched slots 8/9/12, which 26 evaluates even without the planner hooks (option a). 27's original
   styles go to `RemixFXStyles.json` for the engine.
2. **Graph** (`DSPGraph.dspg` = 27's graph with `Fcf1`/`Fcg1` rewired from AUFilter to AURemixFX pseudo-params
   1000/1001).
3. **Carrier**: `aufilter_center_freq` ramps linearly over the transition,
   `value = 10000 + 1000·((style−17)·2 + side) + 100 + 800·progress` (10100–17900 Hz: a plausible center
   frequency, clear of SonicKit's idle 2500). `aufilter_center_gain` = the id the style is served under (diagnostics).
4. **Engine** (`rxdrive.c`, inside every AURemixFX instance via a wrapped dispatch table): decodes the
   carrier and replays 27's `REMIX_FX` envelopes on the real AU in ≤64-frame sub-blocks.
   - **Tempo**: 26's beat-matched planner has **no value for `"parameterName": "beat_length"`** — any lane
     naming it fails the whole transition (`Mapped automation value missing` → `Transition setup failed` →
     Dead-Air Removal). So the carrier uses literals only, and the engine derives the tempo from the ramp:
     the graph runs *before* the player's time-stretch, the carrier is linear in each item's source frames, so
     `beats / source-seconds` of the ramp = that track's native tempo (live: matches the full span within 0.3%).
   - **Truncated regions**: when the incoming song starts at 0:00, SonicKit truncates its region and squeezes
     its ramp into the tail of the transition. Detected by its partner (same style, other side, same process)
     already being part-way through; the span is scaled to whole bars.
   - **Preroll**: incoming graphs are created ~15 s early and process silence with the start value parked on
     the carrier; progress is held until the carrier moves (no extrapolation), extrapolation is capped at two
     slices, and an unchanged parked value doesn't re-arm.
   - **Meter**: while active, input/output/difference energy is logged every 0.5 s (`AUDIO` lines).
5. **Component** (`component.m` + `component/Info.plist` → `RemixFX.component`): maps Apple's image from its
   Resources, captures AURemixFX's factory and hands out engine-wrapped instances. C++ exceptions work in the
   manually mapped image (unwind sections registered via `__unw_add_find_dynamic_unwind_sections`; personality
   + type-info shared-GOT slots bound from `SonicAudioUnits.ehbinds`) — `auval` passes except the benign
   "MakeConnection" (Apple's AU doesn't support it).

Live result (logs `logs/20261116-221712`): outgoing output bit-identical to input until `repeater_on` at
p=0.25, then repeater → gater (0.375) → flanger (0.469) → reverb (0.5) take over while the outgoing fades
to −48 dBFS; the incoming enters through Filter A (−71 → −5 dBFS by p≈0.5). Tempos 71–150 BPM recovered.

## Choosing like macOS 27 (`planner27.c`)

26 has one beat-matched strategy, BMFCF. It evaluates `[8, 9, 12]` with one common genre gate and its own bases
(10/15/10), keeps the best style per region pair, then the overall maximum. 27 (RULES27.md) runs Dance {17, 19},
Hip-Hop {15, 18}, Pop {16, 20} and BMFCF {8, 9, 12}, each with its own genre list, bases and factors (Hip-Hop/Pop
anchored in the outgoing song's last section). It keeps each style's best candidate and makes a weighted random
pick seeded by the song pair. `planner27.c` reproduces that inside Music with six entry hooks on 26's
`_SonicKit_MusicKit_Packages`. The shared-cache text is patched copy-on-write in Music only, and every site is
byte-checked first, so another 26 build just leaves the planner alone.

| hook (26, unslid) | does |
|---|---|
| `0x7ffe071909b4` expected duration | records each song's expected stereo duration (the seed input) |
| `0x7ffe071abdf0` genre check | re-runs 26's own check with 27's Dance / Hip-Hop+R&B / Pop filter lists → per-style gate |
| `0x7ffe0719a1ee` style lookup | BMFCF's `[8, 9, 12]` → `[8, 9, 12, 17, 19, 18, 20]` (the 27 styles in the served catalog) |
| `0x7ffe0719da6a` per-style switch | routes each 27 style through a 26 branch (17, 19 → 9; 18, 20 → 8), remembers its real ID, reads the outgoing song's last section |
| `0x7ffe0719846e` score | 27's base (0 if the genre gate fails), 27's factor list for the style, 17's 90 s cap, Hip-Hop/Pop: outgoing region must end in the last section |
| `0x7ffe071993d4` best-of | per region pair: relabels results with the real style ID and keeps each style's best; at the end: 27's sorted, seeded weighted pick |

Per-style factors, and how they compare with 27's scorers, are in RULES27.md "Option (b) on 26". 26 then builds
the chosen transition with the real style ID, and RemixFX plays it.

`REMIXFX_PLANNER27=0` turns the hooks off (the default catalog then never offers RemixFX; build an option (a)
catalog with `REMIXFX_SLOTS`). `1` (default) logs every decision to the trace, `2` adds durations and the last
section, and `3` adds every candidate score:
```
[planner27] installed 6 hooks (cache slide 0x9043000); BMFCF styles 8 9 12 + 27's 17->b9 19->b9 18->b8 20->b8; ...
[planner27] genre gates: Dance fail, Hip-Hop/R&B pass, Pop fail
[planner27] pick: style 18 | *style 18 (b8) score 77.551 w 77551, style 8 score 15.443 w 15442 | durations 250.427/271.600 s, seed DJB2("250427271600")=…, r=16744 of 92993
[planner27] score style 18 (branch 8): base 10 -> 0 (outgoing ends 143.68 s, before last section 212.89 s), ...
```
Live (logs `logs/20261117-013432`, `…014919`, `…015504`; Autoplay hip-hop pairs): each pick weighed 18 against
26's own 8 or 12 and chose by the seed. 18 played through RemixFX; there were no mapping errors or setup failures.
The weighted pick matches a Python model of 27's rule on 303 random cases.

What still differs from 27: Pop's two outgoing-region checks aren't applied.
26's key check means "compatible" where 27's style 17 wants "identical". Candidate positions are 26's BMFCF end
downbeats, filtered to the last section for Hip-Hop/Pop.
Option (a) (`REMIXFX_SLOTS=…`) still works: the slots carry 27 styles, scored and picked by the same rules.

## Authoritative parameter map (read live from the running AU)

40 params. AU index ↔ DSP-graph code (`DSPGraph.dspg`) ↔ `remixfx_*` catalog id ↔ module.

| idx | code | catalog id | name | min | max | default |
|--:|---|---|---|--:|--:|--:|
| 0 | RXxt | remixfx_beats_per_minute | Beats Per Minute | 20 | 300 | 120 |
| 1/2/3/32 | RXae/af/ar/at | remixfx_filter_a_{on,cutoff,resonance,type} | Filter A | — | — | cutoff 1000, type 0–2 |
| 4/5/6/33 | RXbe/bf/br/bt | remixfx_filter_b_{on,cutoff,resonance,type} | Filter B | — | — | — |
| 7/8/9 | RXpe/pr/pm | remixfx_repeater_{on,rate,mix} | Repeater | rate 0–7 | | mix 1 |
| 10/11/12/36/37 | RXve/vt/vs/vd/vw | remixfx_reverb_{on,time,send,dry,wet} | Reverb | time 1–200 | | dry/wet 1 |
| 13/14/15/16 | RXde/dr/df/dg | remixfx_delay_{on,rate,feedback,level} | Delay | rate 0–23 | | rate 10 |
| 17/18/19/20/21/22 | RXge/gr/gd/gw/gg/gf | remixfx_gater_{on,rate,depth,pw,noise_level,noise_cutoff} | Gater | rate 0–15 | | depth 1 |
| 23/24/25/26/34/35 | RXfe/fr/fm/ff/fd/fc | remixfx_flanger_{on,rate,mix,feedback,depth,center} | Flanger | rate 0–15 | | mix 1 |
| 27/28/29/30 | RXse/sr/sf/sm | remixfx_phaser_{on,rate,feedback,mix} | Phaser | rate 0–15 | | mix 0.5 |
| 31 | RXls | remixfx_lfo_phase_sync | LFO Phase Sync | 0 | 1 | 0 |
| 38/39 | RXte/tr | remixfx_tape_stop_{on,rate} | Tape Stop | rate 3–15 | | rate 7 |

(Full live dump in `paramtable.txt`.)

## Files

- `fetch_apple_files.sh` — copies Apple's files from your own installs (hash-checked against the supported builds):
  27's catalog + DSP graph into `../reference/`, and `payload/SonicAudioUnits`, rebuilt from 27's shared cache.
- `payload/SonicAudioUnits` — Apple's x86_64 AURemixFX (slide-applied). **Not in the repo**; made by the fetch script.
- `payload/SonicAudioUnits.fixups` — the fixup sidecar (rebases + data binds + GOT imports).
- `sauload.c` — the in-process loader (`sau_load(dylib, fixups)` → `registerSonicAudioUnits` pointer).
- `payload/SonicAudioUnits.ehbinds` — personality / catch type-info slots for C++ exception support.
- `RemixFXEnabler.m` / `.dylib` — injected into Music: AutoMix unlock + catalog/graph redirect (+ in-process
  AU only if the component isn't installed, or with `REMIXFX_INPROCESS=1`).
- `component.m`, `component/Info.plist` — the `RemixFX.component` AU bundle (built + installed by install.sh).
- `rxdrive.c/.h` — the Path A engine (carrier decode, envelopes, tempo, wrapped AU dispatch, trace).
- `rxenv.m` — loads 27's REMIX_FX envelopes from `RemixFXStyles.json`.
- `make_catalog.py` — builds the served `TransitionStyles.json`, `RemixFXStyles.json`, `DSPGraph.dspg`.
- `planner27.c/.h`, `hook_x64.s` — macOS 27 planning rules for 26's beat-matched planner (entry hooks, the
  shared register-saving trampoline, and `p27_swiftcall` for calling 26's own Swift accessors).
- `rxsim.m` — offline engine validation: drives the carrier like SonicKit and renders a synthetic loop through
  the real AU (`RXSIM_BEATLEN=1` explicit tempo, `RXSIM_TRUNC=f` truncated-partner case).
- `run_test.sh N [PLAYLIST]` — relaunch Music, force N AutoMix transitions at low volume, collect planner logs +
  engine traces into `logs/<time>/` (`START_INDEX=n|last` to start at a playlist track; `SEEK_BEFORE_END`).
- `sautest.m` — standalone validation harness (register → instantiate → enumerate 40 params → render).
- `install.sh` — `./install.sh` (stage + install component + launch Music), `--stage` (no launch), `--launcher`
  (create `~/Applications/Music (RemixFX).app`), `uninstall`. Runs the fetch script first if Apple's files are missing.
- `paramtable.txt` — the 40-parameter dump from the live AU.

## Install / test / uninstall

```sh
./fetch_apple_files.sh   # once: Apple's files from your macOS 26 + mounted macOS 27 volume (--vol27 PATH)
./install.sh --launcher  # build, stage, install the component, create "Music (RemixFX).app"; open Music via it
./install.sh --stage     # build dylib, stage payload under ~/Library/Application Support/RemixFX
./install.sh             # also quit+relaunch Music with the dylib injected
./install.sh uninstall   # remove payload, component and launcher
START_INDEX=last SEEK_BEFORE_END=80 WAIT=84 REMIXFX_PLANNER27=3 ./run_test.sh 4 Playlist
                         # live test from the last track (→ Autoplay); 80 s lead so 16-bar styles fit; logs/<time>/
log stream --predicate 'subsystem == "dev.remixfx"' --info     # engine events from AUHostingServiceXPC
```
The engine also writes `/tmp/remixfx-trace-<process>-<pid>.log` (ACTIVATE / BPM / APPLY / AUDIO / DEACTIVATE).

## Remaining work

- **A/B against macOS 27** by ear/recording (same song pair): Path A replays 27's envelopes; what 27 sends as
  `bpm` may differ from the native-tempo estimate on time-stretched sides.
- **Pop's two outgoing-region checks** (outgoing region bars = style bars, outgoing loudness significant) and
  Dance 17's "identical" key check (26 uses "compatible").
- **Dance/Pop live wins**: 15 and 18 (Hip-Hop) pick and play live; 16/20 (Pop) and 17/19 (Dance) are routed,
  scored and genre-gated correctly but haven't won a pair live (Autoplay rarely serves two consecutive Pop or
  Dance songs). 16 resolves its delay value through the same hook already proven on 15.
- Library-to-library transitions fail before planning on this machine (`NOT NULL constraint failed:
  cloud_library_source.cloud_revision`, Apple's content DB); Autoplay/store tracks plan fine.
- A truncated *outgoing* region (song ends early) isn't corrected (logged as `ended-before-partner`).
- Path B (native `remixfx_*` mapping in SonicKit) was not needed.

## Provenance / scope

RemixFX here is **Apple's own x86_64 binary**, read from your own macOS 27 install and run locally on your
macOS 26 for interoperability. This repository contains none of Apple's code or resource files; the fetch script
copies them from your machine. The sealed system volume is never modified. Loading the ad-hoc-signed dylib into
Music needs AMFI relaxed (boot-arg `amfi_get_out_of_my_way=1`).
