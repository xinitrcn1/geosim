# 05 — Politics & institutions

## Regimes are continuous, not categorical

The game has no `government_type` enum. A nation's regime is a vector of
continuous indices, modelled on **V-Dem** (Coppedge et al.) so the 2020 state
can be initialised from real data:

| Index | Meaning | Mechanical effect |
|-------|---------|-------------------|
| Electoral | free, fair, regular elections | size of the selectorate that actually matters; whether elections can remove leaders |
| Liberal | judicial independence, legislative constraints | veto-player strength; how strong "Force" overrides look |
| Participatory | civil society, direct democracy | protest channelled into politics vs into riots |
| Media freedom | censorship, media pluralism | opinion-graph topology, information quality for AI agents |
| Rule of law / corruption | legal predictability | tax compliance, TFP, sovereign spread |
| State capacity | can the state implement policy | policy effectiveness multiplier, tax collection |

These indices are **stocks** that drift. They slowly erode under forced
overrides, repression, corruption and crisis, and slowly recover under
stable rule-following. Sudden changes (coups, revolutions, constitutional
reforms) are events.

## Selectorate theory: why regimes behave differently

Bueno de Mesquita et al. (2003). Each nation has:

- **S** — selectorate: people with any formal say in choosing leaders
- **W** — winning coalition: the subset whose support keeps the leader in power

Both are derived from the regime indices and the class/identity make-up of
pops. Implemented today: W follows the electoral index only; small W tilts
fiscal packages toward firms and raises coup risk. S and the pop make-up are
planned. Consequences:

- Large W (democracies) → leaders survive by providing **public goods**
  (growth, health, education), which reach many people.
- Small W (autocracies) → leaders survive by providing **private goods** to
  coalition members (rents, positions). The loyalty norm W/S is small, so
  defection is risky for members.
- This is the main **pragmatic** term in head-of-state utility
  ([06](06-geopolitics-ai.md)).

## Agents in a nation

| Agent | Powers (legal) | Typical utility | Status |
|-------|----------------|-----------------|--------|
| Head of state / executive | decrees within limits, appointments, foreign policy, emergency powers | stay in office, coalition welfare, ideology | implemented (`agent_kind::executive`) |
| Cabinet / ministries (finance, interior, foreign, health) | budget proposals, regulation, NPIs | the executive's utility + portfolio priorities | planned (the executive pulls these levers today) |
| Parliament parties | vote on bills, budgets and treaties; no-confidence | seats, voters' welfare weighted by electorate, ideology | implemented as `party` objects voting by rule, not as agents |
| Central bank | policy rate, QE | mandate (π, gap), independence | implemented (`agent_kind::central_bank`); FX intervention and macroprudential planned |
| Regional governors (federal states) | regional budgets, local NPIs, police | regional voters, party line | planned |
| Courts | strike down forced levers (scales with judicial independence) | legal consistency | implemented as a rule ([Courts](#courts)), not an agent |
| Union federation | wage demands, strikes | members' real income | implemented (`agent_kind::union_federation`) |
| Firm boards | labour hoarding, investment | profit, varieties of capitalism, state ownership | implemented (`agent_kind::board`, one per large-firm cohort; [03](03-economy.md#firms)) |
| Sector associations / lobbies | lobbying, investment strikes | members' income | planned |
| Military / security services | loyalty, coup, defection | institutional interest, budget | implemented (`agent_kind::military`; [Military](#military)) |

The player can override any implemented agent
([01](01-gameplay-loop.md#3-act--overrides)).

## Parliaments and elections

**Implemented** (`politics.cpp`, 28 days):

- **Parties from data.** Real party systems from `parties_curated.csv`
  (positions from the Chapel Hill Expert Survey 2019, Global Party Survey and
  V-Party; results from ParlGov), in a two-dimensional space: economic
  left-right and GAL-TAN. Nations without rows get three generic parties.
- **Voting.** Salience-weighted spatial distance to each party (Downs 1957)
  plus retrospective economic and health voting for the incumbent
  (Lewis-Beck & Stegmaier 2000), logit choice; turnout by kind.
- **Elections** on the national calendar: proportional seats (pr) or regional
  pluralities (fptp). Government = minimal connected winning coalition in
  left-right order (Axelrod 1970); the winner's position reshapes the
  executive's ideology.
- **Adaptive parties** (Laver 2005; Laver & Sergenti 2011): strategies
  *sticker* (niche and ruling parties keep their position; Adams et al. 2006),
  *aggregator* (mainstream parties drift to their supporters' centroid),
  *hunter* (challengers keep moving while their vote grows) and *predator*.
- **Challenger entry and exit.** A challenger forms when many voters are far
  from every party (de Vries & Hobolt 2020; Meguid 2005); a party below 1% of
  the vote for two years dissolves.

**Planned:** party identification inertia, mixed and two-round systems,
explicit seat formulas with thresholds (D'Hondt / Sainte-Laguë), portfolio
bargaining, and initial positions from the Manifesto Project.

## The Overton window

Implemented per nation and value axis (`politics.cpp`):

- **Centre and width.** Centred on public opinion; wider with polarisation
  (the dispersion of public values) and fragmented media (social-media
  connectivity).
- **Normalisation.** The centre is pulled toward the governing party's
  position: success of a once-extreme position makes it sayable (Bursztyn,
  Egorov & Fiorin 2020; Kuran 1995; Noelle-Neumann 1974).
- **Stigma.** Voters penalise parties outside the window; mainstream parties
  stay just inside it.
- **Legitimacy cost.** Policy outside the window lowers the legitimacy target.
  The government's policy position is its party's position moved by what it
  does (fiscal expansion and directed credit lean left; movement and media
  restrictions lean toward the order end).
- **Status-quo baseline.** The public accepts the policies it already lives
  with; only moves beyond the starting distance count (Samuelson & Zeckhauser
  1988).
- **Rally effect.** Under a salient threat the public accepts restrictions it
  otherwise would not (Mueller 1970; Bol et al. 2021); the acceptance fades
  with prolonged restrictions.
- **Backlash.** Liberty-restricting policy beyond the window adds protest
  drive ([04](04-society.md#unrest-the-temperature)).
- **Thermostat.** Public opinion moves against the direction of economic
  policy (Wlezien 1995; Soroka & Wlezien 2010).

## Legislation

Implemented in `institutions.cpp` (28 days). Levers set by law go through
the legislature as **bills** (`enact`). These are the household tax
scale, public investment, the minimum wage, defence spending, tariffs and
cross-border internet filtering. Fiscal crisis packages and emergency measures
(NPIs, domestic information control) stay executive decisions, anchored in 2020 on
what was decided (sanction escalations need ratification,
[06](06-geopolitics-ai.md#two-level-games)).

- **Support.** Each party's seats support a bill if it governs (discipline) or
  if the bill moves policy toward the party's position; a salient threat adds
  cross-party support (crisis legislation).
- **Veto players.** Passing needs a majority plus a premium per unit of
  veto-player strength (bicameralism, presidential veto, federalism; Henisz
  POLCON; Tsebelis 2002), from `politics_curated.csv`.
- **Partial passage.** A bill short of support passes in part: the change the
  pivotal veto players accept.
- **Decree.** Executives without strong legislative constraints can decree
  part of the rest; regimes without competitive legislatures decree.
- **No-confidence and snap elections.** In parliamentary systems a minority
  government, or one whose bills keep failing, can lose a confidence vote; a
  snap election follows (about 60 days later).

Every bill, confidence vote and court ruling is journalled as a `decision`.

## Courts

An independent judiciary (liberal index) strikes down levers forced beyond
their legal range, with a monthly probability. Planned: court agents and
appointments.

## Military

A military agent's **loyalty** follows legitimacy, its budget relative to the
start, war losses and its ideological distance to the executive (Powell 2012;
Svolik 2012). **Coups** need low loyalty and an opportunity (unrest, low
legitimacy, a small winning coalition). Facing mass protest beyond ~3.5%
participation, a disloyal military **defects** and the executive falls
(Chenoweth & Stephan 2011): a **regime change** to more liberal, less
order-minded rulers with legitimacy reset, and elections where they exist.
Defence spending (`lever::defense`) is a bill: pragmatic from external threat
and war, ideological for order-minded rulers.

## Unions

A union-federation agent per nation sets a real-wage demand
(`lever::wage_push`) applied through bargaining coverage; real-wage losses
where unions are strong feed strikes ([03](03-economy.md#labor-market)).

## Legitimacy

Legitimacy per nation (Weber; Lipset 1959) combines:

- **Performance legitimacy** (welfare delivered vs expectations)
- **Procedural legitimacy** (rule-following, elections, use of Force overrides)
- **Identity legitimacy** (does the pop see the state as "theirs")

Implemented: procedural (electoral + liberal indices) and performance
(grievance) components; forced overrides erode legitimacy and the liberal
index and raise the capture index; policy outside the Overton window costs
legitimacy. Identity legitimacy and per-pop legitimacy are planned.

Legitimacy scales tax compliance, sovereign spreads, unrest, military loyalty
and coup risk; conscription willingness is planned.

## Policy catalogue

Each policy is a parameter vector, not a boolean. **Implemented levers**
(`enum class lever` in `src/sim/model.hpp`, plus one lever per OxCGRT NPI
indicator):

| Lever | Agent | Notes |
|-------|-------|-------|
| NPIs (one per OxCGRT indicator, with a targeted share) | executive | containment, travel, testing, tracing, income support |
| `fiscal` | executive | fiscal packages, % of GDP |
| `stw_coverage`, `stw_replacement` | executive | job retention (short-time work) |
| `tti` | executive | test-trace-isolate suppression strategy |
| `directed_credit` | executive | state-bank lending |
| `info_control` | executive | restrictions on online media and messaging |
| `net_filtering` | executive | filtering of foreign internet services |
| `tariff` | executive | average import tariff |
| `tax_scale` | executive (bill) | household tax level |
| `public_investment` | executive (bill) | above-baseline public investment |
| `min_wage` | executive (bill) | minimum wage relative to the start |
| `defense` | executive (bill) | defence spending, % of GDP |
| `oil_target` | executive of producers | crude production target |
| `policy_rate`, `asset_purchases` | central bank | rate and QE |
| `hoarding`, `investment` | board | labour hoarding, investment drive |
| `wage_push` | union federation | real-wage demand |

Sanctions, war, treaties and coups are decisions outside the lever enum
([06](06-geopolitics-ai.md)).

**Planned** (the long-term catalogue):

- **Fiscal:** statutory income tax bands, VAT, corporate tax, subsidies
  (fuel, food, sectors), transfer rules (pensions, unemployment insurance,
  cash transfers, child benefits), public investment by type.
- **Labor:** employment protection, union law, retirement age, immigration
  rules.
- **Monetary/financial:** capital controls, peg changes; bank regulation,
  deposit insurance.
- **Social:** education spending and reach, health spending, housing policy,
  minority rights, religious law.
- **Health:** vaccination priority, border health checks.
- **Security:** police budgets, repression level, surveillance.
- **Environment:** carbon price, energy-mix mandates, subsidies.
- **Constitutional:** electoral system, term limits, federalism, court
  appointments. Changing these needs supermajorities or Force.
