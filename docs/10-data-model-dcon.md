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

Every object and relationship in `schema/geosim_dcon.txt` today. Sizes are
the schema's fixed container sizes (DCON aborts if one fills up,
[09](09-engine-architecture.md#error-handling)).

| Object | Size | Purpose and key properties |
|--------|------|-----------------------------|
| `nation` | 256 | everything national: population, 2019 macro state, regime indices (electoral, liberal, capture, state capacity), legitimacy, PSI, demand and prices, ledger flows (`flows[kind]`), bilateral arrays indexed by partner (trust, threat, trade share, sanctions, alliance, war, FTA, trade distance), FX, spreads, credit, bonds, assets, fiscal, internet controls, Overton window |
| `region` | 4,096 | owner, location, area, urban share, capacity and output, epidemic compartments per pathogen × age group, hospital beds, grievance, unrest, protest participation, drive, sparks, fatigue, insurgency |
| `pop` | 4,096 | kind (low / mid / high skill, retired) × identity group per region: size, employment, wages, incomes by source, consumption, deposits, values, salience, party support, grievance, income σ, informality, network, tax factor, pop CPI |
| `sector` | 32 | role, labour share, oil and import intensity, final-demand shares, skill mix, NPI capacity loss, input-output coefficients, value-added share, critical-input flag, input inventory days, cyber exposure |
| `npi_indicator` | 32 | one OxCGRT indicator: role, maximum level, transmission effect |
| `pathogen` | 8 | SEIR parameters, severity, cross-immunity, vaccine timing and efficacy, emergent-template flag |
| `climate` | 1 | world CO2, temperature, damage, emissions; also the world remittance index |
| `commodity` | 8 | world commodity market (crude oil, gas, coal, grains, metals): price, stocks, supply, demand, elasticities, harvest shock, biofuel share, sector weights |
| `social_group` | 512 | identity group of a nation: population share, education, informality, wage gap, unemployment, wealth, exclusion, network, immigrant flag, grievance, mobilisation |
| `firm` | 1,024 | SME or large cohort of a nation × sector: capacity share, cash days, leverage, exit and entry, state ownership, board choices |
| `sea_node` | 64 | port basin hub of the shipping network |
| `chokepoint` | 32 | strait or canal: capacity, traffic, incident and drought hazards and state, queue, wait |
| `sea_lane` | 256 | lane between two sea nodes: nautical miles, sailing days, optional chokepoint |
| `shipping` | 1 | world container fleet: deployed, utilisation, spot and contract rates, order book |
| `agent` | 2,048 | kind (executive, central bank, board, union federation, military), ρ, utility weights and ideals, override state, last pragmatic / ideological / prior / chosen value per lever |
| `party` | 2,048 | key, position in value space, vote and seat share, government flag, family, Laver strategy and heading |
| `decision` | 65,536 | journal entry: agent, target, tick, lever (or journal code), before / after, pragmatic, ideological, prior, mode |
| `event` | 16,384 | scheduled scenario event: kind, target, tick, parameters, done flag |

**Planned objects** (not in the schema yet): `production_unit` (sector ×
region), `market`, `policy` (policy kinds), `treaty` (terms, start, compliance;
today alliances and FTAs are nation arrays), `flow_tag` (per-flow cause tags,
[02](02-simulation-core.md#accounting-core)).

## Relationships

| Relationship | Links | Purpose |
|--------------|-------|---------|
| `region_ownership` | region (unique) → nation (many) | sovereign control of a region |
| `pop_location` | pop (unique) → region (many) | where a pop lives |
| `region_adjacency` | region ↔ region, weight | migration graph |
| `region_claim` | region × nation, strength | territorial claims (war issue) |
| `executive_seat` | agent (unique) ↔ nation (unique) | the executive agent of a nation |
| `monetary_authority` | nation (unique) → agent (many) | central-bank agent of a nation (shared in currency unions) |
| `currency_peg` | nation (unique) → nation (many, anchor) | pegged currencies |
| `sovereignty` | nation (unique) → nation (many), autonomy | autonomous jurisdictions and their sovereign |
| `party_membership` | party (unique) → nation (many) | the nation's party system |
| `port_access` | nation × sea_node, share | where a nation's seaborne trade enters the network |
| `littoral` | chokepoint × nation | nations whose wars or unrest can close a passage |

**Planned relationships:** `region_control` (occupation shares on front
lines), `unit_location` (production units), `nation_relation` (today the
bilateral fields are partner-indexed arrays on `nation`), `treaty_member`,
`agent_controls` (seats beyond executives and central banks: governors,
parties, lobbies).

## Conventions

- **Array index types are `uint16_t`.** DCON converts an array property's index
  through the signed version of its index type (`dcon::get_index`), so a `uint8_t`
  index wraps past 127 and writes before the buffer. Arrays indexed by partner nation
  (up to 256 nations) or by flow kind x tag slot exceed that, so every array property
  uses `uint16_t` (limit 32767).

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
  region, sector, commodity, social_group, firm, shipping network),
  `erasable` for objects created or destroyed during play (pop, agent, party,
  decision, event; later treaty and flow_tag).
- **Tags:** today every object carries `tag{scenario}`; the `save` /
  `derived` split above is the target.

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
