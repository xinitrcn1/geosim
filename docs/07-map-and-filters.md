# 07 — Map & filters

## Geometry pipeline (offline)

1. **Source:** geoBoundaries (CC-BY, admin-1/admin-2) with GADM as a fallback
   where licensing allows. We pick the level per country to target ~3,000–5,000
   regions worldwide, balanced by population and area (e.g. admin-2 for large
   populous countries, admin-1 for small ones).
2. **Simplify** with topology preserved (Visvalingam–Whyatt on shared arcs),
   so neighbouring regions do not get gaps.
3. **Project:** equirectangular for the default flat map; Robinson/Equal
   Earth as options (the projection runs on the GPU in the vertex shader from
   lon/lat).
4. **Tessellate** into triangle meshes (constrained Delaunay / earcut) with a
   `region_id` vertex attribute. Shared borders are extracted as polylines
   for border rendering (national borders thicker than regional ones).
5. **Adjacency graph** (shared border length) is exported for the sim
   (migration, conflict fronts, opinion graph).
6. **Output:** a versioned binary `map.bin` (vertices, indices, border lines,
   adjacency, label anchor points), built by the data pipeline
   ([11](11-data-calibration.md)).

## Rendering map modes

- Each frame the GPU has a **storage buffer** `float value[region_count]`
  plus a colour-ramp texture (1D, 256 entries) and a uniform with range,
  scale type (linear/log/symlog) and the selected/hovered region ids.
- Switching filters = computing the scalar per region on the sim snapshot
  (CPU, SIMD) + one buffer upload. No re-meshing.
- Optional overlays: flow arcs (trade, migration, remittances) as instanced
  lines with width ∝ flow; point symbols for events (protests, outbreaks,
  battles).

## Filter catalogue

| Group | Filters |
|-------|---------|
| Economy | GDP per capita (PPP), real GDP growth, unemployment, LFP, inflation (per-stratum CPI selectable), median wage, sector share (pick sector), trade exposure, housing prices, informality |
| Inequality | Gini, Palma, top-10% income share, wealth top-10%, poverty headcount (int'l / national line), mobility index |
| Society | population, density, age structure (median age, dependency), fertility, life expectancy, infections / deaths per 100k, schooling, net migration, ethnicity/religion share (pick group), trust |
| Values | each Inglehart–Welzel axis, economic left/right, issue salience (pick issue), polarisation (within-region dispersion) |
| Politics | leading party, party share (pick), incumbent approval, legitimacy, PSI, unrest stage, regime indices, capture index |
| Fiscal / finance | regional budget balance, public debt (nation), sovereign spread, credit growth |
| Geopolitics | relations to selected nation (trust / dependence / threat), alliance blocs, sanction status, conflict control |
| Environment | emissions per capita, temperature anomaly, climate damage |

## Display modes (apply to every filter)

- **Level**: the current value.
- **Δ since date**: the change since a reference date. Default reference =
  the last decision in the journal. This is the main "see the effect" mode
  ([01](01-gameplay-loop.md#1-observe)).
- **Counterfactual Δ**: actual − fork ([01](01-gameplay-loop.md#52-counterfactual-fork-on-demand-background)).
- **Per capita / total**, where it makes sense.
- **Rank / percentile** across regions worldwide or within a nation.
- **Stratum lens**: restrict a pop-level filter to one class tier, age band
  or identity group (e.g. "unemployment among under-30 routine workers").

Snapshots for Δ: the sim keeps a ring of per-region indicator snapshots
(monthly for 10 years, yearly afterwards), and decision dates also take a
snapshot.

## Colour and legend

- Sequential scales: perceptually uniform (viridis/cividis family). Diverging
  scales for Δ and signed values (centre fixed at 0). Categorical palettes for
  party/bloc maps, with ≤ 10 categories and an "other" grey.
- A **colour-blind-safe** setting is the default (cividis + a blue/orange
  diverging scale).
- A legend is **always visible**: unit, scale type, range, data source/model
  name, and the reference date in Δ mode.
- No data / not applicable: a neutral hatched grey (a hatch pattern in the
  shader, so it is not confused with low values).
