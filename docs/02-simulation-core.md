# 02 — Simulation core

## Time

- **Base tick = 1 day.** Daily resolution is needed for epidemic dynamics
  (COVID at game start), financial and sovereign-spread shocks, and fast crisis
  diplomacy. A weekly tick would blur all three.
- Calendar: proleptic Gregorian, starting 2020-01-01. `tick_t` is a `uint32_t`
  day count (enough for ~11 million years).
- Campaign horizon target: 2100+ (~29,000 ticks). Nothing in the design may
  assume a fixed end date.

## Cadence scheduler

Most systems do not need daily updates, and running everything on the 1st of
the month causes frame spikes. Each system declares a **cadence**:

```
struct cadence { uint16_t period_days; uint16_t shards; };
```

`P` must be a multiple of `K`. On every tick where `t % (P/K) == 0`, a
system with `(period P, shards K)` processes shard `k = (t / (P/K)) % K`
(for `K == P` this is simply `t % P`). Region-keyed systems
assign each region a shard `region_id % K`, so every region is processed
**exactly once per period** and the work per tick is ~1/K of the total.
`tests/test_scheduler.cpp` checks this invariant.

| Cadence | Systems |
|---------|---------|
| Daily, unsharded | epidemics (SEIR), FX / sovereign spreads / commodity spot prices, diplomacy events, AI crisis reactions, command application |
| Weekly, 7 shards | production, goods-market price adjustment, labor matching, consumption, firm finance |
| "Month" = 28 days, 28 shards | fiscal accounts, migration, opinion dynamics, polling, AI strategic re-planning, credit cycle |
| Quarter = 91 days, 91 shards | national accounts aggregation, trade-gravity rebalance, elections calendar checks |
| Year = 364 days, 364 shards | demography (cohort ageing, fertility, mortality), TFP and technology diffusion, capital vintages, institutional drift, climate |

All sim periods are whole numbers of weeks (7 / 28 / 91 / 364 days), so the
cadences line up with each other. Calendar months and years are a
**reporting** concern only: national accounts and the UI aggregate the
daily-dated ledger flows into true calendar periods. Implemented in
`src/sim/scheduler.hpp`.

### Consistency rules

- **Stale-read rule.** A shard reads the *last committed* values of entities
  in other shards, so cross-shard reads lag by at most one period. This is a
  documented approximation, not a bug.
- **Flows that cross shards** (trade, migration, remittances) are written into
  a per-period **pending flow buffer** and settled at the destination's next
  update. Conservation holds because a flow is debited and credited exactly
  once each.
- **National aggregates** (CPI, GDP, tax receipts) are running sums updated
  incrementally as shards commit, with a full recomputation each quarter to
  correct floating-point drift.

## Long-campaign detail extension

Over decades the world changes in *structure*, not only in numbers:

- **Technology tree with diffusion.** New sectors and goods (e.g. EVs,
  grid storage, AI services) enter at frontier economies and spread with
  lags following Comin & Hobijn (2010). A new good adds a column to the
  commodity arrays, which are pre-sized with spare slots (see
  [10](10-data-model-dcon.md)).
- **Strata split and merge.** When the dispersion of a stratum along a tracked
  dimension (sector, income) exceeds a threshold, it splits (e.g. a
  platform/gig worker stratum appears). Strata that converge merge. Sizes and
  balance sheets are conserved in both cases.
- **Cohort memory.** Age bands carry history: unemployment at labor-market
  entry leaves lasting wage scarring (Kahn 2010). Formative-years conditions
  shape values (Inglehart).
- **Climate.** An optional DICE-style damage function ([03](03-economy.md#growth--long-run)).
- **Spatial LOD.** Regions that are quiet (small Δ on all tracked indicators)
  and far from the player's focus may drop from weekly to monthly cadence. The
  aggregation they use is conservation-safe. A region is promoted back
  immediately on an alert, an override touching it, or the player selecting it.

## Determinism

Required for counterfactual forks, replays, saves and regression tests.

- Fixed system order. Fixed iteration order within a system (DCON index order).
- RNG: counter-based (Philox4x32-10). Each draw is `philox(key = (seed, system_id),
  counter = (tick, entity_id, draw_index))`. There is no shared RNG state, so
  parallel iteration stays deterministic.
- Parallelism comes only from DCON's `execute_parallel` (oneTBB with a static
  partitioner). Parallel bodies write only to the entities they were given.
  **Reductions are never done inside a parallel body**: either use
  `execute_serial`, or write per-entity partials in parallel and then sum them
  serially in index order. Unordered `std::atomic` float accumulation is
  forbidden in the sim.
- Same build + same platform ⇒ bit-identical results. Cross-platform bit
  identity is a *should*: we avoid `-ffast-math` and build with
  `-mavx2 -mfma -ffp-contract=on`. That allows FMA fusion only inside a
  single expression, which is deterministic for a given compiler and flags.
  It is tested in CI.
- Player and AI commands are queued and applied only at the start of a tick,
  in a stable order (by agent id, then by sequence).

## Accounting core

The economy is **stock-flow consistent** (SFC; Godley & Lavoie 2007). Each
nation keeps a balance-sheet matrix and a transactions-flow matrix over these
institutional sectors:

| Sector | Granularity |
|--------|-------------|
| Households | per pop (stratum) |
| Non-financial firms | per sector × region |
| Banks | per nation (aggregate) |
| Government | national + per region (sub-national budgets) |
| Central bank | per currency area |
| Rest of world | per partner nation (via trade/finance flows) |

Invariants (asserted in debug builds and tested in ctest):

1. Every flow has exactly one payer and one payee: rows and columns of the
   transaction matrix sum to zero.
2. Every financial asset has a matching liability.
3. Δ stocks = flows + revaluations over each period.

Every flow carries a **cause tag** (`flow_tag` object; see
[10](10-data-model-dcon.md)), which powers the "Why?" trace
([01](01-gameplay-loop.md#51-the-why-trace-always-on-cheap)). Each flow stores
the top 4 tag contributions plus a remainder.

## Counterfactual forks

- The whole `game_state` (including `world`, the DCON container) is copied to
  a worker thread. DCON's flat SoA arrays make this a set of memcpys.
- The fork runs at **reduced detail**: every system at monthly cadence,
  strata aggregated to class tier, AI on cached strategic plans. The UI
  labels it clearly as approximate.
- Up to 2 forks at once. Results are kept as indicator time series, not as
  full states.

## Performance budget

Target scale: ~4,000 regions, ~200 nations, ~120,000 pops (30 per region),
~40 sectors, ~60 commodities.

| Item | Budget (amortised per tick, 8 threads) |
|------|------------------------------------------|
| Daily systems | ≤ 2 ms |
| Weekly (1/7 shard) | ≤ 4 ms |
| Monthly (1/28 shard) | ≤ 1.5 ms |
| Quarterly + annual shards | ≤ 1 ms |
| Snapshot publish | ≤ 0.5 ms |
| **Total** | **≤ 9 ms → ~110 ticks/s → 1 year ≈ 3.3 s** |

The hard requirement from the vision is 1 year in ≤ 10 s (≈ 37 ticks/s).
The ~3× gap up to this budget is reserved for long-campaign detail growth
and for running forks.
