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
and over 65) and five demographic age bands (0–14, 15–29, 30–49, 50–64, 65+;
[Demography](#demography)) live on the region. A worker pop can split off a
**precarious stratum** after a shock and merge back later
([02](02-simulation-core.md#long-campaign-detail-extension)).

Pop fields today: size; employed / unemployed / inactive; wages by sector and
a wage multiplier; income by source (work, benefits, other), expected and
permanent income; consumption plans and deposits; values (3 axes) and issue
salience; party support; grievance; income dispersion σ; identity group,
informality and formal-benefit access; network strength; effective tax
factor; and a pop-specific CPI.

**Planned** (the long-term key): class tiers (Erikson–Goldthorpe–Portocarero
and Oesch schemes) and age bands on pops (today they are region accounts),
giving `region × class tier × skill × age band`, with these additional fields:

| Group | Fields |
|-------|--------|
| Income | per-pop Pareto tail α (today a national top tail), source shares incl. profit/rent/remittance |
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

- **Age bands.** A coarse cohort-component account (the UN WPP method) of
  five bands per region, started from WDI 0–14 and 65+ shares (working ages
  split 30/42/28): births enter band 0, each band ages into the next at
  1/width a year, deaths fall on the bands of their group, and the account is
  rescaled to the population total.
- **Mortality** follows income and health spending relative to 2019
  (`mortality_income`, `mortality_health`; Preston 1975; Cutler, Deaton &
  Lleras-Muney 2006).
- **Retirement** moves a share of each worker pop to the retired pop of its
  group; a later statutory age (`lever::retirement_age`) lengthens working
  lives. **Pension outlays** scale with the 65+ share relative to 2019
  (`pension_ageing`).
- **Intergenerational mobility** between skill tiers (low → mid → high) is
  lower where inequality is high, the Great Gatsby curve (Corak 2013; Chetty
  et al. 2014): IGE = `gatsby_slope` × Gini − 0.2, and `mobility_rate` × (1 −
  IGE) of a tier's unemployed a year move up and re-enter as job seekers.

- **Fertility transition.** The birth rate drifts toward a long-run level set
  by income per head and education (Bongaarts 1978; Lee 2003), over about a
  decade.
- **Baby bust.** Births fall when unemployment rises (Kearney & Levine 2021).
- **Cohort scarring.** Entrants to a slack labour market carry a lasting wage
  penalty (Kahn 2010).
- **Educational expansion.** The tertiary share rises with income growth,
  moving people between skill tiers.
- **Formative years.** Cohorts entering in insecure times pull their pop's
  values toward survival (Inglehart 1997).

**Planned:** age-specific fertility and mortality schedules (today rates are
per group, distributed over bands), age bands on pops, values that follow
cohort replacement explicitly (`cohort_replacement` and `formative_security`
are declared in `parameters.txt` but not yet used), class tiers beyond skill.

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
- **Commuting.** Adjacent regions inside a nation are coupled: infection
  pressure flows from higher- to lower-prevalence neighbours by adjacency
  weight (`commute_rate`; Balcan et al. 2009), cut by internal movement
  restrictions (C7).
- **Vaccines.** A pathogen with `vaccine_days > 0` gets a vaccine that many
  days after it is first seen (SARS-CoV-2: first emergency authorisation ~11
  months after the outbreak). World production ramps up; doses go to nations
  by population weighted by income per head (advance purchases; Wouters et
  al. 2021), except a **COVAX** share (`covax_share`) pooled for equal
  per-capita allocation. Within a nation **`lever::vaccine_priority`** sets the
  share of doses offered to the old first, the rest by population (Bubar et
  al. 2021); a dose makes a susceptible immune with the vaccine's efficacy.
- **Variants** (`update_variants`, daily): a new variant emerges with a hazard
  proportional to world infections (`variant_rate`; more replication, more
  mutation; Otto et al. 2021), up to four per pathogen; each multiplies R0 by
  `variant_r0_gain` (Alpha, Delta; Davies et al. 2021; Campbell et al. 2021)
  and returns `variant_escape` of the immune to susceptible. Journalled
  (`journal_variant`).
- **WHO surveillance** sharing raises the detection that transparent members
  see (`who_detection` × media freedom; IHR 2005;
  [06](06-geopolitics-ai.md#international-organisations)).
- **Emergent epidemics.** A novel epidemic emerges at random (about 2% a year
  for a COVID-scale event; Marani et al. 2021): an unseeded `emergent`
  template starts in a nation drawn by population.

**Planned:** air-travel coupling between regions, variant-specific
severity and vaccine escape.

## Migration

**Implemented** (`update_migration`, 28 days): Harris–Todaro moves toward
higher expected income net of a moving cost; internal moves follow region
adjacency, are cut by movement restrictions and deterred by **housing costs**
in booming regions (regional house prices; Ganong & Shoag 2017);
international moves follow a gravity term scaled by diaspora networks
(Massey et al. 1993; Beine, Docquier & Özden 2011) and the destination's
immigration openness (`lever::immigration`), cut by travel controls at both
ends and by a **migration deal** with the origin
([06](06-geopolitics-ai.md#agreements-and-rebels)); war and occupation push
refugees, scaled by the destination's **asylum openness** (`lever::asylum`;
Hatton 2009). Migrants arrive as job seekers, join the immigrant group and
remit; salient immigration feeds attitudes ([values](#values-and-opinion)).
Distance and language terms, disaster-driven displacement and
skill-specific wage effects below are planned.

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

Plus issue salience weights over seven issues: economy, health, security,
identity, immigration (with migrant inflows), environment (with warming and
self-expression values) and corruption (made visible online in hard times);
salient immigration pulls values toward tradition (Hainmueller & Hopkins
2014), and the issues enter voting
([05](05-politics-institutions.md#parliaments-and-elections)).

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
([05](05-politics-institutions.md#the-overton-window)). Formative-years
values enter with new cohorts ([Demography](#demography)); explicit cohort
replacement and a regional media graph are planned.

## Unrest: the "temperature"

Instability comes from **Turchin's structural-demographic theory** (Goldstone
1991; Turchin 2003, 2016) combined with **relative deprivation** (Gurr 1970).

**Political Stress Index** per nation, concentrated by region:

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

- **Regional PSI**: the national index concentrated where grievance is,
  `psi_region = PSI × (g_r + 0.02) / (g_nation + 0.02)` clamped to 0.5–2, so
  its population-weighted mean stays near the national value (Turchin 2016
  applied to regions; cf. Goldstone et al. 2010 on local instability). The
  drive uses the regional value.
- **Fiscal resentment**: regions with low sub-national fiscal capacity
  ([03](03-economy.md#fiscal)) add to the drive (Boadway & Shah 2009).
- **Regional trust**: national social trust eroded locally by unrest
  (`trust_region_unrest`; Putnam 1993); computed and reported, not yet read by
  other systems.

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

- **Repression** (`lever::repression`, [05](05-politics-institutions.md#policy-catalogue))
  raises the cost of joining at once but leaves a memory that feeds later
  backlash and erodes legitimacy (Davenport 2007; Francisco 1995); it is
  capped by the military's loyalty. Naming and shaming by the Human Rights
  Council follows high repression
  ([06](06-geopolitics-ai.md#international-organisations)).

## Social capital and demography

Implemented in `update_social_capital` (28 days per nation):

- **Social trust**, a slow stock (Putnam 1993; Knack & Keefer 1997) starting
  from World Values Survey wave 7 / EVS: falls with rising inequality, unrest
  and corruption and recovers slowly. It raises tax compliance
  ([03](03-economy.md#fiscal)) and NPI compliance (Bargain & Aminjonov 2020).
  Cyber operations also lower it.
- **Crime** (Becker 1968): property crime rises with unemployment and poverty
  (Raphael & Winter-Ebmer 2001) and deters investment; **police** spending
  above baseline (`lever::police`) cuts it (`police_crime` elasticity 0.3;
  Levitt 1997; Chalfin & McCrary 2017).
- **Fertility and education:** the demographic transition and educational
  expansion ([Demography](#demography)).
- **CPI per pop:** food weighs more in poorer budgets (Engel's law), so food
  inflation hits the poor harder.

Planned: regional trust raising contract enforcement (TFP); crime by
region; surveillance as a policy.
