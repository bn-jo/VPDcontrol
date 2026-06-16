#!/usr/bin/env python3
"""Generate downloadable crop climate profiles for the VPD Grow Room Controller.

Each output file is a *partial* backup that the device's /api/restore endpoint
accepts: it contains only the `profiles` array (5 stage slots) plus a light
schedule, so uploading it via Dashboard -> Restore loads the crop's climate
targets without touching WiFi, relays, irrigation or calibration.

The controller has 5 fixed stage slots:
  0 Seedling  ->  1 Vegetative  ->  2 Early Bloom  ->  3 Late Bloom  ->  4 Harvest

Setpoints are drawn from greenhouse / controlled-environment horticulture
references (USDA Shamshiri 2018 microclimate review; Bayer protected-culture
tomato guide; CEA VPD guidance for leafy greens & fruiting crops). They are
sensible starting points — always tune to your cultivar and environment.
Run:  python3 scripts/gen_veg_profiles.py
"""
import json, os

OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "profiles")

# Per stage: dT(min,max), nT(min,max), dH(min,max), dV(min,max), light(on,off)
# Night humidity is derived (+5 %, capped 90); night VPD is day VPD - 0.2 (floored).
CROPS = [
    ("tomato", "Tomato", "\U0001F345", "Warm-season fruiting",
     "Warm · 22-27 °C day / 16-18 °C night · VPD 0.8-1.3 kPa", [
        dict(dT=(22,26), nT=(18,20), dH=(65,75), dV=(0.45,0.80), L=(16,8)),
        dict(dT=(22,27), nT=(17,19), dH=(60,70), dV=(0.80,1.10), L=(16,8)),
        dict(dT=(23,27), nT=(16,18), dH=(60,70), dV=(0.90,1.20), L=(14,10)),
        dict(dT=(22,26), nT=(16,18), dH=(55,65), dV=(1.00,1.30), L=(14,10)),
        dict(dT=(22,26), nT=(16,18), dH=(55,65), dV=(1.00,1.30), L=(12,12)),
     ]),
    ("cucumber", "Cucumber", "\U0001F952", "Warm, humidity-loving",
     "Warm & humid · 24-28 °C · VPD 0.4-1.15 kPa", [
        dict(dT=(24,27), nT=(19,21), dH=(70,80), dV=(0.40,0.70), L=(16,8)),
        dict(dT=(24,28), nT=(18,20), dH=(65,75), dV=(0.65,0.95), L=(16,8)),
        dict(dT=(24,28), nT=(18,20), dH=(65,75), dV=(0.80,1.10), L=(14,10)),
        dict(dT=(23,27), nT=(18,20), dH=(60,70), dV=(0.85,1.15), L=(14,10)),
        dict(dT=(23,27), nT=(18,20), dH=(60,70), dV=(0.85,1.15), L=(12,12)),
     ]),
    ("bell-pepper", "Bell Pepper", "\U0001FAD1", "Warm-season fruiting",
     "Warm · 22-28 °C · VPD 0.8-1.3 kPa", [
        dict(dT=(22,26), nT=(18,20), dH=(65,75), dV=(0.45,0.80), L=(16,8)),
        dict(dT=(22,27), nT=(17,20), dH=(60,70), dV=(0.80,1.10), L=(16,8)),
        dict(dT=(23,28), nT=(17,19), dH=(55,70), dV=(0.90,1.20), L=(14,10)),
        dict(dT=(22,27), nT=(16,19), dH=(50,65), dV=(1.00,1.30), L=(14,10)),
        dict(dT=(22,27), nT=(16,19), dH=(50,65), dV=(1.00,1.30), L=(12,12)),
     ]),
    ("chili-pepper", "Chili Pepper", "\U0001F336", "Hot & dry tolerant",
     "Hot & drier · 24-30 °C · VPD 0.9-1.5 kPa", [
        dict(dT=(24,28), nT=(18,21), dH=(60,70), dV=(0.50,0.85), L=(16,8)),
        dict(dT=(24,29), nT=(18,20), dH=(55,65), dV=(0.90,1.20), L=(16,8)),
        dict(dT=(25,30), nT=(18,20), dH=(50,60), dV=(1.00,1.30), L=(14,10)),
        dict(dT=(24,29), nT=(17,20), dH=(45,60), dV=(1.10,1.50), L=(14,10)),
        dict(dT=(24,29), nT=(17,20), dH=(45,60), dV=(1.10,1.50), L=(12,12)),
     ]),
    ("eggplant", "Eggplant", "\U0001F346", "Warm-season fruiting",
     "Warm · 24-29 °C · VPD 0.8-1.3 kPa", [
        dict(dT=(24,28), nT=(18,21), dH=(65,75), dV=(0.45,0.80), L=(16,8)),
        dict(dT=(24,29), nT=(18,21), dH=(60,70), dV=(0.80,1.10), L=(16,8)),
        dict(dT=(24,29), nT=(18,20), dH=(55,65), dV=(0.90,1.20), L=(14,10)),
        dict(dT=(23,28), nT=(17,20), dH=(50,65), dV=(1.00,1.30), L=(14,10)),
        dict(dT=(23,28), nT=(17,20), dH=(50,65), dV=(1.00,1.30), L=(12,12)),
     ]),
    ("zucchini", "Zucchini / Squash", "\U0001F383", "Warm-season fruiting",
     "Warm · 21-28 °C · VPD 0.45-1.3 kPa", [
        dict(dT=(24,27), nT=(18,20), dH=(65,75), dV=(0.45,0.80), L=(16,8)),
        dict(dT=(22,28), nT=(17,20), dH=(60,70), dV=(0.80,1.10), L=(16,8)),
        dict(dT=(22,28), nT=(17,19), dH=(55,70), dV=(0.90,1.20), L=(14,10)),
        dict(dT=(21,27), nT=(16,19), dH=(50,65), dV=(1.00,1.30), L=(14,10)),
        dict(dT=(21,27), nT=(16,19), dH=(50,65), dV=(1.00,1.30), L=(12,12)),
     ]),
    ("green-beans", "Green Beans", "\U0001FAD8", "Warm-season podding",
     "Warm · 18-27 °C · VPD 0.45-1.15 kPa", [
        dict(dT=(20,25), nT=(16,19), dH=(65,75), dV=(0.45,0.80), L=(16,8)),
        dict(dT=(20,27), nT=(16,19), dH=(60,70), dV=(0.70,1.00), L=(16,8)),
        dict(dT=(20,27), nT=(16,18), dH=(55,65), dV=(0.80,1.10), L=(14,10)),
        dict(dT=(18,26), nT=(15,18), dH=(50,65), dV=(0.85,1.15), L=(14,10)),
        dict(dT=(18,26), nT=(15,18), dH=(50,65), dV=(0.85,1.15), L=(12,12)),
     ]),
    ("melon", "Melon", "\U0001F348", "Warm-season fruiting",
     "Warm · 24-30 °C · VPD 0.5-1.5 kPa", [
        dict(dT=(24,28), nT=(18,21), dH=(65,75), dV=(0.50,0.85), L=(16,8)),
        dict(dT=(24,30), nT=(18,21), dH=(60,70), dV=(0.80,1.10), L=(16,8)),
        dict(dT=(25,30), nT=(18,20), dH=(55,65), dV=(0.90,1.25), L=(14,10)),
        dict(dT=(24,30), nT=(17,20), dH=(45,60), dV=(1.10,1.50), L=(14,10)),
        dict(dT=(24,30), nT=(17,20), dH=(45,60), dV=(1.10,1.50), L=(12,12)),
     ]),
    ("strawberry", "Strawberry", "\U0001F353", "Cool-season fruiting",
     "Cool · 17-23 °C · VPD 0.45-1.15 kPa", [
        dict(dT=(18,22), nT=(10,13), dH=(65,75), dV=(0.45,0.80), L=(12,12)),
        dict(dT=(18,23), nT=(10,14), dH=(60,70), dV=(0.70,1.00), L=(14,10)),
        dict(dT=(18,23), nT=(8,12),  dH=(60,70), dV=(0.80,1.10), L=(12,12)),
        dict(dT=(17,22), nT=(8,12),  dH=(55,65), dV=(0.85,1.15), L=(12,12)),
        dict(dT=(17,22), nT=(8,12),  dH=(55,65), dV=(0.85,1.15), L=(10,14)),
     ]),
]

STAGE_LABELS = ["Seedling", "Vegetative", "Early Bloom", "Late Bloom", "Harvest"]


def night(dv):
    return round(max(0.30, dv - 0.20), 2)


def build(crop):
    slug, name, emoji, kind, summary, stages = crop
    profiles = []
    for st in stages:
        dHmin, dHmax = st["dH"]
        nHmin, nHmax = min(dHmin + 5, 90), min(dHmax + 5, 90)
        dVmin, dVmax = st["dV"]
        profiles.append({
            "dtMin": st["dT"][0], "dtMax": st["dT"][1],
            "dhMin": dHmin,       "dhMax": dHmax,
            "dvMin": dVmin,       "dvMax": dVmax,
            "ntMin": st["nT"][0], "ntMax": st["nT"][1],
            "nhMin": nHmin,       "nhMax": nHmax,
            "nvMin": night(dVmin),"nvMax": night(dVmax),
            # NOTE: light hours (lOn/lOff) are intentionally omitted — the
            # controller's /api/restore applies only the climate ranges, so we
            # don't ship fields it would silently ignore. Photoperiod stays as
            # the grower has it. (st["L"] kept in data for reference only.)
        })
    return {
        "version": 1,
        "crop": name,
        "type": kind,
        "summary": summary,
        "stageMap": STAGE_LABELS,
        "note": ("Climate profile for the VPD Grow Room Controller. "
                 "Upload via Dashboard -> Restore (device reboots to apply). "
                 "Only climate stage targets are changed; WiFi, relays and "
                 "irrigation are left untouched. Tune to your cultivar."),
        "profiles": profiles,
    }


def main():
    os.makedirs(OUT, exist_ok=True)
    index = []
    for crop in CROPS:
        slug = crop[0]
        doc = build(crop)
        path = os.path.join(OUT, f"{slug}.json")
        with open(path, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        index.append((slug, crop[2], crop[1], crop[3], crop[4]))
        print(f"wrote {path}")
    print(f"\n{len(index)} profiles generated.")


if __name__ == "__main__":
    main()
