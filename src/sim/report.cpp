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
        n.set_loans_start(n.get_loans_hh() + n.get_loans_firm());
    }
    for(auto s : w.in_shipping) {
        s.set_acc_rate(0.0);
        s.set_acc_days(0.0);
        s.set_acc_idle(0.0);
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
        s.yield10_start_pct = n.get_yield10_0();
        s.yield10_end_pct = n.get_yield10();
        s.credit_growth_pct = n.get_credit_growth() * 100.0;
        s.credit_impulse_pct = n.get_credit_impulse() * 100.0;
        s.npl_pct = n.get_npl() * 100.0;
        s.exchange_rate = n.get_exchange_rate();
        out.nations.push_back(s);
    }
    for(auto sh : w.in_shipping) {
        out.freight_index_mean = sh.get_acc_days() > 0.0 ? sh.get_acc_rate() / sh.get_acc_days() : sh.get_spot_rate();
        out.freight_index_end = sh.get_spot_rate();
        out.fleet_idle_peak = sh.get_acc_idle();
    }
    return out;
}

void write_flows(game_state const& state, dcon::nation_id n, std::ostream& out) {
    static constexpr char const* names[] = {"consumption", "gov_purchases", "exports", "imports", "wages_private",
        "wages_public", "job_retention_pay", "unemployment_benefits", "pensions", "fiscal_households", "fiscal_firms",
        "tax_households", "tax_indirect", "tax_firms", "tax_resource", "dividends", "interest_gov", "interest_households", "household_transfers", "remittances_in", "remittances_out", "sovereign_default",
        "credit", "credit_deposits", "interest_loans", "credit_guarantee", "commodity_terms", "network_transfers", "war_spending"};
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

void write_groups(game_state const& state, dcon::nation_id n, std::ostream& out) {
    auto const& w = state.world;
    out << "identity groups of " << core::unpack_key(w.nation_get_iso3(n))
        << " (income per head relative to the national mean; start -> now)\n";
    char line[160];
    std::snprintf(line, sizeof(line), "  %-5s %7s %13s %8s %9s %9s %9s %9s\n", "group", "share", "income_rel", "unemp%", "informal",
                  "excluded", "grievance", "mobilise");
    out << line;
    for(auto g : w.in_social_group) {
        if(g.get_nation() != n) continue;
        double informal = 0.0, size = 0.0;
        for(auto p : w.in_pop) {
            if(p.get_group() == g.id) {
                informal += p.get_informal() * p.get_size();
                size += p.get_size();
            }
        }
        std::snprintf(line, sizeof(line), "  %-5s %7.3f %6.2f->%5.2f %8.2f %9.2f %9.2f %9.3f %9.3f\n", core::unpack_key(g.get_key()).c_str(),
                      g.get_pop_share(), g.get_income_rel0(), g.get_income_rel(), g.get_unemployment() * 100.0f,
                      size > 0.0 ? informal / size : 0.0, g.get_excluded(), g.get_grievance(), g.get_mobilisation());
        out << line;
    }
}

void write_logistics(game_state const& state, std::ostream& out) {
    auto const& w = state.world;
    char line[200];
    out << "chokepoints\n";
    std::snprintf(line, sizeof(line), "  %-4s %8s %8s %6s %8s %8s %9s %11s\n", "key", "traffic0", "traffic", "open", "queue", "wait_d",
                  "incidents", "shipdays_lost");
    out << line;
    for(auto c : w.in_chokepoint) {
        std::snprintf(line, sizeof(line), "  %-4s %8.1f %8.1f %6.2f %8.1f %8.2f %9.0f %11.0f\n", core::unpack_key(c.get_key()).c_str(),
                      c.get_traffic0(), c.get_traffic(), c.get_open(), c.get_queue(), c.get_wait_days(), c.get_incidents(), c.get_ship_days_lost());
        out << line;
    }
    for(auto s : w.in_shipping) {
        std::snprintf(line, sizeof(line), "container fleet: spot rate %.2f, contract %.2f, utilisation %.3f, deployed %.3f, fleet %.3f, orderbook %.3f\n",
                      s.get_spot_rate(), s.get_contract_rate(), s.get_utilisation(), s.get_deployed(), s.get_fleet(), s.get_orderbook());
        out << line;
    }
}

void write_daily_header(std::ostream& out) {
    out << "date,iso3,stringency,new_infections,new_deaths_reported,deaths_reported_per_million,gdp_index,"
           "unemployment_pct,cpi,policy_rate,gov_balance_pct_gdp,consumption_index,exports_index,gini,oil_price,r_eff,oil_demand_mbd,oil_supply_mbd,oil_inventory_mb,nation_oil_demand_index,"
           "freight_index,logistics_delay_days,port_dwell_days,yield10,credit_growth,credit_impulse,npl,exchange_rate,food_price,unrest,protest,directed_credit_pct_gdp,qe_pct_gdp,info_control,lending_rate,capital_ratio\n";
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
            << n.get_oil_demand() / (n.get_oil_demand0() * gf);
        float freight = 1.0f, unrest = 0.0f, protest = 0.0f, rpop = 0.0f;
        for(auto sh : w.in_shipping) freight = sh.get_spot_rate();
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            unrest += r.get_unrest() * r.get_population();
            protest += r.get_protest() * r.get_population();
            rpop += r.get_population();
        }
        out << ',' << freight << ',' << n.get_logistics_delay() << ',' << n.get_port_dwell() << ',' << n.get_yield10() << ','
            << n.get_credit_growth() << ',' << n.get_credit_impulse() << ',' << n.get_npl() << ',' << n.get_exchange_rate() << ','
            << n.get_food_price() << ',' << (rpop > 0 ? unrest / rpop : 0.0f) << ',' << (rpop > 0 ? protest / rpop : 0.0f) << ','
            << n.get_directed_credit() << ',' << n.get_qe_rate() << ',' << n.get_info_control() << ',' << n.get_lending_rate() << ','
            << n.get_capital_ratio() << '\n';
    }
}

} // namespace geosim::sim
