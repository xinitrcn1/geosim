# 03 — Economy

## Modelling approach: a stock-flow-consistent meso agent-based model

| Approach | Verdict | Reason |
|----------|---------|--------|
| CGE (computable general equilibrium) | Rejected as the core | Assumes markets clear each period; cannot produce unemployment spells, inventory cycles or crisis dynamics such as COVID-2020 or 2008 |
| DSGE | Rejected as the core | Representative agents hide the distributional effects the game is about. Used for *calibration intuition* only (e.g. Taylor-rule parameters) |
| Individual-level ABM | Rejected | ~8 billion agents, or even a 1:10,000 sample, is too costly at daily resolution across 4,000 regions |
| **Meso ABM + SFC accounting** | **Chosen** | Strata and sector × region production units are the agents. Prices adjust out of equilibrium, and the SFC ledger guarantees conservation. Precedents: Dosi et al. "Keynes+Schumpeter" (K+S) family; Caiani et al. 2016 benchmark AB-SFC; Poledna et al. 2023, whose ABM matched or beat VAR/DSGE forecasts for Austria |

The model is out of equilibrium and adapts as it goes. Equilibrium is only a
long-run attractor. Behaviour rules are simple and published, and the
complexity comes from how they interact through the network.

## Production

**Unit:** production unit = (sector `s`, region `r`). ~40 sectors aligned to
ISIC Rev.4 sections / OECD ICIO industries.

Technology (nested CES, standard in multi-sector models):

```
Y = A_sr · min( VA / a_VA , X_1 / a_1 , … , X_C / a_C )          (Leontief over intermediates)
VA = [ α·K^ρ + (1−α)·(L_eff)^ρ ]^(1/ρ)                            (CES capital–labor)
L_eff = Σ_skill  θ_skill · L_skill                                (skill-weighted labor)
```

- `a_c` input coefficients come from OECD ICIO / WIOD (Leontief 1936; Timmer et al. 2015).
- `ρ` follows the capital–labor substitution elasticity σ≈0.5–0.8 (Chirinko 2008).
- **Capacity and utilisation.** Firms plan output from expected demand
  (adaptive expectations plus an inventory target). They cannot exceed capital
  capacity in the short run.
- **Capital vintages.** Investment adds capital with the frontier technology of
  its year, and old vintages depreciate. This matters for climate and energy
  transitions (stranded assets).
- **Supply-chain shocks.** If an input runs short, output is rationed
  proportionally through the Leontief layer. Shortages spread through the
  input-output network, which is how COVID-era chip and shipping shortages
  appear on their own.

## Prices

- Firms set **markup pricing** over unit cost (Kalecki 1954; post-Keynesian pricing):
  `p = (1 + μ) · ulc`, with the markup μ adjusting to inventory deviation and
  capacity utilisation. Prices are sticky. Each week only part of the units
  reprice (Calvo-like hazard, calibrated to micro price data, ≈ 10–20% per month).
- **Goods markets** are national, with an internal transport-cost matrix
  between regions. Imports compete through Armington aggregation
  ([Trade](#trade)).
- **World commodity markets** (crude oil, gas (three regional hubs), coal,
  grains, metals) clear daily on a spot price set by inventory stocks:
  `Δln p = κ · (target_stock − stock)/target_stock + supply_shock`. Producer
  nations' AI (e.g. cartels) can change supply.
- **CPI per stratum.** Each stratum has its own price index weighted by its
  consumption basket, so inflation hits groups differently and the map shows it.

## Consumption

**Linear Expenditure System / Stone–Geary** (Stone 1954):

```
x_pc = γ_pc + (β_pc / p_c) · ( E_p − Σ_k p_k γ_pk )
```

- `γ` is subsistence quantities (food, energy, housing), `β` is marginal
  budget shares.
- Poor strata spend mostly on subsistence, so food and energy price shocks are
  regressive, as in the real world (a core trade-off lever).
- Expenditure `E_p` comes from disposable income and a buffer-stock saving rule
  (Carroll 1997): marginal propensity to consume (MPC) falls with wealth and
  rises with liquidity constraints. This gives the high MPC of poor households
  that fiscal multipliers depend on.
- Later upgrade: AIDS (Deaton & Muellbauer 1980) for price-dependent
  substitution.

## Labor market

**Search and matching** (Diamond–Mortensen–Pissarides; Pissarides 2000) per
region × skill tier:

```
matches = m · U^η · V^(1−η)       (Cobb–Douglas matching, η ≈ 0.5–0.7; Petrongolo & Pissarides 2001)
```

- Firms post vacancies from expected profitability. Separations are
  exogenous + output-driven.
- **Wages.** Nash bargaining between the worker's outside option (benefits,
  home production) and the firm's surplus. Bargaining power rises with union
  density and collective-agreement coverage. Statutory minimum wages bind
  where they apply. Nominal wage rigidity: downward adjustment is slower.
- **Emergent curves.** The Beveridge curve (U vs V), the Phillips curve
  (wage inflation vs U) and Okun's law (output gap vs U) are *not imposed*.
  Validation tests check that they emerge ([11](11-data-calibration.md)).
- **Informal sector.** In each region a share of employment is informal: it
  pays no tax and has no benefits but is easy to enter. It grows when
  regulation or tax wedges are high (Schneider & Enste 2000). Very important
  for developing economies.
- **Furlough schemes / short-time work** are explicit policies that keep
  matches alive while cutting hours (a COVID-2020 lever).

## Money, banking and finance

- **Central bank AI.** Baseline is a Taylor rule (Taylor 1993):
  `i = r* + π + 0.5(π − π*) + 0.5·gap`. The agent then deviates according
  to its mandate (inflation-only vs dual), its independence index and its own
  utility ([06](06-geopolitics-ai.md)). It has an effective lower bound and QE
  as tools.
- **Endogenous money.** Banks lend when borrowers are creditworthy and
  capital ratios allow, and loans create deposits (McLeay, Radia & Thomas
  2014). Credit cycles follow Minsky-style leverage dynamics.
- **Sovereign risk premium.**
  `spread = f(debt/GDP, external-currency debt share, primary balance,
  institutions, reserve-currency status, contagion)`, calibrated on
  Reinhart & Rogoff (2009) and the debt-intolerance literature. Above a
  threshold, rollover fails → default/IMF-programme event.
- **Exchange rates.** Floating rates follow uncovered interest parity with a
  risk premium plus current-account pressure. Pegs need reserves. Currency
  unions (eurozone) share one central bank agent.
- **Asset prices.** One housing price per region (supply elasticity, credit,
  rents) and an equity index per nation (discounted profits). Housing wealth
  feeds stratum wealth, inequality and consumption (wealth effects).

## Fiscal

- **Taxes.** Progressive personal income tax bands applied to each stratum's
  income distribution (analytically, see [Inequality](#inequality-within-strata)),
  payroll, VAT, corporate, tariffs, resource rents, property, wealth (optional).
- **Behavioural response.** Elasticity of taxable income (Saez, Slemrod &
  Giertz 2012), so very high rates collect less (a Laffer region). Evasion
  depends on enforcement capacity and legitimacy ([05](05-politics-institutions.md)).
- **Spending.** Transfers (pensions, unemployment, family, cash transfers),
  public goods (health, education, infrastructure, security, R&D) and public
  wages. Public goods feed sector capacities and human capital with lags.
- **Debt.** Issuance by maturity bucket, interest from the policy rate + spread.
  The debt-dynamics identity `Δb = (i − g)/(1+g)·b − pb` falls straight out of
  the ledger.
- **Sub-national budgets** for federal states, with transfers and
  equalisation formulas.

## Trade

- **Structural gravity** (Tinbergen 1962; Anderson & van Wincoop 2003;
  Head & Mayer 2014):
  `X_ij = (Y_i E_j / Y_W) · (τ_ij / (Π_i P_j))^(1−σ)`
- **Trade costs** `τ_ij` = distance, contiguity, language, colonial ties
  (CEPII), plus *policy wedges*: tariffs, non-tariff barriers, sanctions,
  FTAs, war.
- **Armington** substitution between domestic and imported varieties per
  commodity (σ per commodity from the GTAP elasticities literature).
- Bilateral flows rebalance quarterly (multilateral resistance is expensive),
  and price-pass-through adjustments run weekly in between.
- **Financial account.** Portfolio and FDI flows react to return
  differentials and risk. Remittances are a household flow tied to diaspora
  stocks ([04](04-society.md#migration)).

## Growth & long run

- **Accumulation** (Solow 1956) via the vintage capital stock.
- **TFP** `A_sr` grows from human capital (Lucas 1988), R&D stocks (Romer
  1990), frontier distance (catch-up; Gerschenkron/Nelson-Phelps) and
  **institutions** (Acemoglu, Johnson & Robinson 2001). The institutions
  channel is how forced overrides and corruption cost growth over decades.
- **Technology diffusion** with adoption lags (Comin & Hobijn 2010) drives the
  long-campaign tech tree ([02](02-simulation-core.md#long-campaign-detail-extension)).
- **Climate** (optional system): emissions from the energy commodity mix →
  global temperature anomaly (simple carbon-cycle box model) → regional damage
  functions (Nordhaus DICE; Burke, Hsiang & Miguel 2015 for the
  temperature–productivity non-linearity) and disaster events.

## Inequality within strata

Every pop stores parameters of its income distribution, not only the mean:

- **Body: lognormal(μ, σ).** **Tail: Pareto(α) above a threshold**
  (Piketty & Saez 2003; WID methodology).
- Gini, Palma ratio, top-10% share and poverty headcount for any region or
  nation follow from mixing the strata distributions. They are computed
  analytically or by fixed quadrature, without sampling.
- Policy affects the *shape* as well as the mean: progressive taxes
  compress σ and the tail, while capital-income growth thickens the tail
  (α falls).
- This lets the map show **inequality within groups** as well as between
  them, which is a key difference from Victoria-style pop systems.

## Open questions

- Housing: should regional rent be its own market (it is a big share of
  subsistence spending) or derived from house prices? Leaning towards its own market.
- Firm granularity: is one representative unit per sector × region enough, or
  do we need a size distribution for market concentration / markups? Likely a
  2-bucket SME vs large-firm split.
