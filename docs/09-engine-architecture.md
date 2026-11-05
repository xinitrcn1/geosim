# 09 — Engine architecture

## Stack

| Concern | Choice |
|---------|--------|
| Language | C++20, GCC or Clang (MSVC unsupported) |
| Build / tests | CMake ≥ 3.25 + CTest |
| Window, input, audio, timing | SDL3 |
| Graphics | Vulkan 1.3 (dynamic rendering, no render passes), shaders in GLSL → SPIR-V via `glslc` at build time |
| Text | FreeType (rasterise) + HarfBuzz (shape) |
| Simulation data | DCON (`schombert/DataContainer`): generated SoA containers |
| Parallelism | DCON's `execute_serial` / `execute_parallel` (ve SIMD + **oneTBB** workers). No separate job system. `std::jthread` only for the long-lived sim / render / fork threads |

## Modules

Each module is a static library with a one-way dependency order. A module
may only depend on modules listed above it.

```
core       allocators, logging, assert, math, Philox RNG, fixed containers, time/calendar
sim        DCON generated data + systems + cadence scheduler + ledger    (deps: core)  ← no rendering deps
ai         agents, utility evaluation, planners                           (deps: core, sim)
data       scenario/map import, validation                                (deps: core, sim)
save       DCON serialisation + versioned migrations                      (deps: core, sim)
platform   SDL3 window, input, Vulkan surface, file dialogs               (deps: core)
render     Vulkan device, swapchain, map renderer, 2D batcher            (deps: core, platform)
text       FreeType atlas, HarfBuzz shaping, font fallback chain          (deps: core, render)
ui         immediate-mode widgets, layout, panels                         (deps: core, render, text)
app        game client: wires sim ↔ snapshot ↔ ui, command queue          (deps: all)
headless   CLI runner for CI, back-tests, benchmarks                      (deps: core, sim, ai, data, save)
```

`sim` + `ai` must build and run with **no** graphics dependencies, so that
CI, back-tests ([11](11-data-calibration.md)) and counterfactual forks can
run headless.

**Implemented today** (`src/CMakeLists.txt`): `geosim_core` (calendar, CSV,
defs, scheduler, RNG, diagnostics) and `geosim_sim`, which holds the DCON
data, every system in `src/sim/systems/`, the ledger, scenario loading,
saves, forks and back-tests. The AI is not a separate library yet: it lives
in `src/sim/systems/ai.cpp`, with agent-specific choices in the systems that
own the lever (credit, bonds, firms, labour, fiscal, institutions,
geopolitics, treaties). Executables: `geosim_headless` and, with
`GEOSIM_BUILD_CLIENT=ON`, the `geosim` client. The `data`, `save`, `ai`,
`platform`, `render`, `text` and `ui` split above is the target.

## Threading

```
 ┌──────────── sim thread ────────────┐        ┌────── main / render thread ──────┐
 │ apply queued commands (tick start) │        │ SDL events → UI                   │
 │ run scheduled systems (DCON + TBB) │        │ UI reads latest snapshot          │
 │ commit; publish snapshot ──────────┼──────► │ map mode scalar → GPU upload      │
 └────────────────────────────────────┘        │ draw                              │
            ▲                                  └──────────────┬────────────────────┘
            └──────────── command queue (MPSC, lock-free) ◄───┘
 ┌──── fork worker(s) ────┐
 │ copy of state, reduced │──► indicator series → UI
 └────────────────────────┘
```

- **Fork workers** are planned; today `run_fork` copies the state and runs
  it at full detail in the calling thread
  ([02](02-simulation-core.md#counterfactual-forks)).
- The **sim thread** owns the `game_state` (and so `game_state::world`) exclusively.
- **Snapshot:** after each tick, the sim writes the UI-relevant fields
  (per-region indicator vectors, selected entity details, alerts, journal
  appends) into one of a triple buffer, then flips an atomic index. The UI
  never blocks the sim.
- **Commands:** UI/player actions → MPSC queue → applied at the next tick
  start, in stable order (keeps runs deterministic, [02](02-simulation-core.md#determinism)).
- **Inside systems:** all data-parallel work goes through DCON's own
  executors: `world.execute_serial_over_<object>(fn)` and
  `world.execute_parallel_over_<object>(fn)`. `fn` receives SIMD-width
  index groups (`ve` vectors) and runs on oneTBB's worker pool (static
  partitioner, fixed chunks). The engine has no job system of its own.
  Reductions follow the determinism rules in
  [02](02-simulation-core.md#determinism).
- **Thread cap:** `sim::set_thread_limit` (oneTBB `global_control`) caps the
  workers at 2 by default (the client and `geosim_headless`, `--threads N`).
  Parallel loops write only their own entity's fields and reduce SIMD lanes
  into doubles serially, so output is bit-identical at any thread count.
- **World-scale hot spots** (`--profile 1` prints wall time per system):
  per-market Armington rivals once a day (`update_trade_rivals`, parallel
  over markets, vectorised over suppliers; they were recomputed in every
  pair-access call, O(N³) a day), one sorted income distribution per nation
  for Gini, Palma and poverty (nations in parallel), migration lookups hoisted
  out of the pair loops, and the epidemic pre-pass in parallel over regions.
  world2020 (220 nations, 3,244 regions) went from 58 to ~16 ms per simulated
  day on two cores (RelWithDebInfo).
- **Build:** `cmake/Dcon.cmake` links oneTBB (a system package if found,
  otherwise built from source) and enables AVX2 + FMA (`-ffp-contract=on`), which `ve` needs to pick its
  SIMD backend.
- **Frame pacing:** the render loop is vsync'd. The sim runs as fast as the
  speed setting allows, independent of frame rate.

## Game state

**Every game variable lives on `game_state`** (`src/sim/game_state.hpp`):

```cpp
struct game_state {
    dcon::data_container world;   // all entity data incl. ledger balances, flows, journal (SoA)
    sim::params   params;         // global model parameters (assets/defs/parameters.txt)
    int32_t       current_tick;   // days since 2020-01-01
    uint64_t      rng_seed;       // Philox key
    derived_indices derived;      // rebuilt after load, never saved
};
```

- Per-entity data goes in the DCON schema, so it is a property of `world`.
  Global scalars and non-entity structures are members of `game_state`.
- No globals, singletons or function-local statics may hold game data.
  Systems are free functions `void update_x(game_state&)`, and read-only
  queries take `game_state const&`.
- This makes saves (`world` serialisation + the member fields), forks (copy
  the `game_state`) and tests (build a small `game_state`) straightforward,
  and keeps runs deterministic.
- UI and render code never hold a `game_state`. They see only the snapshot.

## Error handling

- The engine builds with **`-fno-exceptions -fno-rtti`**, applied to every
  geosim target by `geosim_target_defaults()` in
  `cmake/GeosimOptions.cmake`. No `throw`, `try`, `dynamic_cast` or `typeid`.
- **`core::diagnostics`** (`src/core/diagnostics.hpp`) is the error sink:
  loaders take a `diagnostics&`, record the **first** error with its
  `source:line`, and return a neutral value. Later errors are usually knock-on
  effects, so only the first is kept ("sticky first error"). Callers check
  `failed()` at stage boundaries and stop early. Forks report a failed state
  copy the same way (`fork_result::error`).
- **DCON** aborts when a fixed-size container is full: `DCON_USE_EXCEPTIONS`
  is never defined. Schema sizes ([10](10-data-model-dcon.md)) must therefore
  cover the full-world scenario. oneTBB's headers disable their exception
  paths when `__cpp_exceptions` is off.
- **`std::filesystem`** is used only through its `std::error_code` overloads
  (e.g. `exists(path, ec)`), which never throw.

## Rendering

- Vulkan 1.3 with dynamic rendering, one graphics queue, 2 frames in flight.
- **Passes:** map fill (region meshes, colour via storage buffer lookup) →
  borders (polyline quads, screen-space width) → overlays (arcs, symbols) →
  UI batch (rects + glyph quads, one atlas) → present.
- **Memory:** VMA (Vulkan Memory Allocator) is acceptable as a small dependency.
- **Map navigation:** pan/zoom with a 2D camera. LOD switches to simplified
  geometry at low zoom (two simplification levels exported by the pipeline).

## Text

- Font fallback chain: primary sans → Noto CJK/Arabic/Devanagari etc. as needed.
- HarfBuzz shapes runs (script + direction segmentation). FreeType
  rasterises glyphs on demand into a single-channel atlas texture (LRU eviction).
- Shaped-run cache keyed by (string hash, font, size), cleared per UI scale change.

## Saves

- **Format:** a header (version, scenario hash, tick, RNG seed) + DCON's
  serialisation of tagged objects/properties + ledger + journal + AI plans.
  Compressed with zstd.
- **Migrations:** per-version transform functions, so old saves load after
  schema changes (DCON's tagged serialisation tolerates added/removed properties).
- **Replay:** seed + command log reproduce a campaign deterministically
  (also used for bug reports).

## Testing strategy

| Level | What | Where |
|-------|------|-------|
| Unit | scheduler maths, RNG, distribution maths (Gini from lognormal-Pareto), matching functions, per-system tests (credit, finance, firms, institutions, internet, logistics, politics, society groups, …) | `tests/` (ctest) |
| Invariant | SFC zero-sum rows/columns, population conservation, determinism (run twice → identical hash) | `tests/` with small synthetic scenarios |
| Stylised facts | Okun, Beveridge, Phillips emergence on a synthetic economy | ctest label `slow` |
| Back-test | 2020–2025 against real data with tolerance bands | ctest label `backtest`, nightly |
| Perf | ticks/s on the full scenario | `headless --bench`, tracked over time |
