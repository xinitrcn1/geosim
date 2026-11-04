#!/usr/bin/env python3
"""Flag nations that misbehave in a run (docs/13-calibration-notes.md).

Reads the --daily CSV of geosim_headless (best: a no-shock run, --no-events 1, where
every nation should stay close to its calibrated 2019 path) and prints, per nation,
the first problem of each kind:
  nan        first NaN / inf in any column
  gdp        |gdp_index - 1| above the threshold
  cpi_jump   a one-day CPI change above the threshold
  unrest     unrest above the threshold
  unemp      unemployment change (pp) above the threshold
Usage:
  geosim_headless run --assets assets --scenario world2020 --days 120 --no-events 1 --daily d.csv
  python3 tools/diagnose_run.py d.csv [--gdp 0.03] [--cpi 0.01] [--unrest 1.0] [--unemp 1.5]
"""

import argparse
import csv
import math
import sys
from collections import defaultdict


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("daily")
    ap.add_argument("--gdp", type=float, default=0.03)
    ap.add_argument("--cpi", type=float, default=0.01)
    ap.add_argument("--unrest", type=float, default=1.0)
    ap.add_argument("--unemp", type=float, default=1.5)
    args = ap.parse_args()
    problems = defaultdict(dict)  # iso -> kind -> (date, detail)
    last_cpi, u0 = {}, {}
    seen = set()
    with open(args.daily, newline="") as f:
        for r in csv.DictReader(f):
            iso, date = r["iso3"], r["date"]
            seen.add(iso)
            p = problems[iso]

            def flag(kind, detail):
                if kind not in p:
                    p[kind] = (date, detail)

            for c, v in r.items():
                if c in ("iso3", "date") or v in ("", None):
                    continue
                try:
                    x = float(v)
                except ValueError:
                    continue
                if math.isnan(x) or math.isinf(x):
                    flag("nan", c)
                    break

            def num(c):
                try:
                    x = float(r.get(c, ""))
                    return None if math.isnan(x) or math.isinf(x) else x
                except ValueError:
                    return None
            g, cpi, un, u = num("gdp_index"), num("cpi"), num("unrest"), num("unemployment_pct")
            if g is not None and abs(g - 1.0) > args.gdp:
                flag("gdp", f"{g:.3f}")
            if cpi is not None:
                if iso in last_cpi and last_cpi[iso] > 0 and abs(cpi / last_cpi[iso] - 1.0) > args.cpi:
                    flag("cpi_jump", f"{last_cpi[iso]:.4f}->{cpi:.4f}")
                last_cpi[iso] = cpi
            if un is not None and un > args.unrest:
                flag("unrest", f"{un:.2f}")
            if u is not None:
                u0.setdefault(iso, u)
                if abs(u - u0[iso]) > args.unemp:
                    flag("unemp", f"{u0[iso]:.1f}->{u:.1f}")
    bad = {i: p for i, p in problems.items() if p}
    kinds = ("nan", "gdp", "cpi_jump", "unrest", "unemp")
    counts = {k: sum(1 for p in bad.values() if k in p) for k in kinds}
    print(f"{len(seen)} nations, {len(bad)} with problems: " + ", ".join(f"{k} {counts[k]}" for k in kinds))
    for iso in sorted(bad, key=lambda i: min(d for d, _ in bad[i].values())):
        print(f"  {iso}: " + "; ".join(f"{k} {bad[iso][k][0]} ({bad[iso][k][1]})" for k in kinds if k in bad[iso]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
