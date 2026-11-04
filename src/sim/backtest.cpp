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
    std::string pathogen; // deaths_per_million: restrict to one pathogen
};

double model_value(nation_summary const& s, std::string const& metric, std::string const& pathogen) {
    // Excess deaths are scored against all of the model's deaths (reported or not).
    if(metric == "excess_deaths_per_million" && !pathogen.empty()) {
        for(auto const& [key, v] : s.true_deaths_by_pathogen) {
            if(key == pathogen) return v;
        }
        return NAN;
    }
    if(metric == "deaths_per_million" && !pathogen.empty()) {
        for(auto const& [key, v] : s.deaths_by_pathogen) {
            if(key == pathogen) return v;
        }
        return NAN;
    }
    if(metric == "gdp_growth_pct") return s.gdp_growth_pct;
    if(metric == "unemployment_pct") return s.unemployment_pct;
    if(metric == "inflation_pct") return s.inflation_pct;
    if(metric == "gov_net_lending_pct") return s.gov_net_lending_pct;
    if(metric == "deaths_per_million") return s.deaths_per_million;
    if(metric == "yield10_end_pct") return s.yield10_end_pct;
    if(metric == "credit_growth_pct") return s.credit_growth_pct;
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
    // World-level metrics (one value each): name -> (tolerance, real value).
    struct world_metric {
        std::string name;
        tolerance tol;
        double real = 0.0;
    };
    std::vector<world_metric> world_metrics;
    backtest_result result;
    core::diagnostics diag;
    for(auto const& node : core::load_defs(tol_path, diag)) {
        tolerance t;
        t.abs = node.num_or("abs", 0.0, tol_path, diag);
        t.rel = node.num_or("rel", 0.0, tol_path, diag);
        t.log_factor = node.num_or("factor", 0.0, tol_path, diag);
        t.min_pass = node.num_or("min_pass", 0.75, tol_path, diag);
        if(auto pk = node.find("pathogen")) t.pathogen = pk->text;
        const std::string metric(node.req("metric", tol_path, diag));
        if(auto real = node.num("real", tol_path, diag)) {
            world_metrics.push_back({metric, t, *real});
        } else {
            tols[metric] = t;
            metric_order.push_back(metric);
        }
    }

    const auto targets = core::csv_table::load(assets_dir + "/backtest/" + year + "_targets.csv", diag);
    // Optional hand-written target columns, merged by iso3.
    const std::string curated_path = assets_dir + "/backtest/" + year + "_targets_curated.csv";
    std::map<std::string, std::map<std::string, double>> curated;
    {
        core::diagnostics optional;
        const auto c = core::csv_table::load(curated_path, optional);
        if(!optional.failed()) {
            for(std::size_t row = 0; row < c.rows(); ++row) {
                for(auto const& col : c.header()) {
                    if(col == "iso3") continue;
                    if(auto v = c.number(row, col)) curated[std::string(c.get(row, "iso3"))][col] = *v;
                }
            }
            if(c.failed()) diag.fail(c.error());
        }
    }
    if(diag.failed()) {
        result.error = diag.error();
        out << "back-test: " << result.error << "\n";
        return result;
    }
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
            if(!real) {
                auto it = curated.find(iso);
                if(it != curated.end() && it->second.count(m)) real = it->second.at(m);
            }
            const double model = model_value(*s, m, tols[m].pathogen);
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
    out << "\n";
    for(auto const& wm : world_metrics) {
        double model = NAN;
        if(wm.name == "oil_price_mean") model = summary.oil_price_mean;
        if(wm.name == "freight_index_mean") model = summary.freight_index_mean;
        if(wm.name == "freight_index_end") model = summary.freight_index_end;
        if(wm.name == "fleet_idle_peak") model = summary.fleet_idle_peak;
        const bool pass = std::isfinite(model) && within(model, wm.real, wm.tol);
        result.metrics.push_back(metric_result{wm.name, pass ? 1 : 0, 1, 1.0});
        std::snprintf(line, sizeof(line), "%-22s model %8.2f vs real %8.2f %c\n", wm.name.c_str(), model, wm.real, pass ? ' ' : '*');
        out << line;
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
