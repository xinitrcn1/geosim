// Scenario creation from text assets + calibration of the 2019 baseline.
//
// Calibration makes the starting economy a consistent steady state:
//  - final demand (C, G, I, X, M from WDI) is mapped to sectors with a RAS
//    (biproportional) fit so that sector value added matches the data;
//  - government revenue is split into household, indirect, firm and resource
//    taxes; transfers are the residual that reproduces the 2019 balance;
//  - the dividend payout makes household disposable income consistent with
//    the observed consumption share and a baseline saving rate;
//  - the within-group income dispersion reproduces the national Gini.

#include "sim/scenario.hpp"

#include "core/calendar.hpp"
#include "core/csv.hpp"
#include "core/defs.hpp"
#include "core/keys.hpp"
#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <system_error>

namespace geosim::sim {

namespace {

namespace fs = std::filesystem;

constexpr int max_sectors = 32;

struct nation_input {
    std::string iso;
    double population = 0, pop65 = 0.1, labor_force = 0, unemployment = 0.05, gdp = 0, gdp_ppp = 0;
    double trend_growth = 0.02, inflation_trend = 0.02;
    double hh_cons = 0.6, gov_cons = 0.17, investment = 0.22, exports = 0.3, imports = 0.3;
    double gov_revenue = 0.25, gov_net_lending = -0.03, gov_debt = 0.6, gini = 0.35, beds = 3.0, vulnerable = 0.2;
    double hemisphere = 1, oil_production = 0, oil_elasticity = 0, epl = 0.35, benefit_replacement = 0.3;
    double benefit_coverage = 0.15, death_reporting = 0.5, oil_take = 0.0;
    double voice = 0.0, effectiveness = 0.0; // WGI estimates (about -2.5 .. 2.5)
    double birth_rate = 0.015, death_rate = 0.008, pop014 = 0.2, tertiary = 0.2;
    double lat = 0.0, lon = 0.0;
    double remit_in = 0.0, remit_out = 0.0; // % of GDP received; USD paid per year
    std::array<double, max_sectors> va_share{};
    std::array<double, max_sectors> emp_share{};
    double urban = 0.5;
    // finance_curated.csv (defaults when the nation has no row)
    double private_credit = 0.5, hh_credit_share = 0.4, state_bank_share = 0.2, cb_independence = 0.6;
    double fx_loan_share = 0.1, fx_deposit_share = 0.1, npl = 0.03, capital_ratio = 0.16, lending_spread = 3.0;
    double yield10 = NAN, effective_rate = NAN, avg_maturity = 7.0, foreign_debt_share = 0.3, fx_debt_share = 0.1, food_cpi = 0.25;
    double coordination = 0.5, tariff = 0.05;
    double union_density = 0.12, bargaining_coverage = 0.15, min_wage_bite = 0.4, tax_progressivity = 0.08, trust = 0.25;
    double presidential = 0, veto = 0.3, defense = 2.0, carbon_price = 0.0, top1 = 0.0;
    // society_curated.csv
    double youth_share = 0.33, youth_unemp_rel = 2.5, social_media = 0.5, corruption_cpi = 50.0, emigrant_stock = 0.03, net_freedom = 60.0;
    struct group {
        std::string key;
        double share = 1, edu_rel = 1, informal_rel = 1, wage_gap = 0, unemp_rel = 1, wealth_rel = 1, excluded = 0, network = 0.3;
        bool immigrant = false;
    };
    std::vector<group> groups; // empty = one group
};

struct region_input {
    std::string id;
    std::string iso;
    double lat = 0, lon = 0, area = 0, pop_share = 1, urban = 0.5;
    std::array<double, max_sectors> va_share{};  // share of the nation's sector VA
    std::array<double, max_sectors> emp_share{}; // share of the nation's sector employment
    bool has_sectors = false;
};

struct builder {
    builder(game_state& s, std::string a, std::string sc) : state(s), assets(std::move(a)), scenario(std::move(sc)) {}
    game_state& state;
    std::string assets;
    std::string scenario;
    scenario_report report;
    core::diagnostics diag; // first error stops the build (no exceptions)
    std::map<std::string, uint8_t> sector_by_key;
    std::map<std::string, uint8_t> npi_by_key;
    std::map<std::string, nation_input> inputs;
    std::map<std::string, std::vector<region_input>> regions_of; // optional regions.csv
    std::map<std::string, dcon::region_id> region_by_id;
    std::vector<std::string> order;

    void warn(std::string msg) { report.warnings.push_back(std::move(msg)); }
    // std::filesystem without exceptions: an unreadable path counts as missing.
    static bool exists(std::string const& path) {
        std::error_code ec;
        return fs::exists(path, ec) && !ec;
    }

    sector_role parse_sector_role(std::string_view s, std::string const& src) {
        if(s == "generic") return sector_role::generic;
        if(s == "government") return sector_role::government;
        if(s == "energy") return sector_role::energy;
        if(s == "contact") return sector_role::contact;
        if(s == "manufacturing") return sector_role::manufacturing;
        diag.fail(src + ": unknown sector role '" + std::string(s) + "'");
        return sector_role::generic;
    }

    npi_role parse_npi_role(std::string_view s, std::string const& src) {
        if(s == "generic") return npi_role::generic;
        if(s == "travel") return npi_role::travel;
        if(s == "testing") return npi_role::testing;
        if(s == "tracing") return npi_role::tracing;
        if(s == "income_support") return npi_role::income_support;
        if(s == "stay_home") return npi_role::stay_home;
        if(s == "movement") return npi_role::movement;
        diag.fail(src + ": unknown npi role '" + std::string(s) + "'");
        return npi_role::generic;
    }

    // ------------------------------------------------------------ defs
    void load_params() {
        const std::string path = assets + "/defs/parameters.txt";
        if(!exists(path)) {
            return;
        }
        for(auto const& node : core::load_defs(path, diag)) {
            const double v = core::parse_number(node.text, path, node.line, diag).value_or(0.0);
            if(!set_param(state.params, node.key, v)) {
                { diag.fail(path + ":" + std::to_string(node.line) + ": unknown parameter '" + node.key + "'"); return; }
            }
        }
    }

    void load_npis() {
        const std::string path = assets + "/defs/npi.txt";
        auto& w = state.world;
        for(auto const& node : core::load_defs(path, diag)) {
            if(node.key != "npi") {
                { diag.fail(path + ":" + std::to_string(node.line) + ": expected npi{...}"); return; }
            }
            const auto id = w.create_npi_indicator();
            const std::string key(node.req("key", path, diag));
            w.npi_indicator_set_key(id, core::pack_key(key));
            w.npi_indicator_set_role(id, static_cast<uint8_t>(parse_npi_role(node.req("role", path, diag), path)));
            w.npi_indicator_set_max_level(id, static_cast<float>(node.num_or("max_level", 1.0, path, diag)));
            w.npi_indicator_set_transmission_effect(id, static_cast<float>(node.num_or("transmission_effect", 0.0, path, diag)));
            npi_by_key[key] = static_cast<uint8_t>(id.index());
        }
    }

    std::map<std::string, uint8_t> pathogen_by_key;

    void load_pathogens() {
        const std::string path = assets + "/defs/pathogens.txt";
        auto& w = state.world;
        if(!exists(path)) {
            return;
        }
        auto nodes = core::load_defs(path, diag);
        std::vector<std::pair<dcon::pathogen_id, core::def_node const*>> made;
        for(auto const& node : nodes) {
            if(node.key != "pathogen") {
                { diag.fail(path + ":" + std::to_string(node.line) + ": expected pathogen{...}"); return; }
            }
            const std::string key(node.req("key", path, diag));
            if(key.size() > 4 || pathogen_by_key.count(key)) {
                { diag.fail(path + ":" + std::to_string(node.line) + ": pathogen key must be unique and at most 4 characters"); return; }
            }
            const auto id = w.create_pathogen();
            auto num = [&](char const* k, double d) { return static_cast<float>(node.num_or(k, d, path, diag)); };
            w.pathogen_set_key(id, core::pack_key(key));
            w.pathogen_set_r0(id, num("r0", 2.0));
            w.pathogen_set_incubation_days(id, num("incubation_days", 4.0));
            w.pathogen_set_infectious_days(id, num("infectious_days", 5.0));
            w.pathogen_set_death_delay_days(id, num("death_delay_days", 14.0));
            w.pathogen_set_ifr_young(id, num("ifr_young", 0.001));
            w.pathogen_set_ifr_old(id, num("ifr_old", 0.01));
            w.pathogen_set_old_contact_factor(id, num("old_contact_factor", 0.6));
            w.pathogen_set_seasonality(id, num("seasonality", 0.2));
            w.pathogen_set_waning_days(id, num("waning_days", 0.0));
            w.pathogen_set_hosp_share(id, num("hosp_share", 0.03));
            w.pathogen_set_detection_ratio(id, num("detection_ratio", 0.1));
            w.pathogen_set_npi_sensitivity(id, num("npi_sensitivity", 1.0));
            w.pathogen_set_initial_immunity(id, num("initial_immunity", 0.0));
            w.pathogen_set_vaccine_days(id, num("vaccine_days", 0.0));
            w.pathogen_set_vaccine_efficacy(id, num("vaccine_efficacy", 0.0));
            w.pathogen_set_vaccine_ramp_days(id, num("vaccine_ramp_days", 270.0));
            w.pathogen_set_emergent(id, node.num_or("emergent", 0.0, path, diag) > 0.0 ? 1 : 0);
            w.pathogen_set_first_seen(id, -1);
            pathogen_by_key[key] = static_cast<uint8_t>(id.index());
            made.emplace_back(id, &node);
        }
        w.pathogen_resize_cross_immunity(static_cast<uint32_t>(made.size()));
        for(auto const& [id, node] : made) {
            if(auto c = node->find("cross_immunity")) {
                for(auto const& q : c->children) {
                    auto it = pathogen_by_key.find(q.key);
                    if(it == pathogen_by_key.end()) {
                        { diag.fail(path + ":" + std::to_string(q.line) + ": unknown pathogen '" + q.key + "'"); return; }
                    }
                    w.pathogen_set_cross_immunity(id, it->second, static_cast<float>(core::parse_number(q.text, path, q.line, diag).value_or(0.0)));
                }
            }
        }
    }

    void load_sectors() {
        const std::string path = assets + "/defs/sectors.txt";
        auto& w = state.world;
        auto nodes = core::load_defs(path, diag);
        w.sector_resize_skill(worker_kinds);
        w.sector_resize_npi_capacity(static_cast<uint32_t>(npi_by_key.size()));
        for(auto const& node : nodes) {
            if(node.key != "sector") {
                { diag.fail(path + ":" + std::to_string(node.line) + ": expected sector{...}"); return; }
            }
            const auto id = w.create_sector();
            if(id.index() >= max_sectors) {
                { diag.fail(path + ": too many sectors"); return; }
            }
            const std::string key(node.req("key", path, diag));
            w.sector_set_key(id, core::pack_key(key));
            w.sector_set_role(id, static_cast<uint8_t>(parse_sector_role(node.req("role", path, diag), path)));
            w.sector_set_labor_share(id, static_cast<float>(node.num_or("labor_share", 0.5, path, diag)));
            w.sector_set_oil_intensity(id, static_cast<float>(node.num_or("oil_intensity", 1.0, path, diag)));
            w.sector_set_import_intensity(id, static_cast<float>(node.num_or("import_intensity", 1.0, path, diag)));
            w.sector_set_voluntary_distancing(id, static_cast<float>(node.num_or("voluntary_distancing", 0.0, path, diag)));
            if(auto d = node.find("demand")) {
                w.sector_set_demand_hh(id, static_cast<float>(d->num_or("hh", 0.0, path, diag)));
                w.sector_set_demand_gov(id, static_cast<float>(d->num_or("gov", 0.0, path, diag)));
                w.sector_set_demand_inv(id, static_cast<float>(d->num_or("inv", 0.0, path, diag)));
                w.sector_set_demand_exp(id, static_cast<float>(d->num_or("exp", 0.0, path, diag)));
            }
            if(auto sk = node.find("skills")) {
                const float lo = static_cast<float>(sk->num_or("low", 0.33, path, diag));
                const float mi = static_cast<float>(sk->num_or("mid", 0.34, path, diag));
                const float hi = static_cast<float>(sk->num_or("high", 0.33, path, diag));
                const float tot = lo + mi + hi;
                w.sector_set_skill(id, 0, lo / tot);
                w.sector_set_skill(id, 1, mi / tot);
                w.sector_set_skill(id, 2, hi / tot);
            }
            if(auto nc = node.find("npi_capacity")) {
                for(auto const& c : nc->children) {
                    auto it = npi_by_key.find(c.key);
                    if(it == npi_by_key.end()) {
                        { diag.fail(path + ":" + std::to_string(c.line) + ": unknown NPI '" + c.key + "'"); return; }
                    }
                    w.sector_set_npi_capacity(id, it->second, static_cast<float>(core::parse_number(c.text, path, c.line, diag).value_or(0.0)));
                }
            }
            w.sector_set_shipped(id, static_cast<float>(node.num_or("shipped", 0.0, path, diag)));
            w.sector_set_critical_input(id, static_cast<float>(node.num_or("critical_input", 0.0, path, diag)));
            w.sector_set_cyber_exposure(id, static_cast<float>(node.num_or("cyber_exposure", 0.0, path, diag)));
            w.sector_set_subsistence(id, static_cast<float>(std::clamp(node.num_or("subsistence", 0.0, path, diag), 0.0, 0.9)));
            w.sector_set_inventory_days(id, static_cast<float>(std::max(7.0, node.num_or("inventory_days", 14.0, path, diag))));
            sector_by_key[key] = static_cast<uint8_t>(id.index());
        }
        // Input-output coefficients (need every sector key).
        w.sector_resize_io_coeff(static_cast<uint32_t>(sector_by_key.size()));
        for(auto const& node : nodes) {
            auto it = sector_by_key.find(std::string(node.req("key", path, diag)));
            if(it == sector_by_key.end()) continue;
            const dcon::sector_id id{it->second};
            double used = 0.0;
            if(auto inp = node.find("inputs")) {
                for(auto const& c : inp->children) {
                    auto src = sector_by_key.find(c.key);
                    if(src == sector_by_key.end()) {
                        diag.fail(path, c.line, "unknown input sector '" + c.key + "'");
                        return;
                    }
                    const double a = core::parse_number(c.text, path, c.line, diag).value_or(0.0);
                    w.sector_set_io_coeff(id, src->second, static_cast<float>(a));
                    used += a;
                }
            }
            if(used >= 0.9) {
                diag.fail(path, node.line, "input coefficients of '" + std::string(node.req("key", path, diag)) + "' sum to 0.9 or more");
                return;
            }
            w.sector_set_value_added_share(id, static_cast<float>(1.0 - used));
        }
    }

    // ------------------------------------------------------------ nation data
    void load_nations() {
        const std::string dir = assets + "/scenario/" + scenario;
        const auto t = core::csv_table::load(dir + "/nations.csv", diag);
        auto pct = [&](std::size_t row, std::string_view col, double fallback, nation_input const& in) {
            auto v = t.number(row, col);
            if(!v) {
                warn(in.iso + ": missing " + std::string(col) + ", using " + std::to_string(fallback));
                return fallback;
            }
            return *v / 100.0;
        };
        for(std::size_t row = 0; row < t.rows(); ++row) {
            nation_input in;
            in.iso = std::string(t.get(row, "iso3"));
            in.population = t.number_or(row, "population", 0.0);
            in.labor_force = t.number_or(row, "labor_force", 0.0);
            in.gdp = t.number_or(row, "gdp_usd", 0.0);
            in.gdp_ppp = t.number_or(row, "gdp_ppp", in.gdp);
            if(in.population <= 0 || in.labor_force <= 0 || in.gdp <= 0) {
                { diag.fail(dir + "/nations.csv: " + in.iso + " needs population, labor_force and gdp_usd"); return; }
            }
            in.pop65 = pct(row, "pop_65up_pct", 0.10, in);
            in.unemployment = pct(row, "unemployment_pct", 0.05, in);
            in.trend_growth = pct(row, "trend_growth_pct", 0.02, in);
            in.inflation_trend = pct(row, "inflation_trend_pct", 0.02, in);
            in.hh_cons = pct(row, "hh_cons_pct", 0.6, in);
            in.gov_cons = pct(row, "gov_cons_pct", 0.17, in);
            in.investment = pct(row, "investment_pct", 0.22, in);
            in.exports = pct(row, "exports_pct", 0.3, in);
            in.imports = pct(row, "imports_pct", 0.3, in);
            in.gov_revenue = pct(row, "gov_revenue_pct", 0.25, in);
            in.gov_net_lending = pct(row, "gov_net_lending_pct", -0.03, in);
            in.gov_debt = pct(row, "gov_debt_pct", 0.6, in);
            in.gini = pct(row, "gini", 0.35, in);
            in.vulnerable = pct(row, "vulnerable_emp_pct", 0.2, in);
            in.beds = t.number(row, "hospital_beds_per_1000").value_or(3.0);
            in.voice = t.number_or(row, "voice_est", 0.0);
            in.birth_rate = t.number_or(row, "birth_rate_per_1000", 15.0) / 1000.0;
            in.death_rate = t.number_or(row, "death_rate_per_1000", 8.0) / 1000.0;
            in.pop014 = t.number_or(row, "pop_0_14_pct", 20.0) / 100.0;
            in.tertiary = t.number_or(row, "tertiary_attainment_pct", 20.0) / 100.0;
            in.lat = t.number_or(row, "capital_lat", 0.0);
            in.lon = t.number_or(row, "capital_lon", 0.0);
            in.remit_in = t.number_or(row, "remittances_in_pct", 0.0) / 100.0;
            in.remit_out = t.number_or(row, "remittances_out_usd", 0.0);
            in.effectiveness = t.number_or(row, "gov_effectiveness_est", 0.0);
            in.urban = t.number_or(row, "urban_pct", 50.0) / 100.0;
            order.push_back(in.iso);
            inputs[in.iso] = in;
        }

        const auto c = core::csv_table::load(dir + "/nations_curated.csv", diag);
        for(std::size_t row = 0; row < c.rows(); ++row) {
            const std::string iso(c.get(row, "iso3"));
            auto it = inputs.find(iso);
            if(it == inputs.end()) {
                { diag.fail(dir + "/nations_curated.csv: unknown nation " + iso); return; }
            }
            auto& in = it->second;
            in.hemisphere = c.number_or(row, "hemisphere", 1.0);
            in.oil_production = c.number_or(row, "oil_production_mbd", 0.0);
            in.oil_elasticity = c.number_or(row, "oil_supply_elasticity", 0.0);
            in.epl = c.number_or(row, "epl", 0.35);
            in.benefit_replacement = c.number_or(row, "benefit_replacement", 0.3);
            in.benefit_coverage = c.number_or(row, "benefit_coverage", 0.15);
            in.death_reporting = c.number_or(row, "death_reporting", 0.5);
            in.oil_take = c.number_or(row, "oil_gov_take", 0.0);
        }

        const auto s = core::csv_table::load(dir + "/sectors.csv", diag);
        for(std::size_t row = 0; row < s.rows(); ++row) {
            const std::string iso(s.get(row, "iso3"));
            const std::string key(s.get(row, "sector"));
            auto it = inputs.find(iso);
            auto sk = sector_by_key.find(key);
            if(it == inputs.end() || sk == sector_by_key.end()) {
                { diag.fail(dir + "/sectors.csv: unknown nation or sector " + iso + "/" + key); return; }
            }
            it->second.va_share[sk->second] = s.number_or(row, "va_share", 0.0);
            it->second.emp_share[sk->second] = s.number_or(row, "emp_share", 0.0);
        }
        load_nation_extras(dir);
    }

    // Optional per-nation tables: banking / bonds / prices, society, identity groups.
    // Nations without rows keep the defaults in nation_input (the full-world data
    // comes later; every mechanism must work with defaults).
    void load_nation_extras(std::string const& dir) {
        auto rows = [&](std::string const& file, auto&& fn) {
            if(!exists(dir + "/" + file)) return;
            const auto t = core::csv_table::load(dir + "/" + file, diag);
            for(std::size_t row = 0; row < t.rows(); ++row) {
                auto it = inputs.find(std::string(t.get(row, "iso3")));
                if(it == inputs.end()) continue; // nation not in this scenario
                fn(t, row, it->second);
            }
        };
        rows("finance_curated.csv", [&](core::csv_table const& t, std::size_t row, nation_input& in) {
            in.private_credit = t.number_or(row, "private_credit_pct", in.private_credit * 100.0) / 100.0;
            in.hh_credit_share = t.number_or(row, "hh_credit_share", in.hh_credit_share);
            in.state_bank_share = t.number_or(row, "state_bank_share", in.state_bank_share);
            in.cb_independence = t.number_or(row, "cb_independence", in.cb_independence);
            in.fx_loan_share = t.number_or(row, "fx_loan_share", in.fx_loan_share);
            in.fx_deposit_share = t.number_or(row, "fx_deposit_share", in.fx_deposit_share);
            in.npl = t.number_or(row, "npl_pct", in.npl * 100.0) / 100.0;
            in.capital_ratio = t.number_or(row, "capital_ratio_pct", in.capital_ratio * 100.0) / 100.0;
            in.lending_spread = t.number_or(row, "lending_spread_pp", in.lending_spread);
            if(auto v = t.number(row, "yield10_pct")) in.yield10 = *v;
            if(auto v = t.number(row, "effective_rate_pct")) in.effective_rate = *v;
            in.avg_maturity = t.number_or(row, "avg_maturity_years", in.avg_maturity);
            in.foreign_debt_share = t.number_or(row, "foreign_debt_share", in.foreign_debt_share);
            in.fx_debt_share = t.number_or(row, "fx_debt_share", in.fx_debt_share);
            in.food_cpi = t.number_or(row, "food_cpi_share", in.food_cpi);
            in.coordination = std::clamp(t.number_or(row, "coordination_index", in.coordination), 0.0, 1.0);
            in.tariff = t.number_or(row, "tariff_pct", in.tariff * 100.0) / 100.0;
            in.union_density = t.number_or(row, "union_density", in.union_density);
            in.bargaining_coverage = t.number_or(row, "bargaining_coverage", in.bargaining_coverage);
            in.min_wage_bite = t.number_or(row, "min_wage_bite", in.min_wage_bite);
            in.tax_progressivity = t.number_or(row, "tax_progressivity", in.tax_progressivity);
            in.carbon_price = t.number_or(row, "carbon_price_usd", in.carbon_price);
        });
        rows("society_curated.csv", [&](core::csv_table const& t, std::size_t row, nation_input& in) {
            in.youth_share = t.number_or(row, "youth_share", in.youth_share);
            in.youth_unemp_rel = t.number_or(row, "youth_unemp_rel", in.youth_unemp_rel);
            in.social_media = t.number_or(row, "social_media_pct", in.social_media * 100.0) / 100.0;
            in.corruption_cpi = t.number_or(row, "corruption_cpi", in.corruption_cpi);
            in.emigrant_stock = t.number_or(row, "emigrant_stock_pct", in.emigrant_stock * 100.0) / 100.0;
            in.net_freedom = t.number_or(row, "net_freedom", in.net_freedom);
            in.trust = t.number_or(row, "trust_wvs", in.trust);
            in.top1 = t.number_or(row, "top1_share", in.top1);
        });
        rows("groups_curated.csv", [&](core::csv_table const& t, std::size_t row, nation_input& in) {
            nation_input::group g;
            g.key = std::string(t.get(row, "group"));
            g.share = t.number_or(row, "pop_share", 0.0);
            g.edu_rel = std::max(0.05, t.number_or(row, "edu_rel", 1.0));
            g.informal_rel = std::max(0.0, t.number_or(row, "informal_rel", 1.0));
            g.wage_gap = t.number_or(row, "wage_gap", 0.0);
            g.unemp_rel = std::max(0.0, t.number_or(row, "unemp_rel", 1.0));
            g.wealth_rel = std::max(0.0, t.number_or(row, "wealth_rel", 1.0));
            g.excluded = std::clamp(t.number_or(row, "excluded", 0.0), 0.0, 1.0);
            g.network = std::clamp(t.number_or(row, "network", 0.3), 0.0, 1.0);
            g.immigrant = t.number_or(row, "immigrant", 0.0) > 0.0;
            if(g.key.empty() || g.key.size() > 4 || g.share <= 0.0) {
                diag.fail(t.source() + ": " + in.iso + " group keys need 1-4 characters and a positive pop_share");
                return;
            }
            in.groups.push_back(g);
        });
        for(auto& [iso, in] : inputs) {
            double sum = 0.0;
            for(auto const& g : in.groups) sum += g.share;
            if(!in.groups.empty() && std::abs(sum - 1.0) > 0.02) {
                warn(iso + ": identity-group shares sum to " + std::to_string(sum) + "; rescaled");
            }
            for(auto& g : in.groups) g.share /= sum;
            if(in.groups.empty()) {
                nation_input::group all;
                all.key = "ALL";
                in.groups.push_back(all);
            }
        }
    }

    // Optional sub-national tables (regions.csv, region_sectors.csv, adjacency.csv).
    // Without them every nation is a single region.
    void load_regions() {
        const std::string dir = assets + "/scenario/" + scenario;
        if(!exists(dir + "/regions.csv")) {
            return;
        }
        const auto t = core::csv_table::load(dir + "/regions.csv", diag);
        std::map<std::string, std::pair<std::string, std::size_t>> index;
        for(std::size_t row = 0; row < t.rows(); ++row) {
            region_input r;
            r.id = std::string(t.get(row, "region"));
            r.iso = std::string(t.get(row, "iso3"));
            if(!inputs.count(r.iso)) {
                { diag.fail(dir + "/regions.csv: region " + r.id + " of unknown nation " + r.iso); return; }
            }
            r.lat = t.number_or(row, "lat", 0.0);
            r.lon = t.number_or(row, "lon", 0.0);
            r.area = t.number_or(row, "area_km2", 0.0);
            r.pop_share = t.number_or(row, "pop_share", 0.0);
            r.urban = t.number_or(row, "urban_share", 0.5);
            auto& list = regions_of[r.iso];
            index[r.id] = {r.iso, list.size()};
            list.push_back(r);
        }
        for(auto& [iso, list] : regions_of) { // normalise shares within each nation
            double sum = 0.0;
            for(auto const& r : list) sum += r.pop_share;
            for(auto& r : list) r.pop_share = sum > 0.0 ? r.pop_share / sum : 1.0 / static_cast<double>(list.size());
        }
        if(exists(dir + "/region_sectors.csv")) {
            const auto rs = core::csv_table::load(dir + "/region_sectors.csv", diag);
            for(std::size_t row = 0; row < rs.rows(); ++row) {
                auto it = index.find(std::string(rs.get(row, "region")));
                auto sk = sector_by_key.find(std::string(rs.get(row, "sector")));
                if(it == index.end() || sk == sector_by_key.end()) {
                    { diag.fail(dir + "/region_sectors.csv: unknown region or sector at row " + std::to_string(row + 1)); return; }
                }
                auto& r = regions_of[it->second.first][it->second.second];
                r.va_share[sk->second] = rs.number_or(row, "va_share", 0.0);
                r.emp_share[sk->second] = rs.number_or(row, "emp_share", 0.0);
                r.has_sectors = true;
            }
        }
    }

    void load_adjacency() {
        const std::string path = assets + "/scenario/" + scenario + "/adjacency.csv";
        if(!exists(path)) {
            return;
        }
        auto& w = state.world;
        const auto t = core::csv_table::load(path, diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            auto a = region_by_id.find(std::string(t.get(row, "region_a")));
            auto b = region_by_id.find(std::string(t.get(row, "region_b")));
            if(a == region_by_id.end() || b == region_by_id.end()) {
                { diag.fail(path + ": unknown region at row " + std::to_string(row + 1)); return; }
            }
            const auto rel = w.force_create_region_adjacency(a->second, b->second);
            w.region_adjacency_set_weight(rel, static_cast<float>(t.number_or(row, "weight", 1.0)));
        }
    }

    // ------------------------------------------------------------ events
    void load_events() {
        const std::string dir = assets + "/events/" + scenario;
        if(!exists(dir)) {
            warn("no events directory " + dir);
            return;
        }
        std::vector<std::string> files;
        std::error_code ec;
        for(fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if(it->path().extension() == ".txt") {
                files.push_back(it->path().string());
            }
        }
        if(ec) {
            diag.fail("cannot list " + dir + ": " + ec.message());
            return;
        }
        std::sort(files.begin(), files.end()); // deterministic event order
        auto& w = state.world;
        for(auto const& path : files) {
            for(auto const& node : core::load_defs(path, diag)) {
                const std::string where = path + ":" + std::to_string(node.line);
                auto date = core::parse_date(node.req("date", path, diag));
                if(!date) {
                    { diag.fail(where + ": bad date"); return; }
                }
                const std::string nation_key(node.req("nation", path, diag));
                // nation{ALL} expands to one event per nation (useful for global seeds).
                std::vector<dcon::nation_id> targets;
                if(nation_key == "ALL") {
                    for(auto n : w.in_nation) targets.push_back(n.id);
                } else {
                    const auto nation = find_nation(state, nation_key);
                    if(!nation) {
                        { diag.fail(where + ": unknown nation '" + nation_key + "'"); return; }
                    }
                    targets.push_back(nation);
                }
                for(auto nation : targets) {
                const auto e = w.create_event();
                w.event_set_tick(e, core::tick_from_date(*date));
                w.event_set_target(e, nation);
                auto num = [&](std::string_view key) {
                    auto v = node.num(key, path, diag);
                    if(!v) {
                        diag.fail(where + ": " + node.key + " needs " + std::string(key) + "{...}");
                        return 0.0f;
                    }
                    return static_cast<float>(*v);
                };
                if(node.key == "npi") {
                    const std::string ind(node.req("indicator", path, diag));
                    auto it = npi_by_key.find(ind);
                    if(it == npi_by_key.end()) {
                        { diag.fail(where + ": unknown indicator '" + ind + "'"); return; }
                    }
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::npi_level));
                    w.event_set_param(e, it->second);
                    w.event_set_value(e, num("level"));
                    w.event_set_value2(e, static_cast<float>(node.num_or("targeted", 0.0, path, diag)));
                } else if(node.key == "policy_rate") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::policy_rate));
                    w.event_set_value(e, num("rate_pct"));
                } else if(node.key == "fiscal_package") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::fiscal_package));
                    w.event_set_value(e, num("pct_gdp"));
                } else if(node.key == "epidemic_seed") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::epidemic_seed));
                    const std::string pk(node.req("pathogen", path, diag));
                    auto pit = pathogen_by_key.find(pk);
                    if(pit == pathogen_by_key.end()) {
                        { diag.fail(where + ": unknown pathogen '" + pk + "'"); return; }
                    }
                    w.event_set_param(e, pit->second);
                    if(auto pm = node.num("infections_per_million", path, diag)) {
                        w.event_set_value(e, static_cast<float>(*pm * inputs[core::unpack_key(w.nation_get_iso3(nation))].population / 1e6));
                    } else {
                        w.event_set_value(e, num("infections"));
                    }
                } else if(node.key == "stw") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::job_retention));
                    w.event_set_value(e, num("coverage"));
                    w.event_set_value2(e, num("replacement"));
                } else if(node.key == "test_trace_isolate") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::test_trace_isolate));
                    w.event_set_value(e, num("strength"));
                } else if(node.key == "oil_production") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::oil_production));
                    w.event_set_value(e, num("mbd"));
                } else if(node.key == "oil_production_delta") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::oil_production_delta));
                    w.event_set_value(e, num("mbd"));
                } else {
                    { diag.fail(where + ": unknown event kind '" + node.key + "'"); return; }
                }
                ++report.events;
                }
            }
        }
    }

    // ------------------------------------------------------------ calibration
    void create_nation(nation_input const& in) {
        auto& w = state.world;
        const auto n = w.create_nation();
        w.nation_set_iso3(n, core::pack_key(in.iso));
        w.nation_set_lat(n, static_cast<float>(in.lat));
        w.nation_set_lon(n, static_cast<float>(in.lon));
        // Identity groups (pops are split by group in every region).
        uint8_t gi = 0;
        for(auto const& g : in.groups) {
            const auto id = w.create_social_group();
            w.social_group_set_key(id, core::pack_key(g.key));
            w.social_group_set_nation(id, n);
            w.social_group_set_local_index(id, gi++);
            w.social_group_set_pop_share(id, static_cast<float>(g.share));
            w.social_group_set_edu_rel(id, static_cast<float>(g.edu_rel));
            w.social_group_set_informal_rel(id, static_cast<float>(g.informal_rel));
            w.social_group_set_wage_gap(id, static_cast<float>(g.wage_gap));
            w.social_group_set_unemp_rel(id, static_cast<float>(g.unemp_rel));
            w.social_group_set_wealth_rel(id, static_cast<float>(g.wealth_rel));
            w.social_group_set_excluded(id, static_cast<float>(g.excluded));
            w.social_group_set_network(id, static_cast<float>(g.network));
            w.social_group_set_immigrant(id, g.immigrant ? 1 : 0);
        }
        std::vector<region_input> list = regions_of.count(in.iso) ? regions_of[in.iso] : std::vector<region_input>{};
        if(list.empty()) {
            region_input whole;
            whole.id = in.iso;
            whole.iso = in.iso;
            list.push_back(whole);
        }
        for(auto const& ri : list) {
            create_region(n, in, ri);
        }
    }

    void create_region(dcon::nation_id n, nation_input const& in, region_input const& ri) {
        auto& w = state.world;
        const double pop = in.population * ri.pop_share;
        const auto r = w.create_region();
        region_by_id[ri.id] = r;
        w.force_create_region_ownership(r, n);
        w.region_set_owner(r, n);
        w.region_set_shard(r, static_cast<int32_t>(static_cast<uint32_t>(n.index()) % weekly.shards));
        w.region_set_population(r, static_cast<float>(pop));
        w.region_set_lat(r, static_cast<float>(ri.lat));
        w.region_set_lon(r, static_cast<float>(ri.lon));
        w.region_set_area_km2(r, static_cast<float>(ri.area));
        w.region_set_urban_share(r, static_cast<float>(ri.urban));
        w.region_set_old_share(r, static_cast<float>(in.pop65));
        // Seasonality follows the region's latitude when known, else the nation's hemisphere.
        w.region_set_hemisphere(r, ri.lat != 0.0 ? static_cast<float>(std::clamp(ri.lat / 23.5, -1.0, 1.0))
                                                  : static_cast<float>(in.hemisphere));
        w.region_set_beds_per_1000(r, static_cast<float>(in.beds));
        // Every pathogen starts with the same people; some may already be immune.
        for(auto pth : w.in_pathogen) {
            const auto p = static_cast<uint8_t>(pth.id.index());
            const double immune = pth.get_initial_immunity();
            w.region_set_ep_s(r, epi_index(p, age_young), static_cast<float>(pop * (1.0 - in.pop65) * (1.0 - immune)));
            w.region_set_ep_r(r, epi_index(p, age_young), static_cast<float>(pop * (1.0 - in.pop65) * immune));
            w.region_set_ep_s(r, epi_index(p, age_old), static_cast<float>(pop * in.pop65 * (1.0 - immune)));
            w.region_set_ep_r(r, epi_index(p, age_old), static_cast<float>(pop * in.pop65 * immune));
        }
        w.region_set_ifr_mult(r, 1.0f);
        w.region_set_children(r, static_cast<float>(pop * in.pop014));
        w.region_set_unrest_capacity(r, 1.0f);
        for(auto g : w.in_social_group) {
            if(g.get_nation() != n) continue;
            for(uint8_t k = 0; k < 4; ++k) {
                const auto p = w.create_pop();
                w.pop_set_kind(p, k);
                w.pop_set_group(p, g.id);
                w.pop_set_is_worker(p, k < worker_kinds ? 1.0f : 0.0f);
                w.pop_set_nation(p, n);
                w.pop_set_home(p, r);
                w.pop_set_shard(p, w.region_get_shard(r));
                w.pop_set_network(p, g.get_network());
                w.pop_set_wage_mult(p, 1.0f);
                w.pop_set_tax_factor(p, 1.0f);
                w.pop_set_cpi_p(p, 1.0f);
                w.force_create_pop_location(p, r);
            }
        }
    }

    void calibrate(dcon::nation_id n, nation_input const& in) {
        auto& w = state.world;
        auto const& P = state.params;
        auto const& D = state.derived;
        const uint8_t S = D.sector_count;

        w.nation_set_population(n, static_cast<float>(in.population));
        w.nation_set_gdp0(n, in.gdp);
        w.nation_set_trend_growth(n, static_cast<float>(in.trend_growth));
        w.nation_set_inflation_trend(n, static_cast<float>(in.inflation_trend));
        w.nation_set_inflation_expect(n, static_cast<float>(in.inflation_trend));
        w.nation_set_informality(n, static_cast<float>(in.vulnerable));
        w.nation_set_epl(n, static_cast<float>(in.epl));
        w.nation_set_benefit_replacement(n, static_cast<float>(in.benefit_replacement));
        w.nation_set_benefit_coverage(n, static_cast<float>(in.benefit_coverage));
        w.nation_set_death_reporting(n, static_cast<float>(in.death_reporting));
        w.nation_set_gini0(n, static_cast<float>(in.gini));
        w.nation_set_core_price(n, 1.0f);
        w.nation_set_cpi(n, 1.0f);
        w.nation_set_gf(n, 1.0f); // growth factor at tick 0 (systems read it before the first economy step)
        w.nation_set_util_smoothed(n, 1.0f);

        // Daily output at the start of 2020 (2019 annual level grown half a year).
        const double y = in.gdp * std::pow(1.0 + in.trend_growth, 0.5) / 365.0;
        w.nation_set_gdp0_daily(n, static_cast<float>(y));
        w.nation_set_remit_in0(n, static_cast<float>(in.remit_in * y));
        w.nation_set_remit_out0(n, static_cast<float>(in.remit_out / 365.0));
        w.nation_set_exchange_rate(n, 1.0f);
        w.nation_set_gov_rate(n, static_cast<float>(state.params.gov_effective_rate));

        double c = in.hh_cons, g = in.gov_cons, x = in.exports, m = in.imports;
        double i = 1.0 - c - g - x + m;
        if(i < 0.05) {
            warn(in.iso + ": expenditure shares inconsistent, investment floored at 5%");
            i = 0.05;
            c = 1.0 - g - i - x + m;
        }
        const double C0 = c * y, G0 = g * y, I0 = i * y, X0 = x * y, M0 = m * y;
        const double m_share = std::min(0.9, M0 / (C0 + I0 + X0));
        w.nation_set_c0(n, static_cast<float>(C0));
        w.nation_set_g0(n, static_cast<float>(G0));
        w.nation_set_i0(n, static_cast<float>(I0));
        w.nation_set_x0(n, static_cast<float>(X0));
        w.nation_set_m0(n, static_cast<float>(M0));
        w.nation_set_import_share(n, static_cast<float>(m_share));
        w.nation_set_consumption(n, static_cast<float>(C0));
        // The calibration probe must see baseline household demand (pop plans come later).
        w.nation_set_c_des_total(n, C0);
        w.nation_set_investment(n, static_cast<float>(I0));
        w.nation_set_gov_spend(n, static_cast<float>(G0));
        w.nation_set_exports(n, static_cast<float>(X0));
        w.nation_set_imports(n, static_cast<float>(M0));
        w.nation_set_gdp_real(n, static_cast<float>(y));

        // Sector value added and employment.
        double va_total = 0.0;
        for(uint8_t s = 0; s < S; ++s) {
            va_total += in.va_share[s];
        }
        const double employed = in.labor_force * (1.0 - in.unemployment);
        std::array<double, max_sectors> va{};
        for(uint8_t s = 0; s < S; ++s) {
            va[s] = y * in.va_share[s] / va_total;
            w.nation_set_va0(n, s, static_cast<float>(va[s]));
            w.nation_set_emp0(n, s, static_cast<float>(employed * in.emp_share[s]));
            w.nation_set_output(n, s, static_cast<float>(va[s]));
            w.nation_set_capacity(n, s, static_cast<float>(va[s]));
            w.nation_set_demand(n, s, static_cast<float>(va[s]));
            w.nation_set_hours(n, s, 1.0f);
            w.nation_set_labor_need(n, s, w.nation_get_emp0(n, s));
        }

        double import_base = 0.0;
        for(uint8_t s = 0; s < S; ++s) {
            import_base += va[s] * w.sector_get_import_intensity(dcon::sector_id{s});
        }
        w.nation_set_import_base(n, static_cast<float>(import_base));

        // RAS: sector x component matrix with row sums = VA, column sums = domestic demand.
        const std::array<double, 4> col = {(1.0 - m_share) * C0, G0, (1.0 - m_share) * I0, (1.0 - m_share) * X0};
        std::array<std::array<double, 4>, max_sectors> a{};
        for(uint8_t s = 0; s < S; ++s) {
            const dcon::sector_id sid{s};
            const std::array<double, 4> prior = {w.sector_get_demand_hh(sid), w.sector_get_demand_gov(sid),
                                                 w.sector_get_demand_inv(sid), w.sector_get_demand_exp(sid)};
            for(int j = 0; j < 4; ++j) {
                a[s][static_cast<std::size_t>(j)] = std::max(1e-6, prior[static_cast<std::size_t>(j)]);
            }
        }
        for(int iter = 0; iter < 500; ++iter) {
            for(int j = 0; j < 4; ++j) {
                double sum = 0.0;
                for(uint8_t s = 0; s < S; ++s) sum += a[s][static_cast<std::size_t>(j)];
                for(uint8_t s = 0; s < S; ++s) a[s][static_cast<std::size_t>(j)] *= col[static_cast<std::size_t>(j)] / sum;
            }
            for(uint8_t s = 0; s < S; ++s) {
                double sum = 0.0;
                for(int j = 0; j < 4; ++j) sum += a[s][static_cast<std::size_t>(j)];
                for(int j = 0; j < 4; ++j) a[s][static_cast<std::size_t>(j)] *= va[s] / sum;
            }
        }
        double worst = 0.0;
        for(int j = 0; j < 4; ++j) {
            double sum = 0.0;
            for(uint8_t s = 0; s < S; ++s) sum += a[s][static_cast<std::size_t>(j)];
            worst = std::max(worst, std::abs(sum / col[static_cast<std::size_t>(j)] - 1.0));
        }
        if(worst > 0.01) {
            warn(in.iso + ": RAS did not fully converge (max column error " + std::to_string(worst) + ")");
        }
        for(uint8_t s = 0; s < S; ++s) {
            w.nation_set_comp_c(n, s, static_cast<float>(a[s][0] / col[0]));
            w.nation_set_comp_g(n, s, static_cast<float>(a[s][1] / col[1]));
            w.nation_set_comp_i(n, s, static_cast<float>(a[s][2] / col[2]));
            w.nation_set_comp_x(n, s, static_cast<float>(a[s][3] / col[3]));
        }

        // Identity groups: split each skill tier's workers across groups with a RAS
        // (rows: group population shares; columns: national skill totals; prior:
        // group share x education odds), so national totals hold exactly.
        const double unemployed = in.labor_force * in.unemployment;
        const std::array<float, 3> u_share = {0.5f, 0.35f, 0.15f};
        const std::array<float, 4> owner = {0.02f, 0.13f, 0.55f, 0.30f};
        const std::array<float, 4> transfer = {0.20f, 0.05f, 0.0f, 0.75f};
        std::vector<dcon::social_group_id> groups;
        for(auto g : w.in_social_group) {
            if(g.get_nation() == n) groups.push_back(g.id);
        }
        const std::size_t G = groups.size();
        std::array<double, 3> W{};
        for(uint8_t s = 0; s < S; ++s) {
            for(uint8_t k = 0; k < worker_kinds; ++k) W[k] += w.nation_get_emp0(n, s) * w.sector_get_skill(dcon::sector_id{s}, k);
        }
        for(uint8_t k = 0; k < worker_kinds; ++k) W[k] += unemployed * u_share[k];
        const double W_total = W[0] + W[1] + W[2];
        std::vector<std::array<double, 3>> M(G);
        for(std::size_t g = 0; g < G; ++g) {
            const double edu = w.social_group_get_edu_rel(groups[g]);
            const std::array<double, 3> odds = {1.0 / edu, 1.0, edu};
            for(uint8_t k = 0; k < worker_kinds; ++k) M[g][k] = w.social_group_get_pop_share(groups[g]) * W[k] * odds[k];
        }
        for(int it = 0; it < 200; ++it) {
            for(uint8_t k = 0; k < worker_kinds; ++k) {
                double col = 0.0;
                for(std::size_t g = 0; g < G; ++g) col += M[g][k];
                for(std::size_t g = 0; g < G; ++g) M[g][k] *= col > 0.0 ? W[k] / col : 0.0;
            }
            for(std::size_t g = 0; g < G; ++g) {
                const double row = M[g][0] + M[g][1] + M[g][2];
                const double target = w.social_group_get_pop_share(groups[g]) * W_total;
                for(uint8_t k = 0; k < worker_kinds; ++k) M[g][k] *= row > 0.0 ? target / row : 0.0;
            }
        }
        w.social_group_resize_skill(worker_kinds);
        std::vector<std::array<double, 3>> F(G), U(G), WF(G);
        double inf_norm = 0.0;
        for(std::size_t g = 0; g < G; ++g) inf_norm += w.social_group_get_pop_share(groups[g]) * w.social_group_get_informal_rel(groups[g]);
        for(uint8_t k = 0; k < worker_kinds; ++k) {
            double u_norm = 0.0, w_norm = 0.0;
            for(std::size_t g = 0; g < G; ++g) {
                F[g][k] = W[k] > 0.0 ? M[g][k] / W[k] : 1.0 / static_cast<double>(G);
                u_norm += F[g][k] * w.social_group_get_unemp_rel(groups[g]);
                w_norm += F[g][k] * (1.0 - w.social_group_get_wage_gap(groups[g]));
            }
            for(std::size_t g = 0; g < G; ++g) {
                U[g][k] = u_norm > 0.0 ? F[g][k] * w.social_group_get_unemp_rel(groups[g]) / u_norm : F[g][k];
                WF[g][k] = w_norm > 0.0 ? (1.0 - w.social_group_get_wage_gap(groups[g])) / w_norm : 1.0;
            }
        }
        for(std::size_t g = 0; g < G; ++g) {
            const double row = M[g][0] + M[g][1] + M[g][2];
            for(uint8_t k = 0; k < worker_kinds; ++k) w.social_group_set_skill(groups[g], k, static_cast<float>(row > 0.0 ? M[g][k] / row : 1.0 / 3.0));
        }
        auto group_index = [&](dcon::pop_id p) -> std::size_t {
            const auto g = w.pop_get_group(p);
            for(std::size_t i = 0; i < G; ++i) if(groups[i] == g) return i;
            return 0;
        };

        // Regions: value added and employment by the region's share (population share
        // until regional tables are provided), then pops per region and group.
        std::array<double, 4> kind_size{};
        std::array<double, 4> kind_wealth{};
        std::size_t ridx = 0;
        auto const& rlist = regions_of.count(in.iso) ? regions_of[in.iso] : std::vector<region_input>{};
        for(auto own : w.nation_get_region_ownership(n)) {
            auto reg = own.get_region();
            const double share = reg.get_population() / in.population;
            region_input const* ri = ridx < rlist.size() ? &rlist[ridx] : nullptr;
            ++ridx;
            std::array<double, max_sectors> va_sh{};
            std::array<double, max_sectors> emp_sh{};
            for(uint8_t s = 0; s < S; ++s) {
                va_sh[s] = ri && ri->has_sectors ? ri->va_share[s] : share;
                emp_sh[s] = ri && ri->has_sectors ? ri->emp_share[s] : share;
                reg.set_va0(s, static_cast<float>(va[s] * va_sh[s]));
                reg.set_capacity(s, static_cast<float>(va[s] * va_sh[s]));
                reg.set_output(s, static_cast<float>(va[s] * va_sh[s]));
                reg.set_util(s, 1.0f);
            }
            for(auto loc : reg.get_pop_location()) {
                auto p = loc.get_pop();
                const auto k = p.get_kind();
                const std::size_t gi = group_index(p.id);
                const auto grp = groups[gi];
                double size = 0.0;
                if(k < worker_kinds) {
                    for(uint8_t s = 0; s < S; ++s) {
                        const dcon::sector_id sid{s};
                        const float e_region = static_cast<float>(w.nation_get_emp0(n, s) * emp_sh[s]);
                        const float e = static_cast<float>(e_region * w.sector_get_skill(sid, k) * F[gi][k]);
                        double mix = 0.0;
                        for(uint8_t kk = 0; kk < worker_kinds; ++kk) {
                            mix += w.sector_get_skill(sid, kk) * (kk == 0 ? P.wage_low : kk == 1 ? P.wage_mid : P.wage_high);
                        }
                        const double mult = k == 0 ? P.wage_low : k == 1 ? P.wage_mid : P.wage_high;
                        p.set_employed(s, e);
                        p.set_emp0(s, e);
                        p.set_need_slow(s, e);
                        p.set_hours(s, 1.0f);
                        p.set_wage0(s, static_cast<float>(w.sector_get_labor_share(sid) * va[s] * va_sh[s] / std::max(1.0f, e_region) * mult / mix
                                                          * WF[gi][k]));
                        size += e;
                    }
                    const double u = unemployed * share * u_share[k] * U[gi][k];
                    p.set_unemployed(static_cast<float>(u));
                    size += u;
                    double wsum = 0.0, esum = 0.0;
                    for(uint8_t s = 0; s < S; ++s) {
                        wsum += static_cast<double>(p.get_emp0(s)) * p.get_wage0(s);
                        esum += p.get_emp0(s);
                    }
                    // A group's average wage for benefits, from its own wages.
                    p.set_wage_avg0(static_cast<float>(esum > 0.0 ? wsum / esum : 0.0));
                } else {
                    size = reg.get_population() * in.pop65 * w.social_group_get_pop_share(grp);
                }
                // Informality by group, rescaled so the national mean is the data (WDI).
                const double inf = inf_norm > 0.0 ? std::clamp(in.vulnerable * w.social_group_get_informal_rel(grp) / inf_norm, 0.0, 0.98) : in.vulnerable;
                p.set_informal(k < worker_kinds ? static_cast<float>(inf) : 0.0f);
                p.set_formal_rel(static_cast<float>(std::clamp((1.0 - inf) / std::max(0.02, 1.0 - in.vulnerable), 0.0, 3.0)));
                p.set_size(static_cast<float>(size));
                p.set_owner_share(owner[k]);
                p.set_transfer_share(transfer[k]);
                kind_size[k] += size;
                kind_wealth[k] += size * w.social_group_get_wealth_rel(grp);
            }
        }
        // Income weights: each kind's national share, split across regions by pop size;
        // profit and interest income (wealth) also by the group's relative wealth.
        double owner_total = 0.0, transfer_total = 0.0, size_total = 0.0;
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                const auto k = p.get_kind();
                const double frac = kind_size[k] > 0.0 ? p.get_size() / kind_size[k] : 0.0;
                const double wfrac = kind_wealth[k] > 0.0 ? p.get_size() * w.social_group_get_wealth_rel(p.get_group()) / kind_wealth[k] : frac;
                p.set_owner_weight(static_cast<float>(owner[k] * wfrac));
                p.set_transfer_weight(static_cast<float>(transfer[k] * frac));
                owner_total += owner[k] * wfrac;
                transfer_total += transfer[k] * frac;
                size_total += p.get_size();
            }
        }
        w.nation_set_owner_weight_total(n, static_cast<float>(owner_total));
        w.nation_set_transfer_weight_total(n, static_cast<float>(transfer_total));
        w.nation_set_size_total(n, static_cast<float>(size_total));

        // Government and taxes.
        const auto gov_s = static_cast<uint8_t>(D.gov_sector.index());
        const double hh_dom = col[0];
        const double fees = w.nation_get_comp_c(n, gov_s) * hh_dom;
        const double revenue = in.gov_revenue * y;
        const double oil_value = in.oil_production * 1e6 * P.oil_price0;
        double resource = std::min(0.7 * revenue, in.oil_take * oil_value);
        double r_hh = P.tax_share_hh * revenue;
        double r_ind = P.tax_share_indirect * revenue;
        if(r_hh + r_ind + resource > 0.95 * revenue) {
            const double scale = (0.95 * revenue - resource) / (r_hh + r_ind);
            r_hh *= scale;
            r_ind *= scale;
        }
        const double r_firm = revenue - r_hh - r_ind - resource;
        double w_priv = 0.0, firm_va = 0.0;
        for(uint8_t s = 0; s < S; ++s) {
            if(s == gov_s) continue;
            w_priv += w.sector_get_labor_share(dcon::sector_id{s}) * va[s];
            firm_va += va[s];
        }
        const double w_pub = va[gov_s] * w.sector_get_labor_share(D.gov_sector);
        const double c_to_firms = C0 - fees;
        w.nation_set_tax_indirect(n, static_cast<float>(r_ind / c_to_firms));
        w.nation_set_resource_tax(n, oil_value > 0.0 ? static_cast<float>(resource / oil_value) : 0.0f);
        double profit0 = firm_va - w_priv - r_ind - resource;
        if(profit0 < 0.05 * firm_va) {
            warn(in.iso + ": baseline profits very low; firm tax base floored");
            profit0 = 0.05 * firm_va;
        }
        w.nation_set_tax_firm(n, static_cast<float>(std::clamp(r_firm / profit0, 0.0, 0.6)));
        const double firm_tax = w.nation_get_tax_firm(n) * profit0;
        w.nation_set_profit_smoothed(n, static_cast<float>(profit0));
        w.nation_set_profit0(n, static_cast<float>(profit0));

        const double debt0 = in.gov_debt * in.gdp;
        w.nation_set_gov_debt0(n, debt0);
        // Effective interest rate on the debt: interest bill / debt (IMF Fiscal Monitor), else the default.
        const double eff_rate = std::isfinite(in.effective_rate) ? in.effective_rate / 100.0 : P.gov_effective_rate;
        w.nation_set_gov_rate(n, static_cast<float>(eff_rate));
        w.nation_set_gov_rate0(n, static_cast<float>(eff_rate));
        const double interest0 = debt0 * eff_rate / 365.0;
        const double purchases0 = G0 * (1.0 - w.nation_get_comp_g(n, gov_s));
        const double deficit = -in.gov_net_lending * y;

        // Baseline unemployment benefits (same formula as the pop pass: each pop's own
        // average wage, scaled by its formal-job coverage).
        double benefits0 = 0.0;
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                if(p.get_kind() >= worker_kinds) continue;
                benefits0 += static_cast<double>(p.get_unemployed()) * p.get_wage_avg0() * in.benefit_replacement * in.benefit_coverage
                             * p.get_formal_rel();
            }
        }
        double pensions0 = revenue + fees + deficit - w_pub - purchases0 - interest0 - benefits0;
        if(pensions0 < 0.02 * y) {
            warn(in.iso + ": transfers residual below 2% of GDP; baseline deficit will differ from data");
            pensions0 = 0.02 * y;
        }
        w.nation_set_pensions0(n, static_cast<float>(pensions0));

        // Dividend payout so that household disposable income supports the observed consumption.
        const double yd_target = C0 / (1.0 - P.household_saving_rate);
        const double wages = w_priv + w_pub;
        double dividends = yd_target - wages - interest0 - pensions0 - benefits0 + r_hh;
        const double after_tax_profit = std::max(1.0, profit0 - firm_tax);
        double payout = dividends / after_tax_profit;
        if(payout < 0.05 || payout > 1.0) {
            warn(in.iso + ": dividend payout " + std::to_string(payout) + " clamped; household saving rate differs from target");
            payout = std::clamp(payout, 0.05, 1.0);
        }
        dividends = payout * after_tax_profit;
        w.nation_set_dividend_payout(n, static_cast<float>(payout * after_tax_profit / profit0));
        w.nation_set_tax_hh(n, static_cast<float>(r_hh / (wages + dividends + interest0)));

        // Policy rate at start (anchored by the 2020-01-01 BIS event when present).
        float rate0 = static_cast<float>(P.taylor_neutral + 100.0 * in.inflation_trend);
        for(auto e : w.in_event) {
            if(e.get_target() == n && e.get_tick() == 0 && e.get_kind() == static_cast<uint8_t>(event_kind::policy_rate)) {
                rate0 = e.get_value();
            }
        }
        w.nation_set_policy_rate0(n, rate0);
        w.nation_set_policy_rate(n, rate0);
        calibrate_finance(n, in, rate0, y);

        // Labour state.
        w.nation_set_unemployment0(n, static_cast<float>(in.unemployment));
        w.nation_set_unemployment(n, static_cast<float>(in.unemployment));

        // Oil.
        w.nation_set_oil_production0(n, static_cast<float>(in.oil_production));
        w.nation_set_oil_production(n, static_cast<float>(in.oil_production));
        w.nation_set_oil_target(n, static_cast<float>(in.oil_production));
        w.nation_set_oil_supply_elasticity(n, static_cast<float>(in.oil_elasticity));
        if(D.energy_sector && in.oil_production > 0.0) {
            const double ene = va[static_cast<uint8_t>(D.energy_sector.index())];
            w.nation_set_oil_share_of_energy(n, static_cast<float>(std::min(0.9, oil_value * P.oil_value_added_share / ene)));
        }

    }

    // Banking, bonds, prices, society and the production network (docs/03, docs/04).
    void calibrate_finance(dcon::nation_id n, nation_input const& in, float rate0, double y) {
        auto& w = state.world;
        auto const& P = state.params;
        const uint8_t S = state.derived.sector_count;
        // Bonds: the starting term premium reproduces the December 2019 yield.
        const float y10 = std::isfinite(in.yield10) ? static_cast<float>(in.yield10) : rate0 + 1.5f;
        w.nation_set_yield10(n, y10);
        w.nation_set_yield10_0(n, y10);
        w.nation_set_term_premium0(n, y10 - rate0); // expected short rate = policy rate at the start
        w.nation_set_avg_maturity(n, static_cast<float>(in.avg_maturity));
        w.nation_set_foreign_debt_share(n, static_cast<float>(in.foreign_debt_share));
        w.nation_set_fx_debt_share(n, static_cast<float>(in.fx_debt_share));
        w.nation_set_debt_prev(n, w.nation_get_gov_debt0(n));
        // Credit: stocks, lending rate, banks.
        const double loans = in.private_credit * in.gdp;
        w.nation_set_loans_hh0(n, loans * in.hh_credit_share);
        w.nation_set_loans_firm0(n, loans * (1.0 - in.hh_credit_share));
        w.nation_set_loans_hh(n, loans * in.hh_credit_share);
        w.nation_set_loans_firm(n, loans * (1.0 - in.hh_credit_share));
        w.nation_set_loans_prev(n, 1.0);
        w.nation_set_loans_start(n, loans);
        w.nation_set_state_bank_share(n, static_cast<float>(in.state_bank_share));
        w.nation_set_cb_independence(n, static_cast<float>(in.cb_independence));
        w.nation_set_fx_loan_share(n, static_cast<float>(in.fx_loan_share));
        w.nation_set_fx_deposit_share(n, static_cast<float>(in.fx_deposit_share));
        w.nation_set_npl0(n, static_cast<float>(in.npl));
        w.nation_set_npl(n, static_cast<float>(in.npl));
        w.nation_set_capital_ratio0(n, static_cast<float>(in.capital_ratio));
        w.nation_set_capital_ratio(n, static_cast<float>(in.capital_ratio));
        w.nation_set_bank_capital(n, in.capital_ratio * P.rwa_density * loans);
        w.nation_set_lending_spread(n, static_cast<float>(in.lending_spread));
        const float iw = static_cast<float>(P.i_long_weight);
        w.nation_set_lending_rate0(n, (1.0f - iw) * rate0 + iw * y10 + static_cast<float>(in.lending_spread));
        w.nation_set_lending_rate(n, w.nation_get_lending_rate0(n));
        w.nation_set_real_rate0(n, rate0 - static_cast<float>(100.0 * in.inflation_trend));
        // Exchange-rate pass-through rises with trend inflation (Taylor 2000).
        w.nation_set_fx_passthrough_n(n, static_cast<float>(std::clamp(P.fx_passthrough_base + P.fx_passthrough_inflation * in.inflation_trend, 0.0,
                                                                       P.fx_passthrough_max)));
        // Price basket.
        w.nation_set_food_cpi_share(n, static_cast<float>(in.food_cpi));
        w.nation_set_energy_cpi_share(n, static_cast<float>(P.gas_cpi_share));
        w.nation_set_food_price(n, 1.0f);
        // Firms and trade policy.
        w.nation_set_coordination(n, static_cast<float>(in.coordination));
        w.nation_set_soe_share(n, static_cast<float>(in.state_bank_share)); // SOE presence tracks state banking (OECD 2017)
        w.nation_set_tariff(n, static_cast<float>(in.tariff));
        w.nation_set_tariff0(n, static_cast<float>(in.tariff));
        w.nation_set_tariff_prev(n, static_cast<float>(in.tariff));
        // Labour institutions, taxes, trust, assets (docs/03, docs/04).
        w.nation_set_union_density(n, static_cast<float>(in.union_density));
        w.nation_set_bargaining_coverage(n, static_cast<float>(in.bargaining_coverage));
        w.nation_set_min_wage_bite(n, static_cast<float>(in.min_wage_bite));
        w.nation_set_min_wage(n, 1.0f);
        w.nation_set_min_wage0(n, 1.0f);
        w.nation_set_wage_real(n, 1.0f);
        w.nation_set_tax_scale(n, 1.0f);
        w.nation_set_tax_progressivity(n, static_cast<float>(in.tax_progressivity));
        w.nation_set_social_trust(n, static_cast<float>(in.trust));
        w.nation_set_social_trust0(n, static_cast<float>(in.trust));
        w.nation_set_crime(n, 1.0f);
        w.nation_set_house_price(n, 1.0f);
        w.nation_set_equity_price(n, 1.0f);
        w.nation_set_gdp_pc0(n, static_cast<float>(in.gdp / in.population));
        w.nation_set_carbon_price(n, static_cast<float>(in.carbon_price));
        w.nation_set_carbon_price0(n, static_cast<float>(in.carbon_price));
        w.nation_set_top1_share0(n, static_cast<float>(in.top1));
        for(uint8_t s = 0; s < S; ++s) w.nation_set_sector_price(n, s, 1.0f);
        // Society.
        w.nation_set_youth_share(n, static_cast<float>(in.youth_share));
        w.nation_set_youth_unemp_rel(n, static_cast<float>(in.youth_unemp_rel));
        w.nation_set_connectivity(n, static_cast<float>(std::clamp(in.social_media, 0.0, 1.0)));
        w.nation_set_corruption(n, static_cast<float>(std::clamp(1.0 - in.corruption_cpi / 100.0, 0.0, 1.0)));
        w.nation_set_emigrant_stock(n, static_cast<float>(in.emigrant_stock));
        w.nation_set_urban_share(n, static_cast<float>(in.urban));
        // Online controls at the start (Freedom on the Net): domestic information control and
        // cross-border filtering, by one rule for every nation.
        const double unfree = std::clamp(1.0 - in.net_freedom / 100.0, 0.0, 1.0);
        const auto ic = static_cast<float>(P.net_control_scale * unfree);
        const auto filt = static_cast<float>(std::clamp((unfree - P.net_filter_threshold) / (1.0 - P.net_filter_threshold), 0.0, 1.0));
        w.nation_set_info_control(n, ic);
        w.nation_set_info_control0(n, ic);
        w.nation_set_info_control_prev(n, ic);
        w.nation_set_net_filtering(n, filt);
        w.nation_set_net_filtering0(n, filt);
        // Production network: imported share of each intermediate input (import share of
        // demand x the input's import intensity), raised where domestic output could not
        // cover domestic intermediate use; stocks start at target.
        double intensity = 0.0;
        for(uint8_t r = 0; r < S; ++r) intensity += w.sector_get_import_intensity(dcon::sector_id{r}) / S;
        std::array<double, max_sectors> x{};
        for(uint8_t s = 0; s < S; ++s) x[s] = w.nation_get_va0(n, s) / std::max(0.05f, w.sector_get_value_added_share(dcon::sector_id{s}));
        for(uint8_t r = 0; r < S; ++r) {
            const dcon::sector_id rid{r};
            double use = 0.0;
            for(uint8_t u = 0; u < S; ++u) use += w.sector_get_io_coeff(dcon::sector_id{u}, r) * x[u];
            double m = std::clamp(w.nation_get_import_share(n) * w.sector_get_import_intensity(rid) / std::max(1e-6, intensity), P.io_import_floor, 0.9);
            if(use > 0.0) m = std::max(m, 1.0 - 0.85 * x[r] / use);
            w.nation_set_io_import(n, r, static_cast<float>(std::clamp(m, 0.0, 0.95)));
        }
        for(uint8_t u = 0; u < S; ++u) {
            const float days = w.sector_get_inventory_days(dcon::sector_id{u});
            w.nation_set_gross_output(n, u, static_cast<float>(x[u]));
            w.nation_set_io_demand(n, u, static_cast<float>(x[u]));
            w.nation_set_io_constraint(n, u, 1.0f);
            for(uint8_t r = 0; r < S; ++r) {
                const float a = w.sector_get_io_coeff(dcon::sector_id{u}, r);
                const float m = w.nation_get_io_import(n, r);
                const auto k = static_cast<uint8_t>(r * S + u);
                w.nation_set_io_stock(n, k, (1.0f - m) * a * static_cast<float>(x[u]) * days);
                w.nation_set_io_stock_m(n, k, m * a * static_cast<float>(x[u]) * days);
            }
        }
        // Goods share of exports (shipped sectors) for the logistics system.
        double goods = 0.0;
        for(uint8_t s = 0; s < S; ++s) goods += w.sector_get_shipped(dcon::sector_id{s}) * w.nation_get_comp_x(n, s);
        w.nation_set_goods_export_share(n, static_cast<float>(std::clamp(goods, 0.0, 1.0)));
        w.nation_set_seaborne_share(n, 0.75f); // overwritten by logistics_curated.csv
        w.nation_set_input_availability(n, 1.0f);
        w.nation_set_import_cost_index(n, 1.0f);
        w.nation_set_import_cost_prev(n, 1.0f);
        w.nation_set_trade_cost_factor(n, 1.0f);
        w.nation_set_domestic_logistics(n, 1.0f);
        (void)y;
    }

    // HSV progressivity: each pop's tax rate scales as (income per head / mean)^tau,
    // revenue-neutral against the flat calibrated rate (Heathcote, Storesletten &
    // Violante 2017).
    void calibrate_tax_progressivity(dcon::nation_id n) {
        auto& w = state.world;
        const float tau = w.nation_get_tax_progressivity(n);
        double inc = 0.0, size = 0.0;
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                inc += std::max(0.0f, loc.get_pop().get_income());
                size += loc.get_pop().get_size();
            }
        }
        const double mean = size > 0.0 ? inc / size : 1.0;
        double base = 0.0, weighted = 0.0;
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                const double pc = p.get_size() > 0.0f ? std::max(0.0f, p.get_income()) / p.get_size() : mean;
                const double f = std::pow(std::max(1e-3, pc / mean), static_cast<double>(tau));
                base += std::max(0.0f, p.get_income());
                weighted += f * std::max(0.0f, p.get_income());
                p.set_tax_factor(static_cast<float>(f));
            }
        }
        const double norm = weighted > 0.0 ? base / weighted : 1.0;
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) loc.get_pop().set_tax_factor(static_cast<float>(loc.get_pop().get_tax_factor() * norm));
        }
    }

    // Second calibration phase, after one probe pass of the economy has computed
    // every pop's baseline income: consumption and saving per pop, balances, Gini.
    void finish(dcon::nation_id n, nation_input const& in) {
        auto& w = state.world;
        const double C0 = w.nation_get_c0(n);
        const double debt0 = w.nation_get_gov_debt0(n);
        double weighted = 0.0;
        const std::array<double, 4> mpc = {1.0, 0.9, 0.7, 0.95};
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                weighted += mpc[p.get_kind()] * std::max(0.0f, p.get_income());
            }
        }
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                const double inc = std::max(1.0f, p.get_income());
                const double c_p = C0 * mpc[p.get_kind()] * inc / weighted;
                p.set_income0(static_cast<float>(inc));
                p.set_income_perm(static_cast<float>(inc));
                p.set_saving_rate0(static_cast<float>(1.0 - c_p / inc));
                p.set_consumption(static_cast<float>(c_p));
                p.set_c_des(static_cast<float>(c_p));
                const double deposits = inc * 365.0 * (p.get_kind() == 2 ? 1.5 : 0.5);
                p.set_balance(deposits);
                p.set_balance_trend(deposits);
            }
        }
        w.nation_set_c_des_total(n, C0);

        // Society: demography rates, baselines for relative deprivation and stress,
        // initial values (priors from income and voice until survey data, e.g. WVS, is loaded).
        w.nation_set_birth_rate(n, static_cast<float>(in.birth_rate));
        w.nation_set_birth_rate0(n, static_cast<float>(in.birth_rate));
        double retired = 0.0, mass_inc = 0.0, mass_n = 0.0, elite_u = 0.0, elite_n = 0.0;
        const double gdp_pc = in.gdp / in.population;
        const float secular = static_cast<float>(std::clamp(0.35 * std::log(gdp_pc / 12000.0), -1.0, 1.0));
        const float selfexp = static_cast<float>(std::clamp(0.3 * std::log(gdp_pc / 12000.0) + 0.15 * in.voice, -1.0, 1.0));
        const std::array<float, 4> econ_right = {-0.3f, 0.0f, 0.3f, 0.1f};
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                const auto k = p.get_kind();
                p.set_income_expected(p.get_income());
                p.set_values(static_cast<uint8_t>(value_axis::secular), k == 3 ? secular - 0.2f : secular);
                p.set_values(static_cast<uint8_t>(value_axis::self_expression), k == 3 ? selfexp - 0.2f : selfexp);
                p.set_values(static_cast<uint8_t>(value_axis::economic_right), econ_right[k]);
                p.set_salience(0, 0.4f);
                p.set_salience(1, 0.1f);
                p.set_salience(2, 0.2f);
                p.set_salience(3, 0.3f);
                if(k == static_cast<uint8_t>(pop_kind::retired)) {
                    retired += p.get_size();
                } else if(k <= static_cast<uint8_t>(pop_kind::mid_skill)) {
                    mass_inc += p.get_income();
                    mass_n += p.get_size();
                } else {
                    elite_u += p.get_unemployed();
                    elite_n += p.get_size();
                }
            }
        }
        const double old_deaths = in.death_rate * in.population - state.params.young_mortality * (in.population - retired);
        w.nation_set_death_rate_old(n, static_cast<float>(std::clamp(retired > 0 ? old_deaths / retired : 0.04, 0.01, 0.15)));
        w.nation_set_mass_wage0(n, mass_n > 0 ? static_cast<float>(mass_inc / mass_n) : 1.0f);
        w.nation_set_elite_unemp0(n, elite_n > 0 ? static_cast<float>(elite_u / elite_n) : 0.0f);
        w.nation_set_tertiary_share(n, static_cast<float>(in.tertiary));
        // Identity groups: income per head relative to the national mean at the start.
        {
            double inc = 0.0, size = 0.0;
            for(auto own : w.nation_get_region_ownership(n)) {
                for(auto loc : own.get_region().get_pop_location()) {
                    inc += std::max(0.0f, loc.get_pop().get_income());
                    size += loc.get_pop().get_size();
                }
            }
            for(auto g : w.in_social_group) {
                if(g.get_nation() != n) continue;
                double gi = 0.0, gs = 0.0;
                for(auto own : w.nation_get_region_ownership(n)) {
                    for(auto loc : own.get_region().get_pop_location()) {
                        if(loc.get_pop().get_group() != g.id) continue;
                        gi += std::max(0.0f, loc.get_pop().get_income());
                        gs += loc.get_pop().get_size();
                    }
                }
                const float rel = size > 0.0 && gs > 0.0 && inc > 0.0 ? static_cast<float>((gi / gs) / (inc / size)) : 1.0f;
                g.set_income_rel(rel);
                g.set_income_rel0(rel);
            }
        }
        w.nation_set_legitimacy(n, static_cast<float>(std::clamp(0.6 + 0.1 * in.voice, 0.3, 0.85)));
        w.nation_set_media_freedom(n, static_cast<float>(std::clamp((in.voice + 2.5) / 5.0, 0.0, 1.0)));
        // Tax compliance at the start (the reference for later changes; fiscal.cpp).
        w.nation_set_compliance(n, compliance_target(state, n));
        w.nation_set_compliance0(n, w.nation_get_compliance(n));
        // Finance and long-run baselines.
        w.nation_set_ca0(n, (w.nation_get_x0(n) - w.nation_get_m0(n)) / std::max(1.0f, w.nation_get_gdp0_daily(n)));
        double worker_income = 0.0;
        for(auto own : w.nation_get_region_ownership(n)) {
            auto reg = own.get_region();
            const double lat = reg.get_lat() != 0.0f ? reg.get_lat() : in.lat;
            reg.set_temp_base(static_cast<float>(28.0 - 0.45 * std::abs(lat)));
            for(auto loc : reg.get_pop_location()) {
                if(loc.get_pop().get_kind() < worker_kinds) worker_income += loc.get_pop().get_inc_work();
            }
        }
        w.nation_set_worker_income0(n, static_cast<float>(worker_income));
        w.nation_set_trend_growth0(n, static_cast<float>(in.trend_growth));
        w.nation_set_bal_gov(n, -debt0);
        w.nation_set_bal_fin(n, debt0);
        w.nation_set_bal_firms(n, 0.0);
        w.nation_set_bal_row(n, 0.0);

        // Within-group dispersion matching the national Gini (bisection). The top tail
        // keeps the WID shape, but WID shares are pre-tax while the Gini is of disposable
        // income: scale the tail's share down (first to post-tax, ~0.7, then further if the
        // Gini cannot be met even with a tight body) so the national Gini holds.
        w.nation_set_top1_share0(n, static_cast<float>(in.top1 * 0.7));
        if(in.top1 > 0.0 && compute_gini(state, n, 0.02f) > in.gini) {
            double tlo = 0.0, thi = in.top1 * 0.7;
            for(int it = 0; it < 30; ++it) {
                const double mid = 0.5 * (tlo + thi);
                w.nation_set_top1_share0(n, static_cast<float>(mid));
                if(compute_gini(state, n, 0.05f) < in.gini) tlo = mid; else thi = mid;
            }
            w.nation_set_top1_share0(n, static_cast<float>(tlo));
        }
        double lo = 0.02, hi = 2.0;
        for(int it = 0; it < 40; ++it) {
            const double mid = 0.5 * (lo + hi);
            if(compute_gini(state, n, static_cast<float>(mid)) < in.gini) lo = mid; else hi = mid;
        }
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                loc.get_pop().set_inc_sigma(static_cast<float>(0.5 * (lo + hi)));
            }
        }
        w.nation_set_gini(n, compute_gini(state, n));
        if(std::abs(w.nation_get_gini(n) - in.gini) > 0.02) {
            warn(in.iso + ": Gini calibration off (" + std::to_string(w.nation_get_gini(n)) + " vs " + std::to_string(in.gini) + ")");
        }
    }

    // ------------------------------------------------------------ agents and polity links
    // One executive per nation; one central bank per currency area (own currency = own bank).
    // Traits are priors from governance data: less voice -> stronger preference for order
    // (stricter measures), higher income -> higher weight on health, effectiveness -> state capacity.
    void create_agents() {
        auto& w = state.world;
        auto const& D = state.derived;
        w.agent_resize_override_mode(D.lever_count);
        w.agent_resize_override_value(D.lever_count);
        w.agent_resize_last_pragmatic(D.lever_count);
        w.agent_resize_last_ideological(D.lever_count);
        w.agent_resize_last_prior(D.lever_count);
        w.agent_resize_last_choice(D.lever_count);
        w.agent_resize_last_ai_choice(D.lever_count);

        const std::string dir = assets + "/scenario/" + scenario;
        std::map<std::string, std::string> union_of;
        std::map<std::string, std::string> peg_of;
        if(exists(dir + "/monetary_curated.csv")) {
            const auto m = core::csv_table::load(dir + "/monetary_curated.csv", diag);
            for(std::size_t row = 0; row < m.rows(); ++row) {
                const std::string iso(m.get(row, "iso3"));
                const std::string regime(m.get(row, "regime"));
                if(!find_nation(state, iso)) {
                    continue;
                }
                if(regime == "union") {
                    union_of[iso] = std::string(m.get(row, "area"));
                } else if(regime == "peg") {
                    peg_of[iso] = std::string(m.get(row, "anchor"));
                } else if(regime != "own") {
                    { diag.fail(dir + "/monetary_curated.csv: unknown regime '" + regime + "'"); return; }
                }
            }
        }

        std::map<std::string, dcon::agent_id> banks;
        for(auto const& iso : order) {
            auto const& in = inputs[iso];
            const auto n = find_nation(state, iso);
            const double gdp_pc = in.gdp / in.population;
            const float health = gdp_pc >= 12500 ? 1.5f : gdp_pc >= 4000 ? 1.0f : gdp_pc >= 1000 ? 0.7f : 0.5f;

            const auto exec = w.create_agent();
            w.agent_set_kind(exec, static_cast<uint8_t>(agent_kind::executive));
            w.agent_set_rho_base(exec, static_cast<float>(std::clamp(0.3 - 0.08 * in.voice, 0.1, 0.6)));
            w.agent_set_rho(exec, w.agent_get_rho_base(exec));
            w.agent_set_health_weight(exec, health);
            w.agent_set_econ_weight(exec, 1.0f);
            w.agent_set_order_ideal(exec, static_cast<float>(std::clamp(0.2 - 0.12 * in.voice, 0.0, 0.6)));
            w.agent_set_fiscal_hawk(exec, 0.3f);
            // Growth-first credit activism: the means (state banks) and, above all, the
            // absence of an independent check on money and banks (Dinc 2005; Bircan & Saka 2021).
            w.agent_set_credit_bias(exec, static_cast<float>(std::clamp(2.5 * std::sqrt(in.state_bank_share) * (1.0 - in.cb_independence)
                                                                            * (1.0 - in.cb_independence), 0.0, 1.0)));
            w.force_create_executive_seat(exec, n);

            const std::string area = union_of.count(iso) ? union_of[iso] : iso;
            auto it = banks.find(area);
            if(it == banks.end()) {
                const auto bank = w.create_agent();
                w.agent_set_kind(bank, static_cast<uint8_t>(agent_kind::central_bank));
                w.agent_set_rho_base(bank, 0.1f); // technocratic
                w.agent_set_rho(bank, 0.1f);
                it = banks.emplace(area, bank).first;
            }
            w.force_create_monetary_authority(n, it->second);
            // Union federation and military (docs/05 agents).
            const auto uni = w.create_agent();
            w.agent_set_kind(uni, static_cast<uint8_t>(agent_kind::union_federation));
            w.agent_set_rho_base(uni, static_cast<float>(std::clamp(0.2 + in.union_density, 0.1, 0.6)));
            w.agent_set_rho(uni, w.agent_get_rho_base(uni));
            w.nation_set_union_agent(n, uni);
            const auto mil = w.create_agent();
            w.agent_set_kind(mil, static_cast<uint8_t>(agent_kind::military));
            w.agent_set_order_ideal(mil, 0.6f); // militaries value order
            w.agent_set_rho_base(mil, 0.5f);
            w.agent_set_rho(mil, 0.5f);
            w.nation_set_military_agent(n, mil);
            w.nation_set_military_loyalty(n, static_cast<float>(std::clamp(0.6 + 0.1 * in.voice, 0.3, 0.85)));
            w.nation_set_immigration_openness(n, 1.0f);
            w.nation_set_defense_share(n, static_cast<float>(in.defense));
            w.nation_set_defense_share0(n, static_cast<float>(in.defense));
            w.nation_set_voice(n, static_cast<float>(in.voice));
            w.nation_set_state_capacity(n, static_cast<float>(std::clamp(0.5 + 0.25 * in.effectiveness, 0.0, 1.0)));
        }
        for(auto const& [iso, anchor] : peg_of) {
            const auto a = find_nation(state, anchor);
            if(!a) {
                { diag.fail(dir + "/monetary_curated.csv: unknown anchor '" + anchor + "'"); return; }
            }
            w.force_create_currency_peg(find_nation(state, iso), a);
        }

        // Sovereignty (dependent jurisdictions) and territorial claims (optional tables).
        if(exists(dir + "/jurisdictions_curated.csv")) {
            const auto j = core::csv_table::load(dir + "/jurisdictions_curated.csv", diag);
            for(std::size_t row = 0; row < j.rows(); ++row) {
                const auto sub = find_nation(state, j.get(row, "iso3"));
                const auto sov = find_nation(state, j.get(row, "sovereign"));
                if(!sub || !sov) {
                    continue; // jurisdiction not in this scenario
                }
                const auto rel = w.force_create_sovereignty(sub, sov);
                w.sovereignty_set_autonomy(rel, static_cast<float>(j.number_or(row, "autonomy", 0.5)));
            }
        }
        if(exists(dir + "/claims.csv")) {
            const auto c = core::csv_table::load(dir + "/claims.csv", diag);
            for(std::size_t row = 0; row < c.rows(); ++row) {
                const auto claimant = find_nation(state, c.get(row, "claimant"));
                const auto holder = find_nation(state, c.get(row, "holder"));
                if(!claimant || !holder) {
                    continue;
                }
                // Region-level claims need region ids; a nation-level claim covers all its regions.
                for(auto own : w.nation_get_region_ownership(holder)) {
                    const auto rel = w.force_create_region_claim(own.get_region(), claimant);
                    w.region_claim_set_strength(rel, static_cast<float>(c.number_or(row, "strength", 1.0)));
                }
            }
        }
    }

    // ------------------------------------------------------------ party systems
    struct party_input {
        std::string key;
        double econ = 0, social = 0, vote = 0, seats = 0;
        bool in_government = false;
        party_family family = party_family::oth;
    };
    std::map<std::string, std::vector<party_input>> parties_of_nation;

    static party_family parse_family(std::string_view f) {
        if(f == "soc") return party_family::soc;
        if(f == "con") return party_family::con;
        if(f == "lib") return party_family::lib;
        if(f == "chr") return party_family::chr;
        if(f == "grn") return party_family::grn;
        if(f == "rad_right") return party_family::rad_right;
        if(f == "rad_left") return party_family::rad_left;
        if(f == "com") return party_family::com;
        if(f == "reg") return party_family::reg;
        if(f == "agr") return party_family::agr;
        return party_family::oth;
    }

    // Real party systems (parties_curated.csv): positions from expert surveys, last
    // election's shares, cabinet membership. Nations without rows keep generic parties.
    void load_parties(std::string const& dir) {
        if(!exists(dir + "/parties_curated.csv")) return;
        const auto t = core::csv_table::load(dir + "/parties_curated.csv", diag);
        for(std::size_t row = 0; row < t.rows(); ++row) {
            const std::string iso(t.get(row, "iso3"));
            if(!inputs.count(iso)) continue;
            party_input p;
            p.key = std::string(t.get(row, "party"));
            p.econ = std::clamp(t.number_or(row, "econ", 0.0), -1.0, 1.0);
            p.social = std::clamp(t.number_or(row, "social", 0.0), -1.0, 1.0);
            p.vote = t.number_or(row, "vote_share", 0.0);
            p.seats = t.number_or(row, "seat_share", 0.0);
            p.in_government = t.number_or(row, "in_government", 0.0) > 0.0;
            p.family = parse_family(t.get(row, "family"));
            if(p.key.empty() || p.key.size() > 4) {
                diag.fail(t.source() + ": party keys need 1-4 characters (" + iso + ")");
                return;
            }
            parties_of_nation[iso].push_back(p);
        }
        for(auto& [iso, list] : parties_of_nation) {
            if(list.size() > 16) {
                diag.fail(dir + "/parties_curated.csv: " + iso + " has more than 16 parties");
                return;
            }
        }
    }

    // Parties sit in the nation's value space: economic left-right as surveyed, and
    // GAL-TAN as a distance from the electorate's own secular / self-expression level
    // (expert scales are cross-national; voters' values are anchored on the nation).
    void create_real_parties(dcon::nation_id n, std::vector<party_input> list, float secular, float selfexp) {
        auto& w = state.world;
        std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.econ < b.econ; });
        dcon::party_id incumbent;
        double best = -1.0, gov_seats = 0.0;
        for(std::size_t j = 0; j < list.size(); ++j) {
            auto const& in = list[j];
            const auto p = w.create_party();
            w.party_set_key(p, core::pack_key(in.key));
            w.party_set_local_index(p, static_cast<uint8_t>(j));
            w.party_set_family(p, static_cast<uint8_t>(in.family));
            const bool mainstream = in.family == party_family::soc || in.family == party_family::con || in.family == party_family::lib
                                    || in.family == party_family::chr;
            w.party_set_strategy(p, static_cast<uint8_t>(mainstream ? party_strategy::aggregator : party_strategy::sticker));
            w.party_set_position(p, static_cast<uint8_t>(value_axis::economic_right), static_cast<float>(0.8 * in.econ));
            w.party_set_position(p, static_cast<uint8_t>(value_axis::secular), static_cast<float>(secular - 0.6 * in.social));
            w.party_set_position(p, static_cast<uint8_t>(value_axis::self_expression), static_cast<float>(selfexp - 0.6 * in.social));
            w.party_set_vote_share(p, static_cast<float>(in.vote));
            w.party_set_seat_share(p, static_cast<float>(in.seats));
            w.party_set_last_vote(p, static_cast<float>(in.vote));
            w.party_set_in_government(p, in.in_government ? 1 : 0);
            w.force_create_party_membership(p, n);
            if(in.in_government) {
                gov_seats += in.seats;
                if(in.seats > best) {
                    best = in.seats;
                    incumbent = p;
                }
            }
        }
        if(incumbent) {
            w.nation_set_incumbent(n, incumbent);
            w.nation_set_gov_seat_share(n, static_cast<float>(std::clamp(gov_seats, 0.0, 1.0)));
        }
    }

    // ------------------------------------------------------------ politics and relations
    // Three generic parties per nation (L / C / R) until party data is loaded; systems,
    // incumbents and election dates from politics_curated.csv; relations_curated.csv for
    // alliances and status-quo sanctions.
    void create_politics() {
        auto& w = state.world;
        const std::string dir = assets + "/scenario/" + scenario;
        const auto N = static_cast<uint32_t>(w.nation_size());
        w.nation_resize_transit_days(N);
        w.nation_resize_transit_days0(N);
        w.nation_resize_land_share(N);
        w.nation_resize_trade_distance(N);
        w.nation_resize_pair_cost(N);
        w.nation_resize_coupling(N);
        w.nation_resize_fta(N);
        w.nation_resize_bilateral_exports(N);
        w.nation_resize_trust(N);
        w.nation_resize_threat_to(N);
        w.nation_resize_diaspora(N); // bilateral stocks come with the full dataset; grows with flows
        w.nation_resize_energy_dependence(N);
        w.nation_resize_energy_dependence0(N);
        w.nation_resize_control(N);
        w.nation_resize_portfolio_party(static_cast<uint32_t>(portfolio::count));
        w.nation_resize_trade_share(N);
        w.nation_resize_sanction(N);
        w.nation_resize_sanction_prior(N);
        w.nation_resize_alliance(N);
        w.nation_resize_at_war(N);
        w.party_resize_position(static_cast<uint32_t>(value_axis::count));
        w.pop_resize_party_support(16);

        struct sys {
            std::string system = "pr";
            double term = 4;
            std::string next;
            std::string incumbent = "C";
            double presidential = 0, veto = 0.3, defense = 2.0, pr_threshold = 0.0, smd_share = 0.0;
        };
        std::map<std::string, sys> systems;
        if(exists(dir + "/politics_curated.csv")) {
            const auto t = core::csv_table::load(dir + "/politics_curated.csv", diag);
            for(std::size_t row = 0; row < t.rows(); ++row) {
                sys s;
                s.system = std::string(t.get(row, "system"));
                s.term = t.number_or(row, "term_years", 4.0);
                s.next = std::string(t.get(row, "next_election"));
                s.incumbent = std::string(t.get(row, "incumbent"));
                s.presidential = t.number_or(row, "presidential", 0.0);
                s.veto = t.number_or(row, "veto", 0.3);
                s.defense = t.number_or(row, "defense_pct", 2.0);
                s.pr_threshold = t.number_or(row, "pr_threshold", 0.0);
                s.smd_share = t.number_or(row, "smd_share", 0.0);
                systems[std::string(t.get(row, "iso3"))] = s;
            }
        }
        load_parties(dir);
        if(diag.failed()) return;
        w.party_resize_heading(static_cast<uint32_t>(value_axis::count));
        w.nation_resize_window_center(static_cast<uint32_t>(value_axis::count));
        w.nation_resize_window_width(static_cast<uint32_t>(value_axis::count));
        w.nation_resize_policy_position(static_cast<uint32_t>(value_axis::count));
        const char* sides[3] = {"L", "C", "R"};
        for(auto const& iso : order) {
            const auto n = find_nation(state, iso);
            auto const& in = inputs[iso];
            const sys s = systems.count(iso) ? systems[iso] : sys{};
            const auto es = s.system == "fptp"        ? electoral_system::fptp
                            : s.system == "none"      ? electoral_system::none
                            : s.system == "mixed"     ? electoral_system::mixed
                            : s.system == "two_round" ? electoral_system::two_round
                                                      : electoral_system::pr;
            w.nation_set_electoral_system(n, static_cast<uint8_t>(es));
            w.nation_set_pr_threshold(n, static_cast<float>(s.pr_threshold));
            w.nation_set_smd_share(n, static_cast<float>(s.smd_share));
            w.nation_set_presidential(n, s.presidential > 0.0 ? 1 : 0);
            w.nation_set_veto(n, static_cast<float>(s.veto));
            w.nation_set_defense_share(n, static_cast<float>(s.defense));
            w.nation_set_defense_share0(n, static_cast<float>(s.defense));
            w.nation_set_competitive(n, es != electoral_system::none ? 1 : 0);
            w.nation_set_term_days(n, static_cast<int32_t>(s.term * 365.25));
            if(auto d = core::parse_date(s.next)) {
                w.nation_set_next_election(n, core::tick_from_date(*d));
            }
            const float electoral = static_cast<float>(std::clamp((in.voice + 2.5) / 5.0, 0.0, 1.0));
            w.nation_set_electoral(n, electoral);
            w.nation_set_liberal(n, 0.5f * electoral + 0.5f * w.nation_get_state_capacity(n));
            w.nation_set_winning_coalition(n, 0.05f + 0.6f * electoral);
            w.nation_set_gov_seat_share(n, 0.55f);
            const float secular = static_cast<float>(std::clamp(0.35 * std::log(in.gdp / in.population / 12000.0), -1.0, 1.0));
            const float selfexp = static_cast<float>(std::clamp(0.3 * std::log(in.gdp / in.population / 12000.0) + 0.15 * in.voice, -1.0, 1.0));
            if(parties_of_nation.count(iso)) {
                create_real_parties(n, parties_of_nation[iso], secular, selfexp);
                continue;
            }
            // No party data: three generic parties L / C / R.
            for(uint8_t j = 0; j < 3; ++j) {
                const auto p = w.create_party();
                w.party_set_key(p, core::pack_key(sides[j]));
                w.party_set_local_index(p, j);
                w.party_set_strategy(p, static_cast<uint8_t>(party_strategy::aggregator));
                const float side = static_cast<float>(j) - 1.0f; // -1, 0, +1
                w.party_set_position(p, static_cast<uint8_t>(value_axis::economic_right), 0.5f * side);
                w.party_set_position(p, static_cast<uint8_t>(value_axis::secular), secular - 0.2f * side);
                w.party_set_position(p, static_cast<uint8_t>(value_axis::self_expression), secular - 0.2f * side);
                w.force_create_party_membership(p, n);
                if(s.incumbent == sides[j]) {
                    w.nation_set_incumbent(n, p);
                    w.party_set_in_government(p, 1);
                }
            }
        }
        if(exists(dir + "/relations_curated.csv")) {
            const auto t = core::csv_table::load(dir + "/relations_curated.csv", diag);
            for(std::size_t row = 0; row < t.rows(); ++row) {
                const auto a = find_nation(state, t.get(row, "from"));
                const auto b = find_nation(state, t.get(row, "to"));
                if(!a || !b) continue;
                const auto ka = static_cast<uint8_t>(a.index()), kb = static_cast<uint8_t>(b.index());
                if(t.number_or(row, "alliance", 0.0) > 0.0) {
                    w.nation_set_alliance(a, kb, 1.0f);
                    w.nation_set_alliance(b, ka, 1.0f);
                }
                const float sanction = static_cast<float>(t.number_or(row, "sanction", 0.0));
                w.nation_set_sanction(a, kb, sanction);
                w.nation_set_sanction_prior(a, kb, sanction);
            }
        }
        // Reserve currencies (lower sovereign risk) and the starting spread / institutions.
        if(exists(dir + "/monetary_curated.csv")) {
            const auto m = core::csv_table::load(dir + "/monetary_curated.csv", diag);
            if(m.has_column("reserve")) {
                for(std::size_t row = 0; row < m.rows(); ++row) {
                    if(auto n = find_nation(state, m.get(row, "iso3"))) w.nation_set_reserve_currency(n, static_cast<float>(m.number_or(row, "reserve", 0.0)));
                }
            }
        }
        for(auto n : w.in_nation) {
            n.set_liberal0(n.get_liberal());
            n.set_tertiary0(n.get_tertiary_share());
            n.set_spread0(compute_spread(state, n.id));
            n.set_spread(n.get_spread0());
        }
        load_trade_tables(state, assets + "/scenario/" + scenario, diag);
        init_logistics_routes(state); // sea distances feed gravity trade shares
        init_geopolitics(state);
        init_logistics_flows(state);
        for(auto n : w.in_nation) {
            n.set_trade_access0(trade_access(state, n.id));
        }
        init_politics(state);
    }

    void setup_oil() {
        auto& w = state.world;
        auto const& P = state.params;
        const auto c = w.create_climate();
        w.climate_set_co2_ppm(c, static_cast<float>(P.co2_ppm0));
        w.climate_set_temperature(c, static_cast<float>(P.temperature0));
        w.climate_set_damage(c, static_cast<float>(P.damage_quadratic * P.temperature0 * P.temperature0));
        w.climate_set_emissions(c, static_cast<float>(P.emissions0));
        w.climate_set_emission_intensity(c, static_cast<float>(P.emissions0 / P.oil_demand0));
        w.climate_set_remittance_index(c, 1.0f);
        const auto oil = w.create_commodity();
        w.commodity_set_key(oil, core::pack_key("oil"));
        w.commodity_set_price(oil, static_cast<float>(P.oil_price0));
        w.commodity_set_price0(oil, static_cast<float>(P.oil_price0));
        w.commodity_set_demand0(oil, static_cast<float>(P.oil_demand0));
        w.commodity_set_world_demand(oil, static_cast<float>(P.oil_demand0));
        w.commodity_set_supply(oil, static_cast<float>(P.oil_demand0));
    }

    void distribute_oil() {
        auto& w = state.world;
        auto const& P = state.params;
        double ppp_total = 0.0, listed = 0.0;
        dcon::nation_id row;
        for(auto const& iso : order) {
            ppp_total += inputs[iso].gdp_ppp;
            if(iso == "ROW") {
                row = find_nation(state, iso);
            } else {
                listed += inputs[iso].oil_production;
            }
        }
        for(auto const& iso : order) {
            const auto n = find_nation(state, iso);
            w.nation_set_oil_demand0(n, static_cast<float>(P.oil_demand0 * inputs[iso].gdp_ppp / ppp_total));
            w.nation_set_oil_demand(n, w.nation_get_oil_demand0(n));
        }
        if(row) {
            const float residual = static_cast<float>(std::max(0.0, P.oil_demand0 - listed));
            w.nation_set_oil_production0(row, residual);
            w.nation_set_oil_production(row, residual);
            w.nation_set_oil_target(row, residual);
            inputs["ROW"].oil_production = residual;
        }
    }

    void run() {
        auto& w = state.world;
        load_params();
        if(diag.failed()) return;
        load_npis();
        if(diag.failed()) return;
        load_pathogens();
        if(diag.failed()) return;
        load_sectors();
        if(diag.failed()) return;
        // Array sizes are fixed for the scenario.
        const auto S = static_cast<uint32_t>(sector_by_key.size());
        const auto K = static_cast<uint32_t>(npi_by_key.size());
        w.nation_resize_va0(S);
        w.nation_resize_emp0(S);
        w.nation_resize_output(S);
        w.nation_resize_capacity(S);
        w.nation_resize_demand(S);
        w.nation_resize_hours(S);
        w.nation_resize_labor_need(S);
        w.nation_resize_comp_c(S);
        w.nation_resize_comp_g(S);
        w.nation_resize_comp_i(S);
        w.nation_resize_comp_x(S);
        w.nation_resize_npi(K);
        w.nation_resize_npi_targeted(K);
        w.nation_resize_prior_npi(K);
        w.nation_resize_prior_npi_targeted(K);
        w.nation_resize_flows(static_cast<uint32_t>(flow::count));
        w.nation_resize_tag_code(static_cast<uint32_t>(flow::count) * tags_per_flow);
        w.nation_resize_tag_amount(static_cast<uint32_t>(flow::count) * tags_per_flow);
        w.nation_resize_tag_rest(static_cast<uint32_t>(flow::count));
        const auto NP = static_cast<uint32_t>(std::max<std::size_t>(1, pathogen_by_key.size()));
        w.region_resize_ep_s(NP * age_groups);
        w.region_resize_ep_e(NP * age_groups);
        w.region_resize_ep_i(NP * age_groups);
        w.region_resize_ep_r(NP * age_groups);
        w.region_resize_ep_h(NP * age_groups);
        w.region_resize_ep_d(NP * age_groups);
        w.region_resize_beta(NP);
        w.region_resize_imported(NP);
        w.region_resize_infections_p(NP);
        w.region_resize_deaths_p(NP);
        w.region_resize_deaths_reported_p(NP);
        w.region_resize_cum_infections_p(NP);
        w.region_resize_va0(S);
        w.region_resize_capacity(S);
        w.region_resize_output(S);
        w.region_resize_util(S);
        w.pop_resize_employed(S);
        w.pop_resize_emp0(S);
        w.pop_resize_wage0(S);
        w.pop_resize_hours(S);
        w.pop_resize_need_slow(S);
        w.pop_resize_values(static_cast<uint32_t>(value_axis::count));
        w.pop_resize_salience(static_cast<uint32_t>(issue::count));
        w.nation_resize_npi_cut(S);
        w.nation_resize_sector_factor(S);
        w.nation_resize_fill(S);
        w.nation_resize_gross_output(S);
        w.nation_resize_io_demand(S);
        w.nation_resize_io_import(S);
        w.nation_resize_io_constraint(S);
        if(S * S > 255) {
            diag.fail(assets + "/defs/sectors.txt: at most 15 sectors fit the input-output stock arrays");
            return;
        }
        w.nation_resize_io_stock(S * S);
        w.nation_resize_io_stock_m(S * S);
        w.nation_resize_sector_price(S);

        load_nations();
        if(diag.failed()) return;
        load_regions();
        if(diag.failed()) return;
        for(auto const& iso : order) {
            create_nation(inputs[iso]);
        }
        load_adjacency();
        if(diag.failed()) return;
        load_sea_network(state, assets, diag);      // after nations: littoral links
        load_commodity_defs(state, assets, diag);
        load_technology_defs(state, assets, diag);
        if(diag.failed()) return;
        setup_oil();
        rebuild_derived(state);
        if(!state.derived.gov_sector) {
            { diag.fail(assets + "/defs/sectors.txt: a sector with role{government} is required"); return; }
        }
        load_events();
        if(diag.failed()) return;
        distribute_oil();
        for(auto const& iso : order) {
            calibrate(find_nation(state, iso), inputs[iso]);
        }
        update_economy(state, economy_mode::baseline_probe);
        // Progressive household taxes need incomes: calibrate, then probe again.
        for(auto const& iso : order) calibrate_tax_progressivity(find_nation(state, iso));
        update_economy(state, economy_mode::baseline_probe);
        for(auto const& iso : order) {
            finish(find_nation(state, iso), inputs[iso]);
        }
        create_agents();
        if(diag.failed()) return;
        create_politics();
        init_firms(state); // after politics: boards and tariffs read the governing party
        {
            std::vector<observed_adoption> observed;
            const std::string tdir = assets + "/scenario/" + scenario;
            if(exists(tdir + "/technology_curated.csv")) {
                const auto t = core::csv_table::load(tdir + "/technology_curated.csv", diag);
                for(std::size_t row = 0; row < t.rows(); ++row) {
                    const auto nation = find_nation(state, t.get(row, "iso3"));
                    dcon::technology_id tech;
                    for(auto tt : w.in_technology) {
                        if(tt.get_key() == core::pack_key(t.get(row, "tech"))) tech = tt.id;
                    }
                    if(nation && tech) observed.push_back({nation, tech, static_cast<float>(t.number_or(row, "adoption_2019", 0.0))});
                }
            }
            init_technologies(state, observed);
        }
        init_frontier_gaps(state);
        // Fossil share of energy capacity at the start: 1 - renewables adoption (electricity).
        for(auto n : w.in_nation) {
            float ren = 0.0f;
            for(auto t : w.in_technology) {
                if(t.get_emission_cut() > 0.0f) ren = n.get_tech_adoption(static_cast<uint8_t>(t.id.index()));
            }
            n.set_fossil_share0(1.0f - ren);
            n.set_fossil_share(1.0f - ren);
        }
        for(auto n : w.in_nation) {
            if(auto inc = n.get_incumbent()) n.set_gov_econ0(w.party_get_position(inc, static_cast<uint8_t>(value_axis::economic_right)));
            if(auto inc = n.get_incumbent()) n.set_gov_tan0(-w.party_get_position(inc, static_cast<uint8_t>(value_axis::self_expression)));
        }
        init_strategy(state);
    }
};

} // namespace

dcon::nation_id find_nation(game_state const& state, std::string_view iso3) noexcept {
    const uint32_t key = core::pack_key(iso3);
    for(auto n : state.world.in_nation) {
        if(n.get_iso3() == key) {
            return n.id;
        }
    }
    return {};
}

void rebuild_derived(game_state& state) {
    auto& w = state.world;
    auto& d = state.derived;
    d = derived_indices{};
    d.sector_count = static_cast<uint8_t>(w.sector_size());
    d.npi_count = static_cast<uint8_t>(w.npi_indicator_size());
    d.pathogen_count = static_cast<uint8_t>(w.pathogen_size());
    // Severity: population IFR relative to the reference (about SARS-CoV-2 wild type).
    for(auto p : w.in_pathogen) {
        const float ifr = 0.85f * p.get_ifr_young() + 0.15f * p.get_ifr_old();
        p.set_severity(ifr / static_cast<float>(state.params.severity_reference_ifr));
    }
    d.lever_base = d.npi_count;
    d.lever_count = static_cast<uint8_t>(d.npi_count + static_cast<uint8_t>(lever::count));
    for(auto s : w.in_sector) {
        switch(static_cast<sector_role>(s.get_role())) {
        case sector_role::government: d.gov_sector = s.id; break;
        case sector_role::energy: d.energy_sector = s.id; break;
        case sector_role::contact: d.contact_sector = s.id; break;
        case sector_role::manufacturing: d.manufacturing_sector = s.id; break;
        default: break;
        }
    }
    for(auto k : w.in_npi_indicator) {
        const auto idx = static_cast<uint8_t>(k.id.index());
        switch(static_cast<npi_role>(k.get_role())) {
        case npi_role::travel: d.npi_travel = idx; break;
        case npi_role::testing: d.npi_testing = idx; break;
        case npi_role::tracing: d.npi_tracing = idx; break;
        case npi_role::income_support: d.npi_income_support = idx; break;
        case npi_role::stay_home: d.npi_stay_home = idx; break;
        case npi_role::movement: d.npi_movement = idx; break;
        default: break;
        }
    }
    for(auto c : w.in_commodity) {
        if(c.get_key() == core::pack_key("oil")) {
            d.oil = c.id;
        }
    }
}

scenario_report build_scenario(game_state& state, std::string const& assets_dir, std::string const& scenario) {
    builder b{state, assets_dir, scenario};
    b.run();
    state.current_tick = 0;
    b.report.error = b.diag.error();
    return std::move(b.report);
}

} // namespace geosim::sim
