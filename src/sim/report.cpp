#include "sim/report.hpp"

#include "core/calendar.hpp"
#include "core/keys.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <cstdio>

#include <cmath>

namespace geosim::sim {

void begin_measurement(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) {
        n.set_acc_bal_gov_start(n.get_bal_gov());
        n.set_acc_gdp_real(0.0);
        n.set_acc_gdp_nominal(0.0);
        n.set_acc_unemployment(0.0);
        n.set_acc_cpi(0.0);
        n.set_acc_days(0.0);
        n.set_gini0(n.get_gini());
    }
    if(state.derived.oil) {
        w.commodity_set_acc_price(state.derived.oil, 0.0);
        w.commodity_set_acc_days(state.derived.oil, 0.0);
    }
}

world_summary summarize(game_state const& state) {
    auto const& w = state.world;
    world_summary out;
    if(state.derived.oil && w.commodity_get_acc_days(state.derived.oil) > 0) {
        out.oil_price_mean = w.commodity_get_acc_price(state.derived.oil) / w.commodity_get_acc_days(state.derived.oil);
    }
    for(auto n : w.in_nation) {
        nation_summary s;
        s.iso3 = core::unpack_key(n.get_iso3());
        const double days = std::max(1.0, n.get_acc_days());
        s.gdp_growth_pct = (n.get_acc_gdp_real() / days * 365.0 / n.get_gdp0() - 1.0) * 100.0;
        s.unemployment_pct = n.get_acc_unemployment() / days * 100.0;
        // The run starts with CPI = 1 on 2020-01-01; the 2019 mean is ~half a year of trend inflation earlier.
        const double cpi_2019_mean = std::pow(1.0 + n.get_inflation_trend(), -0.5);
        s.inflation_pct = (n.get_acc_cpi() / days / cpi_2019_mean - 1.0) * 100.0;
        const double nominal = n.get_acc_gdp_nominal() / days * 365.0;
        s.gov_net_lending_pct = (n.get_bal_gov() - n.get_acc_bal_gov_start()) / (nominal * days / 365.0) * 100.0;
        double deaths = 0.0;
        for(auto own : n.get_region_ownership()) {
            deaths += own.get_region().get_deaths_reported();
        }
        s.deaths_per_million = deaths / n.get_population() * 1e6;
        for(uint8_t p = 0; p < state.derived.pathogen_count; ++p) {
            double dp = 0.0;
            for(auto own : n.get_region_ownership()) dp += own.get_region().get_deaths_reported_p(p);
            s.deaths_by_pathogen.emplace_back(core::unpack_key(w.pathogen_get_key(dcon::pathogen_id{p})), dp / n.get_population() * 1e6);
        }
        s.gini_start = n.get_gini0();
        s.gini_end = n.get_gini();
        double pop_now = 0.0, unrest = 0.0;
        for(auto own : n.get_region_ownership()) {
            pop_now += own.get_region().get_population();
            unrest += own.get_region().get_unrest() * own.get_region().get_population();
        }
        s.population_change_pct = (pop_now / std::max(1.0, static_cast<double>(n.get_population() - n.get_migration_net())) - 1.0) * 100.0;
        s.net_migration_per_1000 = n.get_migration_net() / std::max(1.0, static_cast<double>(n.get_population())) * 1000.0;
        s.unrest_mean = pop_now > 0.0 ? unrest / pop_now : 0.0;
        s.psi = n.get_psi();
        out.nations.push_back(s);
    }
    return out;
}

void write_flows(game_state const& state, dcon::nation_id n, std::ostream& out) {
    static constexpr char const* names[] = {"consumption", "gov_purchases", "exports", "imports", "wages_private",
        "wages_public", "job_retention_pay", "unemployment_benefits", "pensions", "fiscal_households", "fiscal_firms",
        "tax_households", "tax_indirect", "tax_firms", "tax_resource", "dividends", "interest_gov", "interest_households", "household_transfers"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<std::size_t>(flow::count));
    auto const& w = state.world;
    const double days = std::max(1.0, w.nation_get_acc_days(n));
    const double gdp = w.nation_get_acc_gdp_nominal(n) / days * 365.0 * (days / 365.0);
    out << "flows for " << core::unpack_key(w.nation_get_iso3(n)) << " (year to date, % of nominal GDP over the period)\n";
    for(uint8_t k = 0; k < static_cast<uint8_t>(flow::count); ++k) {
        char line[96];
        std::snprintf(line, sizeof(line), "  %-24s %8.2f\n", names[k], w.nation_get_flows(n, k) / gdp * 100.0);
        out << line;
    }
}

void write_daily_header(std::ostream& out) {
    out << "date,iso3,stringency,new_infections,new_deaths_reported,deaths_reported_per_million,gdp_index,"
           "unemployment_pct,cpi,policy_rate,gov_balance_pct_gdp,consumption_index,exports_index,gini,oil_price,r_eff,oil_demand_mbd,oil_supply_mbd,oil_inventory_mb,nation_oil_demand_index\n";
}

void write_daily_rows(game_state const& state, std::ostream& out) {
    auto const& w = state.world;
    const auto date = core::format_date(core::date_from_tick(state.current_tick - 1));
    const auto oil_id = state.derived.oil;
    const float oil = oil_id ? w.commodity_get_price(oil_id) : 0.0f;
    const float oil_d = oil_id ? w.commodity_get_world_demand(oil_id) : 0.0f;
    const float oil_s = oil_id ? w.commodity_get_supply(oil_id) : 0.0f;
    const double oil_inv = oil_id ? w.commodity_get_inventory(oil_id) : 0.0;
    for(auto n : w.in_nation) {
        // Simple stringency proxy: mean of containment indicators (C1-C8) scaled to 0-100.
        float sum = 0.0f;
        int count = 0;
        for(auto k : w.in_npi_indicator) {
            const auto key = core::unpack_key(k.get_key());
            if(!key.empty() && key[0] == 'C') {
                sum += npi_fraction(state, n.id, static_cast<uint8_t>(k.id.index()));
                ++count;
            }
        }
        double infections = 0.0, deaths = 0.0, cum = 0.0, r_eff = 0.0;
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            double alive = 0.0, susceptible = 0.0;
            // R_eff of the first pathogen (the scenario's main epidemic).
            for(uint8_t a = 0; a < age_groups; ++a) {
                susceptible += r.get_ep_s(epi_index(0, a));
                alive += r.get_ep_s(epi_index(0, a)) + r.get_ep_e(epi_index(0, a)) + r.get_ep_i(epi_index(0, a)) + r.get_ep_r(epi_index(0, a));
            }
            r_eff = alive > 0.0 && state.derived.pathogen_count > 0
                        ? r.get_beta(0) * w.pathogen_get_infectious_days(dcon::pathogen_id{0}) * susceptible / alive : 0.0;
            infections += r.get_new_infections();
            deaths += r.get_new_deaths() * n.get_death_reporting();
            cum += r.get_deaths_reported();
        }
        const float gf = growth_factor(state, n.id);
        const double gdp_index = n.get_gdp_real() / (n.get_gdp0_daily() * gf);
        const double bal_pct = (n.get_bal_gov() - n.get_acc_bal_gov_start()) / n.get_gdp0() * 100.0;
        out << date << ',' << core::unpack_key(n.get_iso3()) << ',' << (count ? 100.0f * sum / static_cast<float>(count) : 0.0f) << ','
            << infections << ',' << deaths << ',' << cum / n.get_population() * 1e6 << ',' << gdp_index << ','
            << n.get_unemployment() * 100.0f << ',' << n.get_cpi() << ',' << n.get_policy_rate() << ',' << bal_pct << ','
            << n.get_consumption() / (n.get_c0() * gf) << ',' << n.get_exports() / (n.get_x0() * gf) << ',' << n.get_gini() << ','
            << oil << ',' << r_eff << ',' << oil_d << ',' << oil_s << ',' << oil_inv << ','
            << n.get_oil_demand() / (n.get_oil_demand0() * gf) << '\n';
    }
}

} // namespace geosim::sim
