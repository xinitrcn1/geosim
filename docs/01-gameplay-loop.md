# 01 — Gameplay loop

The whole game rests on one idea: **the player acts, the world reacts, and
the player can see exactly how and why.** Every other system exists to make
that loop richer, more believable or easier to read.

```
        ┌──────────────────────────────────────────────────────────┐
        │                                                          │
        ▼                                                          │
  1. OBSERVE ──► 2. HYPOTHESISE ──► 3. ACT ──► 4. SIMULATE ──► 5. ATTRIBUTE
  map filters     "if I cut fuel      override    days → years     "Why?" trace
  alerts          subsidies, the      an agent                     counterfactual
  journal         deficit falls but                                journal review
                  the north riots"
```

## 1. Observe

The world runs without the player: every agent is AI-controlled. The player
watches through:

- **Map filters** ([07](07-map-and-filters.md)). Each filter shows a *level*, a
  **Δ since date** (change since a date the player picks, by default the last
  decision), a per-capita or total value, or a rank.
- **Alerts**: threshold crossings (unemployment +2 pp in a quarter, Political
  Stress Index above the riot threshold, sovereign spread > 500 bp, epidemic
  R > 1.5, …). Alerts can pause the game automatically.
- **Inspector**: click a region, nation, stratum or agent to see its current
  state, trends (sparklines) and what the AI agent intends to do next.

## 2. Hypothesise

The game helps the player form expectations before they commit:

- **Agent intent preview**: before a decision, the inspector shows what the AI
  agent *would* do and the utility terms behind it (pragmatic vs ideological;
  see [06](06-geopolitics-ai.md)).
- **Static incidence estimate**: for fiscal and price actions, a fast
  first-order estimate of who pays, by stratum and region, computed from
  current ledger shares. It is clearly marked as an *estimate*, not the outcome.

## 3. Act — overrides

Every decision-making agent in the sim can be overridden: head of state,
cabinet/ministries, parliamentary parties, central bank, regional governors,
sector associations / large-employer blocs, unions. The player can hold any
number of overrides across any number of nations.

| Level | What the player does | Institutional friction | Cost |
|-------|----------------------|------------------------|------|
| **Advise** | Shift the agent's utility weights (e.g. inflation vs employment for a central bank) | Full: the agent still chooses | None directly |
| **Direct** | Pick any action that is *legal for that agent* | Full: bills still need votes, courts can strike, governors can refuse | Political capital of the agent (approval, coalition cohesion) |
| **Force** | Pick actions *beyond* the agent's legal powers (rule by decree, ignore a court, bypass parliament) | Bypassed | Erodes institutional stocks (rule of law, legitimacy, judicial independence); raises the nation's *capture index* |

Rules:

- Overrides are **actions, not state edits**. The player can never set GDP or
  approval directly, only the levers real actors hold.
- Holding several agents in the same nation (e.g. executive + parliament
  majority + central bank) removes institutional friction. This is visible:
  the **capture index** rises, and domestic strata (legitimacy), markets
  (risk premium) and foreign AI (trust) all react to it.
- An overridden agent keeps computing its own preferred action every cycle.
  The "Why?" panel shows *"AI would have chosen X (U = 0.62) — you chose Y
  (U = 0.31)"*, so the player always knows how far they have moved from
  what that actor would naturally do.
- Releasing an override hands control back to the AI, which then reacts to the
  world the player left behind.

## 4. Simulate

Effects show up on **three time scales**, and the UI says which one a number
belongs to:

| Scale | Lag | Examples |
|-------|-----|----------|
| Immediate | days–weeks | prices, sovereign spread, protests, approval, infections |
| Medium | months–2 years | employment, wages, inflation pass-through, migration, credit, elections |
| Structural | years–decades | human capital, fertility, capital stock, values drift, institutions, climate |

The structural lag is deliberate. Many good long-run policies (education,
institutions) cost something in the short run and pay off after the agent who
enacted them has lost office. Many short-run fixes (subsidies, printing money,
repression) do damage that shows up years later.

## 5. Attribute — seeing *why*

This is the game's signature feature. There are three tools, each more
expensive than the last:

### 5.1 The "Why?" trace (always on, cheap)

Every number in the UI can be clicked. Because the economy is a
**stock-flow-consistent ledger** ([02](02-simulation-core.md#accounting-core)),
any change in a stock breaks down exactly into flows, and every flow carries a
**cause tag** (a policy, a decision ID, an event, or "endogenous"). Example:

```
Region: Lombardy · Stratum: low-skill service workers · Real disposable income  −4.1% (Δ since 2020-03-01)
  wages                       −6.3%   ← sector output: hospitality −38% [cause: NPI level 3, decision #0412]
  unemployment benefit        +2.9%   ← furlough scheme [cause: policy cassa_integrazione, decision #0415]
  consumer prices (CPI-p)     −0.4%   ← food +2.1%, energy −9.0% [cause: world oil price, endogenous]
  taxes                       +0.2%
  remittances                 −0.5%
```

The trace can be followed recursively ("why did hospitality output fall?").
Cause tags are kept at limited resolution: the top-K tagged contributors per
flow plus an "other" bucket, so memory stays bounded.

### 5.2 Counterfactual fork (on demand, background)

The sim is deterministic ([02](02-simulation-core.md#determinism)), so the
player can fork the state at any decision in the journal and re-run without
that decision (or with an alternative). The fork runs on a worker thread at
reduced detail (monthly cadence, aggregated strata). The result is a
**with/without** overlay: any filter can display `actual − counterfactual`.

This is how the player answers "did my policy cause this, or would it have
happened anyway?". It is the most honest attribution tool there is, and it
reflects how economists actually evaluate policy.

### 5.3 Decision journal (passive)

Each override and each significant AI decision is logged with a snapshot of
tracked indicators. The journal reviews them automatically at **+1 month,
+6 months, +2 years and +10 years**, so long-lag effects come back to the
player's attention after they have forgotten the original decision.

## Trade-offs: why "you can't do everything good"

Trade-offs are **emergent**. Nothing is scripted with "choice A gives +X
happiness −Y money". They come from four structural sources:

1. **Conservation.** The SFC ledger means spending is always financed by
   taxes, borrowing or money creation, and each of these has a cost: deadweight
   loss, crowding out and spreads, or inflation.
2. **Distributional conflict.** Strata differ in where their income comes from
   (wages, profits, rents, transfers) and how they spend it (Stone–Geary
   subsistence shares). Almost every price or policy change helps some strata
   and hurts others, and they vote, protest and migrate.
3. **Time inconsistency.** Short-run and long-run effects of the same lever
   often have opposite signs (Kydland & Prescott 1977). Agents with short
   tenure discount the future heavily.
4. **Strategic response.** Other nations retaliate (tariffs, sanctions),
   markets reprice risk, and domestic opposition mobilises.

The **COVID-19 opening** is the first dilemma and the tutorial: lockdown
intensity trades infections and deaths against income loss (concentrated on
low-skill service strata), and fiscal support trades that income loss against
debt and future spreads. Every option has a cost.

## Outcomes, not a score

There is no victory score. The **mandate panel** lets the player pin up to
eight indicators per nation they control and set target bands. Default
dashboard (all available per region/nation/stratum):

- Median real disposable income, GDP per capita (PPP)
- Gini, Palma ratio, poverty headcount ($2.15/day and national line)
- Unemployment, labor force participation
- Life expectancy, infant mortality, excess deaths
- Mean years of schooling (human capital)
- Democracy/liberty indices, rule of law, legitimacy
- Political Stress Index (stability)
- Public debt/GDP, sovereign spread, inflation
- CO₂ emissions, climate damages

Scenarios may define objectives. The sandbox has none.

## Session flow and pacing

- Speeds: pause, 1 (1 day/s), 2, 3, 4, max. Auto-pause on alerts the player chooses.
- A typical session: watch an alert → inspect → open the trace → override →
  run 1–6 months → review in the journal → release or extend the override.
- In long campaigns the journal and the Δ-since-date filters keep consequences
  readable across decades.
