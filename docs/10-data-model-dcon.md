# 10 — Data model (DCON)

[DCON](https://github.com/schombert/DataContainer) generates a C++ header
from a schema file. The header contains struct-of-arrays storage for
**objects** (entities with properties) and **relationships** (typed links
between objects, with automatic index maintenance). It also provides `ve`
helpers for SIMD iteration and tagged serialisation.

The real schema lives at `schema/geosim_dcon.txt`. The build
(`cmake/Dcon.cmake`, enabled with `GEOSIM_USE_DCON=ON`) turns it into
`geosim_dcon.hpp` + `geosim_dcon_ids.hpp`. This doc is the design reference;
the syntax follows DataContainer's `file_format_documentation.md`. Note that
the format has **no comment syntax**, so the design notes live here.

## Objects

| Object | Approx. count | Key properties |
|--------|---------------|----------------|
| `nation` | ~200 | name key, currency, regime index vector, legitimacy, capture index, policy vectors, SFC sector balances (gov, CB, banks) |
| `region` | ~4,000 | name key, area, centroid, terrain, infrastructure, housing price, epidemic compartments, PSI, unrest stage, LOD level, shard id |
| `pop` | ~120,000 | size, class tier, skill, age band, income dist (μ, σ, α), wealth portfolio, employment vector, values (3 axes), identity shares, party affinity, expectations |
| `sector` | ~40 | ISIC code, input coefficients `a[commodity]`, CES params |
| `commodity` | ~60 (+ spare slots for tech tree) | unit, tradable flag, Armington σ, world price |
| `production_unit` | ~160,000 (sector × region, sparse) | capital vintages, capacity, output, inventory, markup, employment by skill, TFP |
| `market` | ~200 national + world | prices[commodity], supply/demand last period |
| `party` | ~1,500 | position in value space, issue emphases, seats |
| `agent` | ~5,000 | kind (exec / cabinet / party / CB / governor / court / lobby / military), utility weights, ρ_base, horizon, current intent, override state |
| `policy` | ~200 kinds | parameter schema; per-nation values stored on `nation` as arrays |
| `treaty` | dynamic | kind, terms vector, start tick, compliance |
| `flow_tag` | dynamic (pooled) | cause kind (policy / decision / event / endogenous), source id, tick |
| `event` | dynamic | kind, location, magnitude, tick |

## Relationships

| Relationship | Links | Notes |
|--------------|-------|-------|
| `region_ownership` | region (unique) → nation (many) | sovereign control |
| `region_control` | region × nation, share | occupation/front lines in conflict |
| `region_adjacency` | region ↔ region, border length | from map pipeline |
| `pop_location` | pop (unique) → region (many) | |
| `unit_location` | production_unit (unique) → region (many); → sector (many) | |
| `nation_relation` | nation × nation (ordered pair) | trust, dependence, threat, affinity, bilateral trade flows, tariffs |
| `party_in_nation` | party (unique) → nation (many) | |
| `agent_controls` | agent (unique) → nation / region / party | the agent's institutional seat |
| `treaty_member` | treaty × nation | role, obligations |
| `currency_area` | nation (unique) → nation (many, anchor) | eurozone, pegs |

## Conventions

- **Ownership:** the generated `dcon::data_container` is the member
  `game_state::world` ([09](09-engine-architecture.md#game-state)). All entity
  data is accessed as `state.world.<object>_get_<property>(id)`. Nothing else
  holds entity data.
- **Iteration:** `world.execute_serial_over_<object>` /
  `world.execute_parallel_over_<object>` (ve SIMD + oneTBB) for bulk work, and
  `world.for_each_<object>` / relationship `for_each` helpers for scalar loops.

- **Arrays inside properties** (`prices[commodity]`, `employment[skill]`) use
  DCON array properties with a fixed maximum size, plus a spare region for
  tech-tree goods so adding goods does not change the layout.
- **Types:** `float` for most quantities. `double` for **ledger balances and
  national aggregates**, so long-campaign accounting does not drift.
  `uint8_t` enums for categorical fields. Ids are DCON's typed ids
  (`dcon::region_id`, …), never raw ints in public APIs.
- **Tags for serialisation:** `save` (persisted), `scenario` (loaded from
  scenario data), `derived` (recomputed after load, never saved).
- **Snapshot fields** are copied to the UI each tick, not shared.
- **Generated code** goes to `${CMAKE_BINARY_DIR}/generated/geosim_dcon.hpp`
  and is never committed. The schema is copied into the build tree before
  generation, because the generator writes its output next to its input.
- **Storage types:** `contiguous` for fixed-population objects (nation,
  region, sector, commodity), `erasable` for objects created and destroyed
  during play (pop, through strata split/merge; treaty; flow_tag; event).

## Schema sketch

```
object{
	name{region}
	storage_type{contiguous}
	size{8192}
	tag{scenario}
	property{ name{area_km2}     type{float} }
	property{ name{psi}          type{float}  tag{derived} }
	property{ name{shard}        type{uint8_t} }
	property{ name{housing_price} type{float} tag{save} }
}

object{
	name{pop}
	storage_type{erasable}
	size{262144}
	property{ name{size}        type{float} }
	property{ name{inc_mu}      type{float} }
	property{ name{inc_sigma}   type{float} }
	property{ name{inc_alpha}   type{float} }
	property{ name{values}      type{array{uint8_t}{float}} }
}

relationship{
	name{pop_location}
	link{ object{pop}    name{pop}    type{unique} }
	link{ object{region} name{region} type{many} index_storage{list} }
	primary_key{pop}
}
```
