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
    double deaths_per_million = 0;  // reported, cumulative
    double gini_start = 0;
    double gini_end = 0;
    double population_change_pct = 0; // vs the scenario start
    double net_migration_per_1000 = 0;
    double unrest_mean = 0;           // population-weighted regional unrest (0 calm .. 4 insurgency)
    double psi = 0;                   // Political Stress Index
};

struct world_summary {
    double oil_price_mean = 0;
    std::vector<nation_summary> nations;
};

[[nodiscard]] world_summary summarize(game_state const& state);

// Call once before the run: snapshots the starting government balance.
void begin_measurement(game_state& state);

// The "Why?" ledger: a nation's year-to-date flows by kind, as % of nominal GDP.
void write_flows(game_state const& state, dcon::nation_id n, std::ostream& out);

void write_daily_header(std::ostream& out);
void write_daily_rows(game_state const& state, std::ostream& out);

} // namespace geosim::sim
