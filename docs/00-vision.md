# 00 — Vision

## One-sentence pitch

A plain, data-dense world map where AI governments run the world and you can
take over any of them, then see in numbers and on the map who gained, who lost,
and why.

## Pillars

1. **Consequences you can see.** Every action must lead to effects the player
   can observe, trace and compare against a counterfactual. If an effect cannot
   be shown, it does not belong in the sim. ([01](01-gameplay-loop.md))
2. **No free lunch.** Budgets are conserved (stock-flow consistent). Strata have
   conflicting interests. Short-term and long-term goals pull against each
   other. There is no dominant strategy and no single score.
3. **Grounded models.** Every system uses a published model at a resolution
   we can afford to compute. We prefer models that produce realistic patterns
   (Phillips curve, Okun's law, Beveridge curve, demographic transition) over
   models that hard-code those patterns.
4. **Believable actors.** AI agents trade off ideology against pragmatism the way
   real governments do, under domestic constraints (two-level games). They are
   not optimisers of one national score.
5. **Long horizon.** The campaign spans generations. Structures change, not only
   numbers: new sectors, new strata, ageing societies, a changing climate.
6. **Bland interface.** Tables, legends, units, sources. No ornament.

## Player fantasy

The player is an *invisible hand* over the world's institutions, not a
monarch. Each turn of control is a choice:

- *Which* agent to act through (finance minister? central bank? opposition party?)
- *How hard* to push (advise / direct / force; see [01](01-gameplay-loop.md#3-act--overrides))
- *Who pays* (which strata, which regions, which future generation)

The fun comes from forming a hypothesis ("a fuel subsidy will calm the riots in
the north"), testing it, and then finding the second-order effects (the
subsidy widened the deficit, raised the sovereign spread and squeezed the
pension fund).

## Non-goals

- Tactical warfare, unit micromanagement, or a military-first game.
- Fiction, fantasy tech, or scripted storylines (scripted *historical anchors*
  through 2020–2021 are allowed as optional scenario events).
- Individual characters with personal drama. Agents are institutions or roles.
- Moral scoring by the game. The game shows outcomes; the player judges them.
- Visual spectacle.

## Target platform

Desktop Linux/Windows (macOS via MoltenVK, best effort). Compilers: GCC and
Clang only; MSVC is not supported (Windows builds use Clang or MinGW-GCC). An AVX2+FMA
CPU is required. Mid-range CPU with 8+
threads. At max speed on such hardware the sim must run at least 1 game-year
per 10 real seconds (≈ 37 ticks/s). The design budget aims for ~110 ticks/s
so there is headroom for forks and long-campaign growth (see
[02](02-simulation-core.md#performance-budget)).
