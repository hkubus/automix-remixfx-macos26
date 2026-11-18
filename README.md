# AutoMix and RemixFX on x86_64 macOS 26

Apple Music's **AutoMix** (DJ-style song transitions) is switched off in the x86_64 build of macOS 26's Music app,
and macOS 27's new **RemixFX** transitions (loops, repeaters, tape stops, beat-timed delays) don't exist on 26 at all.
This project turns both on for Intel and other x86_64 Macs:

1. **AutoMix on 26:** a small patch, loaded into Music only, that undoes the x86_64 build's hard-coded "off".
2. **RemixFX on 26:** Apple's own x86_64 `AURemixFX` audio unit, taken from a macOS 27 install you own, loaded on 26
   by a custom in-process loader, and driven inside live AutoMix transitions.
3. **macOS 27's planner rules on 26:** in-process hooks that make 26's transition planner choose like 27's,
   with 27's Dance, Hip-Hop and Pop strategies (styles 15–20), genre gates, scores and seeded weighted pick.

Nothing on the sealed system volume is modified. The patches apply only to the Music process, and only when you open
Music through the launcher this project installs.

## Status

| Part | State |
|---|---|
| AutoMix on 26 | **Working** on any x86_64 macOS 26. |
| RemixFX audio unit on 26 | **Working.** Registers, instantiates and processes audio on 26; validated offline (`03-remixfx/sautest.m`). |
| RemixFX transitions in Music | **Working.** RemixFX styles play in live AutoMix transitions. |
| 27's style choice on 26 | **Working.** Hip-Hop styles 15 and 18 pick and play live. Pop (16, 20) and Dance (17, 19) are evaluated and gated like 27, but haven't won a live pair in testing yet. |

Known differences from 27 and open work are listed in [`03-remixfx/README.md`](03-remixfx/README.md) and
[`03-remixfx/RULES27.md`](03-remixfx/RULES27.md).

## Requirements

- An x86_64 Mac (Intel Mac or Hackintosh) running **macOS 26**. The AutoMix unlock works on any 26 build; RemixFX's
  planner hooks are tied to **26.7.3 (25G342)** and refuse to install on anything else (every patch site is
  byte-checked first).
- **AMFI relaxed** (boot-arg `amfi_get_out_of_my_way=1`) so Music will load an ad-hoc-signed library via
  `DYLD_INSERT_LIBRARIES`. This lowers your Mac's security; understand that before you do it.
- Xcode Command Line Tools (`xcode-select --install`) and Python 3.
- For RemixFX only: a **macOS 27.2 (26B123) system volume** mounted on the same machine. Its x86_64 dyld shared
  cache is where Apple's `SonicAudioUnits` comes from.

## Quick start

AutoMix only:

```sh
cd 01-basic-automix && ./automix-intel.sh     # builds the patch, creates ~/Applications/Music (AutoMix).app
```

AutoMix with RemixFX and 27's planner:

```sh
cd 03-remixfx
./fetch_apple_files.sh          # copies Apple's files from your own installs (--vol27 PATH if not "/Volumes/macOS 27")
./install.sh --launcher         # builds everything, creates ~/Applications/Music (RemixFX).app
```

Open Music through the launcher app from then on (drag it to the Dock in place of Music). To undo:
`./automix-intel.sh uninstall` and `03-remixfx/install.sh uninstall`.

## What's not in this repository

None of Apple's code or resource files: the `SonicAudioUnits` binary, SonicKit's `TransitionStyles.json` and
`DSPGraph.dspg`. `03-remixfx/fetch_apple_files.sh` copies them from your own macOS 26 system and macOS 27 volume
and checks their hashes. What *is* included are this project's own sources, scripts and notes, plus offset tables
(`03-remixfx/payload/SonicAudioUnits.fixups`, `.ehbinds`) that tell the loader how to relink Apple's binary.

## Known issue: songs from your own files

On 26, any transition involving a library song with iCloud status "Uploaded" (your own files, not Apple Music
copies) fails before it starts, so Music hard-cuts. Music's content cache rejects the song's missing cloud revision
(`NOT NULL constraint failed: cloud_library_source.cloud_revision`). This is in stock macOS 26 and unrelated to
this project. Transitions between Apple Music songs aren't affected.

## Layout

| Folder | What's there |
|---|---|
| [`01-basic-automix/`](01-basic-automix) | The AutoMix unlock (`AutoMixEnabler.m`) and its installer. |
| [`02-enhanced-automix/`](02-enhanced-automix) | An early research build that serves 26 a replacement catalog. Superseded by `03-remixfx`; kept for reference. |
| [`03-remixfx/`](03-remixfx) | The RemixFX transplant, its loader, the transition engine, 27's planner hooks, and tests. See its [README](03-remixfx/README.md). |
| [`tools/`](tools) | Reverse-engineering helpers: single-image dyld cache extractor, cache grep, Swift metadata dumper, disassembly annotators. |
| [`reference/`](reference) | Filled locally by the fetch script; empty in the repo. |
| [`NOTES.md`](NOTES.md) | The research log, in the order things were found. |

## License

[MIT](LICENSE), for this project's own code, scripts and notes.

## Disclaimer

This is an independent project, not affiliated with or endorsed by Apple. Apple, Apple Music, AutoMix and macOS are
trademarks of Apple Inc. You need your own licensed copies of macOS 26 and macOS 27. The project patches a running
system app and relies on private, undocumented internals that can change in any update. Use it at your own risk.
