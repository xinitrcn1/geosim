# 06 — Geopolitics & AI

The AI should act like real states: mostly pragmatic, sometimes principled,
held back by domestic politics, and weighing ideals against survival.

## Agent utility: pragmatism vs ideology

Every agent `a` (head of state, party, central bank, …) scores candidate
actions `x` with:

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

Agents cannot run the full sim for each candidate action. They use:

1. **Linearised response tables**: per-nation sensitivities (∂indicator/∂lever)
   estimated cheaply at runtime from recent sim history by rolling regression,
   refreshed monthly. These are *imperfect on purpose*: AI agents have bounded
   rationality, and they misjudge things the way real governments do.
2. **Information quality**: forecasts get noisier with low media freedom and
   low state capacity (autocracies have worse information about their own
   society; Wintrobe 1998, the dictator's dilemma).

## Planning structure

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
- When the player overrides *only* the executive, they still face Level II.
  Overriding parliament as well removes it, at a capture-index cost.

## Diplomacy

**Relations matrix** per nation pair (asymmetric):

| Field | Driven by |
|-------|-----------|
| Trust | treaty compliance history, ideological distance, regime similarity |
| Dependence | trade share, energy/commodity reliance, debt holdings, remittances |
| Threat perception | relative power (CINC), proximity, past conflict, alliance alignment |
| Affinity | identity ties (language, religion, diaspora), shared institutions |

**Instruments:** alliances (defensive, with credibility), trade agreements,
sanctions (sectoral, financial, individual), aid (conditional/unconditional),
loans and debt relief, arms transfers, recognition and UN votes, IMF/World Bank
programmes (conditionality = imposed policy vectors), climate agreements,
migration deals.

**Systemic effects** implemented as conflict-propensity modifiers:

- Democratic peace (Russett 1993; Oneal & Russett 1999)
- Commercial peace: interdependence lowers conflict
- Alliance entrapment and security dilemmas (Jervis 1978)

## Conflict

**Bargaining model of war** (Fearon 1995). War is costly for both sides, so
a peaceful bargain always exists in theory. War breaks out only through:

1. **Private information and incentives to misrepresent.** Modelled by the
   gap between perceived and true relative power (AI information quality).
2. **Commitment problems.** Rapid shifts in relative power (a rising state, a
   closing window) make deals non-credible.
3. **Issue indivisibility.** Some territorial and identity issues carry an
   indivisibility weight in ideological utility.

**Power** is measured with a CINC-like index (Correlates of War: military
spending and personnel, industry, energy use, population, urban population).

**Combat is abstract**: front lines are region-level control shares, updated
weekly by attrition (a Lanchester-style ratio of committed power, terrain and
logistics). The point is the fallout: capital destruction, casualties,
refugee flows, sanctions, commodity shocks, debt and changes in values. These
are all visible through the normal filters.

**Internal conflict** (insurgencies, civil wars) uses the same model, with the
rebel side drawn from pops with high grievance and identity distance.

## Calibration of AI personalities (2020 start)

Per-nation `ρ_base`, ideal points and planning horizons are initialised from
public datasets (V-Dem, party manifestos, UN General Assembly voting ideal
points by Bailey, Strezhnev & Voeten 2017) and then tuned so that 2020–2023
back-tests produce realistic aggregate behaviour (sanction coalitions,
lockdown timing distributions). Exact replication of history is **not** a goal.
