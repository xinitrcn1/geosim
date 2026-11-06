# 07 — Map & filters

The first world map is built: a raster-based political "mill map" over a
Blue-Marble-like terrain, in an oblique Mercator that follows the camera
(sections *Projection*, *Data*, *Layers* below; code in `src/client/map_*`,
`shaders/world_map.frag`, pipeline `tools/pipeline/build_map.py`). The
vector pipeline described next is the longer-term plan (filters, picking,
labels); the raster asset already gives fills and borders.

## Projection

A **spherical oblique Mercator (Hotine)** whose central line is the great
circle through the camera's focus point, running east-west there
(`src/client/map_view.hpp`):

- The sphere is rotated so the focus `P` sits at (0, 0) of an oblique frame
  whose pole is the local north `N` (`E` = local east): `x' = X·P`,
  `y' = X·E`, `z' = X·N`; then ordinary Mercator in that frame,
  `u = atan2(y', x')`, `v = atanh(z')`, `screen = centre + scale·(u, −v)`.
- The scale factor is 1 on the central line and `cosh v` away from it, so the
  distortion is smallest around the screen centre whatever the focus — the
  poles and Antarctica look as undistorted as the equator when centred.
  North is up at the centre. Zoomed out it degrades gracefully into a full
  (oblique) Mercator that repeats east-west.
- **Panning** moves the focus (the new focus is what lay at `centre − drag`),
  so the projection is recomputed every frame around what the user looks at.
  **Zoom** scales it about the pointer (the point under the cursor stays put;
  a few fixed-point pan iterations, because re-centring is not a pure
  scaling). Limits: the whole oblique-longitude circle fits the width at the
  minimum; an ID-raster cell is ~12 px at the maximum.
- The fragment shader runs the inverse per pixel (screen → `u, v` → unit
  vector → lon/lat), so there is no mesh to re-project.

Input: drag with the left button (started outside the panels), wheel zooms at
the pointer (when not over a panel), arrow keys pan, Page Up / Page Down zoom.
`--map-view LON,LAT,SPAN` sets the initial focus and the span of the window
width in degrees; the default is the first `--nation`'s area-weighted
centroid at ~75°.

## Data

`assets/map/world.gsmap` (3.5 MB, tracked) is built by
`python3 tools/pipeline/build_map.py` (needs Pillow; ~40 s, ~3 GB RAM peak)
from inputs that `python3 tools/pipeline/fetch.py --map-only` downloads into
`data/raw/map/` (~190 MB, not committed):

| Input | Use | Size |
|---|---|---|
| geoBoundaries gbOpen ADM1 (`data/raw/geoboundaries/`, already used by `build_world.py`) | region polygons, matched to `assets/scenario/world2020/regions.csv` with the same rules (jurisdictions, claims; e.g. Hong Kong out of China's file) | 132 MB |
| Natural Earth 10m admin-0 | the 21 territories without ADM1 data (Hong Kong, Macao, Puerto Rico, Guam, Faroe, Jersey + Guernsey for CHI, ...), painted over the sovereign's duplicate | 13 MB |
| Natural Earth 50m lakes | cut out of the regions (drawn as water) | 0.9 MB |
| NOAA ETOPO1 ice surface, 5′ (ERDDAP `etopo180` subsample) | land relief and bathymetry; land outside every polygon | 49 MB |
| Köppen-Geiger 1991–2020, 0.1° (Beck et al. 2023) | climate class → land colour | 131 MB zip (one 0.1° GeoTIFF read) |

Layers (equirectangular, cell-centred, zlib streams; format in the script's
docstring):

- `IDS0` — uint16 unit raster, 8192 × 4096 (~4.9 km at the equator):
  0 = water, 1 + row of `adm2.csv` = unit (a county, see [States, clusters and
  counties](#states-clusters-and-counties)), 0xFFFF = land of no region.
  Polygons are painted with holes (masks, so enclaves survive) and wrapped at
  the antimeridian (Fiji). Regions smaller than a cell get a dot at their
  centroid (87). Coastal land outside the simplified polygons (ETOPO above
  sea level) joins the neighbouring region (5 dilation passes); the rest is
  0xFFFF (Antarctica, Svalbard, islands missing from the polygons).
- `ELEV` — uint8 relief code, 4320 × 2160 (5′): sea 0..127 with depth
  `11000 m·((127−c)/127)²` (shelves get most codes), land 128..255 with height
  `8800 m·((c−128)/127)²`; row "sub" filter before zlib.
- `CLIM` — uint8 Köppen class 1..30, resampled to 5′ and spread 10 cells
  into the sea so coastal texels always have a climate.
- Tables: per nation ISO3, fill colour and label point (area-weighted
  circular mean of its regions); per region its `regions.csv` id, nation and
  centroid. **Nation colours** are a greedy colouring of the land-adjacency
  graph (Welsh–Powell order, least-used colour first) over 8 pastels, so
  neighbours never share a colour and colours are stable for the geometry.

At start the client decodes the asset on a worker thread (`map_loader`,
~1 s; `map_data.cpp` is built with -O2) and **colours the terrain on the
CPU**: Köppen class → a Blue-Marble-like palette (sand for BWh — the Sahara
and Arabia —, khaki steppes, olive savanna and Mediterranean scrub, dark green
rainforest and boreal forest, brown tundra, white EF ice caps — Greenland,
Antarctica), blended towards bare rock above ~2200 m and to snow above a
snowline falling from ~5000 m at 28° to ~1600 m at 70° (+600 m in deserts,
+300 m on tundra, so the Himalaya and Karakoram are white and the Tibetan
plateau mostly brown). The alpha channel keeps the relief code. Mips: box
filter for the terrain, point sampling for the IDs.

## States, clusters and counties

Three levels below the nation, all in `assets/scenario/world2020` (built by
`build_world.py` + `tools/pipeline/adm2.py`):

- **Counties (units)** — `adm2.csv`: the geoBoundaries gbOpen ADM2 units
  (49,308 counties, districts, prefectures in 180 nations), plus, for a state
  without ADM2 data, the state itself as one unit (49,570 in all, within the
  16-bit IDs). Only drawn: the map's polygons and raster are units.
- **Clusters (the simulation's regions)** — `regions.csv`: counties grouped
  inside their state by population. A state gets round(population /
  threshold) clusters, at least one and at most its county count; the
  threshold is 3 M people, raised only if the totals outgrow the simulation
  (≤ 5000 regions — the region container holds 8192 — and ≤ 200,000 pops, 4
  per identity group per region, of a 250,000 container): 4,715 clusters —
  Texas 10, California 13, a Saharan wilaya or a Slovene region 1, China 470,
  India 443. Clustering is a population-balanced recursive bisection of the
  counties' centroids along the wider axis, then a contiguity repair (a
  detached piece joins the cluster it borders most). Every simulated
  quantity (population, epidemics, unrest, output...) lives on clusters.
- **States** — `states.csv`: the ADM1 units, the legal level: one governor per
  state (shared by its clusters), district seats apportioned per state, and
  the units a government may redraw (`redraw_state`, docs/05).

Population comes from **GHS-POP R2023A** (2020, 30″; JRC, CC BY 4.0), summed to
a 2.5′ grid and over each county's cells (counties smaller than a cell take
their centroid cell, scaled by area; 7.84 bn on the grid). A county belongs to
the state it overlaps most; counties a sovereign's file shares with a
dependent or held territory (Taiwan's in China's) are left to it. Urban
share: the population in 30″ cells of ≥ 1500 people/km² (the Degree of
Urbanisation's urban centres), rescaled to the national urbanisation rate.

**Drawing.** The map file maps units → clusters (`UNIT`) and lists the
states with clusters → states (`STAT`); the client expands the per-region
table to a `uvec4` per unit (nation key, fill, state key, cluster key), so the
map pass draws the hierarchy from the resolved unit per pixel: nations thick,
states 1 px at 60 %, clusters 1 px at 32 % (fading out below ~0.1 px/km),
counties as hairlines only once zoomed in to about a state (≥ 0.25 px/km).
The raster mode (zoomed out, `--vector-map 0`) draws states and clusters.

**Levels of detail.** Two vector levels: `POLY` at 0.01° (fine) and `POL1`
at 0.06° (coarse), both simplified per nation on shared arcs (crack-free).
Zoomed out (< 0.25 px/km) the ID pass draws the coarse level of the whole
world; zoomed in, the fine level of the **visible states only**: units are
stored grouped by state, so each state is one contiguous index range with a
bounding cap (centre, angular radius) culled against the view (focus angle −
radius < half-diagonal / scale), and neighbouring visible states merge into
one draw.

**Electoral map.** The Politics inset is a county-level map: counties coloured
by their cluster's result, faint lines between counties and white lines between
states only (clusters are never outlined). Clicking a county zooms the inset
into its state — the same Mercator projection and inset packing as for a nation
(islands and exclaves as insets) — and a link above the map goes back.

## Vector regions

Regions are drawn from polygons, so zooming reaches micro-states (Andorra's
parishes, Monaco, San Marino, Caribbean and Pacific islands) at ~30 px per km
(the zoom limit is 16× deeper than with the raster). The region-ID raster is
no longer kept on the CPU: the client point-samples the asset's 8192-wide
raster down to 2048 cells at load and uploads only that (with mips) for the
resolve pass's fallbacks (land of no region, wide cracks) and the raster mode
(devices without logic ops, `--vector-map 0`: blocky, picking from a 4 MB CPU
copy kept only then, no electoral map). Memory: CPU −64 MB resident (and −80 MB
at load), GPU raster −80 MB (85 → 5.3 MB); the vector vertices add 4.4 MB on
the CPU for the landmass groups. The asset keeps the full raster (0.5 MB
compressed): `build_region_geo.py` computes the regional geography from it.

- **Pipeline** (`build_map.py`, `polygons.py`): the same ADM1 / Natural Earth
  polygons the raster is painted from, per nation cut into **shared arcs**
  (TopoJSON-style: rings split where the set of rings using a vertex
  changes), each arc simplified once by Douglas–Peucker (tolerance 0.01°,
  ~1.1 km; `--tolerance`) and the rings reassembled, so borders inside a nation
  stay identical on both sides. Regions whose rings all collapse (micro-states)
  keep their largest ring at a 20× finer tolerance; the sovereign duplicates
  of territories (China's Hong Kong SAR, the US Puerto Rico...) are dropped.
  Lakes (Natural Earth 50m) follow. 3.67 M source vertices → 555 k (3237
  regions) + 12 k lake vertices; the `POLY` section is 2.9 MB compressed
  (1e-5° delta-coded int32 rings).
- **ID pass** (`map_ids.vert/.frag`): no triangulation. Every ring is a
  triangle fan, vertex-pulled from storage buffers; each vertex also fetches
  its ring's first vertex, both are projected with the oblique Mercator, and
  the oblique longitude is unwrapped next to the first vertex's, so rings
  crossing the projection's seam stay whole; a second instance draws all rings
  one turn (2π) to the other side. Fragments **XOR** the ring's code (region
  id + a 15-bit checksum; lakes set bit 31) into a screen-size R32_UINT image:
  even-odd filling, so holes and enclaves come out right. ~1.6 M indices per
  instance.
- **Resolve pass** (`map_resolve.frag`): one region per pixel. The lookup is
  displaced by a deterministic 3-octave value-noise warp in world space (up to
  0.6 × the tolerance on screen, wavelength ~2.5 tolerances), so the simplified
  borders gain a natural roughness when zoomed in; fill, borders and picking
  all read this image, so they stay aligned. A pixel whose code fails the
  checksum (two overlapping polygons from different sources) or is empty
  (a crack between nations' polygons) takes the nearest region within 4 px when
  there is land on both sides; otherwise the relief decides: land → the
  raster's region (wide cracks) or land of no region (Antarctica), sea →
  water (the polygon coast wins over the 9 km relief).
- **Map pass**: the region is the resolved pixel; borders are drawn where a
  neighbour differs — 1 px for regions, the line width (UI scale) for
  nations, coasts and lake shores — constant on screen at every zoom.
- **Picking** reads the resolved region under the pointer back to the CPU
  (a 1-texel copy per frame, read two frames later).
- Cost: on llvmpipe (CPU) at 1200×700 the map runs at ~10 fps in vector mode
  (zoomed in) and ~7 fps at the world view, vs ~25–35 fps in raster mode; the
  fan overdraw is the cost. On a GPU both passes are a fraction of a frame.

## Terrain detail

The flat terrain colour gains texture when zoomed in, without new data at
higher resolution:

- An **atlas** of 8 tileable 256² grayscale layers — sand (warped dune
  ripples), grass (patchy fbm), forest (canopy clumps), tundra (mottled, lichen
  spots), rock (ridged fbm), snow (soft drifts), wetland (pools), water (soft
  ripples) — generated procedurally in the pipeline from periodic value-noise
  fbm (`polygons.terrain_atlas`; no images), centred on mid-grey, with box mips
  (`ATLS`, 0.36 MB).
- In the fragment shader the layers are weighted from the base terrain colour,
  relief and latitude (sandy and bright → sand; dark green → forest; high →
  rock; bright and unsaturated → snow; > 55° → tundra; plus a little grass and
  wetland; water by depth) and blended continuously, so climate boundaries
  stay smooth.
- They are tiled in world space (~14 km tiles, a second octave at ~3.3 km
  further in) with **hash-offset tile blending**: each pixel blends the four
  neighbouring tiles' samples, each with a random offset and 90° rotation, and
  the blend preserves variance (Heitz & Neyret 2018), hiding repetition.
- The detail modulates the colour by up to ±28 % on land (less on water) and
  fades in once a terrain texel spans more than ~1.5 px, so the zoomed-out look
  is unchanged.

## Layers

Drawn by `map_renderer` as one fullscreen triangle at the start of the scene
pass, so the frosted panels blur it (docs/08 *Rendering*); until the asset
is loaded the procedural placeholder material is drawn instead.

1. **Terrain** — water (region ID 0): ocean colour from the relief code, a
   ramp from a light shelf blue (0–120 m) through slope blues to a deep navy
   (> 6 km), so continental shelves and trenches are visible. Land: the
   terrain colour. Flat colour: no lighting, no hillshade, no water effects.
2. **Political fill** (default filter 🗺): each region takes its
   controlling nation's colour as a *tinted relief* — the nation's hue at the
   terrain's lightness (deserts lighter, forests darker), 72 % strength,
   fading out on snow and ice. The other filter buttons show the terrain and
   borders only until their scalar maps exist.
3. **Borders**, from the ID raster in the fragment shader: at the mip level
   where a cell is about one pixel, the region is the ID with the largest
   bilinear weight among the four nearest cells, and the distance in pixels to
   the contour where that weight crosses ½ (`|S − ½| / |∇S|`, the gradient
   taken through the screen derivatives of the texture coordinate) gives
   smooth, constant-width lines at any zoom. **Thin** (1 px × UI scale, 55 %
   black, fainter per mip level when zoomed out) where the region changes;
   **thick** (2 px, 90 % black) where the *nation key* changes — national
   borders and, because water and unclaimed land have their own keys,
   coastlines and lake shores.

**Control drives the fill and the thick borders** (`map_layer`): a region
table (per raster ID: nation key + RGBA fill) is rewritten into a per-frame
storage buffer. At scenario load each raster region is bound to the nearest
scenario region of the same nation (all 3243 in world2020; single-region
nations of the 2020 / 2008 slices take all their raster regions); nations a
scenario does not simulate keep their world2020 owner and colour. The owners
(`region.owner`) are re-read every 250 ms; when a region changes hands it
takes its new owner's colour and the national border moves with it. Before
the scenario is loaded the asset's own owners are shown.

4. **Clouds** (climate view only; **C** toggles them there, `--clouds 0`): flat,
   translucent (50 %) cover drawn over the political map. A 3D value-noise fbm
   on the unit sphere (no seam, no pole pinch) is advected by a climatological
   zonal wind — easterly trades peaking at 13°, westerlies at 47°, polar
   easterlies — using a two-phase flow map (1.5-day cycles cross-faded, so the
   latitudinal shear never smears the noise into streaks). It is thresholded
   by a cover climatology: a base of 0.36, the ITCZ band (centred at
   4° + 7°·season, i.e. ~11° N in northern summer, ~3° S in winter), storm
   tracks around 52° ± 4° (poleward in summer), polar stratus, and a clear
   subtropical-high band at 24°; cover drops by up to 75 % over land whose
   colour is sandy and bright (deserts, steppes), so the Sahara and Arabia stay
   clear. The cloud clock follows the simulation (frozen when paused) but runs
   at most 2 days per real second, so clouds stay readable at high speeds.
5. **Tropical cyclones** (hazard UBO, up to 64 "mass points": lon, lat,
   maximum wind, gale radius): a logarithmic spiral of two bands
   (`θ + 3.2·ln r`, anticlockwise in the north and clockwise in the south,
   turning with the cloud clock) out to 2.4 gale radii, a central dense
   overcast inside ~0.5 radii and, from 33 m/s (hurricane / typhoon), a clear
   eye of ~10–25 km; opacity grows with the wind. Denser than the background
   cover.
6. **Earthquakes** (up to 32: epicentre, Mw, age): a news-style ring of four
   red circles expanding from the epicentre (radius
   `40 km · 2^(Mw − 5.5)`, at least 14 + 7·(Mw − 5) px), with a red dot,
   fading out between 3 and 7 sim days after the event.

## Views

The top-bar filter buttons pick the view (`map_filter`: 🗺 political, 🌦 climate,
📈 economy, 🚢 trade, 🛢 energy, 🦠 epidemic, 🔥 unrest; `--view NAME` at start).
Every view keeps the terrain and the borders; `map_layer` rewrites the region
table's fill colour per view (blended as tinted relief, as the political fill).
Views other than political read a `region_snapshot`, copied at the sim's sync
point once a second (`sim_runner::request_regions`, `sim_hazards.cpp`): per sim
region the water balance, storm, unrest and epidemic fields, per nation the
macro, trade and energy values; faults once. Where the sim keeps a value per
nation only, every region of the nation shows it.

- **Political** — the controlling nation's colour (above). No clouds.
- **Climate** — clouds and cyclone swirls appear only here (quake rings in every
  view). Regions are tinted by hazard, saturated when it is happening and light
  when it is potential; "now" wins over "potential", then earthquake > cyclone >
  flood > drought:

  | Hazard | Colour | Now | Potential |
  |---|---|---|---|
  | Earthquake | pink | an earthquake of the last 7 days within its felt radius `10^((1.5 Mw + 2 − 6)/3)` km | within `150 + 60·(Mw − 6)` km of a fault past 80 % of its mean recurrence |
  | Cyclone | grey | under a live storm's gale radius, or hit by it (`storm_seen` = its serial, `storm_wind` ≥ 17 m/s) | on its projected path (heading, speed) over the next 2 days, or under a warning record |
  | Flood | blue | `flood_cooldown` > 0, or a flood record of the last 30 days | flood plain with soil ≥ 80 % of capacity in the wet season, or a rain anomaly > 1.5 sd |
  | Drought | yellow | crop loss > 0 or a drought record | SPEI < −0.8 |

- **Economy** — national real GDP (run mean vs 2019) on a red / green scale.
  Tooltip headline: GDP vs 2019; expanded: output gap, unemployment, inflation,
  policy rate, 10-year yield, budget balance change, credit growth, real wages,
  house and equity prices, Gini, regional population.
- **Trade** — change in exports (% of GDP) since the start, red / green.
  Expanded: imports, tariff, sanction links, seaborne share, freight cost,
  logistics delay, top five export destinations (shares of bilateral exports).
- **Energy** — oil production relative to demand: teal for net exporters,
  orange for importers (darker = more dependent). Expanded: production, demand,
  fossil share, carbon price, war, sanctions.
- **Epidemic** — new infections per 100k per day, purple.
- **Unrest** — dark red where a region has an active insurgency, light red
  where it is potential (unrest > 1.5, protest > 0.2 or grievance in the top
  10 % of regions). Expanded: insurgency, unrest, protest, grievance, the
  nation's rebel movement and wars.

**Picking**: the screen point goes through the inverse oblique Mercator to
lon/lat, then to the full-resolution region-ID raster, which `map_layer` keeps
on the CPU (64 MB). The tooltip names the region and its controller.

## Electoral map

The Politics window (🗳 in the top bar; `--politics ISO3`) shows one nation as an
election map in plain Mercator (north up, not the oblique view projection),
like a US election map with its Alaska and Hawaii insets, for every nation. The nation defaults to the
player's first; ◀ ▶ step through the nations, and clicking a region on the
main map while the tab is open shows its nation.

- **Landmass groups** (built on first view, cached per nation): each of the
  nation's regions is split into connected components on a coarse grid of
  the region-ID raster (every 4th cell, ~20 km; x wraps around the globe),
  longitudes are unwrapped around the largest component, and components
  whose bounding boxes are closer than ~250 km are merged (union-find), so
  archipelagos stay together (Indonesia is one group; Corsica joins France;
  Bornholm joins Denmark). Groups smaller than 0.03 % of the nation's land are
  dropped (slivers left by a territory's own polygon); at most eight insets.
  Each group has its bounding rectangle in plain Mercator.
- **Insets** (`inset_pack.hpp`): the largest group (mainland US) keeps its
  place and scale. Every other group is rescaled so its long side is 12–32 %
  of the main group's (Alaska shrinks, Hawaii grows; factor 0.15–3), then
  placed, largest first, at the candidate position that minimises the area of
  the whole layout (with a mild pull towards its true direction) without
  touching the main group's land (an occupancy grid over its rectangle) or
  another inset: usually in the main rectangle's empty corners (Hawaii lower
  left) or beside it (Alaska). Each inset gets a thin grey frame. For the USA
  the packed layout covers about a quarter of the raw bounding box
  (`test_map_view`).
- **Drawing** (vector, like the main map): the nation's ring fans are drawn on
  the GPU once per inset with that inset's plain-Mercator transform (source rect
  → destination rect in the window), scissored to it, XORed into an electoral ID
  image (`map_elect.vert`); a `material::electoral` quad in the window colours
  it (`electoral.frag`): the current mode's colour per region (a per-region
  colour buffer), white 1-px lines between regions, dark grey coasts, a thin
  grey frame per inset. Crisp at any window size; the UI hands it the rect it
  lays out (half the Politics window), and the region under the pointer comes
  back through the GPU readback (tooltips). The landmass groups come from the
  polygons too: each ring is a piece with its box and shoelace area, grouped
  when closer than ~250 km, specks below 0.03 % dropped, the main group's land
  rasterised by scanline into the packer's occupancy grid.
- **Democracies** (electoral index ≥ 0.3, competitive, ≥ 2 parties): per
  region, the vote shares of the nation's parties from pop-level support:
  Σ `party_support[j]` × pop size × turnout (by pop kind: 0.5, 0.6, 0.75, 0.7,
  as `vote_intention`), parties in `local_index` order. Modes: **Winner**
  (the leading party's family colour, lighter for narrow margins), one button
  **per party** (its share on a single-hue scale), **Population** (electorate
  per raster cell, log scale), **Turnout**. Party colours follow the family
  (social democrats red, conservatives blue, liberals yellow, Christian
  democrats orange, greens green, radical right navy, radical left dark red,
  communists crimson, regionalists purple, agrarians lime, challengers
  magenta, others grey), except the US convention (Democrats blue,
  Republicans red; the generic L / C / R parties red / yellow / blue). The strip under the map: national vote intention, seats,
  the governing coalition, the swing in vote intention since the start of the
  run, and the next election date.
- **Non-democracies**: no candidates. Modes: **Regime support** (labelled as
  not an election: `0.5·(1 − grievance) + 0.5·trust − 0.15·unrest − insurgency`,
  red → green), **Unrest**, **Insurgency**, **Population**; the strip shows
  legitimacy, military loyalty, repression and the liberal index.
- Tiles have tooltips like the main map: the region's shares (or regime
  inputs), expanded with the national result, seats, coalition and next
  election.

Swing is national (vote intention now vs at the start): the sim keeps no
per-region result of the last election.

## Hazards

`hazard_feed` requests a `hazard_snapshot` every 250 ms
(`sim_runner::request_hazards`, `sim_hazards.cpp`; storms and disasters are
erasable objects, so they are copied at the sim's sync point, never read
racily): every live `storm` (position, wind, gale radius) and every
`disaster` record; earthquakes of the last 7 days feed the rings. Each
disaster record becomes an alert (docs/08 *Layout*). `--hazard-demo 1`
draws two fixed cyclones and a Mw 9 earthquake for visual checks.

## Regional geography

`python3 tools/pipeline/build_region_geo.py` (15 s, after `build_map.py`;
WorldClim from `fetch.py --map-only`) writes per-region zonal statistics of the
map raster for the disaster model: `assets/scenario/world2020/regions_geo.csv`
(one row per `regions.csv` region) and, for the slices, `2020/` and
`2008/regions_geo.csv` (one row per nation, area-weighted over its world2020
regions; ROW = every nation outside the slice). On the 5′ grid, cells weighted
by cos(latitude):

| Column | Definition |
|---|---|
| `area_km2`, `lat`, `lon` | land area (85.86 km² · cos φ per cell; includes the coastal gap fill, so small islands read high) and spherical land centroid; empty for ROW |
| `elevation_m` | mean land elevation (ETOPO1, negative as 0) |
| `lecz_share` | low-elevation coastal zone (McGranahan et al. 2007): land < 10 m within 2 cells (~18 km) of water connected to the open ocean (lakes and the Caspian excluded) |
| `coastal` | region next to the ocean (0/1; slices: share of the nation's regions) |
| `floodplain_share` | proxy: land < 100 m whose relief (max − min) in a 5 × 5-cell (~45 km) window is < 50 m — low, flat deltas and river and coastal plains |
| `precip_mm`, `precip_peak_month`, `precip_seasonality` | WorldClim 2.1 (1970–2000, 10′): annual total, wettest month, CV of the 12 monthly means |
| `koppen` | area-dominant Köppen-Geiger main class A–E |

Regions without a cell (156 tiny islands and enclaves) take the cell at their
centroid and their nation's mean. WorldClim's terms forbid redistributing its
data: only these regional aggregates ship.

Not done yet: hover / picking (the CPU keeps no ID raster), labels, the
scalar filters and legend below, hatching for no-data, vector borders for
very close zoom (beyond ~12 px per cell the border staircase of the
4.9 km raster becomes visible, smoothed by the bilinear contour).

## Geometry pipeline (original plan, superseded by *Vector regions*)

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
