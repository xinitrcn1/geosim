# 04 — Society

## Pops (strata)

A **pop** is the unit of society: a group of people in one region who share
the same socioeconomic position. The key is:

```
pop = region × class tier × education/skill × age band
      (5)          (3)                (3: 15–29, 30–64, 65+)   → up to 45 per region, ~20–40 populated
```

Children (0–14) are tracked as a dependent cohort on household pops, not as
separate pops.

Class tiers (based on Erikson–Goldthorpe–Portocarero (EGP) and Oesch class schemes):
`precarious / informal`, `routine workers`, `intermediate / small owners`,
`professionals & managers`, `capital owners / elite`.

Pop attributes:

| Group | Fields |
|-------|--------|
| Size | headcount, dependants, households |
| Income | distribution params (lognormal μ, σ; Pareto α), source shares (wage/profit/rent/transfer/remittance) |
| Wealth | deposits, bonds, equity, housing, debt |
| Labor | employed by sector, unemployed, inactive, informal share |
| Human capital | mean years of schooling, skill index |
| Health | life expectancy contribution, morbidity, epidemic compartments |
| Identity | ethnicity/language/religion shares (vector, top-K + other) |
| Values | Inglehart–Welzel axes + economic left/right ([below](#values-and-opinion)) |
| Politics | party affinity vector, turnout propensity, protest propensity |
| Expectations | expected income (for relative deprivation), inflation expectation |

## Demography

- **Cohort-component projection** (the UN WPP method): fertility by age band,
  mortality by age band, migration. Annual cadence, sharded.
- **Demographic transition.** Fertility responds to female education, child
  mortality, income and urbanisation (Bongaarts; Lee 2003). Mortality responds
  to health spending, income, epidemics and conflict.
- **Ageing** pushes up pension spending and the dependency ratio, lowers
  labor supply, and shifts values. It is a key long-campaign pressure.
- **Mobility between strata** happens through education (moves skill tier),
  labor-market outcomes (moves class tier) and wealth accumulation. It is
  calibrated to intergenerational mobility estimates (Chetty et al.; the
  Great Gatsby curve relation between inequality and mobility).

## Health and epidemics

- **Metapopulation SEIR** (Kermack & McKendrick 1927) per region, stratified by age:
  `S → E → I → R (→ S with waning)` plus `D`. Coupled across regions by a
  mobility matrix (commuting + air travel), daily cadence.
- **COVID-19 scenario start.** Seeded in Wuhan in late 2019. Parameters by
  variant (R₀, IFR by age) from the published literature, and vaccines appear
  as a technology with production capacity in 2020–21 if the scenario
  follows history.
- **NPIs** (lockdown levels 0–4, school closures, travel bans, masks) cut
  contact rates *and* sector capacities (hospitality, retail, transport most).
  The epidemic–economy trade-off follows Eichenbaum, Rebelo & Trabandt (2021):
  people also reduce contacts voluntarily as infections rise, so the economy
  drops even without mandates.
- Health system capacity (beds per capita) changes the IFR when overwhelmed.
- Later pandemics come from a low-probability random event process.

## Migration

- **Harris–Todaro** (1970): the incentive to move is the *expected* income
  gap, `p_employment × wage`, minus moving costs.
- **Gravity/network term.** Flows scale with diaspora stock (network effects;
  Beine, Docquier & Özden 2011) and fall with distance, language and border
  policy.
- Forced migration: conflict, disaster and persecution push refugee flows,
  which neighbouring and destination states' policies absorb or block.
- Internal migration (rural→urban, declining→booming regions) uses the same
  model without border costs.
- Effects on destinations: labor supply (skill-specific wage effects, mostly
  small in the literature; Card 1990, Borjas 2003), fiscal contribution,
  identity shares, and anti-immigration attitudes in the opinion model.
- Remittances flow back as household income.

## Values and opinion

**Value space** (three axes per pop):

1. Traditional ↔ secular-rational (Inglehart & Welzel 2005, WVS)
2. Survival ↔ self-expression (same)
3. Economic left ↔ right (state vs market, redistribution)

Plus issue salience weights (economy, security, immigration, environment,
identity, corruption).

**Dynamics:**

- **Structural drift** (slow): rising existential security moves pops towards
  secular/self-expression values. Insecurity (unemployment, war, inflation)
  pulls them back (Inglehart's scarcity and socialisation hypotheses). Values
  change mostly through cohort replacement, which is why age bands matter.
- **Opinion dynamics** (monthly): a bounded-confidence model
  (Hegselmann–Krause 2002; Deffuant et al. 2000). Pops shift towards
  neighbours in value space within a confidence radius ε, weighted by a
  **media/connectivity graph** (shared language, regional proximity, media
  ecosystem). A small ε gives polarisation, a large ε gives consensus. Media
  freedom and social-media penetration adjust ε and the graph.
- **Issue salience** reacts to events: inflation raises economy salience,
  terror attacks raise security salience.

## Unrest: the "temperature"

Instability comes from **Turchin's structural-demographic theory** (Goldstone
1991; Turchin 2003, 2016) combined with **relative deprivation** (Gurr 1970).

**Political Stress Index** per region and nation:

```
PSI = MMP × EMP × SFD
  MMP  mass mobilisation potential  = f(real wage relative to trend, youth bulge share, urbanisation)
  EMP  elite mobilisation potential = f(elite stratum growth vs elite positions, intra-elite income spread)
  SFD  state fiscal distress        = f(debt/GDP, spread, distrust in state)
```

**Short-term grievance** per pop:

```
G_p = max(0, expected_income_p − realised_income_p) / expected_income_p    (relative deprivation)
      + perceived unfairness (inequality aversion, corruption salience)
      + identity grievance (discrimination policies, minority status)
```

**Escalation ladder** (per region), with thresholds that depend on
legitimacy ([05](05-politics-institutions.md)) and repression capacity:

```
calm → protests → strikes → riots → insurgency / secession movement
```

- Repression lowers immediate unrest but erodes legitimacy and raises future
  grievance, the classic repression trade-off (Davenport 2007).
- Strikes cut sector output. Riots destroy capital and deter investment.
  Insurgency becomes an internal conflict ([06](06-geopolitics-ai.md#conflict)).
- **Crime** (Becker 1968): the property-crime rate depends on legal vs
  illegal returns (poverty, unemployment) and enforcement. It lowers investment
  and trust.

## Trust and social capital

A slow per-region stock (Putnam 1993; Knack & Keefer 1997) that raises tax
compliance, contract enforcement (TFP) and policy acceptance. It falls with
inequality, corruption and conflict, and grows only slowly.
