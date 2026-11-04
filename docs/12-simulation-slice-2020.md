# 12 — The 2020 simulation slice (as implemented)

This is the first working cut of the simulation described in
[02](02-simulation-core.md)–[06](06-geopolitics-ai.md). It runs the year 2020
for **20 nations + rest of world** (G20 members + Spain; ROW = World Bank
world total minus the slice), with **one region per nation**. It is meant to
reproduce what happened, driven by real dated events, so that the mechanics
can be validated before the AI and the map are built.

Code: `src/sim/` (systems in `src/sim/systems/`), data: `assets/`
(see `assets/README.md`), runner: `src/headless/main.cpp`.

## What is in the slice and what is not

| Design element | Slice status |
|----------------|--------------|
| SFC ledger ([02](02-simulation-core.md#accounting-core)) | **Yes.** Pops, firms, government, finance, rest of world per nation; every payment via `sim::transfer`; conservation tested |
| "Why?" flows | **Partial.** Year-to-date flows by kind per nation (`--flows ISO3`); no per-decision cause tags yet |
| Cadence scheduler | **Yes.** Epidemic, oil, events, indicators daily; economy weekly (7 shards by nation); Taylor rule and Gini on 28-day shards |
| Determinism, saves | **Yes.** DCON serialisation; save -> load -> continue is bit-identical (tested) |
| Production / prices / labour / fiscal / money | **Yes**, simplified (below) |
| Pops (strata) | **Partial.** 4 per region: low/mid/high-skill workers, retirees; lognormal within groups; no values/identity yet |
| Epidemics | **Yes.** Any number of simultaneous pathogens (data-defined), two-age-group SEIR each, interacting |
| Trade | **Simplified.** Exports follow partners' import demand; no bilateral gravity matrix yet |
| Oil market | **Yes.** World crude with inventories, OPEC+ events, price-elastic producers |
| Exchange rates, credit, housing | No (all values in USD; see open issues) |
| AI agents | **Yes.** Executive + central bank per nation (one bank per currency union); anchors are priors (below) |
| Overrides | **Yes (engine side).** advise / direct / force per agent and lever; AI choice still logged |
| Sovereignty, pegs, claims | **Yes (data model + AI).** Subjects blend with their sovereign by autonomy; pegged banks follow the anchor |
| Politics (parties, elections, legitimacy) | No |

## Day loop (`src/sim/simulation.cpp`)

```
apply_events (anchors -> priors) -> update_ai -> update_epidemic
  -> update_economy (nations whose weekly shard is due) -> update_oil_market -> update_indicators -> ++tick
```

## AI agents and historical anchors (`systems/ai.cpp`)

Dated real-world decisions (OxCGRT NPIs, BIS rates, fiscal packages, job retention,
suppression strategies, OPEC+ quotas) are **not** applied to the world. They set
the deciding agent's *prior* for that lever. Each lever x is then chosen by
maximising

```
U(x) = -(1 - rho) (x - P)^2 - rho (x - I)^2 - kappa (x - A)^2
```

whose optimum is the precision-weighted mean of the **pragmatic** target P
(projected detected cases vs the agent's tolerance; Taylor rule or the anchor
currency's rate; counter-cyclical fiscal support limited by debt; market-driven
oil output), the **ideological** target I (order-minded leaders answer threats
harder; fiscal hawks spend less), and the **anchor** A (`anchor_prior_weight`,
10 by default; `historical_anchors{0}` removes it). Rigidity rho falls with the
threat (pragmatism under pressure). Traits come from WGI governance data: less
voice -> more order-minded and more rigid; effectiveness -> state capacity
(testing, tracing, suppression, job retention).

- Subjects of a sovereign (`jurisdictions_curated.csv`) blend NPI and suppression
  choices with the sovereign's: `x = autonomy x_own + (1 - autonomy) x_sovereign`.
- Pegged currencies (`monetary_curated.csv`) take the anchor's rate as their pragmatic target.
- Player overrides per agent and lever: advise (an extra prior), direct (within legal
  bounds), force (unbounded; political costs come with the politics system).
- Every material change is logged as a `decision` (before/after, P, I, A, mode): the
  decision journal behind the "Why?" panel.

With anchors on the back-test still passes (the 2020 slice is reproduced through the
priors); with anchors off the agents produce an emergent 2020 (`--set historical_anchors=0`).

## Calibration of the starting state (`src/sim/scenario.cpp`)

The 2019 data are turned into a consistent steady state, checked by
`tests/test_scenario.cpp` (no-shock run stays within 0.5 pp unemployment and
2% GDP of baseline for every nation):

1. Daily output = 2019 GDP grown half a year at trend; sector value added from `sectors.csv`.
2. Final demand C, G, I, X, M from WDI shares; imports split by sector import intensity.
3. **RAS** (biproportional) fit of a sector x {C, G, I, X} matrix: rows = sector value added, columns = domestic demand.
4. Government: revenue split into household / indirect / firm / resource taxes; transfers are the residual that reproduces the 2019 balance.
5. Dividend payout solves for household disposable income = C / (1 - saving rate).
6. A probe step computes each pop's income; consumption and saving rates per pop follow from it.
7. Within-group income dispersion is found by bisection so the national Gini matches the data.

## Mechanisms and their sources

**Epidemics** (`systems/epidemic.cpp`, daily, SIMD over regions with DCON `execute_parallel_over_region`)

- Pathogens are data (`assets/defs/pathogens.txt`): SARS-CoV-2 and seasonal influenza are
  seeded in 2020; 2009-like and 1918-like pandemic influenzas ship as templates. Each runs
  its own two-age-group SEIR on the same population (arrays indexed `epi_index(pathogen, age)`)
  with waning immunity and initial immunity. They interact through shared hospital capacity,
  cross-immunity, deaths (removed from every pathogen's compartments) and the policy and
  behavioural response to the combined, severity-weighted burden.
  `tests/test_epidemics.cpp`: influenza falls ~99% after March 2020 under the COVID measures
  (as WHO FluNet recorded); a concurrent severe pandemic raises COVID deaths through hospital
  overload; full cross-immunity stops spread among the recovered.
- The parameters below are those of the `cov` pathogen.

- SEIR + dying compartment, R0 = 3.0 (Flaxman et al. 2020), IFR 0.15% / 4.5% for under/over 65, doubled at 100% hospital overload.
- NPI effects multiply transmission by (1 - compliance x effect x level); effects from Brauner et al. (2021). Regional (targeted) measures reach 50% of transmission.
- Compliance falls with informality (Maloney & Taskin 2020) and with **fatigue**: effects halve-ish over ~6 months of restrictions (Sharma et al. 2021 found second-wave effects about half the first-wave ones).
- Voluntary distancing scales with reported deaths (Eichenbaum, Rebelo & Trabandt 2021), also fading with fatigue.
- Seasonality amplitude 0.25 (Gavenčiak et al. 2022), sign by hemisphere.
- Tracing only acts on detected chains and saturates on the case load of a 3M-person hotspot; testing raises detection.
- `test_trace_isolate` events encode suppression strategies (China, Korea, Japan, Australia): extra tracing capacity, centralised isolation (Pan et al. 2020), arrival quarantine.
- Seeding: 30 days before cumulative reported deaths reach 10 (Flaxman et al. 2020); Wuhan seeded on 2020-01-01.

**Economy** (`systems/economy.cpp`, weekly per nation, sharded by nation index)

- Structure: production capacity and output live on **regions**, labour, incomes and
  consumption on **pops**; both are DCON `execute_parallel_over_*` SIMD passes that
  gather nation values through cached ids and blend lanes with a `mask_vector`, so only
  nations whose weekly shard is due change. National goods markets clear in a scalar
  nation pass between them; a serial reduction posts every pop flow to the ledger in
  index order (exact money conservation). Labour markets are local: each pop hires from
  its own unemployed, by skill.
- Optional `regions.csv`, `region_sectors.csv`, `adjacency.csv` split nations into
  regions (tested with a synthetic split in `tests/test_multiregion.cpp`); without them
  a nation is one region.

- Capacity = potential x (1 - NPI cut) x (1 - sickness); NPI cuts per sector from `sectors.txt` (Dingel & Neiman 2020; del Rio-Chanona et al. 2020), smaller for targeted measures, halving with business **adaptation** (OECD Economic Outlook Dec 2020); supply-chain exposure for manufacturing.
- Household demand from real permanent income, precautionary saving, rates, excess deposits; contact services hit by voluntary avoidance (Goolsbee & Syverson 2021); 30% of blocked contact spending shifts to goods.
- Investment accelerator, exports from partner imports, tourism from world travel controls (UNWTO: arrivals -73%), imports by sector import intensity.
- Labour: layoffs follow *persistent* demand drops (lag rises with EPL); job-retention schemes hold a share of pre-crisis jobs on reduced hours; matching for rehiring; laid-off workers who cannot search exit the labour force.
- Prices: core inflation = expectations + Phillips slope 0.1 (Hazell et al. 2022) x output gap; oil passes into CPI.
- Fiscal: packages first fund job retention and income-support top-ups (E1), the rest goes to households and firms; firm support slows layoffs.
- Monetary: BIS rate events; Taylor (1993) rule otherwise, with the neutral rate implied by the 2019 setting.

**Oil** (`systems/markets.cpp`, daily): demand from oil-weighted activity and personal mobility (orders, fear, fatigue) with short-run price elasticity -0.07 (Hamilton 2009); OPEC+ targets from events; other producers price-elastic (shale highest); price follows flow and inventory gaps.

## Back-test (2020, in-sample)

`ctest -L backtest` runs the year and compares it with real outcomes
(`assets/backtest/2020_targets.csv`) using the bands in
`assets/backtest/2020_tolerances.txt`. **All six metrics pass**:

| Metric | Band | Nations within band |
|--------|------|---------------------|
| Real GDP growth 2020 | ±3 pp | 15/20 |
| Unemployment, 2020 mean | ±max(2 pp, 25%) | 16/20 |
| CPI inflation 2020 | ±max(1.5 pp, 35%) | 18/20 |
| Government net lending | ±4 pp | 10/16 |
| Reported COVID deaths per million | within 3x | 13/20 |
| Brent 2020 mean | ±6 USD | model 42.9 vs real 42.0 |

**Caveat — this is in-sample.** The behavioural parameters in
`assets/defs/parameters.txt` were tuned against these same targets, so the
back-test shows the mechanisms *can* reproduce 2020 jointly, not that they
forecast it. A held-out check (2021, Delta wave, vaccines, reopening inflation)
is the next validation step.

**Known misses** (and the missing mechanism):

- GDP: Spain (-5.3 vs -10.9: tourism dependence is a global share, not national), India (-0.3 vs -5.8: the informal-sector income collapse under the national lockdown), Russia (-7.3 vs -2.7), Turkey (-4.4 vs +1.8: the 2020 state-bank credit boom is not modelled), Great Britain (-7.0 vs -10.0).
- Deaths: Japan, Indonesia, India and Canada overshoot; Korea, Australia and China undershoot. One region per nation cannot represent where outbreaks happen; East Asian behaviour (masking, cluster tracing) is only partly captured by policy indicators.
- Deficits: France, Italy, Korea, Turkey, South Africa run 4–7 pp too large. OxCGRT E3 announced fiscal measures are a noisy proxy for above-the-line spending (see the scaling in `tools/pipeline/build_assets.py`); the IMF Fiscal Monitor database would be better but its API refuses automated clients.
- Inflation: India (2.5 vs 6.6: food supply shocks) and China.

## Open issues

- Exchange rates (everything is USD), credit and housing, bilateral trade.
- Per-decision cause tags for the "Why?" trace.
- Sub-national regions (the data path exists: `region` objects and the `pop_location` relationship).
- Calibration warnings printed at build: dividend payouts clamped for high-saving economies (CHN, SAU, TUR, KOR, JPN, FRA), Gini floors in low-inequality nations (CAN, IND).
