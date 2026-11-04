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
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <stdexcept>

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
    std::array<double, max_sectors> va_share{};
    std::array<double, max_sectors> emp_share{};
};

struct builder {
    game_state& state;
    std::string assets;
    std::string scenario;
    scenario_report report;
    std::map<std::string, uint8_t> sector_by_key;
    std::map<std::string, uint8_t> npi_by_key;
    std::map<std::string, nation_input> inputs;
    std::vector<std::string> order;

    void warn(std::string msg) { report.warnings.push_back(std::move(msg)); }

    sector_role parse_sector_role(std::string_view s, std::string const& src) {
        if(s == "generic") return sector_role::generic;
        if(s == "government") return sector_role::government;
        if(s == "energy") return sector_role::energy;
        if(s == "contact") return sector_role::contact;
        if(s == "manufacturing") return sector_role::manufacturing;
        throw std::runtime_error(src + ": unknown sector role '" + std::string(s) + "'");
    }

    npi_role parse_npi_role(std::string_view s, std::string const& src) {
        if(s == "generic") return npi_role::generic;
        if(s == "travel") return npi_role::travel;
        if(s == "testing") return npi_role::testing;
        if(s == "tracing") return npi_role::tracing;
        if(s == "income_support") return npi_role::income_support;
        if(s == "stay_home") return npi_role::stay_home;
        if(s == "movement") return npi_role::movement;
        throw std::runtime_error(src + ": unknown npi role '" + std::string(s) + "'");
    }

    // ------------------------------------------------------------ defs
    void load_params() {
        const std::string path = assets + "/defs/parameters.txt";
        if(!fs::exists(path)) {
            return;
        }
        for(auto const& node : core::load_defs(path)) {
            const double v = core::parse_number(node.text, path, node.line);
            if(!set_param(state.params, node.key, v)) {
                throw std::runtime_error(path + ":" + std::to_string(node.line) + ": unknown parameter '" + node.key + "'");
            }
        }
    }

    void load_npis() {
        const std::string path = assets + "/defs/npi.txt";
        auto& w = state.world;
        for(auto const& node : core::load_defs(path)) {
            if(node.key != "npi") {
                throw std::runtime_error(path + ":" + std::to_string(node.line) + ": expected npi{...}");
            }
            const auto id = w.create_npi_indicator();
            const std::string key(node.req("key", path));
            w.npi_indicator_set_key(id, core::pack_key(key));
            w.npi_indicator_set_role(id, static_cast<uint8_t>(parse_npi_role(node.req("role", path), path)));
            w.npi_indicator_set_max_level(id, static_cast<float>(node.num_or("max_level", 1.0, path)));
            w.npi_indicator_set_transmission_effect(id, static_cast<float>(node.num_or("transmission_effect", 0.0, path)));
            npi_by_key[key] = static_cast<uint8_t>(id.index());
        }
    }

    void load_sectors() {
        const std::string path = assets + "/defs/sectors.txt";
        auto& w = state.world;
        auto nodes = core::load_defs(path);
        w.sector_resize_skill(worker_kinds);
        w.sector_resize_npi_capacity(static_cast<uint32_t>(npi_by_key.size()));
        for(auto const& node : nodes) {
            if(node.key != "sector") {
                throw std::runtime_error(path + ":" + std::to_string(node.line) + ": expected sector{...}");
            }
            const auto id = w.create_sector();
            if(id.index() >= max_sectors) {
                throw std::runtime_error(path + ": too many sectors");
            }
            const std::string key(node.req("key", path));
            w.sector_set_key(id, core::pack_key(key));
            w.sector_set_role(id, static_cast<uint8_t>(parse_sector_role(node.req("role", path), path)));
            w.sector_set_labor_share(id, static_cast<float>(node.num_or("labor_share", 0.5, path)));
            w.sector_set_oil_intensity(id, static_cast<float>(node.num_or("oil_intensity", 1.0, path)));
            w.sector_set_import_intensity(id, static_cast<float>(node.num_or("import_intensity", 1.0, path)));
            w.sector_set_voluntary_distancing(id, static_cast<float>(node.num_or("voluntary_distancing", 0.0, path)));
            if(auto d = node.find("demand")) {
                w.sector_set_demand_hh(id, static_cast<float>(d->num_or("hh", 0.0, path)));
                w.sector_set_demand_gov(id, static_cast<float>(d->num_or("gov", 0.0, path)));
                w.sector_set_demand_inv(id, static_cast<float>(d->num_or("inv", 0.0, path)));
                w.sector_set_demand_exp(id, static_cast<float>(d->num_or("exp", 0.0, path)));
            }
            if(auto sk = node.find("skills")) {
                const float lo = static_cast<float>(sk->num_or("low", 0.33, path));
                const float mi = static_cast<float>(sk->num_or("mid", 0.34, path));
                const float hi = static_cast<float>(sk->num_or("high", 0.33, path));
                const float tot = lo + mi + hi;
                w.sector_set_skill(id, 0, lo / tot);
                w.sector_set_skill(id, 1, mi / tot);
                w.sector_set_skill(id, 2, hi / tot);
            }
            if(auto nc = node.find("npi_capacity")) {
                for(auto const& c : nc->children) {
                    auto it = npi_by_key.find(c.key);
                    if(it == npi_by_key.end()) {
                        throw std::runtime_error(path + ":" + std::to_string(c.line) + ": unknown NPI '" + c.key + "'");
                    }
                    w.sector_set_npi_capacity(id, it->second, static_cast<float>(core::parse_number(c.text, path, c.line)));
                }
            }
            sector_by_key[key] = static_cast<uint8_t>(id.index());
        }
    }

    // ------------------------------------------------------------ nation data
    void load_nations() {
        const std::string dir = assets + "/scenario/" + scenario;
        const auto t = core::csv_table::load(dir + "/nations.csv");
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
                throw std::runtime_error(dir + "/nations.csv: " + in.iso + " needs population, labor_force and gdp_usd");
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
            in.effectiveness = t.number_or(row, "gov_effectiveness_est", 0.0);
            order.push_back(in.iso);
            inputs[in.iso] = in;
        }

        const auto c = core::csv_table::load(dir + "/nations_curated.csv");
        for(std::size_t row = 0; row < c.rows(); ++row) {
            const std::string iso(c.get(row, "iso3"));
            auto it = inputs.find(iso);
            if(it == inputs.end()) {
                throw std::runtime_error(dir + "/nations_curated.csv: unknown nation " + iso);
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

        const auto s = core::csv_table::load(dir + "/sectors.csv");
        for(std::size_t row = 0; row < s.rows(); ++row) {
            const std::string iso(s.get(row, "iso3"));
            const std::string key(s.get(row, "sector"));
            auto it = inputs.find(iso);
            auto sk = sector_by_key.find(key);
            if(it == inputs.end() || sk == sector_by_key.end()) {
                throw std::runtime_error(dir + "/sectors.csv: unknown nation or sector " + iso + "/" + key);
            }
            it->second.va_share[sk->second] = s.number_or(row, "va_share", 0.0);
            it->second.emp_share[sk->second] = s.number_or(row, "emp_share", 0.0);
        }
    }

    // ------------------------------------------------------------ events
    void load_events() {
        const std::string dir = assets + "/events/" + scenario;
        if(!fs::exists(dir)) {
            warn("no events directory " + dir);
            return;
        }
        std::vector<std::string> files;
        for(auto const& e : fs::directory_iterator(dir)) {
            if(e.path().extension() == ".txt") {
                files.push_back(e.path().string());
            }
        }
        std::sort(files.begin(), files.end()); // deterministic event order
        auto& w = state.world;
        for(auto const& path : files) {
            for(auto const& node : core::load_defs(path)) {
                const std::string where = path + ":" + std::to_string(node.line);
                auto date = core::parse_date(node.req("date", path));
                if(!date) {
                    throw std::runtime_error(where + ": bad date");
                }
                const auto nation = find_nation(state, node.req("nation", path));
                if(!nation) {
                    throw std::runtime_error(where + ": unknown nation '" + std::string(node.req("nation", path)) + "'");
                }
                const auto e = w.create_event();
                w.event_set_tick(e, core::tick_from_date(*date));
                w.event_set_target(e, nation);
                auto num = [&](std::string_view key) {
                    auto v = node.num(key, path);
                    if(!v) {
                        throw std::runtime_error(where + ": " + node.key + " needs " + std::string(key) + "{...}");
                    }
                    return static_cast<float>(*v);
                };
                if(node.key == "npi") {
                    const std::string ind(node.req("indicator", path));
                    auto it = npi_by_key.find(ind);
                    if(it == npi_by_key.end()) {
                        throw std::runtime_error(where + ": unknown indicator '" + ind + "'");
                    }
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::npi_level));
                    w.event_set_param(e, it->second);
                    w.event_set_value(e, num("level"));
                    w.event_set_value2(e, static_cast<float>(node.num_or("targeted", 0.0, path)));
                } else if(node.key == "policy_rate") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::policy_rate));
                    w.event_set_value(e, num("rate_pct"));
                } else if(node.key == "fiscal_package") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::fiscal_package));
                    w.event_set_value(e, num("pct_gdp"));
                } else if(node.key == "epidemic_seed") {
                    w.event_set_kind(e, static_cast<uint8_t>(event_kind::epidemic_seed));
                    w.event_set_value(e, num("infections"));
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
                    throw std::runtime_error(where + ": unknown event kind '" + node.key + "'");
                }
                ++report.events;
            }
        }
    }

    // ------------------------------------------------------------ calibration
    void create_nation(nation_input const& in) {
        auto& w = state.world;
        const auto n = w.create_nation();
        w.nation_set_iso3(n, core::pack_key(in.iso));
        const auto r = w.create_region();
        w.force_create_region_ownership(r, n);
        w.region_set_population(r, static_cast<float>(in.population));
        w.region_set_old_share(r, static_cast<float>(in.pop65));
        w.region_set_hemisphere(r, static_cast<float>(in.hemisphere));
        w.region_set_beds_per_1000(r, static_cast<float>(in.beds));
        w.region_set_ep_s(r, age_young, static_cast<float>(in.population * (1.0 - in.pop65)));
        w.region_set_ep_s(r, age_old, static_cast<float>(in.population * in.pop65));
        w.region_set_ifr_mult(r, 1.0f);
        for(uint8_t k = 0; k < 4; ++k) {
            const auto p = w.create_pop();
            w.pop_set_kind(p, k);
            w.force_create_pop_location(p, r);
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
        w.nation_set_util_smoothed(n, 1.0f);

        // Daily output at the start of 2020 (2019 annual level grown half a year).
        const double y = in.gdp * std::pow(1.0 + in.trend_growth, 0.5) / 365.0;
        w.nation_set_gdp0_daily(n, static_cast<float>(y));

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

        // Pops.
        const double unemployed = in.labor_force * in.unemployment;
        const std::array<float, 3> u_share = {0.5f, 0.35f, 0.15f};
        const std::array<float, 4> owner = {0.02f, 0.13f, 0.55f, 0.30f};
        const std::array<float, 4> transfer = {0.20f, 0.05f, 0.0f, 0.75f};
        for(auto own : w.nation_get_region_ownership(n)) {
            for(auto loc : own.get_region().get_pop_location()) {
                auto p = loc.get_pop();
                const auto k = p.get_kind();
                double size = 0.0;
                if(k < worker_kinds) {
                    for(uint8_t s = 0; s < S; ++s) {
                        const float e = w.nation_get_emp0(n, s) * w.sector_get_skill(dcon::sector_id{s}, k);
                        p.set_employed(s, e);
                        size += e;
                    }
                    p.set_unemployed(static_cast<float>(unemployed * u_share[k]));
                    size += unemployed * u_share[k];
                } else {
                    size = in.population * in.pop65;
                }
                p.set_size(static_cast<float>(size));
                p.set_owner_share(owner[k]);
                p.set_transfer_share(transfer[k]);
            }
        }

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

        const double debt0 = in.gov_debt * in.gdp;
        w.nation_set_gov_debt0(n, debt0);
        const double interest0 = debt0 * P.gov_effective_rate / 365.0;
        const double purchases0 = G0 * (1.0 - w.nation_get_comp_g(n, gov_s));
        const double deficit = -in.gov_net_lending * y;

        // Baseline unemployment benefits (same formula as the economy step).
        double benefits0 = 0.0;
        {
            std::array<double, 3> num{}, den{};
            for(uint8_t s = 0; s < S; ++s) {
                const dcon::sector_id sid{s};
                double mix = 0.0;
                for(uint8_t k = 0; k < worker_kinds; ++k) {
                    mix += w.sector_get_skill(sid, k) * (k == 0 ? P.wage_low : k == 1 ? P.wage_mid : P.wage_high);
                }
                const double e0 = std::max(1.0f, w.nation_get_emp0(n, s));
                const double base = w.sector_get_labor_share(sid) * va[s] / e0 / mix;
                for(uint8_t k = 0; k < worker_kinds; ++k) {
                    const double weight = e0 * w.sector_get_skill(sid, k);
                    num[k] += weight * base * (k == 0 ? P.wage_low : k == 1 ? P.wage_mid : P.wage_high);
                    den[k] += weight;
                }
            }
            for(uint8_t k = 0; k < worker_kinds; ++k) {
                benefits0 += unemployed * u_share[k] * (num[k] / den[k]) * in.benefit_replacement * in.benefit_coverage;
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

        // Probe: incomes at the baseline, then consumption and saving per pop.
        update_economy(state, n, 1.0f, economy_mode::baseline_probe);
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
                const double deposits = inc * 365.0 * (p.get_kind() == 2 ? 1.5 : 0.5);
                p.set_balance(deposits);
                p.set_balance_trend(deposits);
            }
        }
        w.nation_set_bal_gov(n, -debt0);
        w.nation_set_bal_fin(n, debt0);
        w.nation_set_bal_firms(n, 0.0);
        w.nation_set_bal_row(n, 0.0);

        // Within-group dispersion matching the national Gini (bisection).
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
        if(fs::exists(dir + "/monetary_curated.csv")) {
            const auto m = core::csv_table::load(dir + "/monetary_curated.csv");
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
                    throw std::runtime_error(dir + "/monetary_curated.csv: unknown regime '" + regime + "'");
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
            w.nation_set_voice(n, static_cast<float>(in.voice));
            w.nation_set_state_capacity(n, static_cast<float>(std::clamp(0.5 + 0.25 * in.effectiveness, 0.0, 1.0)));
        }
        for(auto const& [iso, anchor] : peg_of) {
            const auto a = find_nation(state, anchor);
            if(!a) {
                throw std::runtime_error(dir + "/monetary_curated.csv: unknown anchor '" + anchor + "'");
            }
            w.force_create_currency_peg(find_nation(state, iso), a);
        }

        // Sovereignty (dependent jurisdictions) and territorial claims (optional tables).
        if(fs::exists(dir + "/jurisdictions_curated.csv")) {
            const auto j = core::csv_table::load(dir + "/jurisdictions_curated.csv");
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
        if(fs::exists(dir + "/claims.csv")) {
            const auto c = core::csv_table::load(dir + "/claims.csv");
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

    void setup_oil() {
        auto& w = state.world;
        auto const& P = state.params;
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
        load_npis();
        load_sectors();
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
        w.region_resize_ep_s(age_groups);
        w.region_resize_ep_e(age_groups);
        w.region_resize_ep_i(age_groups);
        w.region_resize_ep_r(age_groups);
        w.region_resize_ep_h(age_groups);
        w.region_resize_ep_d(age_groups);
        w.pop_resize_employed(S);

        load_nations();
        for(auto const& iso : order) {
            create_nation(inputs[iso]);
        }
        setup_oil();
        rebuild_derived(state);
        if(!state.derived.gov_sector) {
            throw std::runtime_error(assets + "/defs/sectors.txt: a sector with role{government} is required");
        }
        load_events();
        distribute_oil();
        for(auto const& iso : order) {
            calibrate(find_nation(state, iso), inputs[iso]);
        }
        create_agents();
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
    builder b{state, assets_dir, scenario, {}, {}, {}, {}, {}};
    b.run();
    state.current_tick = 0;
    return std::move(b.report);
}

} // namespace geosim::sim
