# 11 — Data & calibration

## Implemented pipeline (2020 slice)

What `tools/pipeline/` uses today. The wider list below is the target.

| Domain | Source used | Notes |
|--------|-------------|-------|
| Macro state 2019, outcomes 2020 | World Bank WDI API | GDP, shares, labour, prices, central-government finances |
| Lockdowns, cases, deaths, fiscal measures | OxCGRT national compact (CC BY 4.0) | level changes become `npi` events; E3 scaled to fiscal packages |
| Policy rates | BIS WS_CBPOL | changes only; euro-area rate applies to members |
| Brent | EIA RBRTE via datahub.io `oil-prices` | back-test only |
| Gaps, labour institutions, oil production | `nations_curated.csv` (hand-written, sources in header) | approximate values flagged as such |

The IMF DataMapper API rejects automated clients, so IMF WEO is not used; WDI
provides both the 2019 state and the 2020 targets. Results:
[12](12-simulation-slice-2020.md#back-test-2020-in-sample).

## Source datasets (2020 initial state)

| Domain | Dataset | Use |
|--------|---------|-----|
| Boundaries | geoBoundaries (admin-1/2), Natural Earth (coastlines, labels) | regions, adjacency |
| Population | UN WPP 2019/2022 revisions; WorldPop / GHS-POP rasters | national age structure; sub-national disaggregation |
| Macro | World Bank WDI, IMF WEO, IMF IFS (monetary), IMF GFS (fiscal), BIS (credit, policy rates) | GDP, prices, debt, rates, balance sheets |
| Input–output | OECD ICIO (2018–2020), WIOD; Eora for countries not covered | sector coefficients, trade in intermediates |
| Labor | ILOSTAT (unemployment, informality, wages, union density) | labor market state |
| Inequality | World Inequality Database (WID), PIP (World Bank poverty) | strata income distributions (μ, σ, α) |
| Trade | UN Comtrade / BACI (CEPII), CEPII Gravity dataset, WITS tariffs | bilateral flows, trade costs |
| Migration | UN DESA migrant stocks, UNHCR, World Bank remittances | diaspora stocks, refugee stocks |
| Regime | V-Dem (v12+) | regime index vector |
| Values | World Values Survey / EVS waves 7 | Inglehart–Welzel axes, left/right |
| Parties | ParlGov, Manifesto Project, Global Party Survey | party positions, seats |
| Military | Correlates of War NMC, SIPRI milex | CINC power index |
| Health | WHO GHO, Our World in Data COVID dataset | baseline mortality, health capacity, COVID back-test |
| Energy/climate | IEA (where licensable) / EIA / Our World in Data energy, Global Carbon Project | energy mix, emissions |
| Sub-national economy | DOSE (Wenz et al. 2023), Kummu et al. gridded GDP, VIIRS nightlights | regional GDP disaggregation |

Licences must be recorded per dataset in `data/LICENSES.md`, and the
scenario build fails if a source has no licence entry.

## Pipeline

```
raw downloads (pinned versions, checksums)
  → harmonise (ISO3 codes, USD 2020 PPP, ISIC Rev.4)
  → disaggregate national → regional (population/nightlights weights; DOSE where available)
  → build strata (fit lognormal-Pareto per region from WID + PIP + regional GDP)
  → initialise balance sheets (SFC: enforce zero-sum via RAS/cross-entropy balancing)
  → calibrate behavioural parameters (see below)
  → emit scenario.bin + map.bin (versioned, hashed)
```

- The pipeline lives in `tools/pipeline/` (Python for data wrangling;
  C++ `headless` for calibration runs).
- **Balancing:** national accounts, IO tables and financial balance sheets
  never agree perfectly, so we use RAS / cross-entropy (Robinson, Cattaneo &
  El-Said 2001) to get a consistent starting SAM (social accounting matrix).
- **Gaps:** a documented imputation hierarchy (national → regional peer
  group → global); every imputed value carries a flag that the UI shows as
  "estimated".

## Calibration

1. **Direct from data:** shares, coefficients, stocks (IO coefficients,
   tax rates, demographics).
2. **From the literature:** elasticities with published ranges (CES σ,
   Armington σ, matching η, ETI, Taylor-rule coefficients). We store
   the central value and its range.
3. **Indirect inference / method of simulated moments:** remaining
   behavioural parameters (markup adjustment speed, MPC rule, opinion ε,
   PSI thresholds) are tuned so the model reproduces target moments on
   the 2010–2019 *warm-up* (unemployment volatility, inflation persistence,
   protest frequency by regime type).

## Validation (back-testing)

The sim starts 2020-01-01, and real history is known through ~2025. With the
**historical anchors** scenario setting (AI follows recorded major policies:
lockdown dates, fiscal packages, rate decisions), we compare:

| Episode | Target |
|---------|--------|
| COVID 2020 | quarterly GDP drop by country (sign, magnitude ±40%), unemployment spike in US vs furlough-economy EU |
| 2021–22 inflation | headline CPI peak and timing, energy-led in Europe |
| 2022 energy shock | European gas price spike → industrial output and household real income |
| Sovereign stress | spreads for high-debt economies within plausible bands |

**Stylised facts** (ctest `slow` label, on synthetic and full scenarios):
Okun's law coefficient sign and size, Beveridge curve slope, Phillips curve
existence, Kaldor facts over long runs (stable capital/output, labor share
in range), Zipf-like city size distribution maintained by migration,
demographic transition pattern.

Back-tests are not meant to fit history exactly. They show that the model's
*responses* have the right sign, magnitude and timing. They also catch
regressions: tolerance bands live in `tests/backtest/*.toml`.
