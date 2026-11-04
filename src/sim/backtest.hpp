#pragma once
// Compares a run's summary with real outcomes (assets/backtest/<year>_targets.csv)
// using tolerance bands from assets/backtest/<year>_tolerances.txt.

#include "sim/report.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace geosim::sim {

struct metric_result {
    std::string metric;
    int passed = 0;
    int total = 0;
    double required_share = 0;
    [[nodiscard]] bool ok() const noexcept {
        return total == 0 || static_cast<double>(passed) >= required_share * static_cast<double>(total) - 1e-9;
    }
};

struct backtest_result {
    std::vector<metric_result> metrics;
    std::string error; // unreadable tolerance or target files
    [[nodiscard]] bool ok() const noexcept {
        if(!error.empty()) return false;
        for(auto const& m : metrics) {
            if(!m.ok()) return false;
        }
        return true;
    }
};

// Writes a per-nation comparison table to `out`. Targets come from
// <year>_targets.csv (generated) and, when present, <year>_targets_curated.csv
// (hand-written columns, merged by iso3).
backtest_result run_backtest(world_summary const& summary, std::string const& assets_dir, std::string const& year, std::ostream& out);

} // namespace geosim::sim
