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

The slice is a **calibration and measurement device**: every mechanism is generic
and data-driven (optional tables, documented defaults for nations without rows,
nothing keyed to a country), so the full-world dataset needs no code changes:
the world 2020 scenario (218 nations, 3,243 regions) already builds and runs on
the same code, uncalibrated ([11](11-data-calibration.md#world-2020-dataset)).

| Design element | Slice status |
|----------------|--------------|
| SFC ledger ([02](02-simulation-core.md#accounting-core)) | **Yes.** Pops, firms, government, finance, rest of world per nation; every payment via `sim::transfer`; conservation tested |
| "Why?" flows | **Yes.** Year-to-date flows by kind per nation with the top 4 cause tags each (`--flows ISO3`), decision journal (levers, bills, elections, coups, wars, cyber operations, chokepoint incidents, treaties, agreements, UN and WTO votes, variants, plan steps, term limits, court seats) |
| Cadence scheduler | **Yes.** Daily: events, AI, epidemic, vaccination, variants, logistics, commodities, oil, internet coupling, protest cascade, organisation memberships and GA sessions. Weekly (7 shards): economy, firms, executive levers. 28-day shards: society, groups, politics, institutions, geopolitics, diplomacy, treaties, agreements, organisations, strategy, reforms, finance, credit, bonds, labour, fiscal, assets, long run, technology, climate policy, strata |
| Determinism, saves, forks | **Yes.** DCON serialisation; save -> load -> continue is bit-identical; counterfactual forks copy the state in memory and run ahead, on a worker thread and optionally at reduced detail (`sim/fork.hpp`) |
| Error handling | **Yes.** `-fno-exceptions -fno-rtti`; first error with file:line through `core::diagnostics` ([09](09-engine-architecture.md)) |
| Production | **Yes.** Dynamic input-output network with input stocks and critical inputs (Pichler et al. 2022); capacity from NPIs, sickness, strikes, cyber damage, disasters; LES demand and sector markups |
| Logistics and transport | **Yes.** Sea-lane graph, 8 chokepoints with stochastic blockages, rerouting, queues, port congestion, container freight market, delays and freight costs in trade, prices and input arrivals |
| Commodities | **Yes.** Oil (OPEC+ decisions) plus gas, coal, grains, metals; terms of trade, rents, food prices; cartel restraint and grain export bans |
| Firms | **Yes.** SME and large cohorts per sector: cash buffers, bankruptcies, entry; boards as agents |
| Credit, banks, bonds | **Yes.** Credit as deviation from the calibrated path, directed state-bank lending, NPLs, capital, forbearance, countercyclical buffer; 10-year yields, term premium, QE, roll-over of the debt; financial account, FX intervention, capital controls |
| Labour | **Yes.** Layoffs, job retention, EPL, informality by identity group, wage curve with bargaining coverage, Nash bargaining, nominal rigidity, unions, minimum wage, retirement age |
| Fiscal | **Yes.** Progressive household taxes (HSV), compliance, behavioural base, VAT, corporate and wealth taxes, subsidies, carbon price, public investment, R&D, police, education and health as bills; sub-national fiscal capacity; ageing pensions |
| Trade | **Yes.** Gravity shares with sea-route distances and contiguity, Armington competition, a stored bilateral export matrix, sanctions, FTAs with bargained depth, tariffs, EU customs union, WTO disputes, freight costs |
| Asset prices | **Yes.** House and equity prices (national and regional house prices), wealth effects, shock events |
| Pops | **Yes.** Skill kind (low / mid / high / retired) x identity group (caste, race, hukou, migrant status) per region, with precarious strata splitting off |
| Society | **Yes.** Demography with age bands, mortality, fertility transition and Great Gatsby mobility, migration with asylum and housing, values with cross-border diffusion, trust, crime and police, horizontal inequality, protest cascades, PSI with youth bulge (national and regional) |
| Epidemics | **Yes.** Multi-pathogen SEIR, NPIs, vaccines with COVAX and age priority, variants, random novel epidemics, WHO detection (commuting coupling needs regions; one region per nation here) |
| Politics | **Yes.** Real parties (CHES / GPS / V-Party), adaptive strategies, challenger entry, Overton window, elections with presidential runoffs and term limits, cabinets, governors, lobbies, legislation with veto players, confidence votes, courts with appointments, military, identity legitimacy |
| Geopolitics | **Yes.** Trust (incl. language and religion), threat, sanctions, alliances (formed by shared threat), FTAs, IMF programmes, disputes and crisis bargaining, war emergent from the economy, fronts with terrain and willingness to serve, drones and precision strike, insurgency and rebel agents, cyber operations, the internet as political infrastructure; UN General Assembly, Security Council, WTO, EU, Human Rights Council, WHO; climate pledges, migration deals, debt relief, conditional aid, power sharing |
| Long run | **Yes.** Capital hysteresis, trend growth with catch-up, R&D and vintage capital, technology diffusion, climate with carbon pricing, energy vintages and disasters; strategic goals with multi-step plans |
| Not yet | Production units per sector x region, structural gravity, sub-national budgets as ledger accounts, regional rents, class tiers and age bands on pops, region-level fronts |

## Day loop (`src/sim/simulation.cpp`)

```
apply_events -> update_ai -> epidemic -> vaccination -> logistics -> commodities
  -> economy -> firms -> oil -> demography -> migration -> groups -> internet -> opinion
  -> social capital -> unrest (drive) -> protest (cascade) -> politics -> institutions
  -> geopolitics -> finance -> credit -> bonds -> labour -> fiscal -> assets -> treaties
  -> long run -> technology -> climate policy -> policy -> strategy -> diplomacy
  -> organisations -> reforms -> politics extensions -> agreements -> variants
  -> strata -> indicators -> ++tick
```

## New systems in this slice

Each system file starts with a header that states its mechanisms and sources; the
design docs give the detail ([03](03-economy.md), [04](04-society.md),
[05](05-politics-institutions.md), [06](06-geopolitics-ai.md)).

- **Logistics** (`systems/logistics.cpp`). A 2021-style Suez grounding is not
  scripted: blockages arrive from a hazard that rises with traffic, vessel size and
  the spring winds; `tests/test_logistics.cpp` forces a 6-day blockage and finds a
  queue of ~230 ships clearing 8 days after it began, Europe-Asia transit +7 days and
  higher freight rates; a long closure diverts traffic round the Cape; 40 years of
  draws give 8 Suez blockages (calibration ~0.1 a year); a war of Turkey closes the
  Turkish Straits.
- **Credit** (`systems/credit.cpp`). Turkey's 2020 credit boom emerges from data, not a
  rule: large state banks plus a dependent central bank make a growth-first
  executive (`credit_bias`), whose directed lending peaks at ~9% of GDP a year;
  credit grows 30% (real 35%). The same executive presses a dependent central bank
  for lower rates (`tests/test_credit.cpp`).
- **Identity groups** (`systems/society.cpp`). India's forward castes, OBCs, Dalits,
  Adivasis and Muslims start in the observed income order; the 2020 lockdown cuts
  the income of the informal-heavy groups most (Deshpande & Ramachandran 2020).
- **Protest cascades.** No Nepal-specific rule: a young, connected society under
  strain that suddenly restricts social media reaches insurgency-level unrest,
  while the same restriction phased in slowly does not
  (`tests/test_society_groups.cpp`).
- **Internet.** Starting online controls follow one rule from Freedom on the Net
  (China filters most, then Russia; the US not at all); protest abroad reaches
  coupled publics (US -> Canada) ~50x more than filtered ones (`tests/test_internet.cpp`).
- **Institutions.** A majority passes its bill in full; an opposition legislature
  passes ~45%, ~20% with strong veto players; courts strike down forced levers; a
  disloyal military defects under mass protest (`tests/test_institutions.cpp`).
- **Vaccines.** Approval ~345 days after the outbreak, then a production ramp: 4.4M
  Americans vaccinated by 31 Dec 2020 (real: ~2.8M doses).
- **Organisations** (`systems/organisations.cpp`). The UN, WTO, EU, HRC and WHO are
  data (`defs/organisations.txt`, `organisations_curated.csv`), not code: one generic
  organisation object with memberships and a decision rule. In the slice the UK
  leaves the EU on 2020-01-31 and the elected UNSC seats rotate on 1 January.
  `tests/test_un.cpp`: a permanent member occupying a neighbour vetoes sanctions on
  itself; a tariff hike draws WTO complaints, and with the Appellate Body paralysed
  the loser appeals into the void, while with it working the loser complies or
  faces retaliation.
- **Financial shock events.** `house_price_shock`, `equity_shock`,
  `credit_spread_shock` (with an unwind horizon) and `bank_capital_shock` let a
  scenario anchor a financial crisis (the coming 2008 slice); assets, credit and
  banks then propagate them ([03](03-economy.md#money-banking-and-finance)).

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
`tests/test_scenario.cpp` (a 120-day no-shock run stays within 0.5 pp
unemployment and 2% GDP of baseline for every nation; longer runs drift, see
[Open issues](#open-issues)):

1. Daily output = 2019 GDP grown half a year at trend; sector value added from `sectors.csv`.
2. Final demand C, G, I, X, M from WDI shares; imports split by sector import intensity.
3. **RAS** (biproportional) fit of a sector x {C, G, I, X} matrix: rows = sector value added, columns = domestic demand.
4. Government: revenue split into household / indirect / firm / resource taxes; transfers are the residual that reproduces the 2019 balance.
5. Dividend payout solves for household disposable income = C / (1 - saving rate).
6. A probe step computes each pop's income; consumption and saving rates per pop follow from it.
7. Within-group income dispersion is found by bisection so the national Gini matches the data.
8. Identity groups split each skill tier by a RAS on education odds (national totals hold);
   group wages, unemployment, wealth and informality are rescaled to national data.
9. Household tax progressivity (HSV) is calibrated revenue-neutral on the probe incomes,
   then the probe runs again before consumption and saving are set.
10. Credit, bonds, online controls, tariffs, labour institutions, trust and the
    production network's input stocks start at their 2019 levels; all later behaviour
    is a response to changes from that state.

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

## Society, politics, geopolitics (`systems/society.cpp`, `politics.cpp`, `geopolitics.cpp`)

All on 28-day shards. Demography moves people between children, workers and
retirees and removes epidemic and war deaths; migration follows Harris-Todaro
expected-income gaps (adjacency internally, gravity internationally, refugees
from wars); values drift with insecurity and converge by bounded confidence;
unrest climbs a ladder driven by relative deprivation and the Political Stress
Index. Pops vote by salience-weighted distance and retrospective performance;
elections are held on their 2020 dates (USA 2020-11-03, KOR 2020-04-15) with PR or
FPTP seats and minimal connected coalitions; legitimacy falls with forced
overrides; the winning-coalition size (selectorate) tilts fiscal support toward
firms in narrow regimes and weights health in broad ones. Relations hold trust,
threat, gravity trade shares, sanctions (status quo anchored: US-China tariffs,
Russia sanctions), alliances and war; exports follow bilateral trade access.
`tests/test_politics.cpp` checks election dates, no 2020 war, persistent
status-quo sanctions, the legitimacy cost of force and the trade cost of sanctions.

Parties are the real 2020 party systems (`parties_curated.csv`; expert-survey
positions are approximate and flagged in the file); nations without data keep three
generic parties. The 2020 US and Korean elections are now contests between real
parties inside an Overton window.

## Finance, long run, climate (`systems/finance.cpp`, `longrun.cpp`)

Every 28 days. Floating real exchange rates move with the current account, interest
differentials vs the US and sovereign risk; pegs and unions follow their anchor; rates
feed trade volumes and import inflation. Spreads rise with debt, deficits, weak
capacity and legitimacy (reserve currencies lower them) and reach the debt cost as it
rolls over; an exploding spread triggers a default (haircut through the ledger).
Remittances (WDI received / paid) follow host-economy earnings. Potential output follows
the investment gap; trend growth follows institutions and human capital. Emissions
(36.4 Gt in 2019, scaled by oil demand) drive CO2, transient warming and DICE-2016R
damages tilted toward hot regions. `tests/test_finance.cpp`: 2020 emissions fall
(model 33.5 Gt; real -5.4%), Mexico's remittances rise as a share of GDP, a debt
blowout ends in a default that conserves money.

## Back-test (2020, in-sample)

`ctest -L backtest` runs the year and compares it with real outcomes
(`assets/backtest/2020_targets.csv`, plus hand-written yields, credit growth and
shipping in `2020_targets_curated.csv`) using the bands in
`assets/backtest/2020_tolerances.txt`. Current state: **8 of 10 metrics pass**:

| Metric | Band | Within band |
|--------|------|-------------|
| Real GDP growth 2020 | ±3 pp | **13/20 (needs 15)** |
| Unemployment, 2020 mean | ±max(2 pp, 25%) | 14/20 (needs 14) |
| CPI inflation 2020 | ±max(1.5 pp, 35%) | 16/20 |
| Government net lending | ±4 pp | 10/16 (needs 10) |
| Reported COVID deaths per million | within 3x | 14/20 |
| 10-year yield, end 2020 | ±1 pp | 17/18 |
| Bank credit growth 2020 | ±6 pp | 13/19 |
| Brent 2020 mean | ±6 USD | pass |
| Container freight index, 2020 mean / 2019 | ±0.3 | **fail** (2020H2 surge missing) |
| Idle container fleet, 2020 peak | ±3 pp | pass |

Since the previous documented run (before the policy catalogue, organisations,
agreements and demography) GDP fell from 14/20 to 13/20, net lending from 12/16 to
10/16 and credit from 14/19 to 13/19; yields rose from 15/18 to 17/18. Every new behaviour is a response to
*changes* from the calibrated 2019 state (credit, tariffs, boards, censorship,
policy acceptance, every reform lever), so the first months of a no-shock run
stay at the baseline.

**Caveat — this is in-sample.** Behavioural parameters were tuned against these
targets, so the back-test shows that the mechanisms *can* reproduce 2020 jointly,
not that they forecast it. A held-out year (2021) is the next validation step.

**Known misses** (itemised before the latest systems; to be re-checked):

- GDP: India (-0.5 vs -5.8), Russia (-9.0 vs -2.7), Turkey (-4.5 vs +1.8; the credit boom
  now emerges but GDP responds too little: all economies recover too slowly in 2020H2),
  Brazil, Spain, Argentina.
- Container freight: the 2020H2 rate surge is missing because simulated import demand
  stays below baseline (consumer demand has no durable-goods composition; no port
  productivity data). Idle capacity in the spring is right.
- Deaths: Japan, Indonesia, India and Canada overshoot; Korea, Australia and China
  undershoot (one region per nation).
- Deficits: UK too small, Korea, Saudi Arabia, South Africa too large.
- Inflation: India (food), China, Saudi Arabia, Russia.
- Credit growth: India and Indonesia too high, Brazil and Saudi Arabia too low, Mexico.

## Open issues

See [13](13-calibration-notes.md) for the calibration findings so far (causes and fixes) and the diagnosis of the remaining baseline drift.

- **Multi-year baseline drift.** A no-shock run is steady for the 120 days the test
  checks, but over several years it drifts: unemployment rises steadily from the
  first year. Being worked on; until fixed, multi-year results (and the 2009 scores
  of the 2008 slice) are not meaningful.
- 2020H2 recovery speed and the goods-demand shift (freight surge, Turkey).
- Journal review at +1m / +6m / +2y for the "Why?" trace.
- Sub-national regions and budgets in the slice (one region per nation).
- Party data, identity groups, finance tables and organisation memberships for the
  full world (the world 2020 scenario runs on defaults).
- A held-out year (2021) and the 2008 slice as out-of-sample tests.
- Calibration warnings at build: dividend payouts clamped (CHN, SAU, TUR, KOR, JPN, FRA),
  transfers residual below 2% of GDP (CAN, IND), Gini floors (CAN, IND).
