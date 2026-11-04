# geosim design documents

geosim is a socioeconomic simulation of the contemporary world. It starts on
**1 January 2020**, just before COVID-19, and runs as an open-ended campaign
(target horizon 2100+). AI agents govern every nation: heads of state, cabinets,
parliaments, central banks, regional governors. The player can take over any
of these agents, one or many at a time, and then **watch what happens** to
incomes, jobs, prices, health, inequality, values, unrest and international
relations on a plain top-down map.

The game is really a realistic socioeconomic model with a video-game interface.
Every model below is chosen for a reason, and each choice is traced to
published literature in [references.md](references.md).

## Reading order

| # | Document | What it settles |
|---|----------|-----------------|
| 00 | [Vision](00-vision.md) | Pillars, player fantasy, non-goals |
| 01 | [Gameplay loop](01-gameplay-loop.md) | **Core doc.** Act → simulate → observe → attribute; overrides; trade-offs |
| 02 | [Simulation core](02-simulation-core.md) | Time, tick scheduler and sharding, determinism, accounting ledger, forks |
| 03 | [Economy](03-economy.md) | Production, prices, labor, money, fiscal, trade, growth |
| 04 | [Society](04-society.md) | Strata, demography, health, migration, values, unrest |
| 05 | [Politics & institutions](05-politics-institutions.md) | Regimes, parliaments, elections, legitimacy |
| 06 | [Geopolitics & AI](06-geopolitics-ai.md) | Agent utility model, ideology vs pragmatism, diplomacy, conflict |
| 07 | [Map & filters](07-map-and-filters.md) | Map modes, colour scales, region geometry pipeline |
| 08 | [UI](08-ui.md) | Plain UI rules, layout, the "Why?" inspector |
| 09 | [Engine architecture](09-engine-architecture.md) | Modules, threads, SDL3/Vulkan/FreeType/HarfBuzz, saves |
| 10 | [Data model (DCON)](10-data-model-dcon.md) | Objects, relationships, property conventions |
| 11 | [Data & calibration](11-data-calibration.md) | Source datasets, 2020 initial state, back-testing |
| 12 | [2020 simulation slice](12-simulation-slice-2020.md) | **What is implemented now**, mechanisms, back-test results |
| — | [References](references.md) | Bibliography |

## Conventions used in these docs

- **Must / should / may** have their RFC 2119 meanings.
- *Open question* blocks mark decisions not yet made.
- Symbols: `r` region, `n` nation, `p` pop (stratum), `s` sector, `c` commodity,
  `t` tick (day). Money is in nominal local currency unless marked `real` or `USD`.
