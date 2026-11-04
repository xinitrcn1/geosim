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

Each section below says what is **implemented** (with its source file in
`src/sim/systems/`) and what is still **planned**. The economy runs weekly per
nation (7 shards); finance, credit, bonds, labour, fiscal and assets run every
28 days; logistics and commodity markets run daily
([02](02-simulation-core.md#cadence-scheduler)).

## Production

### Implemented: dynamic input-output network (`economy.cpp`)

Production is national per sector (7 sectors in `assets/defs/sectors.txt`),
with regional capacity (potential × NPI cut × sickness absence × sector
factors) summed to the nation and output shared back to regions. On top of the
final-demand block runs a dynamic input-output model with input stocks
(Pichler et al. 2022):

- Technical coefficients `a[s][s']` (input from `s` per unit of gross output)
  are rounded world averages of the OECD input-output tables aggregated to
  the 7 sectors (Leontief 1936; Miller & Blair 2009; Timmer et al. 2015).
  Gross requirement = value-added demand / value-added share.
- Each sector orders inputs to cover planned production and to bring its
  input stocks back to `inventory_days` of use (over `io_order_days`).
- Production = min(capacity, demand incl. orders, what **critical input**
  stocks allow). Goods and energy are critical (Leontief); services are not,
  so a sector can run short of them for a while without stopping.
- A constrained sector rations all customers in proportion, so shortages
  travel down the chain and demand shortfalls travel up it, with stocks as
  buffers.
- Imported inputs (about 60% of imports are intermediate; OECD TiVA) arrive in
  proportion to partners' manufacturing capacity × the logistics arrival rate
  ([below](#logistics-and-transport)).
- In the calibrated steady state stocks sit at target and output equals
  min(demand, capacity) exactly. `io_network = 0` switches the network off.

Business adaptation lowers the output cost of a given NPI stringency over
time (OECD 2020), and cyber operations cut energy and business-services
capacity ([06](06-geopolitics-ai.md#information-and-the-internet)).

### Planned: production units with CES technology

The long-term target is a production unit per (sector, region), ~40 sectors
aligned to ISIC Rev.4 / OECD ICIO, with nested CES technology:

```
Y = A_sr · min( VA / a_VA , X_1 / a_1 , … , X_C / a_C )          (Leontief over intermediates)
VA = [ α·K^ρ + (1−α)·(L_eff)^ρ ]^(1/ρ)                            (CES capital–labor)
L_eff = Σ_skill  θ_skill · L_skill                                (skill-weighted labor)
```

- `ρ` follows the capital–labor substitution elasticity σ≈0.5–0.8 (Chirinko 2008).
- **Capital vintages.** Investment adds capital with the frontier technology
  of its year; old vintages depreciate (stranded assets in transitions).

## Firms

Implemented in `firms.cpp` (weekly, with the economy).

- **Cohorts.** One SME and one large-firm cohort per nation and market
  sector, each holding a share of the sector's capacity. Revenue follows
  sector output; costs fall less than output (fixed costs, hoarded labour);
  government support (firm support, job-retention pay) covers part of the gap.
  The cash buffer, in days of costs, absorbs it (SMEs ~27 days; JPMorgan Chase
  Institute 2016).
- **Bankruptcies.** The exit rate rises convexly as buffers run down
  (financial fragility; Delli Gatti et al. 2005; Gourinchas et al. 2020 for
  COVID SME failures). Failed firms' capacity and jobs disappear except the
  share survivors take over (Caballero & Hammour 1994); their loans become
  non-performing ([credit](#credit-and-banking)).
- **Entry** when buffers are above normal and demand is there, rebuilding
  capacity over time (Dosi, Fagiolo & Roventini 2010).
- **Boards.** The large cohort of each sector is run by a board agent
  ([05](05-politics-institutions.md#agents-in-a-nation)) that chooses
  *labour hoarding* in a downturn (pragmatic when the shock looks temporary;
  ideological by varieties of capitalism, Hall & Soskice 2001; pulled toward
  the executive's employment goal by state ownership, Shleifer & Vishny 1994)
  and an *investment drive* (expected utilisation and cost of capital;
  shareholder-value boards pay out rather than invest, Lazonick & O'Sullivan
  2000).

## Logistics and transport

Implemented in `logistics.cpp` (daily). Data: `assets/defs/sea_lanes.txt`
(sea nodes, lanes in nautical miles, chokepoints with calibration notes),
`ports_curated.csv`, `logistics_curated.csv`, `overland_curated.csv`.

1. **Sea-lane graph.** Sea nodes (port basins) joined by lanes, some through
   chokepoints (Suez, Panama, straits). Nations reach the network through
   `port_access` shares; seaborne and overland trade shares come from data.
2. **Chokepoint incidents.** Blocking incidents arrive at random
   (counter-based RNG) with a hazard rising with traffic, mean vessel size
   (Merk 2015) and the season; durations are lognormal. Droughts cut canal
   capacity (Panama locks), more often with warming. A littoral nation's war
   or riots close part of the passage. Nothing is scheduled: a 2021-style Suez
   grounding is one possible draw.
3. **Routing on expected residual blockage.** A lane costs its sailing days
   plus, at a chokepoint, the passage time and the *expected* wait of a ship
   arriving today: the mean residual duration of a lognormal blockage given
   how long it has lasted, plus the time to clear the queue. Carriers take the
   cheapest route (all-pairs shortest paths), so they wait while a blockage is
   expected to end soon and divert round the Cape once waiting costs more.
4. **Queues.** Ships arrive at the passage's traffic rate and are served at
   its open capacity; the queue drains at spare capacity after reopening
   (Ever Given: ~420 ships, about 3.5 days).
5. **Port congestion.** Throughput over capacity sets port waits by the
   heavy-traffic approximation (Kingman 1961), growing as ρ/(1 − ρ). Sickness
   absence and closures lower port capacity.
6. **Container freight market** (Stopford 2009). Demand for ship-days = cargo
   × voyage time (longer routes, port waits and queued ships absorb capacity);
   carriers deploy or idle ships with a lag (blank sailings); the spot rate
   follows a convex supply curve near full utilisation; contract rates lag;
   high rates order new ships delivered after ~2.5 years (Kalouptsidi 2014).
7. **Effects on nations.** Freight costs raise import prices
   (Carrière-Swallow et al. 2023) and act as a trade cost (Hummels 2007;
   elasticity ~4, Simonovska & Waugh 2014); persistent extra transit days cut
   trade (~1% per day; Djankov, Freund & Pham 2010); longer transit delays
   the arrival of imported inputs, which the production network buffers with
   input stocks. **Domestic logistics:** internal freight capacity falls with
   movement restrictions and sickness, which disrupts food distribution
   ([prices](#prices)).

Sea-route distances also replace great-circle distances in trade gravity
([Trade](#trade)).

## Prices

### Implemented

- **Core inflation** (`economy.cpp`): expected inflation + a Phillips term on
  the smoothed output gap + exchange-rate pass-through + wage growth beyond
  productivity ([labour](#labor-market)); freight-driven import costs and
  tariffs then pass into the price level through the import share.
  Expectations adapt slowly. Pass-through rises with trend inflation (Taylor
  2000).
- **Crude oil** (`markets.cpp`, daily): demand from activity and personal
  mobility, producers' targets (OPEC+ decisions as events), and a price that
  follows flow and inventory gaps.
- **Commodity markets** (`commodities.cpp`, daily): gas, coal, grains and
  metals (`assets/defs/commodities.txt`) clear with the same stock-flow rule.
  Demand follows the sectors that use each commodity (or population and
  income for food) with a short-run price elasticity; supply follows 2019
  producer shares with a supply elasticity and, for grains, quarterly weather
  shocks; stocks absorb the gap (Working 1949; Deaton & Laroque 1992). Part
  of grain demand is biofuel and tracks the crude price. Nations feel them
  through **terms of trade** (net exports × price change, which also moves the
  current account and the exchange rate), **resource rents** (the state's
  share of the production value change) and **food prices**: the domestic
  relative food price passes through part of the world grain price plus a
  distribution shock when internal freight is disrupted. With large CPI food
  weights this drives headline inflation (Anand, Prasad & Zhang 2015). Gas
  reaches the CPI through household energy.
- **CPI per pop.** Food weighs more in poorer budgets (Engel's law), so food
  inflation hits the poor harder ([04](04-society.md#social-capital-and-demography)).

### Planned

- **Markup pricing** over unit cost (Kalecki 1954): `p = (1 + μ) · ulc`, with
  μ adjusting to inventories and utilisation, and Calvo-like repricing
  (≈ 10–20% of units per month).
- National goods markets with an internal transport-cost matrix between
  regions; Armington competition with imports ([Trade](#trade)).
- Regional gas hubs; producer AI (cartels) for markets other than oil.

## Consumption

**Implemented** (`economy.cpp`): each pop spends from smoothed real permanent
income with a baseline saving rate, cuts spending as unemployment rises
(precautionary saving), spends down excess deposits slowly, and shifts plans
with housing and equity wealth ([assets](#asset-prices-and-wealth-effects)).
Contact-services demand falls with deaths (voluntary avoidance, with fatigue)
and part of blocked contact spending moves to goods.

**Planned:** a Linear Expenditure System / Stone–Geary (Stone 1954):

```
x_pc = γ_pc + (β_pc / p_c) · ( E_p − Σ_k p_k γ_pk )
```

with subsistence quantities γ (food, energy, housing), so price shocks to
necessities are regressive, and a buffer-stock saving rule (Carroll 1997) for
the MPC. Later upgrade: AIDS (Deaton & Muellbauer 1980).

## Labor market

### Implemented

- **Flows** (`economy.cpp`, weekly): layoffs follow labour demand with a lag
  that rises with employment protection (EPL); job retention and EPL keep
  part of the pre-crisis jobs attached; laid-off workers leave the labour
  force more often where work is informal or stay-at-home orders bind (ILO
  2020). Hiring is a simple matching rule, hires = m·√(U·V); the full DMP
  model is not implemented. Labour demand falls with real labour costs
  (elasticity ~0.3; Lichter, Peichl & Siegloch 2015).
- **Wages** (`labour.cpp`, 28 days). The 2019 wage structure is the baseline
  and a real-wage index moves from there: a **wage curve** (elasticity ~0.1;
  Blanchflower & Oswald 1994), weaker where collective bargaining covers many
  workers; **inflation surprises** erode real pay, and covered workers recover
  part through **indexation**; **downward nominal rigidity** (Dickens et al.
  2007). Nash bargaining is not implemented.
- **Unions.** A union-federation agent pushes for real-wage gains
  (`lever::wage_push`): pragmatic (recover purchasing power, held back by
  unemployment), ideological (militancy of dense, left-leaning movements),
  applied through bargaining coverage. Real-wage losses where unions are
  strong feed strikes ([04](04-society.md#unrest-the-temperature)).
- **Minimum wage** (an executive lever passed as a bill): lifts low-skill pay
  in proportion to its bite, with small job losses (Cengiz et al. 2019).
- **Informality by group.** Informal jobs, set per identity group
  ([04](04-society.md#identity-groups)), get no job retention, no employment
  protection and less unemployment insurance.
- **Short-time work** (job retention coverage and replacement) is an explicit
  lever that keeps matches alive while cutting hours.

### Planned

- Search and matching (Pissarides 2000) per region × skill with
  Cobb–Douglas matching `m · U^η · V^(1−η)` (Petrongolo & Pissarides 2001)
  and vacancies from expected profitability.
- Nash wage bargaining with outside options; informality that grows with
  regulation and tax wedges (Schneider & Enste 2000).
- **Emergent curves.** The Beveridge, Phillips and Okun relations are not
  imposed; validation checks that they emerge ([11](11-data-calibration.md)).

## Money, banking and finance

- **Central bank AI** (`ai.cpp`, daily meetings). Pragmatic target: a Taylor
  rule (Taylor 1993) around the neutral rate implied by the starting setting,
  bounded below; pegs follow the anchor's rate, currency unions share one
  central-bank agent. A dependent central bank leans toward the rate a
  growth-first executive wants (Alesina & Summers 1993; Binder 2021), a bias
  that fades in a currency crisis. Independence comes from data
  (`finance_curated.csv`).
- **Exchange rates** (`finance.cpp`). Floating real rates move with the
  current account (incl. commodity terms of trade), the interest differential
  vs the US (UIP) and sovereign risk (capital flight); two capital-account
  channels add pressure: currency substitution in dollarised economies when
  real rates fall (Ize & Levy Yeyati 2003) and credit-financed purchases of
  foreign assets.
- **Sovereign spreads** from debt, deficits, state capacity, legitimacy and
  reserve-currency status (Reinhart & Rogoff 2009). When the spread explodes
  the sovereign **defaults**: a haircut through the ledger, a legitimacy hit
  and a risk premium; an IMF programme can intervene first
  ([06](06-geopolitics-ai.md#diplomacy)).
- **Remittances** follow the income of migrants' host economies.

### Credit and banking

Implemented in `credit.cpp` (28 days). The calibrated 2019 economy already
contains the credit that financed it, so credit is a **deviation from its
baseline path** (the loan stock growing with nominal trend output); only
departures move spending, which keeps the steady state exact.

- **Loan growth** (households, firms) rises when the real lending rate falls
  (policy rate, long yields, NPL premium, minus expected inflation; Bernanke &
  Gertler 1995) and with a positive output gap, and falls when unemployment
  rises (lending standards; Lown & Morgan 2006), when capital falls short of
  requirements (credit crunch; Peek & Rosengren 1995), when NPLs are high
  (Jiménez et al. 2012) and when the stock is above its path (deleveraging;
  Mian & Sufi 2014). In a downturn firms draw on credit lines (Li, Strahan &
  Zhang 2020).
- **Directed credit.** The executive can direct state-owned banks to lend
  (`lever::directed_credit`; Sapienza 2004; Dinç 2005; Bircan & Saka 2021),
  scaled by the state-bank share and credit depth, partly through guarantee
  schemes on private banks; the government pays the guaranteed share of
  losses.
- **Flows.** Loans create deposits (McLeay, Radia & Thomas 2014). The credit
  impulse (Biggs, Mayer & Pick 2010) is partly spent (durables, investment),
  partly held as deposits and, in dollarised economies, partly converted to
  foreign assets, a capital outflow that weakens the currency. Loan interest
  above or below the path flows between borrowers and banks.
- **NPLs and bank capital.** The NPL ratio follows unemployment, the output
  gap, depreciation × FX-denominated loans (balance-sheet effect; Cespedes,
  Chang & Velasco 2004) and past booms (Schularick & Taylor 2012), with a
  recognition lag that lengthens while crisis support runs (**forbearance**;
  BCBS 2020). Losses come out of bank capital. Depreciation also cuts the
  investment of firms with FX debt.
- **Central-bank dependence.** A growth-first executive presses a dependent
  central bank for lower rates and directs credit until the currency falls
  fast enough to turn it orthodox.
- **Pass-through** of depreciation into prices rises with trend inflation
  (Taylor 2000), so inflationary credit booms feed on themselves.

### Bonds and yields

Implemented in `bonds.cpp` (28 days). The 10-year yield = expected average
short rate + term premium:

- **Expectations:** the policy rate is expected to revert toward a neutral
  rate (Campbell & Shiller 1991 for the term structure).
- **Term premium** = its starting value + the change in the sovereign spread
  + inflation uncertainty − central-bank holdings (**QE**, ~5bp per % of GDP
  purchased; Gagnon et al. 2011) + spillovers from the US term premium (Rey
  2013; Obstfeld 2015) + compensation for expected depreciation demanded by
  foreign holders (Arslanalp & Tsuda 2014). Reserve currencies gain in a world
  downturn (**flight to quality**) and pay little of spread changes.
- **QE lever** (`lever::asset_purchases`): the central bank buys when the
  Taylor rate is below the lower bound or spreads blow out (fragmentation);
  purchases saturate and are capped at a share of debt.
- **Roll-over.** The government's effective rate moves toward the new-issue
  rate (bills at the policy rate, bonds at the 10-year yield) as debt matures
  and deficits are financed, so average maturity matters (UK ~15 years,
  Brazil ~4). The cost of capital for investment blends the policy rate and
  the long yield.

### Asset prices and wealth effects

Implemented in `assets.cpp` (28 days), per nation:

- **House prices:** the fundamental price-to-rent ratio is the inverse of the
  user cost (Poterba 1984; Himmelberg, Mayer & Sinai 2005); rents follow
  household real income; credit lifts prices beyond fundamentals (Favara &
  Imbs 2015; Mian & Sufi 2009). Prices adjust slowly.
- **Equity:** smoothed profits discounted at the real long rate plus an equity
  risk premium.
- **Wealth effects:** households spend ~5 cents per dollar of housing wealth
  change (Mian, Rao & Sufi 2013; Case, Quigley & Shiller 2005) and ~2 cents
  of equity wealth a year.

Planned: one housing price per region with supply elasticity, and housing
wealth in each pop's balance sheet.

## Fiscal

### Implemented (`fiscal.cpp`, `economy.cpp`)

- **Progressivity.** Each pop's effective household tax rate follows the
  Heathcote–Storesletten–Violante form, rate ∝ (income / mean)^τ, with τ from
  data and the level calibrated to 2019 revenue. Indirect and firm taxes are
  shares of revenue.
- **Behavioural response.** Taxable income falls with the net-of-tax rate
  (ETI ~0.25; Saez, Slemrod & Giertz 2012).
- **Compliance.** The share of statutory taxes collected follows legitimacy,
  social trust and state capacity (Levi 1988; Slemrod 2007; Allingham & Sandmo
  1972); it moves slowly from the calibrated start.
- **Levers** (bills, [05](05-politics-institutions.md#legislation)): the
  household tax scale (`lever::tax_scale`; pragmatic consolidation when
  spreads rise, ideological by the governing party's economic position) and
  **public investment** (`lever::public_investment`), which buys construction
  now and builds public capital that raises potential output (elasticity
  ~0.1; Bom & Ligthart 2014).
- **Fiscal packages** (announced as a share of GDP, disbursed over time; first
  funding job retention and benefit top-ups), defence spending and war
  procurement are government purchases. Small winning coalitions tilt packages
  toward firms (selectorate theory).
- **Tariffs** are trade policy ([Trade](#trade)) and also raise revenue.
- **Debt** follows from the ledger; interest rolls over at market yields
  ([bonds](#bonds-and-yields)).

### Planned

- Statutory tax bands applied to each pop's income distribution, VAT,
  corporate, property and wealth taxes; resource-rent taxation beyond the
  current state take.
- Public goods feeding sector capacities and human capital; issuance by
  maturity bucket; sub-national budgets with equalisation.

## Trade

### Implemented

- **Gravity trade shares** (`geopolitics.cpp`; Tinbergen 1962; Head & Mayer
  2014): destination GDP over effective distance (elasticity ~1), with
  **sea-route distances** from the logistics network for seaborne trade and
  road distance for overland trade, and a shared land border roughly
  doubling trade (contiguity).
- **Export access** relative to the start is cut by **sanctions** and war,
  raised by **FTAs** (phased in over a decade; Baier & Bergstrand 2007), and
  reduced by freight costs and delays and by internet filtering of digitally
  delivered services (Freund & Weinhold 2004).
- **Tariffs** (`firms.cpp`, `lever::tariff`): an average import tariff set
  by the executive. Pragmatic: protect when manufacturing jobs are lost or
  the currency is strong, hold back when inflation is high (Bown & Crowley
  2013); ideological: nationalist governing parties protect (Colantone &
  Stanig 2018; Rodrik 2018). Tariffs pass fully into import prices (Amiti,
  Redding & Weinstein 2019; Fajgelbaum et al. 2020), cut import volumes
  (elasticity ~4), raise revenue and invite retaliation through sanctions.
- **Tourism** follows travel controls, not tariffs.
- Exports and imports respond to the real exchange rate; imports follow
  output weighted by import intensity and the production network's input
  orders.

### Planned

- Structural gravity with multilateral resistance (Anderson & van Wincoop
  2003): `X_ij = (Y_i E_j / Y_W) · (τ_ij / (Π_i P_j))^(1−σ)`, with CEPII trade
  costs (language, colonial ties) and non-tariff barriers.
- **Armington** substitution per commodity (GTAP elasticities) and a bilateral
  flow matrix that rebalances quarterly. Neither exists yet.
- **Financial account.** Portfolio and FDI flows reacting to returns and risk.

## Growth & long run

**Implemented** (`longrun.cpp`, 28 days):

- **Capital hysteresis:** regional potential grows or shrinks with the
  investment gap.
- **Trend growth** responds to institutions (liberal index; Acemoglu, Johnson
  & Robinson 2001), human capital (tertiary share; Lucas 1988), public capital
  (Bom & Ligthart 2014) and internet filtering (Czernich et al. 2011).
- **Climate** (DICE-lite; Nordhaus 2017): emissions from world oil demand,
  an airborne fraction, transient warming toward equilibrium, and DICE-2016R
  quadratic damages, larger in hotter regions (Burke, Hsiang & Miguel 2015).
  Warming also raises the canal drought hazard ([logistics](#logistics-and-transport)).

**Planned:** a vintage capital stock (Solow 1956); TFP from R&D (Romer 1990)
and frontier distance (catch-up); technology diffusion with adoption lags
(Comin & Hobijn 2010) for the tech tree
([02](02-simulation-core.md#long-campaign-detail-extension)); emissions from
the full energy mix; disaster events.

## Inequality within strata

Implemented today: each pop has a lognormal dispersion σ around its mean
income, and Gini and related indicators are computed from the mixture
(`indicators.cpp`). The Pareto tail and the policy effects on shape below are
planned.

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
- Firm granularity: the SME vs large-firm split per nation × sector is now
  implemented ([Firms](#firms)); is a regional split or a size distribution
  needed for market concentration / markups?
