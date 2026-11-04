#include "sim/backtest.hpp"

#include "core/csv.hpp"
#include "core/defs.hpp"

#include <cmath>
#include <cstdio>
#include <map>

namespace geosim::sim {

namespace {

struct tolerance {
    double abs = 0;        // |model - real| <= max(abs, rel * |real|)
    double rel = 0;
    double log_factor = 0; // alternatively: max(model, real) / min(model, real) <= factor
    double min_pass = 0.75;
};

double model_value(nation_summary const& s, std::string const& metric) {
    if(metric == "gdp_growth_pct") return s.gdp_growth_pct;
    if(metric == "unemployment_pct") return s.unemployment_pct;
    if(metric == "inflation_pct") return s.inflation_pct;
    if(metric == "gov_net_lending_pct") return s.gov_net_lending_pct;
    if(metric == "deaths_per_million") return s.deaths_per_million;
    return NAN;
}

bool within(double model, double real, tolerance const& t) {
    if(t.log_factor > 0) {
        const double a = std::max(model, 1.0);
        const double b = std::max(real, 1.0);
        return std::max(a, b) / std::min(a, b) <= t.log_factor;
    }
    return std::abs(model - real) <= std::max(t.abs, t.rel * std::abs(real));
}

} // namespace

backtest_result run_backtest(world_summary const& summary, std::string const& assets_dir, std::string const& year, std::ostream& out) {
    const std::string tol_path = assets_dir + "/backtest/" + year + "_tolerances.txt";
    std::map<std::string, tolerance> tols;
    std::vector<std::string> metric_order;
    tolerance oil_tol;
    double oil_real = 0.0;
    for(auto const& node : core::load_defs(tol_path)) {
        tolerance t;
        t.abs = node.num_or("abs", 0.0, tol_path);
        t.rel = node.num_or("rel", 0.0, tol_path);
        t.log_factor = node.num_or("factor", 0.0, tol_path);
        t.min_pass = node.num_or("min_pass", 0.75, tol_path);
        const std::string metric(node.req("metric", tol_path));
        if(metric == "oil_price_mean") {
            oil_tol = t;
            oil_real = node.num_or("real", 0.0, tol_path);
        } else {
            tols[metric] = t;
            metric_order.push_back(metric);
        }
    }

    const auto targets = core::csv_table::load(assets_dir + "/backtest/" + year + "_targets.csv");
    backtest_result result;
    std::map<std::string, metric_result> by_metric;
    for(auto const& m : metric_order) {
        by_metric[m] = metric_result{m, 0, 0, tols[m].min_pass};
    }

    char line[256];
    std::snprintf(line, sizeof(line), "%-5s", "iso3");
    out << line;
    for(auto const& m : metric_order) {
        std::snprintf(line, sizeof(line), " | %22s", m.c_str());
        out << line;
    }
    out << "\n" << std::string(5 + metric_order.size() * 25, '-') << "\n";

    for(std::size_t row = 0; row < targets.rows(); ++row) {
        const std::string iso(targets.get(row, "iso3"));
        nation_summary const* s = nullptr;
        for(auto const& ns : summary.nations) {
            if(ns.iso3 == iso) s = &ns;
        }
        if(!s) continue;
        std::snprintf(line, sizeof(line), "%-5s", iso.c_str());
        out << line;
        for(auto const& m : metric_order) {
            auto real = targets.number(row, m);
            const double model = model_value(*s, m);
            if(!real) {
                std::snprintf(line, sizeof(line), " | %8.1f vs %8s   ", model, "n/a");
            } else {
                const bool pass = within(model, *real, tols[m]);
                by_metric[m].total += 1;
                by_metric[m].passed += pass ? 1 : 0;
                std::snprintf(line, sizeof(line), " | %8.1f vs %8.1f %c ", model, *real, pass ? ' ' : '*');
            }
            out << line;
        }
        out << "\n";
    }
    for(auto const& m : metric_order) {
        result.metrics.push_back(by_metric[m]);
    }
    if(oil_real > 0.0) {
        metric_result oil{"oil_price_mean", within(summary.oil_price_mean, oil_real, oil_tol) ? 1 : 0, 1, 1.0};
        std::snprintf(line, sizeof(line), "\nBrent mean: model %.1f vs real %.1f USD/bbl\n", summary.oil_price_mean, oil_real);
        out << line;
        result.metrics.push_back(oil);
    }
    out << "\n";
    for(auto const& m : result.metrics) {
        std::snprintf(line, sizeof(line), "%-22s %2d/%2d within band (need %3.0f%%) %s\n", m.metric.c_str(), m.passed, m.total,
                      m.required_share * 100.0, m.ok() ? "PASS" : "FAIL");
        out << line;
    }
    out << "('*' = outside tolerance band)\n";
    return result;
}

} // namespace geosim::sim
