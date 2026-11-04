# assets/

Moddable text data. A scenario is built from these files once (parsing and
calibration, `src/sim/scenario.cpp`), then saved as a DCON binary
(`geosim_headless build ... --out scenario.bin`) so later starts skip parsing.

```
assets/
  SOURCES.txt                    provenance + licences (generated)
  defs/                          hand-written model definitions
    parameters.txt               every global parameter (name{value} # description)
    sectors.txt                  production sectors
    npi.txt                      non-pharmaceutical interventions (OxCGRT indicators)
  scenario/2020/
    nations.csv                  macro state per nation, 2019 data (generated)
    sectors.csv                  value-added / employment shares per nation x sector (generated)
    nations_curated.csv          hand-written per-nation values and gap fills (sources in header)
  events/2020/                   dated events; every *.txt file is loaded, in name order
    npi.txt                      lockdown level changes (generated, OxCGRT)
    policy_rates.txt             central-bank rate changes (generated, BIS)
    fiscal.txt                   fiscal packages (generated, OxCGRT E3 scaled)
    epidemic_seeds.txt           epidemic seeds (generated, OxCGRT deaths)
    curated.txt                  hand-written anchors: Wuhan seed, OPEC+, job retention, suppression
  backtest/
    2020_targets.csv             real 2020 outcomes (generated)
    2020_brent.csv               daily Brent 2020 (generated)
    2020_tolerances.txt          tolerance bands (hand-written)
```

**Generated** files are written by `tools/pipeline/build_assets.py` from raw
downloads (`tools/pipeline/fetch.py` -> `data/raw/`, not committed). Edit the
pipeline, or override values in the `*_curated` files, rather than editing
generated files by hand: a rebuild overwrites them.

```sh
python3 tools/pipeline/fetch.py          # download (cached in data/raw/)
python3 tools/pipeline/build_assets.py   # regenerate assets/
```

## Formats

**CSV**: first non-`#` line is the header; `#` lines are comments; empty
field = missing (the loader uses a documented default and prints a warning).

**Definitions** (`.txt`): the same brace syntax as DCON schema files, plus
`#` comments.

```
key{ value }                       # leaf
key{ child{ value } other{ x } }   # node
```

## Event kinds (`assets/events/<scenario>/*.txt`)

Every event has `date{YYYY-MM-DD}` and `nation{ISO3}` (`ROW` = rest of world).
Policy kinds are applied only when `historical_anchors{1}` (the default).

| Kind | Fields | Effect |
|------|--------|--------|
| `npi` | `indicator{C1..H6}` `level{x}` `targeted{0..1}` | sets an NPI level; `targeted` = regional share (OxCGRT `_Flag = 0`) |
| `policy_rate` | `rate_pct{x}` | sets the central-bank policy rate |
| `fiscal_package` | `pct_gdp{x}` | adds a package (% of 2019 GDP); it funds crisis programmes first, the rest is disbursed |
| `stw` | `coverage{0..1}` `replacement{0..1}` | job-retention scheme: share of jobs kept on reduced hours, wage share paid |
| `test_trace_isolate` | `strength{0..1}` | suppression strategy: mass testing, centralised isolation, arrival quarantine |
| `epidemic_seed` | `infections{n}` | adds latent infections (exogenous; always applied) |
| `oil_production` | `mbd{x}` | absolute crude production target (exogenous) |
| `oil_production_delta` | `mbd{x}` | change of production target (exogenous) |

Any field not listed (e.g. `source{...}`) is ignored by the loader and is the
place to cite where a hand-written value comes from.
