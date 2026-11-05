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
| Cabinet / ministries (finance, interior, foreign, health) | budget proposals, regulation, NPIs | the executive's utility + portfolio priorities | implemented as portfolios (`policy.cpp`): ministers' parties set the ideology of their levers; the executive agent still pulls them |
| Parliament parties | vote on bills, budgets and treaties; no-confidence | seats, voters' welfare weighted by electorate, ideology | implemented as `party` objects voting by rule, not as agents |
| Central bank | policy rate, QE, FX intervention, countercyclical buffer | mandate (π, gap), independence | implemented (`agent_kind::central_bank`; FX and macroprudential tools in `reforms.cpp`) |
| Regional governors (multi-region nations) | local NPIs | regional incidence, party line | implemented (`agent_kind::governor`, the region's plurality party); regional budgets and police planned |
| Courts | strike down forced levers (scales with judicial independence) | legal consistency | implemented as a rule with appointed seats ([Courts](#courts)), not an agent |
| Union federation | wage demands, strikes | members' real income | implemented (`agent_kind::union_federation`) |
| Firm boards | labour hoarding, investment | profit, varieties of capitalism, state ownership | implemented (`agent_kind::board`, one per large-firm cohort; [03](03-economy.md#firms)) |
| Sector associations / lobbies | lobbying for protection | members' income | implemented as a pressure stock (`lobby_pressure`, `policy.cpp`), not an agent; investment strikes planned |
| Military / security services | loyalty, coup, defection | institutional interest, budget | implemented (`agent_kind::military`; [Military](#military)) |
| Rebels | insurgency strength, negotiation | the excluded group's inclusion | implemented (`agent_kind::rebel`, `agreements.cpp`; [06](06-geopolitics-ai.md#agreements-and-rebels)) |

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
  (Lewis-Beck & Stegmaier 2000), logit choice; turnout by kind. Voters judge
  performance against a reference that adapts over `vote_memory_years`
  (myopic retrospection; Healy & Lenz 2014) and resets when the government
  changes (a new government is not blamed for its predecessor's record); the
  effect saturates at `vote_retrospective_cap`, since partisans discount bad
  news about their own side (Bartels 2002). The incumbent is placed partly by
  what it does (`thermostat`: the government's economic policy position,
  in which fiscal packages fade over `stimulus_memory_years`).
- **Issue salience** over seven issues (economy, health, security, identity,
  immigration, environment, corruption) follows the crisis of the moment.
  Identity, immigration and environment weight the GAL-TAN axis; salient
  immigration pulls voters toward traditional values (Hainmueller & Hopkins
  2014); corruption made salient by hard times penalises the incumbent.
- **Party identification** (Green, Palmquist & Schickler 2002): pops keep a
  share `party_id_inertia` of last month's support, so vote intention moves
  gradually.
- **Elections** on the national calendar with explicit seat formulas
  (`electoral_system`, `pr_threshold`, `smd_share` in `politics_curated.csv`):
  list PR by D'Hondt over a 500-seat chamber among parties above the legal
  threshold (Turkey 10%, Germany 5%; Lijphart 1994, Gallagher 1991); FPTP:
  each region's seats (in proportion to its population) split on its vote by
  the seat-vote curve of its many districts, flattened by geographic sorting
  (`district_exponent`), or the cube law in single-region nations (Kendall &
  Stuart 1950; Taagepera 1986); two-round systems with a softer exponent
  (Blais & Loewen 2009); parallel mixed systems (Japan, Italy, Korea, Mexico)
  as an `smd_share` blend. Government = minimal connected winning coalition in
  left-right order (Axelrod 1970); the winner's position reshapes the
  executive's ideology.
- **Presidential elections** (`presidential` systems): the executive is
  elected separately, by a runoff between the top two first-round parties
  where none has a majority (second preferences from each pop's support;
  Shugart & Carey 1992; Blais, Massicotte & Dobrzynska 1997). The president's
  party forms the cabinet and governs with the legislature it gets (divided
  government).
- **Term limits** (`politics_ext.cpp`; `term_limit_years` in
  `nations_ext_curated.csv`): a president re-elected at the limit steps down
  and a new leader of the same party takes over (personal rigidity resets
  toward the party norm); where liberal institutions are weak the incumbent
  evades the limit with probability (1 − liberal) × `term_evasion`, at a cost
  to liberal institutions and judicial independence (Versteeg et al. 2020;
  Baturo 2014). Journalled (`journal_term_limit`).
- **Cabinets** (`policy.cpp`): portfolios (finance, health, interior, foreign)
  go to governing parties in proportion to seats (Gamson's law; Laver &
  Shepsle 1996). The finance minister's party sets the ideology of pensions
  and public-service spending, the interior minister's that of repression,
  immigration and capital controls.
- **Governors** of multi-region nations (the region's plurality party) set
  regional NPI stringency relative to the national level - pragmatically with
  local incidence, ideologically by their order ideal (partisan federalism).
- **Lobbies**: associations of tradable sectors losing ground relative to the
  rest of the economy (import competition, not a general slump; Trefler 1993)
  buy protection - an ideological tariff term (Grossman & Helpman 1994).
- **Adaptive parties** (Laver 2005; Laver & Sergenti 2011): strategies
  *sticker* (niche and ruling parties keep their position; Adams et al. 2006),
  *aggregator* (mainstream parties drift to their supporters' centroid),
  *hunter* (challengers keep moving while their vote grows) and *predator*.
- **Challenger entry and exit.** A challenger forms when many voters are far
  from every party (de Vries & Hobolt 2020; Meguid 2005); a party below 1% of
  the vote for two years dissolves.

**Planned:** compensatory mixed systems with overhang seats, portfolio
bargaining beyond Gamson's law (ministerial salience), constitutional changes
to term limits by supermajority, and initial positions from the Manifesto
Project.

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
- **Thermostat.** The public reacts against the direction of economic policy
  (Wlezien 1995; Soroka & Wlezien 2010) through vote choice: the incumbent is
  placed between its party's position and its policy (`thermostat`). Values
  themselves do not move with it (doing so fed back through the window and
  drove voters away from whichever party governed).
- **Party anchors.** Mainstream (aggregator) parties move toward their voters
  only part of the way from their core position (`party_anchor`; activists
  and primaries, Aldrich 1983; Adams & Merrill 2006), so two-party systems do
  not collapse onto the median. Pops' support follows each party when parties
  are reordered, enter or dissolve.

## Legislation

Implemented in `institutions.cpp` (28 days). Levers set by law go through
the legislature as **bills** (`enact`). These are the household tax
scale, public investment, the minimum wage, defence spending, tariffs,
cross-border internet filtering, the carbon price, immigration, the
retirement age, education and health spending, police and R&D spending, VAT,
corporate and wealth taxes, employment protection and the renewables
mandate. Fiscal crisis packages and emergency measures
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

An independent judiciary strikes down levers forced beyond their legal
range, with a monthly probability. **Judicial independence** is a stock that
starts at the liberal index and changes through **appointments**
(`politics_ext.cpp`, monthly): seats fall vacant at random (mean
`court_vacancy_years`); executives in liberal systems appoint professionals
(independence drifts back toward the institutional level), while low-liberal
executives **pack the court**, cutting independence by `court_capture` per
vacancy (Helmke 2017; Ginsburg & Moustafa 2008; Hungary, Poland, Turkey).
Journalled (`journal_court_seat`). Planned: court agents with their own
ideal points.

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

Implemented: legitimacy targets 0.4 procedural (electoral + liberal
indices) + 0.4 performance (grievance) + 0.2 **identity**; forced overrides
erode legitimacy and the liberal index and raise the capture index; policy
outside the Overton window, repression and being named and shamed by the
Human Rights Council cost legitimacy. **Identity legitimacy**
(`politics_ext.cpp`) falls as the exclusion and mobilisation of identity
groups rise above 2019 (`identity_weight`; Gilley 2009; Wimmer 2002) and
rises with power-sharing deals. Per-pop legitimacy enters only through each
pop's grievance; a per-pop stock is planned.

Legitimacy scales tax compliance, sovereign spreads, unrest, military loyalty
and coup risk. **Willingness to serve** (consent to fight; Levi 1997;
Horowitz, Simpson & Stam 2011) moves toward a target raised by legitimacy
and external threat above 2019 and by occupation, and lowered by war
weariness (war cost; Mueller 1973); it scales committed force on the front
(`willingness_war`) and raises the war-settlement hazard when low
([06](06-geopolitics-ai.md#coercion-aid-arms-and-treaties)).

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
| `carbon_price` | executive (bill) | USD/t CO2 ([03](03-economy.md)) |
| `repression` | executive | 0..1; deters protest at once (cost of joining), leaves a memory that feeds backlash and erodes legitimacy (Davenport 2007; Francisco 1995; Chenoweth & Stephan 2011); capped by military loyalty |
| `immigration` | executive (bill) | openness relative to 2019; pragmatic with labour shortages, ideological by the interior minister's TAN shift; scales the migration gravity term |
| `retirement_age` | executive (bill) | years relative to 2019; pragmatic as debt rises above its 2019 level; cuts pension outlays |
| `education`, `health` | executive (bill) | % of GDP above baseline; education raises the tertiary share, health adds hospital beds; health is pragmatic in an epidemic |
| `capital_controls` | executive | 0..1; pragmatic in a sudden stop outside reserve-currency issuers (Rey 2013); cut portfolio flows both ways |
| `regional_npi` | governor | regional NPI stringency relative to national |
| `police` | executive (bill) | % of GDP above baseline; pragmatic with crime and unrest, more for order-minded rulers and traditionalist interior ministers; cuts crime (Levitt 1997; Chalfin & McCrary 2017) |
| `asylum` | executive | refugee admission openness relative to 2019; humanitarian, held back by joblessness and traditionalist interior ministers; scales refugee arrivals (Hatton 2009) |
| `vat`, `corporate_tax` | executive (bill) | pp shifts; consolidation raises VAT first (Alesina, Favero & Giavazzi 2019); weak investment invites corporate tax cuts (Devereux, Lockwood & Redoano 2008), the left raises them |
| `wealth_tax` | executive (bill) | % a year on top wealth; the left and rising inequality; base erodes with avoidance (Jakobsen et al. 2020; Saez & Zucman 2019) |
| `rd` | executive (bill) | public R&D, % of GDP above baseline; growth programmes and technology races against a rising threat; raises trend growth (Romer 1990; Ugur et al. 2016) |
| `subsidy` | executive | share of the energy and food price rise absorbed; a populist answer to price spikes, larger where the winning coalition is small; costs the budget and is regressive (Coady et al. 2015) |
| `epl` | executive (bill) | OECD EPL index points; flexibilisation under high unemployment, resisted by strong unions and the left (Boeri & Jimeno 2005) |
| `energy_mandate` | executive (bill) | 0..1 renewables mandate; follows carbon concern and green governments; speeds clean-capacity diffusion (Johnstone, Hascic & Popp 2010) |
| `vaccine_priority` | executive | 0..1 share of doses to the old first, more where health weighs more (Bubar et al. 2021) |
| `fx_intervention` | central bank | 0..1 reserve sales against depreciation faster than 10% a year, while reserves last (Fratzscher et al. 2019) |
| `macroprudential` | central bank | countercyclical capital buffer (pp) against credit above its 2019 path; released at once in a bust (Basel III CCyB; Cerutti, Claessens & Laeven 2017) |

Every lever is relative to the calibrated 2019 setting, with pragmatic and
ideological targets in the usual blend ([06](06-geopolitics-ai.md#agent-utility-pragmatism-vs-ideology));
strategic plan steps bias several of them
([06](06-geopolitics-ai.md#planning-structure)).

Sanctions, war, treaties and coups are decisions outside the lever enum
([06](06-geopolitics-ai.md)).

**Planned** (the long-term catalogue):

- **Fiscal:** statutory income tax bands, sector subsidies, transfer rules
  (pensions, unemployment insurance, cash transfers, child benefits), public
  investment by type, fiscal equalisation as a lever (today a fixed share).
- **Labor:** union law.
- **Monetary/financial:** peg changes; deposit insurance, other bank
  regulation.
- **Social:** housing policy, minority rights, religious law.
- **Health:** border health checks.
- **Security:** surveillance.
- **Environment:** clean-energy subsidies.
- **Constitutional:** electoral system, term-limit amendments, federalism.
  Changing these needs supermajorities or Force.
