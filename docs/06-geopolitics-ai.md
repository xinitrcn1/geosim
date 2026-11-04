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
biases (`goal_gain`) on its levers. Goal changes are journalled
(`journal_goal`).

**Plans** (HTN-lite): a goal decomposes into ordered steps that unlock every
90 days while the goal persists (`plan_bias`; `journal_plan`):

| Goal | Step 0 | Step 1 | Step 2 |
|------|--------|--------|--------|
| secure energy | softer sanctions on energy suppliers | renewables mandate | R&D |
| restore growth | public investment | R&D | corporate tax cut |
| consolidate | less public investment | VAT rise | pension reform (retirement age) |
| regime security | repression | police | |
| deter a threat | defence | R&D (technology race) | alliances (lower formation thresholds) |
| contain an epidemic | health spending | vaccine priority | |

The layers map onto cadences: strategic goals and plans monthly, tactical
lever choices weekly, reactive crisis responses (rate emergencies, NPIs)
daily. **Planned:** plans with branching methods and concrete deal steps
(`sign LNG deal with X`, `build terminal`), and storing each layer's top
candidates with their utility breakdown for the "Why?" panel
([01](01-gameplay-loop.md#3-act--overrides)); today the journal keeps the
chosen value with its `P`, `I` and `A`.

## Two-level games

Putnam (1988): every international agreement must win at two tables.

- **Level I:** negotiation between nations' executives (bargaining over a
  deal vector, e.g. tariff cuts, aid amounts, emission targets).
- **Level II:** domestic ratification. The deal goes to parliament/coalition
  agents, who vote with their own utilities. The **win-set** (deals that would
  ratify) is computed before offers, so executives with narrow win-sets can
  credibly demand more ("my parliament won't accept this").
- **Implemented:** sanction escalations need legislative ratification
  in competitive systems (`geopolitics.cpp`), domestic bills face veto
  players ([05](05-politics-institutions.md#legislation)), and FTAs are
  bargained at both levels (`treaties.cpp`). Each side's **win-set** narrows
  with protectionist lobby pressure and a thin governing majority; the
  agreement's **depth** is the Nash bargaining outcome bounded by the
  narrower win-set, and the side with the narrower win-set claims the larger
  share of the gains (`nash_split` ± the win-set gap; Schelling 1960). Depth
  scales the FTA trade gain (Dür, Baccini & Elsig 2014); the deal must then
  pass both legislatures. Bargaining over multi-issue deal vectors (aid,
  emissions) is planned.
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
| Affinity | migrant stocks (grown by flows) raise trust; a common official language (`language_trust`; Guiso, Sapienza & Zingales 2009; Melitz & Toubal 2014) and a shared majority religion (`religion_trust`) add a constant to the trust level (`nations_ext_curated.csv`) | implemented |
| Debt holdings | reserves (months of imports) held in each issuer's debt by IMF COFER reserve-currency weights; holdings beyond 2019 raise the cost of sanctioning the issuer (`debt_dependence_cost`; Farrell & Newman 2019) | implemented |
| UN voting alignment | agreement on the last General Assembly session relative to 2019 ([below](#international-organisations)) | implemented |
| Dependence via remittances | | planned |

**Implemented instruments:**

- **Sanctions** per partner: pragmatic (security gain vs trade dependence),
  ideological (regime distance), the status quo as anchor; escalations need
  ratification. Tariff rises invite retaliatory sanction steps.
- **Treaties** (`treaties.cpp`, 28 days), as relation flags:
  **alliances** form against a shared threat between states
  that trust each other and are not too far apart politically (balance of
  threat; Walt 1987) and dissolve when trust collapses; **free-trade
  agreements** form between interdependent, trusting, politically close
  partners, their trade gain phasing in over a decade (Baier & Bergstrand
  2007); **IMF programmes** start when a sovereign's spread blows out: the
  Fund lends from the rest of the world, spreads fall (catalytic effect;
  Corsetti, Guimaraes & Roubini 2006) and the executive must consolidate
  (conditionality; Dreher 2006), for three years.
- **Agreements with terms and compliance** are `treaty` objects
  ([below](#agreements-and-rebels)).

**Planned:** moving alliances and FTAs onto treaty objects, recognition of
states, terms vectors with several issues.

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

### International organisations

**Implemented** (`organisations.cpp`). Organisations are generic data, not
code: an `organisation` object (`assets/defs/organisations.txt`: key, kind,
decision rule, thresholds, seats, elected seats, term) and a `membership`
relationship to nations (role member / elected / permanent, term end, exit
date; per scenario in `organisations_curated.csv`, `ALL` = every nation).

- **Decision rules** (`org_vote`): `majority` (share of members ≥ threshold),
  `veto` (`votes_needed` yes votes and no permanent member against; UN
  Charter art. 27), `qualified` (shares of members *and* of their population;
  TEU art. 16(4): 55% / 65%) and `consensus` (no member against). Seats held
  by nations outside the scenario vote as the rest of the world.
- **Voting.** A member supports acting against a target by its UN alignment
  with the drafter minus its ties to the target (alliance, trust) plus a
  draft-specific shift; it always opposes its own case. Votes are journalled
  (`journal_unsc`: kind 0 ceasefire, 1 sanctions, 2 peace operation, 3 naming
  and shaming).
- **Memberships** (daily): exit dates end membership (the UK leaves the EU on
  2020-01-31); expired elected terms end; on 1 January scenario nations
  compete for free elected seats, larger states more often (Dreher et al.
  2014).

Behaviour by kind:

- **assembly** (UN General Assembly). Each 15 September the session votes on
  ten resolutions placed along the main dimension of UN voting, the US-led
  liberal order (Bailey, Strezhnev & Voeten 2017); each nation's ideal point
  follows its liberal institutions, self-expression values and alliance with
  the leading liberal power (the largest liberal economy), and it votes yes
  when its ideal point lies beyond a resolution's position. Pairwise
  agreement on the last session (Signorino & Ritter 1999) relative to 2019
  shifts trust and weights crisis aid.
- **security** (UN Security Council; five permanent members, ten elected
  seats with two-year terms; 2019-20 Germany, Indonesia, South Africa in the
  slice). Every 28 days wars and insurgencies put drafts on the agenda, drafted
  by the permanent member least tied to the target: a **ceasefire demand**
  once per war (raises the settlement hazard, `ceasefire_settle`; Fortna
  2004), **sanctions on an occupier** (every non-allied state applies at least
  `un_sanction_level`; Bapat & Morgan 2009) and a **peace operation** after a
  war or during an insurgency, with host consent (cuts dispute and insurgency
  recurrence by `peacekeeping_effect` for `peacekeeping_years`; Fortna 2008;
  Hultman, Kathman & Shannon 2014). Permanent members veto to shield
  themselves or clients (Voeten 2001); elected members draw more aid
  (Kuziemko & Werker 2006).
- **trade** (WTO). A member whose exports face a tariff rise above
  `wto_bound` files a complaint; the panel reports after `wto_panel_days`
  (Busch & Reinhardt 2003); the complainant usually wins (`wto_win`; Hudec
  1993; Bown 2009); the loser complies (halving the excess) with a
  probability that grows with its credibility and dependence, otherwise
  retaliation is authorised (a sanction floor) and the loser loses
  credibility. With the Appellate Body paralysed since 2019-12-11
  (`wto_appellate = 0`) the loser appeals into the void and nothing follows
  (Hoekman & Mavroidis 2021).
- **union** (EU). Members share one external tariff (customs union,
  `firms.cpp`). When the union-wide output gap exceeds `eu_fund_trigger`, the
  hardest-hit member drafts a **recovery fund** of `eu_fund_size` of EU GDP,
  financed by common borrowing from the rest of the world and allocated by
  output loss (NextGenerationEU 2020); members support it unless fiscally
  hawkish ("frugal") and little hit, and it needs a qualified majority. Grants
  are committed at once and paid after national ratification
  (`eu_fund_lag_days`) over `eu_fund_years` (`flow::eu_fund`;
  `journal_eu_fund`); no new fund for three years.
- **rights** (UN Human Rights Council; 47 elected seats, three-year terms).
  The most liberal member drafts a resolution naming and shaming a state
  whose repression is high; if adopted the target loses legitimacy
  (`shaming_legitimacy` a year while shamed) and liberal states' trust
  (Hafner-Burton 2008; Lebovic & Voeten 2006 on the politics of shaming).
- **health** (WHO). Members' surveillance sharing raises the detection that
  transparent states see (`who_detection` × media freedom, `ai.cpp`; IHR
  2005).

**Organisation flags** (`flags{...}` in the defs; `org_flag` in `model.hpp`)
give behaviour that cuts across kinds: `democracy_clause`, `sanctions`,
`non_interference`, `crisis_coordination`, `sanction_alignment`,
`counter_hegemonic`, `regime_security`, `mediation`, `customs_union`.
Memberships carry a join date and a suspension date; a suspended member does
not vote and gets none of the organisation's benefits until readmitted.

Organisations with geopolitical weight beyond the UN system
(`geo_orgs.cpp`, `megaevents.cpp`; parameters under "organisations with
geopolitical relevance"):

- **alliance** (NATO, CSTO). Members are allied pairwise, so allies join
  wars through `call_allies`. **Burden sharing:** small members free-ride
  (Olson & Zeckhauser 1966) until the alliance's threat rises; then a member
  below the defence `guideline` closes `burden_gain` of the gap
  (`alliance_burden_push` in `decide_defense`). This follows the 2014 Wales
  pledge after Crimea (Plümper & Neumayer 2015). **Enlargement:** a non-member
  applies when its threat has risen by `accession_threat` since 2019, it
  trusts the members (above 0.6), and its main threat also threatens them:
  the members' mean threat from it must be at least `accession_shared`. In
  other words, it balances against a shared threat. Accession needs consensus, so a member blocks it if its trust in the applicant is below
  `accession_trust`, or if it fears entrapment because it does not itself
  fear the applicant's threat (Snyder 1984) (North Atlantic Treaty
  art. 10; Walt 1987; Reiter 2001; Turkey on Sweden 2022). Outcomes are
  journalled as `journal_org_join`.
- **club** (G7, G20, BRICS, SCO). Summits are held once a year in
  `summit_month`, and each one raises members' trust toward 0.7 by
  `club_trust` (socialisation; Johnston 2001; Putnam & Bayne 1987).
  - `crisis_coordination` (G20, G7): when members' GDP-weighted output gap is
    deeper than `club_slump`, the summit pledges coordinated stimulus. For
    `club_stimulus_days`, members' pragmatic fiscal response rises by
    `club_stimulus` (G20 London 2009; Drezner 2014).
  - `sanction_alignment` (G7): a member's sanction on a target moves
    `sanction_alignment` of the way toward its partners' mean (Russia 2014;
    Drezner 1999).
  - `counter_hegemonic` (BRICS, SCO): sanctions between members are cut by
    `counter_shield` (Stuenkel 2015).
  - `regime_security` (SCO, CSTO): members count as friendly backers of each
    other's governments against insurgents (Ambrosio 2008; Aris 2009). The
    CSTO in Kazakhstan, January 2022, is an example.
- **cartel** (OPEC+). The members meet every 28 days.
  - Quota: below the price band (`price_low`, relative to 2019) the
    collective quota is cut by `cartel_cut_speed` × the shortfall, and above
    `price_high` it is raised.
  - Compliance falls with members' fiscal stress, meaning lost output and a
    price below the 2019 fiscal breakeven (`cartel_cheating`; Kaufmann et al.
    2004). The largest producer acts as swing producer: it complies fully and
    bears a larger share of the cuts (Fattouh & Mahadeva 2013).
  - Price war: when the others' compliance falls below `price_war_trigger`
    while quotas bind, the swing producer starts a price war and floods the
    market for `price_war_days`. A year of discipline follows, as with the
    March 2020 price war and the April cut (Fattouh & Economou 2020). OPEC's
    power stays limited (Colgan 2014).
  - Members produce to `cartel_quota` × capacity in `decide_oil` unless
    anchored to the historical record. Outcomes are journalled as
    `journal_cartel`.
- **development** (World Bank, New Development Bank). Members with GDP per
  head below `dev_income` that are in need borrow concessionally from the
  rest of the world at up to `dev_lending_gdp` of GDP a year. Need means
  output loss, insurgency or an IMF programme. Lending stays within the
  lender's annual `capacity_bn` (`flow::development_lending`; Kenny 2020;
  Humphrey 2016).
- **regional** (AU, ECOWAS, OAS, ASEAN, Arab League, Mercosur, EAEU, GCC).
  - `democracy_clause`: a coup suspends the member for `suspension_years`
    (`on_coup`, `journal_suspension`; AU Lomé Declaration 2000, ECOWAS
    Protocol 2001, Inter-American Democratic Charter 2001). Membership of such
    an organisation cuts the coup hazard by `coup_deterrence` (Powell, Lasley
    & Schiel 2016).
  - `sanctions` (ECOWAS): members also apply at least `regional_sanction` to
    the suspended member (Niger 2023).
  - `non_interference` (ASEAN): neither suspensions nor sanctions apply
    (Acharya 2001; Myanmar 2021).
  - `mediation`: the organisation mediates disputes between its members with
    score `regional_mediation` (Hansen, Mitchell & Nemeth 2008).
  - `customs_union` (EU, Mercosur, EAEU, GCC): members share one external
    tariff, the members' GDP-weighted mean (`firms.cpp`).
- **event** (FIFA, IOC): mega-events (`mega_event` objects; awarded editions
  are listed in `megaevents_curated.csv`, and later editions are awarded in
  the simulation).
  - **Award:** each edition (every `cycle_years` at `cycle_phase`) is awarded
    `lead_years` ahead. Bidders are nations whose yearly build-up cost stays
    under `bid_gdp_share` of GDP; autocracies stretch that threshold up to
    tenfold for prestige (Grix & Lee 2013). The owner favours large economies,
    rotation away from the last three hosts' regions, and hosts the members
    trust, plus a lobbying draw (Feddersen, Maennig & Zimmermann 2008; Poast
    2007).
  - **Build-up:** capital spending (`capex_bn`) is spread evenly from the
    award to the opening and paid by the government (`flow::mega_capex`;
    construction demand). It counts only relative to the start state: a
    build-up under way in 2019 is in the calibrated base, so its end shows as
    a fall in public demand (Baade & Matheson 2016; Flyvbjerg et al. 2021 on
    overruns).
  - **The event:** visitors' spending (`visitor_spend_bn` × the host's share)
    is a services export over the event's days. The host's government gains
    `mega_legitimacy` while it runs (Kavetsos & Szymanski 2010), and other
    nations' trust in the host rises by `mega_trust` afterwards (soft power).
  - **Protest:** build-up spending amid grievance drives unrest
    (`mega_protest`; Brazil 2013).
  - **Postponement:** within 90 days of the opening, the event is postponed
    once, by a year, if the host's epidemic threat exceeds
    `mega_postpone_threat`. The same happens if nations holding half the
    world's population exceed it, because their teams withdraw (Canada and
    Australia, March 2020). With no forcing, Tokyo 2020 is postponed in late
    April in both the slice and the world runs (days 115 and 119). The real
    decision came on 24 March, day 83.

**Planned:** WHO coordination beyond surveillance (vaccine pooling sits in COVAX,
[04](04-society.md#health-and-epidemics)), budget contributions, membership
applications to clubs and regional organisations (BRICS enlargement 2024),
the swing producer chosen by spare capacity rather than size, and co-hosted
bids.

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
  Lanchester square law on committed force = CINC × war effort × willingness
  to serve ([05](05-politics-institutions.md#legitimacy); Lanchester 1916;
  Epstein 1985), with a loss-of-strength gradient over distance (Boulding
  1962) and a defender's multiplier on home ground (Mearsheimer 1989; Biddle
  2004) that rugged terrain raises (`terrain`, the mountainous share; Fearon &
  Laitin 2003; Tkach 2023). War weariness (low willingness) raises the
  settlement hazard (Mueller 1973).
  Occupied territory produces nothing for its owner, pushes refugees, and a
  decisive front raises the settlement hazard (Fearon 1995; Slantchev 2003;
  Reiter 2009). At peace, unclaimed land returns; claimed land freezes
  (Fortna 2004; Vasquez 1993).

**Planned:** region-level front lines (today control shares are per nation
pair; `region.enemy_control` is reserved for them).

### Agreements and rebels

**Implemented** (`agreements.cpp`, 28 days). `treaty` objects carry a kind,
parties, terms, compliance and an active flag; signings, breaches and ends
are journalled (`journal_agreement`).

- **Climate pledges** (Paris Agreement, in force 2016): each nation pledges a
  2030 carbon price above its 2019 price, higher where self-expression values
  and liberal institutions are strong. Compliance is the price reached over
  the pledged path; below `pledge_slack` the nation loses
  `pledge_credibility` a year: pledge-and-review relies on reputation, not
  enforcement (Victor 2011; Keohane & Victor 2016).
- **Migration deals** (EU-Turkey 2016 style): a rich destination with rising
  arrivals and low asylum openness pays the poorer origin with the largest
  diaspora (`migration_deal_aid` of its GDP a year) to control departures;
  migration along that route falls to 30% (Greenhill 2010 on coercive
  engineered migration). Deals run four years.
- **Sovereign debt relief**: a nation in default restructures with its foreign
  creditors after ~6 months on average, a `debt_relief_haircut` on external
  debt (`flow::debt_relief`), and the default ends (Cruces & Trebesch 2013;
  Reinhart & Trebesch 2016).
- **Conditional aid** (World Bank / bilateral programmes): poor, not
  autocratic recipients receive aid (`aid_rate`) from their most trusting rich
  donor; it is suspended when they repress or backslide below the liberal
  level at signing (Dreher, Sturm & Vreeland 2009; Molenaers, Dellepiane &
  Faust 2015 on budget-support suspensions).
- **Rebels** (`agent_kind::rebel`): an insurgency creates a rebel agent whose
  strength grows with unrest, the exclusion and mobilisation of the most
  excluded group, rugged terrain (`ruggedness`; the insurgency hazard also
  rises with it, `ruggedness_insurgency`; Fearon & Laitin 2003) and outside
  support for the rebels, and falls with state capacity and outside support
  for the government. At a **mutually hurting stalemate** (rebel strength
  close to state capacity) the sides negotiate (`rebel_negotiate`, raised by
  mediation; Zartman 1989): a **power-sharing** deal cuts the group's
  exclusion by `power_sharing` and ends the insurgency (Hartzell & Hoddie
  2003); a rebellion whose strength falls near zero is defeated
  (`journal_rebels`).

**Planned:** rebel territorial control, several rebel groups per nation,
renegotiation of terms.

## Calibration of AI personalities (2020 start)

Per-nation `ρ_base`, ideal points and planning horizons are initialised from
public datasets (V-Dem, party manifestos, UN General Assembly voting ideal
points by Bailey, Strezhnev & Voeten 2017) and then tuned so that 2020–2023
back-tests produce realistic aggregate behaviour (sanction coalitions,
lockdown timing distributions). Exact replication of history is **not** a goal.
