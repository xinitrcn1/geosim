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

| Cadence | Systems (implemented) |
|---------|---------|
| Daily, unsharded | scenario events, executive crisis reactions and central-bank meetings (`ai.cpp`, incl. FX intervention and macroprudential buffers), epidemics (SEIR per pathogen, commuting coupling), vaccination, variants, logistics (chokepoints, routing, queues, ports, freight market), commodity markets (gas, coal, grains, metals) and the crude oil market, internet coupling and protest contagion, protest threshold cascade, organisation memberships (exits, elected terms; elections on 1 January) and the UN General Assembly session (15 September), indicators |
| Weekly, 7 shards (by nation) | economy (demand, production network, prices, labour flows, incomes), firms (cash flow, exit, entry), executive lever choices (incl. the policy catalogue of `policy.cpp` and `reforms.cpp`) |
| "Month" = 28 days, 28 shards (by region or nation) | demography (age bands, mortality, mobility), migration, identity groups, opinion, social capital, unrest drive and PSI (national and regional), politics (vote intention, elections, parties, Overton window), institutions (bills, confidence, courts, military), politics extensions (court appointments, identity legitimacy, willingness to serve), geopolitics (trust, threat, sanctions, war), disputes and diplomacy, treaties and agreements (treaty objects, rebels), WTO disputes, strategy (goals and plan steps), finance (FX, spreads, default), credit, bonds, labour (wages, unions, minimum wage), fiscal (taxes, compliance, public investment), reform channels (tax rates, wealth tax, subsidies, vintage capital, regional housing, fiscal equalisation), assets, long run (capital hysteresis, trend growth, climate), climate policy and disasters, technology diffusion, strata split / merge, cabinets / governors / lobbies, internet value diffusion and cyber operations |

The scheduler (`src/core/scheduler.hpp`) also defines `quarterly` (91, 91)
and `annual` (364, 364) cadences, but no system uses them yet: everything
slow runs on the 28-day tier. A few 28-day systems also do a world-level
step on ticks where `t % 28 == 0` (world trade shares, climate, and the
Security Council, Human Rights Council and EU recovery fund in
`organisations.cpp`).

All sim periods are whole numbers of weeks (7 / 28 / 91 / 364 days), so the
cadences line up with each other. Calendar months and years are a
**reporting** concern only: national accounts and the UI aggregate the
daily-dated ledger flows into true calendar periods. The day's system order
is fixed in `src/sim/simulation.cpp` (`advance_day`).

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

Status: the stale-read rule holds as described. The pending flow buffer and
the quarterly recomputation are not implemented yet; cross-entity money moves
settle immediately through `sim::transfer` in index order (serial
reductions), which keeps the ledger exact.

## Long-campaign detail extension

Over decades the world changes in *structure*, not only in numbers.
Implemented in `longcampaign.cpp` unless noted.

- **Technology diffusion** (28 days, nation shard). Technologies are data
  (`assets/defs/technologies.txt`: electric vehicles, renewable power, AI
  services, mobile broadband / 5G, the mRNA vaccine platform). Adoption is
  logistic (Griliches 1957); each nation lags the frontier by a base lag plus
  gaps in income, skills and internet openness (Comin & Hobijn 2010), starting
  from observed 2019 adoption (`technology_curated.csv`; IEA, Ember) where
  given. Trend growth already holds the 2019 diffusion path, so productivity
  effects use only the *deviation* from it; physical effects (oil per unit of
  activity, CO2 per unit of energy, connectivity, vaccine development time)
  follow adoption since 2020. New goods as extra commodity columns are planned.
- **Strata split and merge** (28 days, region shard). When the jobless share
  of a worker pop rises well above what its job slots imply (a long-term
  unemployed layer after a shock), the excess jobless split off into a
  **precarious** stratum with their share of savings, weights and job slots;
  it merges back once its jobless share converges to its parent's. Sizes,
  money and job slots are conserved both ways; entrants join base strata.
  Splits along other dimensions (sector, gig work) are planned.
- **Cohort memory.** Workers entering the labour force during high
  unemployment permanently lower their pop's wage level (scarring; Kahn
  2010), and cohorts entering in insecure times pull their pop's values toward
  survival (formative years; Inglehart 1997). Region age bands carry the
  cohorts ([04](04-society.md#demography)).
- **Climate.** A DICE-lite carbon cycle and damage function, carbon pricing,
  energy vintages and disasters ([03](03-economy.md#growth--long-run)).
- **No spatial LOD.** Every nation runs at full detail; the full world is to
  meet the performance budget by optimisation rather than by lowering the
  detail of quiet nations (an earlier spatial LOD was removed).

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

**Implemented today** (`src/sim/ledger.hpp`): accounts per pop and, per
nation, one each for firms, government, finance (banks and central bank
together) and the rest of the world. Every payment is a `transfer` between two
accounts, so world money is conserved exactly. The finer split above (firms
per sector × region, sub-national budgets, a separate central bank) is planned.

Invariants (asserted in debug builds and tested in ctest):

1. Every flow has exactly one payer and one payee: rows and columns of the
   transaction matrix sum to zero.
2. Every financial asset has a matching liability.
3. Δ stocks = flows + revaluations over each period.

**Implemented today:** each nation keeps year-to-date nominal flows by kind
(`nation.flows`, indexed by `enum class flow` in `src/sim/model.hpp`:
consumption, wages, benefits, taxes, credit, tariffs, war spending, IMF
lending, …), and AI decisions are journalled as `decision` objects with their
pragmatic, ideological and prior components. Together these feed the "Why?"
trace ([01](01-gameplay-loop.md#51-the-why-trace-always-on-cheap)).

**Cause tags** (implemented, `ledger.cpp`): a system sets the cause of the
payments it makes (`cause_scope`: a lever of a nation, a journal entry such as
an agreement, or an event; code `(kind << 24) | (nation << 8) | detail`), and
the ledger keeps, per nation and flow kind, the top 4 causes
(`tag_code`, `tag_amount`) plus an untagged remainder (`tag_rest`).
`--flows ISO3` prints them.

## Counterfactual forks

**Implemented** (`src/sim/fork.hpp`):

- `copy_state` deep-copies the `game_state` through DCON's in-memory
  serialisation (the same path as saves) plus the parameters, tick and seed,
  then rebuilds derived indices. A fork is therefore bit-identical to a
  save/load of the live state, which is never touched.
- `run_fork(state, days, change)` applies a change to the copy (e.g. an
  override), advances it day by day at **full detail** (the normal
  `advance_day`), and returns per-nation indicator paths (GDP index,
  unemployment, CPI, population-weighted unrest). Results are indicator time
  series, not full states.

- `fork_job` runs a fork on a **worker thread**: the state is copied before
  the thread starts, so the caller keeps simulating the live state; the UI
  runs up to two at once.
- **Reduced detail** (`fork_options::reduced`): agents frozen on their current
  levers (`ai_frozen`, cached plans). Faster and approximate; the UI labels it
  so. Aggregated pops in reduced forks are planned.

## Performance budget

Target scale: ~4,000 regions, ~200 nations, ~120,000 pops (30 per region),
~40 sectors, ~60 commodities.

| Item | Budget (amortised per tick, 8 threads) |
|------|------------------------------------------|
| Daily systems | ≤ 2 ms |
| Weekly (1/7 shard) | ≤ 4 ms |
| Monthly (1/28 shard) | ≤ 1.5 ms |
| Quarterly + annual shards (no systems yet) | ≤ 1 ms |
| Snapshot publish | ≤ 0.5 ms |
| **Total** | **≤ 9 ms → ~110 ticks/s → 1 year ≈ 3.3 s** |

The hard requirement from the vision is 1 year in ≤ 10 s (≈ 37 ticks/s).
The ~3× gap up to this budget is reserved for long-campaign detail growth
and for running forks.

## Commands

Everything a player does to the simulation is a **command** (`sim/commands.hpp`). A
command carries a tag (`set_override`, `set_ai`, `action`), the issuing player, a
per-player sequence number, the tick it applies to, and a payload union of plain values:
raw entity indices, enums and floats, with no pointers or strings, so it is trivially
copyable. The client pushes commands into a lock-free single-producer single-consumer
queue (`core/spsc.hpp`). Before simulating each tick (and while paused), the sim thread
drains the commands due (`drain_commands`) and applies them in order (`apply_command`).
Results (accepted, message) return through a second SPSC queue, matched by sequence
number.

Commands are plain data applied at a known tick, so a deterministic lockstep peer that
applies the same stream at the same ticks stays bit-identical, and a recorded stream
replays a session. The client never writes sim state except through commands. Read-only
snapshots for the UI are separate.

- `set_override`: one lever of an agent, with mode (none, advise, direct, force) and value
  (`ai.cpp` `settle` / `apply_override`).
- `set_ai`: the agent's AI on (the player advises or forces) or off (every lever directed
  at its current choice until the player moves it).
- `action`: a diplomatic or political move (`actions.cpp`,
  [06](06-geopolitics-ai.md#player-actions)).
