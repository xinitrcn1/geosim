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

## World 2020 as the calibration target (findings)

Fixed while moving the calibration from the 20-nation slice to all 218 nations:

- **Regions paired with the wrong input rows.** The loader matched each region to
  its `regions.csv` row by iteration position; DCON relationship order is not
  input order, so regions got each other's employment (Australia's "Other
  Territories", 25 residents, received 1.1 million workers). A region whose
  head-count then reached zero produced *negative* deaths and multiplied its pops
  (unemployment 68%, public wages 2,700% of GDP). Regions are matched by identity;
  deaths cannot go negative. *Check: total pop size per region / region population
  should be the adult share (~0.7) everywhere (`GEOSIM_TRACE_POPS=1 --trace ISO`).*
- **Levels imputed as if they were ratios.** The pipeline filled missing remittance
  outflows with the income-group median level (USD 740 million), larger than the
  GDP of St. Martin. Levels are imputed as a median share of GDP.
- **WDI zeros are gaps** (Venezuela's consumption shares) and **missing
  coordinates default to (0, 0)** (five territories): both fixed in the pipeline.
- **Oil was special-cased for the slice** (curated producers, PPP-based demand, a
  rest-of-world residual). The world has no residual, so supply fell short and
  Brent went from 79 to 127 USD in ten days. Production and consumption now come
  from data (Energy Institute via OWID) for every nation, balanced at the start.
- **Flows sized by the donor** (crisis aid: a share of each donor's GDP) were
  harmless with 20 nations and swamped small recipients with 218 (Djibouti's
  budget surplus reached 120% of GDP). Size every bilateral flow by the
  recipient's need and economy, then share it among senders within their budgets.
- **Anchors without data.** With historical anchors on, job retention, suppression
  (TTI) and fiscal packages were pulled toward a zero prior for every nation
  without a recorded event, so only curated nations could use them. Anchor only
  where the scenario has a record for that nation.
- **Spatial LOD removed**: every nation runs at full detail.

Tools: `tools/diagnose_run.py <daily.csv>` lists, per nation, the first NaN, GDP
or unemployment jump, CPI jump and unrest spike - start every world debugging
session with it on a no-shock run, then on the full run. `--trace ISO` prints one
nation's internal state each day.

First world back-test after these fixes (uncalibrated): GDP 97/210, unemployment
113/187, inflation 120/179, net lending 89/134, deaths 63/182, Brent 58.5 vs 42.0
USD. Biases (model minus data, median): unemployment +1.2..+2.9 pp in every income
group; GDP -2.1 pp (high income) and -3.1 pp (low income); deaths x1.9; deficits
slightly too large; hyperinflations (Venezuela, Lebanon, Sudan) not produced. A
no-shock world year barely drifts (median unemployment +0.1 pp), so these are
shock responses, not baseline drift. Under-65 fatality now follows each region's
age mix (stable-population pyramid from the 0-14 / 15-64 ratio), a small effect.

### Second round (generic mechanisms, data instead of priors)

| Finding | Fix | Effect (world back-test) |
|---------|-----|--------------------------|
| OPEC+ anchors in data barrels vs production scaled to model units: the April 2020 "surge" was an accidental cut | anchors use each nation's production scale | Brent 59.0 -> 46.7 USD (real 42.0) |
| Closures cut rich service economies as hard as poor ones | telework: sector teleworkable shares (Dingel & Neiman 2020) scaled by log income per head | GDP 102 -> 107; high-income bias -2.0 -> -0.9 pp |
| The loader read EPL, benefits, death reporting and hemisphere only from curated tables, silently ignoring the generated world values | generated values apply, curated override | unemployment and deaths in non-curated nations |
| Death reporting from income-group priors | reported / estimated excess deaths 2020 per nation (The Economist model) | reported-deaths bias by income group ~1 (high, upper-middle), 1.8 (lower-middle), 2.9 (low; were 0.45 / 1.3 / 21 / 64) |
| True deaths not scored | new target `excess_deaths_per_million` (model true deaths vs estimated excess) | exposes epidemic size separately from reporting |
| Rural populations transmit like cities | crowding factor from the region's urban share (Rader et al. 2020); single-region nations take the national urban share (slice deaths fell 14 -> 7 until this) | lower-middle / low-income true deaths 4.3x / 6.0x -> 2.0x / 2.9x excess |
| Crisis-inactive workers returned to job search in ~6 months | half-life ~11 months (ILO) | unemployment 125 -> 143/187 |

Open (world 2020 after this round): GDP 109/210 in band (need 158): the response to
stringency is flatter than in the data (lax nations fall ~1 pp too much, strict ones
~1 pp too little) and several fast-growing developing economies grew in 2020 where
the model contracts them; inflation 115/179 (hyperinflations not produced; LMC bias
-2 pp); deaths and excess deaths ~80-86 in band (still too many deaths in young,
poor nations; Japan's tiny 2020 toll); low-income deficits ~4 pp too large.
Diagnostics: `data/raw/bias.py`-style tables of median (model - data) by World Bank
income group were the most useful view; per-nation misses are dominated by
microstates and one-off events (Guyana's first oil, Timor-Leste).

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
