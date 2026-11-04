# 13 — Calibration notes and troubleshooting

A log of what went wrong while calibrating the 2020 and 2008 slices and importing
the world 2020 dataset: the symptom, its cause, and the fix. Use it as a checklist
when the full dataset (or a new slice) behaves strangely. Mechanisms are described
in [02](02-simulation-core.md)–[06](06-geopolitics-ai.md); data and back-tests in
[11](11-data-calibration.md) and [12](12-simulation-slice-2020.md).

## The principle that fixes most problems

**Every behavioural rule responds to changes from the calibrated starting state,
not to absolute levels.** The 2019 (or 2007) state is an equilibrium by
construction; any term that is non-zero at the start pushes the baseline away from
the data. The bugs that came from breaking this rule:

| Symptom | Cause | Fix |
|---------|-------|-------|
| Credit crunch and NPL overhang in a no-shock run | absolute NPL / capital thresholds | relative to `npl0`, `capital_ratio0` |
| Autocracies destabilised from day one | Overton backlash measured from the window centre | status-quo baselines `policy_outside0`, `window_outside0` |
| China unrest at the start | censorship backlash with zero starting controls | starting information control from Freedom on the Net |
| Firm boards cutting jobs and investment in calm times | ideology and nationalism read as levels | `nationalism0`, `hoarding0`, board ideology relative to the 2019 government |
| Japan reforming pensions at once (debt 236% of GDP) | pension reform pragmatic above a fixed 90% debt | debt rise above `gov_debt0` |
| Education spending drifting | ideology term with the winning-coalition size (non-zero at start) | only the finance minister's shift from `gov_econ0` |
| Repression / immigration / asylum moving at the start | TAN position of the interior minister as a level | shift from `gov_tan0` |
| Regional unrest in poor regions of federal states | fiscal resentment `1 - capacity` (also 0 before the first update) | `fiscal_capacity0 - fiscal_capacity`, initialised at build |
| Technology productivity in the 2019 baseline | adoption levels | deviation from the 2019 adoption path (`tech_offset`) |
| Container carriers re-optimising at the start | utilisation target ≠ starting utilisation | `fleet_util_target` = start |

Checklist for a new rule: evaluate it on the starting state; it must return the
starting value (or zero for a deviation). Store a `*0` / `*_ref` property at
build time when needed.

## Steady state over the growth path

A model can sit still at tick 0 and still drift, because the starting state must
also be an equilibrium **along the growth path** (potential grows with `gf`, prices
with the inflation trend). Found so far:

- **Oil supply fixed while demand grew with `gf`**: Brent climbed from 64 to 100
  USD in three no-shock years and taxed importers. Producers' capacity now follows
  the demand trend (`oil_trend_index`).
- **Permanent income smoothed in real terms but not detrended**: with potential
  growing ~2.5% a year, smoothed income always lagged actual income, households
  under-spent relative to the path and the gap compounded through employment
  (consumption fell ~1.5% per half-year in the USA with no shock). Now smoothed
  relative to `gf` (households expect trend growth).
- **Remaining drift (open)**: two-year no-shock runs still lose demand, more with
  policy frozen (`--set ai_frozen=1`: USA unemployment 3.7% → 8.2%) than with it
  (→ 4.3%). Known contributors, each legitimate on its own: the effective interest
  rate on government debt converging to the lower 2019 market yields (household
  interest income falls by design), real profits squeezed when output lags,
  amplification by the accelerator, precautionary saving, credit and wealth
  effects, and policy rates near the lower bound in 2019, so monetary policy
  cannot offset the shortfall. Retirement flows faster than labour-market entry
  (Japan) cut demand without cutting capacity. Next steps: a capacity that follows
  the labour force; check the real income of each pop type on the path; a
  steady-state test over several years, not 120 days.

**Diagnose drift with**: `geosim_headless run --scenario 2020 --days 1100
--no-events 1 --daily out.csv` (watch `consumption_index`, `gdp_index`, `oil_price`
first), the same with `--set ai_frozen=1` (removes the AI's counter-cyclical
masking), then toggle one channel at a time with `--set`. **`--set` takes a comma
list** (`--set ai_frozen=1,precautionary=0`); a second `--set` flag replaces the
first, which silently invalidated some early tests. The one-year back-test hides
drift of ~1 pp of unemployment a year.

## Policy rules that fit 2020 badly at first

- **Lobbies raised tariffs in the COVID slump** (GDP and unemployment fell from 14
  to 10 of 20 in band): pressure measured as every tradable sector's shortfall.
  Protection follows import competition, not a general slump (Trefler 1993); now
  the shortfall relative to the whole economy, smoothed over a year.
- **Health spending of 2% of GDP per unit epidemic threat** widened deficits
  (net lending 12 → 11/16): set to 0.5 (2020 rises were 0.5–1.5% of GDP).
- **Strategic goals at gain 0.3** pushed public investment and deficits: 0.2.
- **The EU recovery fund paid 5% of EU GDP at once** (net lending 10 → 8/16):
  NextGenerationEU was agreed in July 2020, ratified by May 2021 and disbursed
  2021–26; now committed, paid after `eu_fund_lag_days`, over `eu_fund_years`.
- **Tariffs, credit and the protest cascade** were too strong in the first passes
  (modest Bown & Crowley tariff pain, credit-line draws and guarantee reach,
  `cascade_social` and protest fatigue): see the git history around these systems.
- **US yields rebounded** through Nash-bargaining outside options with transient
  productivity: productivity is persistent and the benefit regime permanent.
- **Gini calibration broke with the Pareto tail**: WID top shares are scaled (0.7)
  and the tail share is found by bisection.

## Engine and data-handling traps

- **DCON array indices are cast through a signed type**: `uint8_t` indices wrap
  above 127, so arrays sized 148 (cause tags) corrupted memory intermittently. All
  schema arrays use `uint16_t` indices ([10](10-data-model-dcon.md)).
- **Derived indices built before the data they index**: `rebuild_derived` ran
  before the organisation definitions were loaded, so in a fresh run every
  organisation was invisible, while after a save/load they worked. The save/load
  round-trip test (`test_scenario`) caught it as a divergence. Load every `defs/`
  file before `rebuild_derived`.
- **Probabilities above one in sequential draws** (escalate / back down / settle)
  made the first branch always win; normalise competing hazards.
- **Random-stream key collisions**: two systems used the same key prefix
  (`0x40000000`) on the same stream and tick. Give each use its own prefix.
- **Hard-coded calendar years**: ticks counted from 2020-01-01 and technology
  diffusion used 2019/2020 constants. Scenarios now carry their own epoch
  (`scenario.txt start{}`); search for literal years when adding a slice.
- **Courts strike down forced levers**: tests that force a lever through a court
  check must use `override_mode::direct`.

## World-dataset import (world 2020)

- **NaN everywhere within a week** came from one nation: WDI reports `0` instead of
  a gap for discontinued series (Venezuela's household and government consumption
  after 2014). The NaN spread to every nation through world markets. Shares that
  must be positive are now imputed when ≤ 0. *When the world run shows NaN, find
  the first date and nation in `--daily` output before anything else.*
- **Zero-population regions**: four-decimal CSV output rounded 150 small ADM1
  units to a 0 share. Shares are written at full precision with a floor, and the
  loader keeps a minimum population per region.
- **Adjacency weights are shared-vertex counts** (hundreds) in the world data but
  ~1 in the slice: coupling terms must use weighted means, not sums.
- **Overlap reassignment gave Noord-Brabant to Belgium** (Baarle-Hertog enclaves):
  regions are only reassigned between a sovereign and its listed dependencies or
  claims. geoBoundaries gbOpen 2020 has no duplicates for China (Hong Kong, Macau
  and Taiwan are absent from its file); those nations get one region with the WDI
  land area.
- **Object capacities**: the world needs ~13k pops and ~4.5k agents (governors);
  schema sizes were raised. A full `create_*` fails silently, so check sizes when
  adding nations.
- The world scenario builds and runs but is **not calibrated**: 171 build warnings
  (dividend payout clamps, transfer residuals) flag nations whose national
  accounts do not close with imputed data.

## The 2008 slice (first run)

- The first year fits well (GDP 17/20, unemployment 19/20 in band) but **2009
  rebounds instead of falling** (GDP 0/20; e.g. USA +3.7% vs −2.6%, China +25%).
  Likely causes to check first: shocks that unwind too fast (spreads, capital), no
  persistence in balance-sheet repair and confidence, the trade collapse of late
  2008 (−12% world trade) missing as a mechanism, and stimulus packages sized as
  announced rather than spent.
- The oil price misses in 2008 (model 67 vs 97 USD): the starting oil price and
  inventories come from the 2019 commodity definitions; a slice needs its own
  starting commodity prices (scenario overrides).
- Structural curated tables (ports, logistics, groups, commodities) were copied
  from 2020 and flagged in their headers; replace them before trusting results.

## Working practice

- Compile once per batch: schema changes rebuild the whole DCON container. Debug
  builds need about 1.5 GB per compiler process, so on an 8 GB machine use
  `-j3` and never run two builds at once. `/tmp` is RAM-backed (tmpfs); keep
  worktrees and large outputs elsewhere.
- Every new mechanism gets a parameter (so it can be switched off with `--set`)
  and a unit test; then check the 2020 back-test and a multi-year no-shock run
  before committing.
