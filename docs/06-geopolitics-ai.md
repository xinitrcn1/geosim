# 06 — Geopolitics & AI

The AI should act like real states: mostly pragmatic, sometimes principled,
held back by domestic politics, and weighing ideals against survival.

## Agent utility: pragmatism vs ideology

**Implemented** (`src/sim/systems/ai.cpp`). Every lever of every agent
(executive, central bank, board, union federation, military;
[05](05-politics-institutions.md#agents-in-a-nation)) is chosen by
maximising a quadratic utility

```
U(x) = −(1 − ρ)(x − P)² − ρ(x − I)² − κ(x − A)²
```

where `P` is a pragmatic target computed from the simulated situation, `I`
the agent's ideological ideal and `A` the historical anchor (a dated
real-world decision, present only when one exists and `historical_anchors` is
on). The optimum is the precision-weighted mean, so the agent tracks history
while the simulation tracks history and drifts toward `P` as they diverge.
ρ falls under threat. Autonomous jurisdictions blend their choice with their
sovereign's; central banks of pegged currencies follow the anchor. Player
overrides (advise / direct / force) replace or bias the choice, and every
decision is journalled with its `P`, `I` and `A` for the "Why?" panel.

**Target form (partly implemented).** Every agent `a` scores candidate actions `x` with:

```
U_a(x) = (1 − ρ_a) · U_prag_a(x) + ρ_a · U_ideo_a(x) − C_a(x)
```

### Pragmatic utility `U_prag`

A weighted sum of *forecast* changes over the agent's planning horizon `H_a`
(short for insecure leaders; Kydland–Prescott time inconsistency):

| Term | Theory |
|------|--------|
| Probability of staying in office | Selectorate theory (Bueno de Mesquita et al. 2003) |
| Welfare of the winning coalition W | same; public vs private goods split by W/S |
| Relative power and security | Structural realism (Waltz 1979; Mearsheimer 2001): balance of power, threat perception |
| Absolute gains from interdependence | Liberal institutionalism (Keohane & Nye 1977) |
| Regime security (coups, unrest) | PSI forecast ([04](04-society.md#unrest-the-temperature)) |

### Ideological utility `U_ideo`

The negative distance between the forecast world state and the agent's
**ideal point** on value dimensions: human rights, sovereignty, market vs
state, religious law, nationalism, environmentalism, alignment with ideological
blocs. Party agents take their ideal point from their party position.
Heads of state take it from their party plus a personal offset.

### Ideological rigidity ρ (dynamic)

```
ρ_a = ρ_base_a · security_a^k · (1 − crisis_severity)^m
```

**Implemented** (`strategy.cpp: rigidity`): security is the lower of approval
and legitimacy relative to 2019; crisis severity is the executive's epidemic
threat; `k = rho_security_k`, `m = rho_crisis_m`.

- `ρ_base`: the agent's disposition (initialised per real government's record).
- **Secure agents can afford principles.** When an agent's hold on power is
  threatened, or a crisis hits, ρ falls and pragmatism takes over.
- This produces familiar behaviour: buying energy from ideological rivals
  during a crisis, dropping human-rights conditions for security deals,
  populist policy reversals before elections.

### Costs `C`

Implementation cost, political capital (coalition cohesion), and
**commitment and reputation costs**: breaking a treaty lowers that nation's
credibility in every relationship (Tomz 2007, reputation in sovereign
debt and alliances).

## Forecasting inside the AI

Implemented: executives project the epidemic over a short horizon
(`ai_horizon_days`) from detected cases and their growth; other pragmatic
targets read the current simulated state. Agents cannot run the full sim for
each candidate action, so they use (`strategy.cpp`):

1. **Linearised response tables**: per-nation sensitivities (∂indicator/∂lever)
   estimated cheaply at runtime from recent sim history by rolling regression,
   refreshed monthly. These are *imperfect on purpose*: AI agents have bounded
   rationality, and they misjudge things the way real governments do.
   **Implemented for fiscal policy:** a lagged Bayesian regression of next
   month's output-gap change on this month's package, shrunk toward a prior
   multiplier (`response_prior`, weight `response_prior_weight`; Blanchard &
   Leigh 2013); a weak learned multiplier enlarges packages within ±20%.
2. **Information quality** (implemented): perceived epidemic incidence carries
   weekly lognormal noise that grows as media freedom and state capacity fall,
   and autocrats under-see unrest (`info_bias`) when deciding repression
   (Wintrobe 1998, the dictator's dilemma; Kuran 1991).

## Planning structure

**Implemented:** executives choose levers weekly (sharded by nation), central
banks daily (emergency meetings happen), and the slower levers (credit, QE,
tariffs, taxes, wages, defence, boards) on their systems' 28-day or weekly
cadences; sanctions, war and treaties are decided in `geopolitics.cpp` and
`treaties.cpp`. A **strategic layer** (`strategy.cpp`, monthly; HTN-lite)
scores a goal catalogue against 2019 - contain an epidemic, restore growth,
regime security, deter a threat, consolidate, secure energy - and adopts the
best above `goal_threshold` with hysteresis; a goal decomposes into tactical
biases (`goal_gain`) on its levers: public investment, pension reform,
repression, defence, health spending, and softer sanctions on energy
suppliers. Goal changes are journalled (`journal_goal`).

**Planned:** full HTN decomposition into multi-step plans, and the layers below.

- **Strategic layer (monthly, sharded):** goal selection with an HTN-style
  decomposition. Example: `secure energy` → `diversify suppliers` →
  {`sign LNG deal with X`, `build terminal`, `subsidise renewables`}. Goals
  come from a utility-scored catalogue.
- **Tactical layer (weekly):** utility-scored actions under the current goals.
- **Reactive layer (daily):** crisis responses (rate emergency, NPIs, border
  closure, mobilisation) triggered by alerts.
- Each layer stores its top candidates and their utility breakdown. The
  "Why?" panel shows them ([01](01-gameplay-loop.md#3-act--overrides)).

## Two-level games

Putnam (1988): every international agreement must win at two tables.

- **Level I:** negotiation between nations' executives (bargaining over a
  deal vector, e.g. tariff cuts, aid amounts, emission targets).
- **Level II:** domestic ratification. The deal goes to parliament/coalition
  agents, who vote with their own utilities. The **win-set** (deals that would
  ratify) is computed before offers, so executives with narrow win-sets can
  credibly demand more ("my parliament won't accept this").
- **Implemented in part:** sanction escalations need legislative ratification
  in competitive systems (`geopolitics.cpp`), and domestic bills face veto
  players ([05](05-politics-institutions.md#legislation)). Level I bargaining
  over deal vectors and win-set computation are planned.
- When the player overrides *only* the executive, they still face Level II.
  Overriding parliament as well removes it, at a capture-index cost.

## Diplomacy

**Relations** per nation pair (asymmetric, nation arrays indexed by the
partner; `geopolitics.cpp`, 28 days):

| Field | Driven by | Status |
|-------|-----------|--------|
| Trust | alliances, regime distance, sanctions, claims, war; cyber operations | implemented |
| Trade share | gravity with sea-route distances and contiguity ([03](03-economy.md#trade)) | implemented |
| Threat perception | partner's relative power (CINC-like GDP and population index) × proximity × distrust × claims (Waltz 1979; Jervis 1978) | implemented |
| Sanctions, alliance, FTA, war | decisions below | implemented |
| Energy dependence | share of oil demand supplied by each exporter (gravity over trade shares); raises the cost of sanctions beyond its 2019 level (Keohane & Nye 1977; Farrell & Newman 2019) | implemented |
| Credibility | lost by abandoning an ally at war, recovered over `credibility_years`; lowers every partner's trust (Tomz 2007; Gibler 2008; Crescenzi 2018) | implemented |
| Affinity (diaspora) | migrant stocks (grown by flows) raise trust | implemented; language and religion planned |
| Dependence via debt holdings, remittances | | planned |

**Implemented instruments:**

- **Sanctions** per partner: pragmatic (security gain vs trade dependence),
  ideological (regime distance), the status quo as anchor; escalations need
  ratification. Tariff rises invite retaliatory sanction steps.
- **Treaties** (`treaties.cpp`, 28 days), as relation flags rather than
  treaty objects: **alliances** form against a shared threat between states
  that trust each other and are not too far apart politically (balance of
  threat; Walt 1987) and dissolve when trust collapses; **free-trade
  agreements** form between interdependent, trusting, politically close
  partners, their trade gain phasing in over a decade (Baier & Bergstrand
  2007); **IMF programmes** start when a sovereign's spread blows out: the
  Fund lends from the rest of the world, spreads fall (catalytic effect;
  Corsetti, Guimaraes & Roubini 2006) and the executive must consolidate
  (conditionality; Dreher 2006), for three years.

**Planned:** treaty objects with terms vectors and compliance history beyond
alliances, conditional aid and debt relief, recognition and UN votes, World
Bank programmes, climate agreements and migration deals (see
[Coercion, aid, arms and treaties](#coercion-aid-arms-and-treaties) for what is
implemented).

**Systemic effects** implemented as war-hazard modifiers:

- Democratic peace (Russett 1993; Oneal & Russett 1999)
- Commercial peace: trade dependence lowers conflict
- Alliances deter; alliance entrapment and security dilemmas (Jervis 1978)
  enter through threat and trust

## Information and the internet

Implemented in `internet.cpp`. The internet is political infrastructure.

- **Starting controls by one rule.** Every nation starts with the online
  controls its society already lives with: the Freedom on the Net 2020 score
  sets domestic **information control** and, above a threshold of unfreedom,
  cross-border **filtering**, by the same rule for every nation. A "great
  firewall" is just a high filtering level that a capable, order-minded state
  can sustain.
- **Levers.** `lever::info_control` (restrictions on online media and
  messaging; an order-minded executive raises it when unrest climbs) and
  `lever::net_filtering` (filtering of foreign services and content;
  pragmatic against foreign protest contagion).
- **Coupling** (daily) between two societies = connectivity × openness at both
  ends × ties (trade, plus a floor for language, diaspora and media links).
- **Protest contagion:** coupled foreign protest adds to the domestic protest
  drive (Bamert, Gilardi & Wasserfallen 2015; Hussain & Howard 2013);
  filtering cuts it ([04](04-society.md#protest-cascade)).
- **Value diffusion** (28 days): pops move toward the publics they are coupled
  to, within bounded confidence (Gleditsch & Ward 2006).
- **Costs of filtering:** digitally delivered services trade falls with
  filtering above the start (Freund & Weinhold 2004), and so does trend growth
  (Czernich et al. 2011).
- **Cyber operations** (28 days): capable states attack rivals below the
  threshold of war (Valeriano & Maness 2015; Buchanan 2016). The hazard rises
  with the attacker's capability (skills, state capacity, connectivity), the
  pair's threat and distrust, and the target's exposure; filtering defends a
  little. Effects are modest (Rid 2012; Gartzke 2013; Kostyuk & Zhukov 2019):
  days of lost capacity in energy and business services, lower trust, and
  influence operations that add to the target's protest drive.

## Conflict

**Bargaining model of war** (Fearon 1995). War is costly for both sides, so
a peaceful bargain always exists in theory. War breaks out only through:

1. **Private information and incentives to misrepresent.** Modelled by the
   gap between perceived and true relative power (AI information quality).
2. **Commitment problems.** Rapid shifts in relative power (a rising state, a
   closing window) make deals non-credible.
3. **Issue indivisibility.** Territorial claims (`region_claim`) carry an
   indivisibility weight.

**Implemented** (`geopolitics.cpp`, 28 days). War onset is a hazard drawn from
the counter-based RNG, and it is **emergent from the economy**:

- **Rents:** the hazard rises with the target's resource rents (rapacity;
  Collier & Hoeffler 2004; Dube & Vargas 2013).
- **Diversion:** domestic strain raises it in regimes that face no election
  (Levy 1989; Tir 2010).
- **Power transition:** gaps in realised growth shift power (Organski &
  Kugler 1980; Fearon's commitment problem).
- **Commerce:** trade dependence lowers it (Oneal & Russett 1999).

**Military capability** is built on the economy: output, industry and a
technology base for **precision strike and drones** (Horowitz 2010; Fuhrmann
& Horowitz 2017). Precision strike raises the destruction an attacker
inflicts and reaches energy infrastructure across the defender, not only the
front, scaled by the attacker's edge (Calcara et al. 2022 caution that
defences adapt). Wars cut trade, destroy capacity in claimed and capital
regions, kill, close chokepoints whose littoral states fight, and end with a
probability rising with their cost. Belligerents go on a **war footing**:
military procurement from domestic manufacturing (`war_spending`, a share of
GDP).

**Insurgency** (civil war): sustained insurgency-level unrest can turn into
armed rebellion where the state is weak, groups are excluded and the young
lack jobs (Fearon & Laitin 2003; Cederman, Wimmer & Min 2010; Collier &
Hoeffler 2004), with deaths and destruction in the region.

### Disputes and crisis bargaining

**Implemented** (`diplomacy.cpp`, monthly per pair). Wars are the rare end of
an escalation ladder, the Correlates of War MID hostility levels (2 threat,
3 display, 4 use of force, 5 war; Jones, Bremer & Singer 1996). The war
hazard above opens **militarised disputes** `mid_ratio` times as often, more
among rivals. Each month an open dispute escalates, ends when one side backs
down, or is settled (one outcome per month; hazards normalised):

- **Escalation** rises with the stakes (claims, resource rents, rivalry) and
  poor information, and is lower in democratic pairs (Fearon 1995; Senese &
  Vasquez 2008).
- **Nuclear deterrence**: two nuclear powers almost never take the last step to
  war (`nuclear_war_factor`) but skirmish more below it - the
  stability-instability paradox (Snyder 1965; Jervis 1989). A nuclear
  defender deters the final step against a non-nuclear challenger. Arsenals
  come from SIPRI 2020 (`conflict_curated.csv`).
- **Audience costs**: the weaker side (by CINC) backs down more often; a
  democratic leader backs down less once she has escalated publicly, and her
  opponent more (tying hands; Fearon 1994; Schultz 2001; Weeks 2008 on
  autocratic audiences). Backing down costs legitimacy and some credibility.
- **Mediation**: settlement grows with the best third party trusted by both
  and with leverage (Beardsley 2011; Kydd 2003). Mediation also speeds the
  settlement of wars.
- **Rivalry**: every dispute deepens the pair's rivalry (strategic rivalries
  from Thompson & Dreyer 2012 in `rivalries_curated.csv`), which raises future
  dispute hazards, lowers trust and drives **arms races** (rivals' build-ups
  are matched in `decide_defense`; Richardson 1960); rivalry decays over
  decades (Diehl & Goertz 2000).

### Coercion, aid, arms and treaties

- **Sanctions bargaining**: a target concedes with a hazard that grows with its
  export dependence on the sender, more readily to friends (the sanctions
  paradox; Drezner 1999) and as a democracy (Lektzian & Souva 2007); the
  sender lifts them and trusts it more (Hufbauer et al. 2007).
- **Intervention in civil wars**: strong allies and trusting partners back the
  government (faster insurgency end), rivals back the rebels (slower) (Regan
  2002; Cunningham 2010; Salehyan 2009).
- **Crisis aid**: rich donors (`aid_income`) give to trusted partners in need -
  occupation, insurgency, IMF programmes, disasters - through the
  rest-of-world accounts (`flow::aid`; Alesina & Dollar 2000; Strömberg 2007).
- **Arms transfers**: a share of procurement above baseline buys abroad from
  suppliers weighted by capability, trust and alliance; sanctions and war
  exclude suppliers (`flow::arms`; SIPRI arms transfers; Kinsella 1994).
- **FTA ratification** (two-level game; Putnam 1988; Milner 1997): an FTA
  signed by executives must pass both legislatures, where protectionist
  lobbies narrow the win-set; failures are journalled.

**Implemented:**

- **CINC** (Singer, Bremer & Stuckey 1972): the mean of world shares in
  military expenditure, personnel (population × defence share until COW NMC
  data), energy use, iron and steel (manufacturing output), population and
  urban population.
- **Alliance obligations**: at onset, each ally of either side decides whether
  to join - base `alliance_honor` ≈ 75% (Leeds 2003), higher for democracies
  (Gaubatz 1996), for allies the enemy threatens and for strong allies, lower
  for allies with a record of defection. Declining costs credibility and the
  victim's trust (`journal_alliance_call`).
- **Fronts** (monthly): control shares of each side's territory move by the
  Lanchester square law on CINC × war effort (Lanchester 1916; Epstein 1985),
  with a loss-of-strength gradient over distance (Boulding 1962) and a
  defender's multiplier on home ground (Mearsheimer 1989; Biddle 2004).
  Occupied territory produces nothing for its owner, pushes refugees, and a
  decisive front raises the settlement hazard (Fearon 1995; Slantchev 2003;
  Reiter 2009). At peace, unclaimed land returns; claimed land freezes
  (Fortna 2004; Vasquez 1993).

**Planned:** region-level front lines with terrain; rebel sides drawn from
high-grievance pops as agents.

## Calibration of AI personalities (2020 start)

Per-nation `ρ_base`, ideal points and planning horizons are initialised from
public datasets (V-Dem, party manifestos, UN General Assembly voting ideal
points by Bailey, Strezhnev & Voeten 2017) and then tuned so that 2020–2023
back-tests produce realistic aggregate behaviour (sanction coalitions,
lockdown timing distributions). Exact replication of history is **not** a goal.
