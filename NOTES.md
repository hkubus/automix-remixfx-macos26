# Porting macOS 27 Music (AutoMix) to macOS 26: findings so far

Status: **investigation only. Nothing has been modified on either OS.**
Last worked on 2026-11-15, from the macOS 27 install.

## TL;DR

1. **macOS 26's Music app already has AutoMix.** Music 1.6.6 ships the AutoMix
   class, menu item, and "AutoMix Is On" sheet. What 27 adds is
   **"Enhanced AutoMix"**, which uses loops, reverb, delay and other effects in transitions.
2. You can't simply copy the 27 Music.app onto 26. On this machine, 27's apps
   and system libraries are **arm64e only** and run through NullMoth's `a2x`
   translator. 26 has a native x86_64 userland with older frameworks.
3. The realistic next step is to find out **why AutoMix doesn't show up on 26**,
   if it doesn't (still unconfirmed), and unlock it there. Backporting *Enhanced*
   AutoMix is a separate, much harder problem.

## The machine

| | macOS 27 (currently booted) | macOS 26 |
|---|---|---|
| Version | 27.2 (26B123) | 26.7.3 (25G342) |
| System volume | "macOS 27", mounted at `/`, **unsealed** | mounted at `/Volumes/Macintosh HD`, **sealed** |
| Shared cache | `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e*` (**arm64e only**) | inside `/System/Volumes/Preboot/<data-volume-UUID>/cryptex1/current/os.dmg` → `System/Library/dyld/dyld_shared_cache_x86_64h*` |
| Music.app | 1.7.2 | 1.6.6 |

- Hardware: an x86_64 PC running macOS (OpenCore). macOS 26 runs natively on x86_64.
- SIP is fully off on 27: filesystem, kext, debugging, NVRAM and authenticated root are all disabled.
- 27 runs a custom x86_64 `/System/Library/Kernels/kernel`.
- **a2x**, the arm64→x86_64 translator from NullMoth Liberator
  (github.com/nullmoth/liberator), is installed in `/usr/libexec/a2x/`
  (`a2xhost`, `a2xhost-mac`, `a2xhost-cat`, `a2xpre.dylib`, `a2xroute`).
- 27's `Music.app/Contents/MacOS/Music` has been **replaced** by a fat binary with these slices:
  - x86_64 (3.3 MB): the a2x host stub. It's ad-hoc signed with identifier `.a2xhost-mac` and links only Foundation, CoreFoundation, libffi and libSystem.
  - arm64e (36 MB): Apple's original code, which links about 150 libraries.
  - a second arm64e slice (cpusubtype 12): purpose unknown.
- a2x debug logs go to `/var/tmp/a2x-reports/`.

## AutoMix: what exists where

**Music app strings (en.lproj)**

| String | 26 (1.6.6) | 27 (1.7.2) |
|---|---|---|
| `SoundGeneration.strings`: `AUTO_MIX_MENU_TITLE`, `FULL_SHEET_TITLE` "AutoMix Is On", `FULL_SHEET_BODY_COPY` | yes | yes |
| `UserInterface.strings`: `PLAY_QUEUE_AUTOMIX_BUTTON_TITLE` | yes | yes |
| `Localizable.strings`: "Like a DJ, AutoMix seamlessly transitions…" (`f4tmqm8sb5`) | yes | no |
| `Localizable.strings`: **"Enhanced AutoMix"** (`q9rfaw0pw6`), "…richer palette of effects – including looping, reverb, delay…" (`ntd09nvsvw`) | no | **yes** |

**26 Music binary symbols:** `_TtCC5Music21MusicPlayerController7AutoMix`, `doAutomix:`,
`AutoMixBadge`, `AutoMixFullSheetShown`, `automixImage`, `AUTOMIX_OK_BUTTON`.

Running `strings` on 27's Music binary finds **zero** "automix" hits, even though its
localization has them. I couldn't explain this. My guess is the a2x-wrapped slices are
transformed or compressed, but I haven't verified it.

**Feature flags** (`/System/Library/FeatureFlags/Domain/*.plist`): **neither OS has a flag named
AutoMix.** The flag sets do differ:

| Domain | Only in 26 | Only in 27 |
|---|---|---|
| Music | RedSunBottomPlayer, RedSunImmersionPlayer, SNPlayer, atmos_passthrough | Concerto, SMPreviewPlayer, SMVideoWindow, SonicSharePlay, cloud_album_metadata, library_motion_artwork, use_native_music_grid |
| MusicUI | | containers_visual_redesign, content_density, frog_spirit |
| MusicKit | | campo_snippets, live_release_albums, modern_library_item_state, on_device_concert_ranking, swift_media_preview_player |
| MediaPlayer | | AlbumPageRedesign, RFLWRP |

MediaPlayer `cross_fade` is FeatureComplete on both. AMPLibrary has no flags on either OS.

**System frameworks with transition/AutoMix strings** (found with `tools/dsc_grep.py`):

| Framework | Hits in 26 | Hits in 27 | Notes |
|---|---|---|---|
| `PrivateFrameworks/MediaPlaybackCore` | 40 | 32 | has `autoMix` on both; 27 adds HomeKit-speaker "migrate default transitionStyle to AutoMix (Smart)" |
| `PrivateFrameworks/_SonicKit_MusicKit` | 28 | **65** | **prime suspect for the transition engine**; much bigger in 27 |
| `PrivateFrameworks/_SonicKit_MusicKit_Packages` | 16 | 0 | |
| `PrivateFrameworks/MusicKitInternal` | 6 | 6 | |
| `Frameworks/MediaPlayer` | 13 | 9 | |
| `Frameworks/MusicKit` | 4 | 4 | |

The search regex also matched noise: `libusd_ms`, `libswiftsimd`, SwiftUI and others matched
`beat.?match` or `transitionStyle` for unrelated reasons. I haven't looked at the hits in detail yet.

## Open questions (pick up here)

1. **Does AutoMix actually appear in Music on 26 on this machine?** Boot 26 and look:
   - next to AutoPlay in the player or queue,
   - in the Controls menu.

   If it's there, the remaining goal is only "Enhanced AutoMix".
2. If it's missing on 26, **find the gate.** Possible causes:
   - an Apple-silicon or CPU check (26 runs natively on x86),
   - the SMBIOS model,
   - an Apple Media Services bag or account/server setting,
   - a missing ML/asset download.

   Ways to look:
   - `log stream --predicate 'process == "Music"'` while opening the queue,
   - `defaults read com.apple.Music`,
   - disassembling the availability check around `MusicPlayerController.AutoMix` in **26's
     x86_64 Music slice**. It's native code, so `otool -tV` and `lldb` work and a2x doesn't get in the way.
3. If the gate is found, unlocking it could take one of these forms:
   - a `defaults` key,
   - a FeatureFlags override in `/Library/Preferences/FeatureFlags/Domain/` on 26's **data** volume,
   - a hook dylib.

   Avoid touching 26's sealed system volume if possible. Editing it breaks the seal and
   needs a re-blessed snapshot.
4. **Enhanced AutoMix:** diff `_SonicKit_MusicKit` and `MediaPlaybackCore` exports/ObjC/Swift types
   between 26 and 27. If the new effects live mostly in `_SonicKit_MusicKit`, a backport means
   transplanting a 27 framework into 26. But 27's copy is arm64e only and depends on 27 APIs, so it
   won't load natively on x86 26. That would need a2x working on 26, which is unknown.
5. A wholesale copy of 27's Music.app to 26: **not viable** in practice. It would need a2x plus
   essentially the whole 27 arm64e userland.

## How to reproduce / continue

```sh
# Mount macOS 26's OS cryptex (read-only) to get at its shared cache
mkdir -p ~/os26 && hdiutil attach -readonly -nobrowse -noverify -mountpoint ~/os26 \
  /System/Volumes/Preboot/<data-volume-UUID>/cryptex1/current/os.dmg
# ... when done:  hdiutil detach ~/os26

# Search a shared cache for a regex and attribute hits to images (--show prints the strings)
python3 -I tools/dsc_grep.py ~/os26/System/Library/dyld/dyld_shared_cache_x86_64h 'automix' --show
python3 -I tools/dsc_grep.py /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e 'automix' --show

# Grep plists / .strings / loctables (handles binary plists and UTF-16)
python3 -I tools/plgrep.py grep automix "/Volumes/Macintosh HD/System/Applications/Music.app/Contents/Resources/en.lproj/"*.strings
python3 -I tools/plgrep.py keys . /System/Library/FeatureFlags/Domain/Music.plist
```

Limitations of `dsc_grep.py`:
- It does a raw byte search, so it misses UTF-16 strings and Swift small strings
  (15 bytes or less) that are embedded in instructions.
- `?` means the hit fell outside any image segment, for example in shared ObjC/string pools.

**Missing tooling:** ipsw, radare2/rizin, Ghidra and class-dump aren't installed. Xcode
and `lldb`/`otool`/`nm`/`dyld_info` are. To pull individual frameworks out of either cache for
symbol diffing, `ipsw dyld extract` (`brew install blacktop/tap/ipsw`) would help.

---

# Session 2026-11-16 (booted into macOS 26)

## State before this session
- Basic AutoMix already works on 26 (thread "Automix with Apple Music on Intel Macs"):
  `~/Applications/Music (AutoMix).app` launches Music with `~/Library/Application Support/AutoMixEnabler/AutoMixEnabler.dylib`.
  That setup was **not changed**.

## What "Enhanced AutoMix" is, concretely
- 27 ships a bigger `TransitionStyles.json` (24 styles vs 14), in `_SonicKit_MusicKit.framework/Resources` (26 keeps it in
  `_SonicKit_MusicKit_Packages.framework/Resources`). New: 5 short filter, 13 filter soft+reverb, 14 no vocal clash,
  15 Hiphop delay out, 16 Pop delay out, 17 Dance repeater, 18 Hiphop looping, 19 Dance any key, 20 Pop reverb, 22 splice.
- 27's `DSPGraph.dspg` is 26's graph plus one AudioUnit `aufx remx appl` ("AURemixFX", ~40 params) used by styles 17-20.
- 27's planner adds strategies BeatMatchedDance / BeatMatchedHipHop / BeatMatchedPop / SoftSkip (26 has Fallback, Dead-Air
  Removal, Smart Cross-Fade, Beat-Matched Filtered Cross-Fade).

## Findings on 26
- The planner loads TransitionStyles.json / DSPGraph.dspg through NSBundle, so they can be redirected in-process (done in
  `eam/EnhancedAutoMix.m`, resources in `~/Library/Application Support/EnhancedAutoMix/Resources/`).
- 26 decodes 27's catalog fine (node names are free-form; parameter IDs like player_gain, lp_reso, fx_delay_feedback are known)
  EXCEPT `remixfx_*` parameter IDs (Swift enum) -> styles 17-20 must be left out.
- The catalog is loaded once per process and cached.
- 26's Beat-Matched Filtered Cross-Fade only ever evaluates style IDs 8, 9, 12: a hard-coded switch at
  `_SonicKit_MusicKit_Packages` 0x7ffe0719dd05 (unslid) with per-ID region logic; other IDs -> no candidate.
  So new style IDs are never chosen; the only data-only option is putting 27 styles into slots 8/9/12.
- Slot-substitution test (8<-15, 9<-16, 12<-13) was inconclusive: no beat-matched transitions happened in that run.
  Several transitions failed with `NOT NULL constraint failed: cloud_library_source.cloud_revision` (Apple's content DB,
  library tracks) - appears unrelated to the catalog, also seen in another run.
- 26's shared cache fails its code-signature check (modified on disk at some point), so dsc_extractor refuses it.
- Dropped: relinking 27's SonicAudioUnits for 26 / patching planner code. Not pursued further.

## Files
- `eam/EnhancedAutoMix.m|.dylib` - AutoMixEnabler patches + NSBundle resource redirect (research build; not wired to any launcher)
- `eam/run_test.sh` - relaunch Music with the dylib, force N transitions at low volume, dump logs to `eam/logs/`
- To undo the experiment entirely: delete `~/Library/Application Support/EnhancedAutoMix/` (Music only reads it when
  started with EnhancedAutoMix.dylib).
- System changes made during the session (private-data logging, debug log levels) were reverted.

---

# Session 2026-11-16b (booted into macOS 26): RemixFX transplanted to x86_64

**Result: Apple's own native x86_64 `AURemixFX` (`aufx remx appl`) now runs on macOS 26** — registers,
instantiates, initializes, and processes audio, both in a standalone harness and inside the live Music
process (`[RemixFX] aufx/remx/appl AVAILABLE : Apple: AURemixFX`). Full writeup: `03-remixfx/README.md`.

## Key discoveries that overturned earlier assumptions
- macOS 27's **system volume ships native x86_64 shared caches** (`/Volumes/macOS 27/System/Library/dyld/
  dyld_shared_cache_x86_64*`, the "x86legacy" variant) — RemixFX exists as real x86_64 code, so **no arm64e/
  a2x is needed**. The earlier "27 is arm64e-only" note was about the *booted* arm64e cache.
- `AURemixFX` lives in **`SonicAudioUnits.framework`** (absent on 26), class
  `AURemixFX : ausdk::AUBaseProcessAndMultipleLookup`, registered via the single exported
  `registerSonicAudioUnits()`. Deps are all stable system frameworks present on 26.

## Why extraction alone didn't work, and the fix
- Both `/usr/lib/dsc_extractor.bundle` (26 and 27 versions) and `ipsw dyld extract` produce **reverse-
  engineering-only** images: segments keep full cache VM addresses, `__got` is mostly zero, and there are
  **no reconstructed fixups** → crash in `dlopen`.
- The cache routes imports through a **cache-wide shared GOT** outside the image (both code stubs *and*
  `mov rax,[rip→GOT]` data loads). Those slots are the missing imports.
- Solution = a custom in-process loader (`03-remixfx/sauload.c`) + a fixup sidecar built offline:
  - **1779 rebases** — diff of `ipsw --slide` vs non-slide extracts (changed 8-byte slots = fixups; slid value = target; in-image → rebase).
  - **173 data binds** — cross-image targets → 25 stable symbols (cxxabi type-info vtables @+16, `__NSCFConstantString`, `NSObject`, `__cxa_pure_virtual`, typed `operator new/delete`, `__NSConcreteGlobalBlock`). The image's own objc class is left unregistered (7 selector/string refs skipped).
  - **185 shared-GOT imports** — every RIP-relative target in `__TEXT` landing in the shared-GOT range, symbolicated via `ipsw dyld a2s`; the 6 ifunc-resolver ones (memcpy/memmove/memset/memset_pattern16, vvfloorf/vvfmodf) identified by call-site arg setup + undef-list elimination.
  - Loader: reserve the ~471 MB VM span at a fresh base, map 3 segments (page-floor + memcpy to the unaligned cache offsets), anon-map shared-GOT gap pages, apply rebases (`+= slide`) and binds/GOT (`dlsym` on 26, strip one leading `_`, variant-name fallbacks), set protections, call `registerSonicAudioUnits()`.
- Gotcha fixed: some "stub" slots are in the image's own `__got` **inside DATA_CONST** — must NOT be anon-remapped (that zeroed a `dispatch_once` block literal). Only gap/shared-GOT slots get fresh pages.

## Authoritative 40-param map
Read live from the running AU (`03-remixfx/paramtable.txt` / README table). Confirms the DSPGraph-derived
map with Apple's real names: BPM, Filter A/B(+type), Repeater, Reverb(time/send/dry/wet), Delay, Gater
(on/rate/depth/pw/noise level/noise cutoff), Flanger, Phaser, LFO Phase Sync, Tape Stop.

## Still missing for RemixFX transitions to *play* (two separate SonicKit gates)
1. 26's `_SonicKit_MusicKit_Packages` has **no `remixfx_*` → graph-param mapping** (compiled in; confirmed
   zero hits on 26). Styles 17–20's `REMIX_FX` automations don't route.
2. 26's Beat-Matched planner only evaluates style IDs **8/9/12** (hard-coded).
Best next path: **drive AURemixFX directly from `RemixFXEnabler.dylib`** during a slot-8/9/12 transition,
bypassing both gates. Tooling note: `ipsw` (blacktop) downloaded to scratch did the cache extraction + symbolication.

---

# Session 2026-11-16c: RemixFX transitions play in live AutoMix on 26 (Path A)

**Result:** beat-matched AutoMix transitions now pick the RemixFX style in slot 8 (or 12), and Apple's
AURemixFX on both sides runs 27's envelopes. Live meter: outgoing bit-exact until repeater_on at p=0.25, then
repeater → gater → flanger → reverb with the outgoing fading out; incoming filtered in. Writeup: `03-remixfx/README.md`
("Driving the transitions on 26"). Logs: `03-remixfx/logs/20261116-221712`.

## Findings
- The SmartTransitions DSPGraph AU runs **out-of-process in AUHostingServiceXPC** (AudioQueue opens it remotely),
  so an in-process `AudioComponentRegister` is invisible there (`box component ('aufx' 'remx' 'appl') not found`).
  Fix: ship the AU as `~/Library/Audio/Plug-Ins/Components/RemixFX.component`.
- Manually mapped image needed C++ EH: register unwind sections (`__unw_add_find_dynamic_unwind_sections`) and bind
  the personality + catch type-info shared-GOT slots (`payload/SonicAudioUnits.ehbinds`), else auval aborts on the
  first AUException.
- **`"parameterName": "beat_length"` kills a beat-matched transition on 26** — `Mapped automation value missing`
  → `Transition setup failed internalError` → Dead-Air Removal. 26 only uses beat_length in style 10, which its
  beat-matched planner never evaluates. Carrier lanes must use literal values.
- Graph params are pushed to boxes once per 8192/8948-frame slice, on the render thread, via SetParameter.
- The graph sits **before** the player's time-stretch; each side's carrier is linear in that item's source frames,
  so ramp slope × beats = native tempo per side. Style ts_rate lanes (1→2 / 2→1) mean each side plays exactly
  its style's bar count of its own beats.
- Incoming graphs are created ~15 s before the transition and process silence with the start value parked.
- If the incoming song starts at 0:00 the region is truncated and the incoming ramp covers only the tail
  (seen: 6.5 s of a 13.7 s transition) → engine scales that side's beat count using its partner's progress.
- Planner on this hip-hop library: slot 8 scores ~6–7.5, slots 9/12 usually 0; many pairs rejected for
  "Musicality attributes not compatible" / "Tempos not compatible". `run_test.sh` START_INDEX doesn't keep
  playlist order beyond the first track (AutoMix reorders the queue).

# Session 2026-11-16d: macOS 27 planner rules extracted (static RE)

Full write-up: `03-remixfx/RULES27.md`; genre lists with names in `03-remixfx/re27/genres27.json`.
- 27 adds Beat-Matched Dance (17,19), Hip-Hop (15,18) and Pop (16,20) ahead of BMFCF (8,9,12). Each sits behind
  the same common genre gate (52 allowed / 89 denied, identical to 26's BMFCF lists) plus its own allowed
  genre: Dance 17; R&B/Soul 15 + Hip-Hop/Rap 18; Pop 14.
- Applicability is the same for all four, and its thresholds equal 26's:
  - danceability ≥ 0.3;
  - acousticness < 0.85;
  - time signature identical;
  - tempo |ln ratio| ≤ 0.287 after choosing the best ×½/1/2 scale;
  - per-candidate tempo 0.16;
  - at least 8 matching bars.
- Score = base × Π factors + 0.001 × duration. 27 bases: Dance 17→75, 19→17; Hip-Hop 100; Pop 50; BMFCF 9/12→50,
  8→20. 26 bases: 9→15, 8→10, 12→10.
- 27 selection: prefer beat-matched results, then a weighted random pick (weight = score × 1000). The seed is
  DJB2 of the two songs' expectedStereoDuration in ms, so it's deterministic per pair. 26 has no selector or RNG:
  the first algorithm that yields a result wins.
- Hip-Hop and Pop anchor the outgoing region in the **last section**; Dance and BMFCF anchor after the first phrase
  start. Style 15 needs dynamic_beat_length; style 16 needs a beat-duration delay (both unreachable on 26 slots).
- New RE helpers in `tools/`: callers.py, fconst.py (+fstarts.py), genrescan.py.

# Session 2026-11-17: option (a), macOS 27's selection rules inside 26's planner

Write-up: `03-remixfx/README.md` ("Choosing like macOS 27") and `03-remixfx/RULES27.md` ("Option (a) on 26").
- `planner27.c` + `hook_x64.s` live in RemixFXEnabler.dylib. They hook 4 function entries of 26's
  `_SonicKit_MusicKit_Packages` in Music (shared-cache text patched with `VM_PROT_COPY`, which works with SIP off
  and AMFI permissive; every site and call site is byte-checked first):
  - expected duration `0x7ffe071909b4`: records the seed input.
  - genre check `0x7ffe071abdf0`: re-run with 27's per-strategy lists.
  - score `0x7ffe0719846e`: swaps in 27's base and applies the genre gate.
  - best-of `0x7ffe071993d4`: per-slot best, then 27's seeded weighted pick.
- New RE facts:
  - 27 keeps one result per style per strategy (score > 0, first maximum wins).
  - 27 sorts results by score desc, then style ID asc, before the weighted pick.
  - 26's slot 12 only fires in the expanded tempo range (0.16 < |ln ratio| ≤ 0.287).
  - Only slot 9 has a tonality factor.
  - 26 and 27 share the GenreFilter layout. 27's Dance list = 26's BMFCF list with element 1 replaced by
    `tag0(["17"])`.
- Default slots changed to `8:18,9:17,12:20`, the mapping with no tempo/key false positives vs 27.
- Env: `REMIXFX_PLANNER27=0` (off), `1` (log decisions, the default), `2`, `3` (every candidate score).
- Live test `logs/20261117-004225` (Autoplay hip-hop):
  - genre gates came out Hip-Hop pass, Dance and Pop fail;
  - slots 9 and 12 were gated to 0, and slot 8 used base 100;
  - style 18 played.
  The weighted pick matched a Python model on 303 synthetic cases.
- Gotchas:
  - Library-to-library transitions fail before planning on this machine (`NOT NULL constraint failed:
    cloud_library_source.cloud_revision`). Test from the playlist's last track (`START_INDEX=572`) so Autoplay
    takes over.
  - Shuffle can't be set while the queue is in Autoplay. `run_test.sh` now reloads the playlist muted to
    restore it.

### Session 2026-11-17b — option (b): 27's styles at their real IDs inside 26's BMFCF
- Catalog: `make_catalog.py` now defaults to 17–20 at their real IDs with carrier lanes (27's own 8/9/12 kept).
  `--slots` / `REMIXFX_SLOTS` still builds option (a). Files are only rewritten when their content changes, so
  `install.sh` runs it every time.
- Two more hooks in `planner27.c` (6 total):
  - style lookup `0x7ffe0719a1ee`: the static `[8,9,12]` at `0x7ffe4141d798` becomes `[8,9,12,17,19,18,20]`.
  - per-style switch `0x7ffe0719da6a` entry: rewrites element IDs (17, 19 → 9; 18, 20 → 8) and records each
    element's instruction arrays → real ID.
  The score hook prunes factors to 27's lists (RULES27.md "Option (b) on 26"). The best-of hook relabels
  result +0x08 with the real ID.
- New RE:
  - 27 Hip-Hop 18 scorer `0x7ffe0811d9da`: [vocals ×0.75, r ≥ 0.75, r, loudness], base 100.
  - 27 Pop 20 `0x7ffe08120f83`: [outgoing bars = style bars, r ≥ 0.75, vocals, outgoing loudness significant,
    loudness], base 50.
  - 26 branch-8 bar score = incoming bars × 0.125 (`0x7ffe071a1a2d`).
  - 27 Hip-Hop/Pop end downbeats = downbeats ≥ last section start (`0x7ffe0811c179`).
  - 26: `Section.startTime` `0x7ffe0719752b`, `Region.timeRange` `0x7ffe0717d290` (self r13, result via rax). They
    are called through `p27_swiftcall`.
  - The da6a arg rsi is the outgoing song structure (sections at +0x38). The region pair under evaluation is at
    rbp−0x2c0.
  - The score input `x` is a time (~110–230 s), not the transition duration (RULES27 corrected). Style 17's
    90 s cap now uses the outgoing region length.
- Live (Autoplay hip-hop, `logs/20261117-013432`, `…014919`, `…015504`):
  - each pick weighed 18 against 26's 8 or 12;
  - 18 was picked and played through RemixFX (style=18, slot=18);
  - no mapping errors or setup failures;
  - last-section zeroing is visible in the trace.
- 15/16 done (2026-11-17): a 7th hook on 26's value resolver `0x7ffe071ba330` (`resolveMapped`) returns the
  by-name delay value (`beat_length` / `dynamic_beat_length`) that 26's empty +0x40 dict lacks, instead of
  building the Swift dict. `h_best` stashes the chosen 15/16 result's outgoing beat length (regionLen / (bars×4)
  ≈ 27's `60/BPM`, which 27 snaps to 0.5 BPM at `0x7ffe08121b36`); `h_resolve` returns it in xmm0 (r12=0, early
  return via the hook ABI) on the tag-1 lookup path. Verified live: style 15 picked and pivoted ~7× on hip-hop
  pairs (80–172 BPM), zero "Mapped automation value missing", no crash. 16 shares the path; no live Pop win yet.
- Test notes:
  - `SEEK_BEFORE_END=38` only leaves room for 4-bar styles, because placement constraints drop everything
    before the current position. Use 80.
  - `START_INDEX=last` (the playlist shrank from 572 to 569 tracks).
  - One run stalled for 2.5 min after a seek with no log activity (not the planner).
