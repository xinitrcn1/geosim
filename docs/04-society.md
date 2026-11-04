# 04 — Society

## Pops

A **pop** is the unit of society: a group of people in one region who share
the same socioeconomic position.

**Implemented** (`src/sim/model.hpp`, `scenario.cpp`):

```
pop = region × kind (low skill / mid skill / high skill / retired) × identity group
```

Identity groups come from `groups_curated.csv` (one group for nations
without rows); every pop belongs to exactly one, so the economy, migration,
opinion and unrest systems see the groups directly. Children are part of the
young epidemic age group, not separate pops. Two epidemic age groups (under
and over 65) live on the region.

Pop fields today: size; employed / unemployed / inactive; wages by sector and
a wage multiplier; income by source (work, benefits, other), expected and
permanent income; consumption plans and deposits; values (3 axes) and issue
salience; party support; grievance; income dispersion σ; identity group,
informality and formal-benefit access; network strength; effective tax
factor; and a pop-specific CPI.

**Planned** (the long-term key): class tiers (Erikson–Goldthorpe–Portocarero
and Oesch schemes) and age bands, giving
`region × class tier × skill × age band`, with these additional fields:

| Group | Fields |
|-------|--------|
| Income | Pareto tail α, source shares incl. profit/rent/remittance |
| Wealth | bonds, equity, housing, debt |
| Labor | employed by sector |
| Human capital | mean years of schooling, skill index |
| Politics | turnout and protest propensity per pop |

## Identity groups

Implemented in `update_groups` (`society.cpp`, 28 days per nation) with data
from `groups_curated.csv`: caste, race / ethnicity, religion and legal status
(hukou, citizenship, migrant status).

- **Labour-market position.** Each group has an education odds multiplier
  (the loader solves a RAS so national skill totals hold), relative
  informality, a log wage gap within the same skill, relative unemployment
  and relative wealth. Informal jobs get no job retention, no employment
  protection and less unemployment insurance
  ([03](03-economy.md#labor-market)).
- **Exclusion.** Political exclusion from Ethnic Power Relations coding
  (Vogt et al. 2015) and citizenship / hukou status.
- **Horizontal inequality.** Group income relative to the national mean and
  its change since the start (Stewart 2008), plus exclusion (Cederman, Wimmer
  & Min 2010; Cederman, Weidmann & Gleditsch 2011), set group grievance;
  mobilisation = grievance × network cohesion (organisational capacity). It
  adds to the protest drive of the group's pops.
- **Network insurance.** Members whose income falls below expectation receive
  transfers from better-off members of the same group in the same region, and
  networked laid-off migrants go home and return to job search slowly (Munshi
  & Rosenzweig 2016).
- International immigrants join the destination's immigrant group.

## Demography

**Implemented** (`update_demography`, 28 days per region): births, children
entering the labour force by skill (following their group's own skill mix),
retirement, baseline mortality and epidemic and war deaths by age.

- **Fertility transition.** The birth rate drifts toward a long-run level set
  by income per head and education (Bongaarts 1978; Lee 2003), over about a
  decade.
- **Baby bust.** Births fall when unemployment rises (Kearney & Levine 2021).
- **Cohort scarring.** Entrants to a slack labour market carry a lasting wage
  penalty (Kahn 2010).
- **Educational expansion.** The tertiary share rises with income growth,
  moving people between skill tiers.

**Planned:** a full cohort-component projection (the UN WPP method) by age
band; mortality responding to health spending and income; ageing effects on
pensions and values; mobility between class tiers calibrated to
intergenerational mobility estimates (Chetty et al.; the Great Gatsby curve).

## Health and epidemics

**Implemented** (`epidemic.cpp`, daily):

- **Simultaneous epidemics.** One two-age-group SEIR (Kermack & McKendrick
  1927) per pathogen on the same population, per region:
  `S → E → I → R (→ S with waning)` plus `D`. Pathogens are data
  (`assets/defs/pathogens.txt`: SARS-CoV-2 wild type, seasonal influenza, and
  templates). They interact through shared hospital capacity (overload raises
  every IFR), cross-immunity, deaths, and the behavioural and policy response
  to the combined severity-weighted burden. Imports follow world prevalence
  and travel controls.
- **NPIs** (OxCGRT indicators) cut transmission and sector capacities, with
  fatigue after prolonged restrictions; people also cut contacts voluntarily
  as deaths rise (Eichenbaum, Rebelo & Trabandt 2021). Informality and low
  social trust lower compliance. Testing, tracing and a suppression strategy
  are levers.
- **Vaccines.** A pathogen with `vaccine_days > 0` gets a vaccine that many
  days after it is first seen (SARS-CoV-2: first emergency authorisation ~11
  months after the outbreak). World production ramps up; doses go to nations
  by population weighted by income per head (advance purchases; Wouters et
  al. 2021), the old first; a dose makes a susceptible immune with the
  vaccine's efficacy.
- **Emergent epidemics.** A novel epidemic emerges at random (about 2% a year
  for a COVID-scale event; Marani et al. 2021): an unseeded `emergent`
  template starts in a nation drawn by population.

**Planned:** a regional mobility matrix (commuting + air travel) coupling
regions inside nations; variants; vaccination priority as a lever.

## Migration

**Implemented** (`update_migration`, 28 days): Harris–Todaro moves toward
higher expected income net of a moving cost; internal moves follow region
adjacency and are cut by movement restrictions; international moves follow a
gravity term and are cut by travel controls at both ends; war pushes refugees.
Migrants arrive as job seekers and remit. Diaspora networks, distance and
language terms, refugee policies and attitude effects below are planned.

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

Plus issue salience weights. Implemented issues: economy, health, security,
identity; immigration, environment and corruption are planned.

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

**Implemented** (`update_opinion`, 28 days): insecurity pulls values toward
survival, income losers move left, and bounded confidence (Hegselmann &
Krause 2002) runs within each nation with a radius widened by media freedom.
**Across borders**, values diffuse through the internet: pops move toward the
foreign publics they are coupled to, within bounded confidence (Gleditsch &
Ward 2006; [06](06-geopolitics-ai.md#information-and-the-internet)). Public
opinion is also thermostatic against economic policy
([05](05-politics-institutions.md#the-overton-window)). Cohort replacement and
a regional media graph are planned.

## Unrest: the "temperature"

Instability comes from **Turchin's structural-demographic theory** (Goldstone
1991; Turchin 2003, 2016) combined with **relative deprivation** (Gurr 1970).

**Political Stress Index** per nation (a regional PSI is planned):

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

**Implemented PSI** (`update_unrest`, 28 days per nation): mass
mobilisation = real-wage loss of the masses × a **youth bulge** factor (youth
15–29 share of 15–64; Goldstone 1991; Urdal 2006) × urbanisation; elite
mobilisation = educated youth without jobs (elite overproduction; Turchin
2016); both amplified by state fiscal distress. PSI is zero in the calibrated
baseline. Grievance adds group mobilisation ([identity groups](#identity-groups)),
corruption made visible online, blocked emigration (Hirschman 1970), Overton
backlash against liberty-restricting policy
([05](05-politics-institutions.md#the-overton-window)) and strike pressure
from real-wage losses where unions are strong. Every 28 days these set each
region's protest **drive**.

### Protest cascade

Collective action itself is a **daily threshold cascade** per region
(`update_protest`), replacing the earlier smooth ladder. People join when the
share already protesting exceeds their personal threshold (Granovetter 1978),
so participation has tipping points and hysteresis, and the timing of an
uprising is hard to predict even when its conditions are not (Kuran 1991).
Participation `x` solves

```
x = x_max · Φ((drive + s·x − μ) / σ)
```

- **drive:** grievance, PSI and group mobilisation (monthly), plus random
  **sparks** (triggering incidents: a killing on video, a scandal; Poisson
  arrivals, exponential sizes, decaying over days), foreign protest through the
  internet, foreign influence operations and censorship backlash.
- **s, coordination:** a bandwagon weight that rises with social-media
  connectivity (Steinert-Threlkeld 2017; Enikolopov, Makarin & Petrova 2020)
  and is cut by information control (`lever::info_control`).
- **μ, cost of joining:** raised by repression capacity (Davenport 2007) and
  by **fatigue**: a long wave raises the cost until it subsides (cycles of
  contention; Tarrow 1998).
- **Censorship backlash:** a sudden rise of information control angers and
  alerts the connected young (Hobbs & Roberts 2018; Hassanpour 2014 on the
  2011 Egyptian shutdown), so it can trigger the cascade it meant to stop;
  gradual, early control backfires little.

**Ladder mapping.** Participation maps onto the escalation ladder

```
calm (0) → protests (1) → strikes (2) → riots (3) → insurgency / regime crisis (4)
```

with level 4 at ~3.5% of the population active (Chenoweth & Stephan 2011).
Strikes cut regional capacity; riots destroy capital; sustained
insurgency-level unrest can become armed rebellion
([06](06-geopolitics-ai.md#conflict)); a disloyal military may defect
([05](05-politics-institutions.md#military)).

- Repression lowers immediate unrest but erodes legitimacy and raises future
  grievance (Davenport 2007). Planned: repression as an explicit lever.

## Social capital and demography

Implemented in `update_social_capital` (28 days per nation):

- **Social trust**, a slow stock (Putnam 1993; Knack & Keefer 1997) starting
  from World Values Survey wave 7 / EVS: falls with rising inequality, unrest
  and corruption and recovers slowly. It raises tax compliance
  ([03](03-economy.md#fiscal)) and NPI compliance (Bargain & Aminjonov 2020).
  Cyber operations also lower it.
- **Crime** (Becker 1968): property crime rises with unemployment and poverty
  (Raphael & Winter-Ebmer 2001) and deters investment.
- **Fertility and education:** the demographic transition and educational
  expansion ([Demography](#demography)).
- **CPI per pop:** food weighs more in poorer budgets (Engel's law), so food
  inflation hits the poor harder.

Planned: trust per region raising contract enforcement (TFP); crime
enforcement as a policy.
