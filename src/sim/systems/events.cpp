// Applies dated events. Policy events are *anchors*: they update the deciding
// agent's prior for that lever (what was decided in reality at this date); the
// AI (systems/ai.cpp) then chooses the actual policy. Epidemic seeds are
// exogenous and act directly.

#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>

namespace geosim::sim {

namespace {

} // namespace

void apply_events(game_state& state) {
    auto& w = state.world;
    for(auto e : w.in_event) {
        if(e.get_done() || e.get_tick() > state.current_tick) {
            continue;
        }
        e.set_done(1);
        const auto kind = static_cast<event_kind>(e.get_kind());
        const auto n = e.get_target();
        switch(kind) {
        case event_kind::npi_level:
            w.nation_set_prior_npi(n, e.get_param(), e.get_value());
            w.nation_set_prior_npi_targeted(n, e.get_param(), e.get_value2());
            w.nation_set_prior_npi_set(n, 1);
            break;
        case event_kind::policy_rate:
            w.nation_set_prior_rate(n, e.get_value());
            w.nation_set_prior_rate_set(n, 1);
            break;
        case event_kind::fiscal_package:
            w.nation_set_prior_fiscal_cum(n, w.nation_get_prior_fiscal_cum(n) + e.get_value());
            w.nation_set_prior_fiscal_set(n, 1);
            break;
        case event_kind::epidemic_seed: {
            const auto r = capital_region(state, n);
            if(r && e.get_param() < state.derived.pathogen_count) {
                const uint8_t i = epi_index(e.get_param(), age_young);
                const float s = w.region_get_ep_s(r, i);
                const float x = std::min(s, e.get_value());
                w.region_set_ep_s(r, i, s - x);
                w.region_set_ep_e(r, i, w.region_get_ep_e(r, i) + x);
            }
            break;
        }
        case event_kind::job_retention:
            w.nation_set_prior_stw_coverage(n, e.get_value());
            w.nation_set_prior_stw_replacement(n, e.get_value2());
            w.nation_set_prior_stw_set(n, 1);
            break;
        case event_kind::test_trace_isolate:
            w.nation_set_prior_tti(n, e.get_value());
            w.nation_set_prior_tti_set(n, 1);
            break;
        case event_kind::oil_production:
            w.nation_set_prior_oil(n, e.get_value());
            w.nation_set_prior_oil_set(n, 1);
            break;
        // Financial shocks (historical anchors for crises such as 2008): the markets
        // and banks then propagate them (assets.cpp wealth effects, credit.cpp lending).
        case event_kind::house_price_shock:
            w.nation_set_house_price(n, std::max(0.05f, w.nation_get_house_price(n) * (1.0f + e.get_value())));
            for(auto own : w.nation_get_region_ownership(n)) {
                own.get_region().set_house_price_r(std::max(0.05f, own.get_region().get_house_price_r() * (1.0f + e.get_value())));
            }
            break;
        case event_kind::equity_shock:
            w.nation_set_equity_price(n, std::max(0.05f, w.nation_get_equity_price(n) * (1.0f + e.get_value())));
            break;
        case event_kind::credit_spread_shock:
            w.nation_set_lending_spread(n, w.nation_get_lending_spread(n) + e.get_value());
            w.nation_set_spread_shock(n, w.nation_get_spread_shock(n) + e.get_value());
            w.nation_set_spread_unwind_days(n, e.get_value2());
            break;
        case event_kind::bank_capital_shock:
            // Losses (e.g. on securitised mortgages) cut bank capital; credit.cpp recomputes the ratio.
            w.nation_set_bank_capital(n, std::max(0.0, w.nation_get_bank_capital(n) - e.get_value() / 100.0 * state.params.rwa_density
                                                         * (w.nation_get_loans_hh(n) + w.nation_get_loans_firm(n))));
            w.nation_set_capital_ratio(n, std::max(0.0f, w.nation_get_capital_ratio(n) - e.get_value() / 100.0f));
            break;
        case event_kind::oil_production_delta: {
            const float base = w.nation_get_prior_oil_set(n) ? w.nation_get_prior_oil(n) : w.nation_get_oil_production0(n);
            w.nation_set_prior_oil(n, std::max(0.0f, base + e.get_value()));
            w.nation_set_prior_oil_set(n, 1);
            break;
        }
        }
    }
}

} // namespace geosim::sim
