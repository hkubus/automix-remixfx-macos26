#!/usr/bin/env python3
"""make_catalog.py - build the macOS 26 transition catalog + DSP graph that carry macOS 27's RemixFX styles.

26's parameter mapper can't route `remixfx_*` ids. So for each RemixFX style (17-20) we keep its *routable* content
(time-stretch, mixer gains), drop its REMIX_FX lanes, and add a carrier lane that 26 does route
(aufilter_center_*). Our DSPGraph.dspg rewires those AUFilter graph params (Fcf1/Fcg1) onto pseudo-params of the
AURemixFX node, where the RemixFX component (rxdrive.c) decodes them and replays 27's REMIX_FX envelopes onto the
real AU.

Two layouts:
  default (option b)  17-20 keep their real IDs; 8/9/12 stay 27's own BMFCF styles. 26's planner only evaluates
                      [8, 9, 12] on its own, so this needs planner27.c (REMIXFX_PLANNER27 != 0), which adds 27's
                      styles to BMFCF's list and scores/picks them by 27's rules.
  --slots (option a)  RemixFX styles are authored *into* slots 8/9/12, which 26 evaluates even without planner27.

The carrier uses literal values only: 26's beat-matched planner has no value for `"parameterName": "beat_length"`
(nor 27's `bpm` / `dynamic_beat_length`); any lane that names one fails the whole transition with
"Mapped automation value missing" and AutoMix falls back to Dead-Air Removal. The engine derives the tempo from the
carrier's ramp slope instead.

usage: make_catalog.py [--slots 8:18,9:17,12:20] [--src27 DIR] [--out DIR]
  --slots   option (a): slot:style pairs. Slots must be 8, 9 or 12; styles must be RemixFX styles (17-20).
            Suggested: slot 8 (tempo ±17%, no key/bar gate) <- 18 Hip-Hop; slot 9 (only slot with a key check)
            <- 17 Dance; slot 12 (fires only for tempos ±17..33% apart, which 27's Dance rejects but Pop accepts)
            <- 20 Pop. Dance-heavy library: 8:19,9:17,12:20 (19 then takes slot 8 from 18).
  --src27   directory with macOS 27's TransitionStyles.json + DSPGraph.dspg (default ../reference/macos27)
  --out     output directory (default payload/Resources)
Outputs (rewritten only when their content changes): TransitionStyles.json (served to 26's SonicKit),
DSPGraph.dspg (same), RemixFXStyles.json (27's original REMIX_FX styles, read by the dylib for the envelopes).
"""
import argparse, copy, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SLOTS_OK = (8, 9, 12)
# catalog parameterIds known to 26's _SonicKit_MusicKit_Packages (raw values of its parameter enum)
KNOWN_26 = {
    "player_gain", "out_gain", "send_mixer_gain", "fx_mixer_dry", "fx_mixer_wet", "ts_rate",
    "hp_cutoff_freq", "hp_reso", "lp_cutoff_freq", "lp_reso", "fx_hp_cutoff_freq", "fx_hp_reso",
    "fx_lp_cutoff_freq", "fx_lp_reso", "aufilter_center_freq", "aufilter_center_gain", "aufilter_center_bandwidth",
    "fx_delay_delay_time", "fx_delay_dry_wet", "fx_delay_feedback", "fx_delay_lp_cutoff_frequency",
    "fx_reverb_gain", "fx_reverb_dry_wet", "fx_reverb_high_frequency_decay_time",
    "fx_reverb_low_frequency_decay_time", "fx_reverb_max_delay_time", "fx_reverb_min_delay_time",
    "fx_reverb_randomize_reflections",
}
PSEUDO = {"Fcf1": 1000, "Fcg1": 1001}   # must match rxdrive.h RX_PSEUDO_PROGRESS / RX_PSEUDO_SLOT
CARRIER_BASE, CARRIER_STYLE0, CARRIER_NIDX = 10000, 17, 8   # must match rxdrive.h RX_CARRIER_*


def carrier_value(style_id, side, progress):
    return CARRIER_BASE + 1000 * ((style_id - CARRIER_STYLE0) * 2 + side) + 100 + 800 * progress


def is_remix(instr):
    return instr.get("name") == "REMIX_FX" or any(
        a.get("parameterId", "").startswith("remixfx_") for a in instr.get("automations", []))


def lane(pid, t0, t1, v0, v1, interp="linear"):
    return {"parameterId": pid,
            "startTime": {"default": {"relative": t0}}, "endTime": {"default": {"relative": t1}},
            "startValue": v0, "endValue": v1, "interpolation": interp}


def carrier(style_id, side, slot):
    """slot = the catalog id the style is served under (carried as the AUFilter gain, informational)"""
    assert 0 <= (style_id - CARRIER_STYLE0) * 2 + side < CARRIER_NIDX, style_id
    return {"name": "AUFilter",
            "placement": {"start": {"relative": 0}, "end": {"relative": 1}},
            "automations": [
                # progress ramp over the whole transition (10100..17900 Hz: a plausible center frequency)
                lane("aufilter_center_freq", 0, 1, {"default": carrier_value(style_id, side, 0)},
                     {"default": carrier_value(style_id, side, 1)}),
                lane("aufilter_center_gain", 0, 0, {"default": slot}, {"default": slot}),
            ]}


def build_slot(slot, remix, original):
    """option (a): RemixFX style `remix` served under slot id 8/9/12; option (b): slot == its own id"""
    s = copy.deepcopy(remix)
    s["id"] = slot
    s["name"] = f"RemixFX {remix['name']} [27:{remix['id']}]" if slot != remix["id"] else f"RemixFX {remix['name']}"
    if "offset" not in s and original is not None and "offset" in original:
        s["offset"] = copy.deepcopy(original["offset"])
    for side, key in ((0, "outgoing"), (1, "incoming")):
        instrs = [i for i in s["instructions"].get(key, []) if not is_remix(i)]
        instrs.append(carrier(remix["id"], side, slot))
        s["instructions"][key] = instrs
    return s


def check(style):
    """problems 26's beat-matched planner would hit: unknown parameter ids, or named values it can't resolve"""
    bad = set()
    for key in ("outgoing", "incoming"):
        for i in style["instructions"].get(key, []):
            for a in i.get("automations", []):
                if a["parameterId"] not in KNOWN_26:
                    bad.add(f"unknown parameterId {a['parameterId']}")
                for f in ("startValue", "endValue"):
                    if "parameterName" in a.get(f, {}):
                        bad.add(f"{a['parameterId']}.{f} uses parameterName {a[f]['parameterName']!r}")
    return bad


def rewire_dspg(text):
    out, n = [], 0
    for line in text.splitlines():
        m = re.match(r"^param (Fcf1|Fcg1)\b", line)
        if m:
            out.append(f"param {m.group(1)} 0 in"); n += 1; continue
        m = re.match(r"^wireGraphParam (Fcf1|Fcg1) \(AUFilter \d+ 0\)\s*$", line)
        if m:
            out.append(f"wireGraphParam {m.group(1)} (AURemixFX {PSEUDO[m.group(1)]} 0)"); n += 1; continue
        out.append(line)
    if n != 2 * len(PSEUDO):
        sys.exit(f"DSPGraph rewrite: expected {2 * len(PSEUDO)} Fcf1/Fcg1 lines, rewrote {n}")
    if "box AURemixFX ( aufx remx appl )" not in text:
        sys.exit("DSPGraph has no AURemixFX box - is this macOS 27's graph?")
    return "\n".join(out) + "\n"


def write_if_changed(path, text):
    try:
        if open(path).read() == text:
            return False
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slots", default=None)
    ap.add_argument("--src27", default=os.path.join(HERE, "..", "reference", "macos27"))
    ap.add_argument("--out", default=os.path.join(HERE, "payload", "Resources"))
    a = ap.parse_args()

    cat = json.load(open(os.path.join(a.src27, "TransitionStyles.json")))
    remix = {s["id"]: s for s in cat if any(is_remix(i) for k in ("outgoing", "incoming") for i in s["instructions"].get(k, []))}
    slots = {}
    if a.slots:
        for pair in a.slots.split(","):
            sl, st = (int(x) for x in pair.split(":"))
            if sl not in SLOTS_OK: sys.exit(f"slot {sl}: only {SLOTS_OK} are evaluated by 26's beat-matched planner")
            if st not in remix: sys.exit(f"style {st} is not a RemixFX style (have {sorted(remix)})")
            slots[sl] = st

    by_id = {s["id"]: s for s in cat}
    out = []
    for s in cat:
        if s["id"] in remix:
            if slots:
                continue                               # option (a): 17-20 only live inside their slots
            ns = build_slot(s["id"], s, None)          # option (b): real id, carrier instead of REMIX_FX lanes
        elif s["id"] in slots:
            ns = build_slot(s["id"], remix[slots[s["id"]]], by_id.get(s["id"]))
        else:
            out.append(s)
            continue
        bad = check(ns)
        if bad: sys.exit(f"style {s['id']}: 26 can't play this: {sorted(bad)}")
        out.append(ns)

    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.src27, "DSPGraph.dspg")) as f:
        g = rewire_dspg(f.read())
    changed = [n for n, t in (("TransitionStyles.json", json.dumps(out, indent=2)),
                              ("RemixFXStyles.json", json.dumps([remix[k] for k in sorted(remix)], indent=2)),
                              ("DSPGraph.dspg", g))
               if write_if_changed(os.path.join(a.out, n), t)]

    mode = "option (a), RemixFX styles in slots" if slots else "option (b), RemixFX styles at their real IDs (needs planner27)"
    print(f"{a.out}: TransitionStyles.json ({len(out)} styles), RemixFXStyles.json ({len(remix)}), DSPGraph.dspg"
          f" - {mode}; {'updated ' + ', '.join(changed) if changed else 'unchanged'}")
    for sid in (sorted(slots) if slots else sorted(remix)):
        st = remix[slots[sid]] if slots else remix[sid]
        print(f"  id {sid:2d} <- style {st['id']} {st['name']!r} ({st.get('duration')} bars)  carrier "
              f"{carrier_value(st['id'], 0, 0):.0f}..{carrier_value(st['id'], 0, 1):.0f} (outgoing) / "
              f"{carrier_value(st['id'], 1, 0):.0f}..{carrier_value(st['id'], 1, 1):.0f} (incoming)")


if __name__ == "__main__":
    main()
