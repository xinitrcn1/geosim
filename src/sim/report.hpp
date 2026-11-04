#pragma once
// Indicator extraction: annual summaries (for back-tests) and daily CSV rows.

#include "sim/game_state.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace geosim::sim {

struct nation_summary {
    std::string iso3;
    double gdp_growth_pct = 0;      // mean daily real output over the run vs 2019 annual GDP
    double unemployment_pct = 0;    // mean over the run
    double inflation_pct = 0;       // mean CPI over the run vs estimated 2019 mean CPI
    double gov_net_lending_pct = 0; // change in government balance / nominal GDP over the run
    double deaths_per_million = 0;  // reported, cumulative, all pathogens
    std::vector<std::pair<std::string, double>> deaths_by_pathogen; // reported per million, by pathogen key
    std::vector<std::pair<std::string, double>> true_deaths_by_pathogen; // all deaths (reported or not) per million
    double gini_start = 0;
    double gini_end = 0;
    double population_change_pct = 0; // vs the scenario start
    double net_migration_per_1000 = 0;
    double unrest_mean = 0;           // population-weighted regional unrest (0 calm .. 4 insurgency)
    double psi = 0;                   // Political Stress Index
    double yield10_start_pct = 0;     // 10-year government yield at the start / end of the run
    double yield10_end_pct = 0;
    double credit_growth_pct = 0;     // bank credit to the private sector, change over the run
    double credit_impulse_pct = 0;    // latest credit impulse, % of GDP per year
    double npl_pct = 0;               // non-performing loans at the end
    double exchange_rate = 1;         // real exchange rate at the end (1 = start)
};

struct world_summary {
    double oil_price_mean = 0;
    double freight_index_mean = 0;    // container spot rate (1 = start), mean over the run
    double freight_index_end = 0;
    double fleet_idle_peak = 0;       // largest idle share of the container fleet
    std::vector<nation_summary> nations;
};

[[nodiscard]] world_summary summarize(game_state const& state);

// Call once before the run: snapshots the starting government balance.
void begin_measurement(game_state& state);

// The "Why?" ledger: a nation's year-to-date flows by kind, as % of nominal GDP.
void write_flows(game_state const& state, dcon::nation_id n, std::ostream& out);

// Identity groups of a nation: size, income relative to the national mean,
// unemployment, informality, grievance.
void write_groups(game_state const& state, dcon::nation_id n, std::ostream& out);
// Chokepoints (state, incidents, queues) and the container freight market.
void write_logistics(game_state const& state, std::ostream& out);
// Journal event counts by kind (wars, disputes, coups, goals, treaties...) and the
// war / dispute pairs, for checking event rates.
void write_journal_counts(game_state const& state, std::ostream& out);

// One nation's internal state, one row per call (headless --trace ISO): for finding
// where a runaway starts (docs/13).
void write_trace_header(std::ostream& out);
void write_trace_row(game_state const& state, dcon::nation_id n, std::ostream& out);

void write_daily_header(std::ostream& out);
void write_daily_rows(game_state const& state, std::ostream& out);

} // namespace geosim::sim
