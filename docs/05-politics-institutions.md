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
pops. Consequences:

- Large W (democracies) → leaders survive by providing **public goods**
  (growth, health, education), which reach many people.
- Small W (autocracies) → leaders survive by providing **private goods** to
  coalition members (rents, positions). The loyalty norm W/S is small, so
  defection is risky for members.
- This is the main **pragmatic** term in head-of-state utility
  ([06](06-geopolitics-ai.md)).

## Agents in a nation

| Agent | Powers (legal) | Typical utility |
|-------|----------------|-----------------|
| Head of state / executive | decrees within limits, appointments, foreign policy, emergency powers | stay in office, coalition welfare, ideology |
| Cabinet / ministries (finance, interior, foreign, health) | budget proposals, regulation, NPIs | the executive's utility + portfolio priorities |
| Parliament parties | vote on bills, budgets and treaties; no-confidence | seats, voters' welfare weighted by electorate, ideology |
| Central bank | policy rate, QE, FX intervention, macroprudential | mandate (π, gap), independence |
| Regional governors (federal states) | regional budgets, local NPIs, police | regional voters, party line |
| Courts | strike down actions (probabilistic, scales with judicial independence) | legal consistency |
| Sector associations / unions | lobbying, strikes, investment strikes | members' income |
| Military / security services | loyalty or coup | institutional interest, budget |

The player can override any of these ([01](01-gameplay-loop.md#3-act--overrides)).

## Parliaments and elections

- **Parties** sit at positions in the value space ([04](04-society.md#values-and-opinion))
  with issue emphases (initialised from the Manifesto Project / ParlGov
  datasets).
- **Voting.** Each pop's vote shares come from:
  - spatial proximity in weighted value space (Downs 1957; Enelow & Hinich);
  - retrospective economic voting: the incumbent is rewarded or punished for
    real income and unemployment change (Lewis-Beck & Stegmaier 2000);
  - party identification inertia;
  - turnout propensity by class and age.
- **Electoral systems.** FPTP (district plurality over regions), PR (D'Hondt /
  Sainte-Laguë with thresholds), mixed and two-round systems. Seat translation
  is explicit, so electoral reform is a meaningful lever.
- **Coalition formation.** Minimal connected winning coalitions in policy
  space (Axelrod 1970), bargaining over portfolios.
- **Legislation.** Bills need majorities. Veto players (Tsebelis 2002), such
  as a second chamber, a presidential veto or courts, determine how far and
  how fast policy can move. More veto players mean stability but slow reform.
- **Polling** runs monthly. Elections follow the national calendar, plus
  snap elections and no-confidence votes.

## Legitimacy

Legitimacy per nation and per pop (Weber; Lipset 1959) combines:

- **Performance legitimacy** (welfare delivered vs expectations)
- **Procedural legitimacy** (rule-following, elections, use of Force overrides)
- **Identity legitimacy** (does the pop see the state as "theirs")

Legitimacy scales tax compliance, unrest thresholds, conscription willingness
and resistance to coups.

## Policy catalogue (initial)

Grouped as the levers that agents (and the player through them) can pull.
Each policy is a parameter vector, not a boolean.

- **Fiscal:** income tax bands, VAT, corporate tax, tariffs, subsidies
  (fuel, food, sectors), transfers (pension rules, unemployment insurance,
  cash transfers, child benefits), public investment by type.
- **Labor:** minimum wage, employment protection, union law, furlough
  schemes, retirement age, immigration rules.
- **Monetary/financial:** (central bank) rate, QE, capital controls, peg;
  (government) bank regulation, deposit insurance.
- **Social:** education spending and reach, health spending, housing policy,
  minority rights, religious law, media regulation.
- **Health:** NPI level by region, vaccination priority, border health checks.
- **Security:** police and military budgets, repression level, surveillance.
- **Environment:** carbon price, energy-mix mandates, subsidies.
- **Constitutional:** electoral system, term limits, federalism, court
  appointments. Changing these needs supermajorities or Force.
