# macOS 27 Enhanced AutoMix: how the planner picks a transition

Reverse-engineered from 27's x86_64 `_SonicKit_MusicKit` (`TransitionPlanner`), compared with 26's
`_SonicKit_MusicKit_Packages`. Static analysis only. Nothing was run on 27.

Marks: **✔** read directly from code or constants. **~** inferred from structure or log strings, not traced
instruction by instruction. **?** not extracted.

Genre lists with names: [re27/genres27.json](re27/genres27.json) (IDs only: `re27/genres27.ids.json`).

---

## 1. Strategies, in catalog order ✔

| # | Strategy | Style IDs | Genre filter |
|---|---|---|---|
| 1 | Beat-Matched Dance | 17, 19 | common gate **and** allowed = {Dance (17)} |
| 2 | Beat-Matched Hip-Hop | 15, 18 | common gate **and** allowed = {R&B/Soul (15), Hip-Hop/Rap (18)} |
| 3 | Beat-Matched Pop | 16, 20 | common gate **and** allowed = {Pop (14)} |
| 4 | Beat-Matched Filtered Cross-Fade (BMFCF) | 8, 9, 12 | common gate |
| 5 | Smart Cross-Fade | 3, 33, 4, 44 | none |
| 6 | Dead-Air Removal | 0 | denied = {Classical (5)} |
| 7 | Soft Skip | 5 | none (early-to-early placement only) |
| 8 | Fallback Cross-Fade | 1 | none |

**Common gate** = `genreIDs(allowed: 52 IDs, denied: 89 IDs)`. These are the same two lists, byte for byte, as
26's BMFCF uses ✔.
- **Allowed:** Pop, Dance, Electronic, Hip-Hop/Rap, R&B/Soul, Country, Children's, Holiday, Reggae, J-Pop, Anime,
  Latin pop/urbano, Afrobeats/Amapiano/Afro House…, Fitness & Workout, Karaoke, Instrumental, Lounge,
  Christian Pop/Rap, Indie Pop, Pop Punk, Indian regional pop, and others.
- **Denied:** Classical, Jazz (all subgenres), Comedy, Singer/Songwriter, Soundtrack/Score/Musicals, Christian,
  Vocal, Enka, Grunge, New Wave, New Age/Healing/Meditation, Christmas subgenres, Adult Contemporary, Motown,
  World/regional roots, Spoken Word, Disney, Video Game, and others.

~ The genre test uses each song's genre tree, meaning its significant genres plus their ancestors and
descendants. It is applied to **both** songs. "Allowed" needs an overlap; "denied" fails on any overlap.

## 2. Applicability gate: the same for all four beat-matched strategies ✔

The functions at `0x7ffe081349a8`, `…35d00`, `…36fb0` and `…381c0` make identical calls. Every one of these
must pass:

- **Placement:** late-to-early. Soft Skip is the only strategy that takes early-to-early.
- **Complexity:** the maximum complexity allows beat-matched, and there are no timing-accuracy issues.
  Non-subscription items downgrade to fallback. A spatial asset with no edit list downgrades to
  cross-fade-with-effects.
- **Song structure** is present for both songs.
- **Genre filter** (table above).
- **Tempo compatible** at tolerance **0.287** (see §4). If either song's first tempo is unknown, or the outgoing
  song has several tempos, this counts as compatible.
- **Time signature:** first time signatures are *identical*. The only compatible case is `.identical`; unknown
  counts as compatible.
- **Danceability:** significant or unknown for **both** songs. Significant means `0.3 ≤ main ≤ 1.0` ✔.
- **Acousticness:** insignificant or unknown for **both** songs. Significant means `0.85 ≤ main ≤ 1.0`, so a
  song passes when `main < 0.85` ✔.

## 3. Candidate generation: where each strategy differs

All four share one skeleton: pick candidate end-downbeats in the outgoing song, pick candidate downbeats in the
incoming song, then build a region pair per style. **Region lengths in bars come from each style's catalog
entry.** That data is already transplanted.

| | Dance | BMFCF | Hip-Hop | Pop |
|---|---|---|---|---|
| Outgoing anchor ✔ | downbeats after the **first phrase start** | same as Dance | inside the **last section** | same as Hip-Hop |
| Incoming anchor ✔ | section boundaries; the first is dropped if it is less than **16 bars** from the song start (~: value seen in 26's live log, "Discarding first section boundary … less than 16 bars"; 27 passes it as a parameter) | same as Dance | ? | ? |
| Outgoing regions | non-silent only (loudness map) | same | same | same, plus *outgoing loudness significant* |
| Per-candidate tempo check ✔ | relationship at **0.16** | 0.16, then an "expanded tempo range" retry at **0.287** with a shifted region pair | none (uses a scaled region pair) | none (scaled, truncated or shifted pair) |
| Matching bar count ✔ | **≥ 8** | ≥ 8 | — | — |
| Tonality ✔ | style 17: tonalities **identical**, or melodicness insignificant on either song | "compatible" (rule **?**), or melodicness insignificant | — | — |
| Vocals ✔ | style 19: ×0.75 if the incoming song has significant leading vocals | vocal-activity relationship | leading-vocal factor | leading-vocal factor |
| Extra automation ✔ | — | — | style 15: delay time from the **scaled beat duration** (`dynamic_beat_length`) | style 16: delay time from the outgoing beat duration |

- **Melodicness** counts as insignificant when `main < 0.25`. Unknown counts as *not* insignificant ✔.
- **RemixFX `bpm`** ✔ comes from the outgoing song's mean beat duration, rounded to 0.5 BPM. It must fall
  between 20 and 300, otherwise there's no candidate.

## 4. Tempo relationship ✔ (identical in 26)

```
bpm = 60 / beatDuration                       (when the tempo is stored as a duration)
choose s ∈ {½, 1, 2} for the incoming tempo   (TempoBinaryRatio oneToTwo / oneToOne / twoToOne)
compatible  ⇔  | ln(bpm_out) − ln(bpm_in · s) | ≤ tol
tol = 0.16   (≈ ±17 % — per-candidate check)
tol = 0.287  (≈ ±33 % — applicability gate and BMFCF "expanded" retry)
```

26 loads `[0.16, 0.287]` from a table indexed by "expanded". The values are the same.

## 5. Scoring ✔

```
score = base × Π factors  (+ 0.001 × x, only when the product > 0)
```

`x` is a time in seconds from `0x7ffe07198334` (26), a tie-breaker. Live values follow the candidate's position
in the outgoing song (~110–230 s), so it is **not** the transition duration (corrected 2026-11-17).

The factors are 0 or 1 for hard checks (tempo relationship, matching bar count), ×0.75 for leading incoming
vocals, and loudness-ratio factors where a strategy uses them.

Factor lists ✔ (27 from the Dance scorer `0x7ffe081136e4`, 26 from the BMFCF switch `0x7ffe0719da6a`):

| | factors |
|---|---|
| 27 Dance 17 | tempo 0.16 · tonality *identical or melodicness insignificant* · matching bars ≥ min · trailing incoming loudness |
| 27 Dance 19 | tempo 0.16 · matching bars ≥ min · leading incoming vocals 0.75 |
| 26 slot 8 | tempo 0.16 · leading incoming vocals 0.75 · incoming bar-count score (fractional) · trailing incoming loudness |
| 26 slot 9 | tempo 0.16 · tonality *compatible* · matching bars ≥ min · vocal-activity relationship · leading vocals 0.75 · trailing loudness |
| 26 slot 12 | tempo **only in the expanded range** (incompatible at 0.16, compatible at 0.287) · matching bars ≥ min · leading vocals 0.75 · trailing loudness |

27's Hip-Hop scorers (`0x7ffe0811d9da`, `0x7ffe0811e14d`) carry two 0.75 factors; their inputs were not traced.

| Style | 27 base | 26 base (BMFCF only) |
|---|---|---|
| 17 Dance | **75** | — |
| 19 Dance | **17** | — |
| 15, 18 Hip-Hop | **100** | — |
| 16, 20 Pop | **50** | — |
| 9 BMFCF | **50** | 15 |
| 12 BMFCF | **50** | 10 (~) |
| 8 BMFCF | **20** | 10 |

## 6. Final selection ✔ (27 only; 26 has none of this)

1. Run every strategy. Each one first drops candidates that break its duration cap (90 s for style 17, the
   configured maximum otherwise) or the placement constraints. It then keeps **one result per style**: the
   best candidate of that style with score > 0, where the first maximum wins (`0x7ffe08110968`). All strategies'
   results are collected.
2. Sort them: **score descending, ties by style ID ascending** (`0x7ffe0812ddd2`, comparator in `0x7ffe0812e3a1`).
3. Choose the preferred set: **beat-matched results** if any exist. Otherwise non-fallback results. Otherwise
   fallback results.
4. If fewer than 2 preferred results, take the first. Otherwise weighted random pick: `w_i = Int(score_i × 1000)`.
   If `Σw = 0`, take the first result.
   - Compute `r = seed % Σw` (unsigned, `0x7ffe0812e979`: `lo + value % (hi − lo)`).
   - Take the first `i` whose running sum of weights is greater than `r`.
5. The **seed is deterministic per song pair**, so the same pair always gets the same transition:

```
a = Int((outgoing.durationMap.expectedStereoDuration × 1000).rounded(.toNearestOrEven))
b = Int((incoming.durationMap.expectedStereoDuration × 1000).rounded(.toNearestOrEven))
s = "\(a)" + "\(b)"                      // decimal strings, concatenated
seed = DJB2(s): h = 5381; for byte in s.utf8 { h = h &* 33 &+ byte }   // UInt
```

Code: `0x7ffe0815c016`, `0x7ffe0815bed8`, `0x7ffe081576b0` (hash), `0x7ffe0812e979` (index), `0x7ffe0812d87a`
(selector).

**26:** there is no `CandidateSelector`, no random generator, and no "preferred results" step. ~ It walks the
algorithms in priority order and stops at the first one that yields a styling result. ✔ Inside BMFCF it keeps
the best style per region pair (`0x7ffe071995ab`), then the best of those (`0x7ffe071991e1`). Both use
`0x7ffe071993d4`: drop score ≤ 0, keep the maximum, first wins. Result struct: +0x08 style ID, +0xF8 score,
256 bytes.

---

## Option (a) on 26: implemented (`planner27.c`, 2026-11-17)

26's BMFCF evaluates slots 8/9/12, and our RemixFX styles ride in them. `planner27.c` makes Music's copy of
26's planner apply 27's rules to whichever 27 style each slot carries. The carried style is read from the
served catalog's `[27:NN]` names.

- **Genre gate per style.** 26's own genre check (`0x7ffe071abdf0`) is re-run with 27's filter lists.
  26's BMFCF list is `[tag2(common allowed 52, common denied 89), tag0(common allowed 52)]`. 27's Dance,
  Hip-Hop and Pop lists are the same with element 1 replaced by `tag0(["17"])`, `tag0(["15","18"])` and
  `tag0(["14"])` (static arrays at `0x7ffe417528b8`, `…534b8`, `…53620` in 27). A style whose gate fails gets
  base 0.
- **27's bases** replace 26's slot bases in the score function. 26's native styles in a slot get 27's BMFCF
  bases: 8→20, 9/12→50.
- **Factor drops.** Factors 27 doesn't apply to the carried style are set to 1: 17 in slot 9 loses
  vocal-activity relationship and leading vocals; 19 in slot 8 or 12 loses trailing loudness.
- **Selection.** Each slot keeps its best candidate across all region pairs (score > 0, first maximum wins).
  They are sorted by score desc then style asc, and 27's weighted pick is applied with DJB2 of the two
  expected stereo durations in ms.

**Slot mapping (default `8:18,9:17,12:20`).** 26's slot 12 only fires for the expanded tempo range, which 27's
Dance strategies reject but its Pop strategy accepts. Slot 9 is the only slot with a tonality factor, which
27 requires only for style 17. Slot 8 has no tonality or bar gate, like 27's Hip-Hop. This mapping never adds
a transition 27 would rule out on tempo or key. Dance-heavy libraries can use `8:19,9:17,12:20`.

**Still different from 27:**
- **Regions:** they are BMFCF's (after the first phrase), not the last section for Hip-Hop and Pop.
- **Styles offered:** only the 3 carried styles compete. 27 also offers 15, 16 and 19 plus its own 8/9/12, so
  the weighted pick runs over a smaller pool.
- **Key check:** 26's tonality check is "compatible", not 27's "identical".
- **Hip-Hop and Pop factors:** their inputs weren't traced, so their factors are 26's slot factors.

Live check (`logs/20261117-004225`): every candidate of slots 9 and 12 was gated to 0 on hip-hop pairs, slot 8
was rescored with base 100, and style 18 played. The multi-style pick matches a Python model of 27's rule on
303 synthetic cases.

---

## Option (b) on 26: implemented (`planner27.c`, 2026-11-17)

The served catalog keeps 27's own 8/9/12 and adds RemixFX styles 17–20 **at their real IDs**
(`make_catalog.py` default; `--slots` still builds option (a)). 26 evaluates them inside BMFCF:

- **More styles.** `0x7ffe0719a1ee` (BMFCF's catalog lookup, called only from `0x7ffe0719bce8`) gets
  `[8, 9, 12, 17, 19, 18, 20]` instead of the static `[8, 9, 12]` at `0x7ffe4141d798`. It throws on a missing ID,
  so only IDs present in the served catalog are added. The other users of that list (`0x7ffe0719d0e4`,
  `0x7ffe0719d732`, and the helpers `0x7ffe071a0e1c`/`…0448`/`…2a12` that `0x7ffe0719da6a` passes it to) keep the
  original list.
- **Routing.** `0x7ffe0719da6a` switches on the style ID: 8 → `0x…e104`, 9 → `0x…de78`, 12 → `0x…dd31`, any other
  ID → no candidate. The style list it gets (stack arg 3, 0x38-byte elements: id, name, bars, bars-nil, two
  instruction arrays) is rewritten in place: 17, 19 → 9; 18, 20 → 8. The real ID is remembered by the style's
  instruction-array pointers. The region pair is built *before* the switch from the element's own bar count,
  so it uses the real style's length.
- **Factors** (27 factor lists read from 27's scorers):

  | style | 26 branch | factors kept | 27 |
  |---|---|---|---|
  | 17 | 9 | tempo 0.16, tonality, matching bars, trailing loudness | same 4 (tonality there is "identical") |
  | 19 | 9 | tempo 0.16, matching bars, leading vocals | exactly these |
  | 18 | 8 | leading vocals, bar ratio r ≥ 0.75, ×r, trailing loudness | `0x7ffe0811d9da`: exactly these |
  | 20 | 8 | leading vocals, r ≥ 0.75, trailing loudness | `0x7ffe08120f83`: + outgoing bars = style bars, + outgoing loudness significant |

  r = tempo-scaled incoming bars / style bars. 26's branch-8 bar score is incoming bars × 0.125
  (`0x7ffe071a1a2d`), so r = that × 8 / bars. Neither Hip-Hop nor Pop checks the tempo per candidate, so branch
  8's tempo factor is dropped for them (the 0.287 applicability gate still applies).
- **Last-section anchor (Hip-Hop, Pop).** 27 (`0x7ffe0811c179`, `0x7ffe0811f6c9`) keeps the outgoing end downbeats
  at or after the last section's start. In 26: `0x7ffe0719da6a`'s rsi is the outgoing song structure, with
  sections (0x50 bytes each) at +0x38. `Section.startTime` (`0x7ffe0719752b`) of the last one is compared with the
  end of the candidate's outgoing region. That end comes from `Region.timeRange` (`0x7ffe0717d290`) on the region
  pair at rbp−0x2c0. Both take self in r13 and return through rax (`p27_swiftcall` in `hook_x64.s`), and both
  borrow, so nothing needs to be released.
- **Style 17's 90 s cap**: outgoing region length from the same `Region.timeRange`.
- **Relabel + pick.** 26's branches write their own constant as the result's style ID (+0x08). Results carry
  the real style's name, bars and instructions (+0x10…+0x3f). In the per-pair best hook, after 26's
  placement/duration filters (`0x7ffe0719450a`, `…4b9b`, `0x7ffe07181e29`, which read placement and the region
  pair, not the ID), +0x08 is set to the real ID. The best candidate per style is collected across all region
  pairs, and 27's sorted, seeded weighted pick is returned. 26 then builds and plays the transition with the
  real ID.

Live (`logs/20261117-013432`, `…014919`, `…015504`, Hip-Hop pairs): the pick ran over 18 plus 26's own 8 or 12.
The seeded rule chose 18 three times out of five in one run (18 played through RemixFX, `ACTIVATE style=18`),
and 8 or 12 otherwise. No mapping errors or setup failures. 18 candidates ending before the last section are
zeroed, e.g. "outgoing ends 143.68 s, before last section 212.89 s".

**15/16 done (2026-11-17), via the value resolver instead of building a dict.** Their delay lanes name
`dynamic_beat_length` / `beat_length`. 26 builds each result's mapped-value dictionary (+0x40) empty
(`0x7ffe071f03b4(_swiftEmptyArrayStorage, key type 0x7ffe4141e408 = MappedAutomationValue.ID, value Double)`), so
26's resolver `0x7ffe071ba330` throws "Mapped automation value missing" for the by-name lookup and the transition
fails. 27's Pop computes the value at `0x7ffe081216d0`: `beat_length` (a 16-byte String small-string key) → Double
`60 / BPM`, BPM from `0x7ffe08121b36` = round(2·60·beats/regionLen)·0.5 (snapped to 0.5 BPM), clamped to a beat
length in [0.0001, 2.01] s. Rather than reconstruct that Swift Dictionary in-process (crash-prone), `planner27.c`
hooks the resolver (`resolveMapped`): it takes the value's enum tag in r8b (0/2 = inline literal, already present;
1 = by-ID lookup, which only 15/16 use) and returns a Double in xmm0. When a 15/16 result is picked, `h_best`
stashes the outgoing beat length = regionLen / (bars×4) (= 27's value to within its 0.5-BPM snap); `h_resolve`
returns it for the tag-1 lookup (xmm0 = value, r12 = 0, early-return through the hook ABI). The key isn't parsed —
each style has exactly one by-name value. 15's `dynamic_beat_length` (tempo-scaled, a >15-byte large String) is
approximated by the same outgoing beat (tempos are compatible, so the echo stays on the outgoing beat). Verified
live: style 15 picked & pivoted repeatedly on hip-hop pairs, resolver supplied 80–172 BPM, no missing-value error.

**Still different from 27:**
- **Pop's two outgoing checks** (outgoing region bars = style bars, outgoing loudness significant) are not
  applied. Dance 17's key check is 26's "compatible", not "identical".
- **Candidate positions** are 26's BMFCF end downbeats (after the first phrase start) filtered to the last
  section, not 27's full set of last-section downbeats.
