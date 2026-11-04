// Applies dated events (historical anchors, epidemic seeds, oil supply).

#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>

namespace geosim::sim {

namespace {

bool is_policy(event_kind k) noexcept {
    switch(k) {
    case event_kind::npi_level:
    case event_kind::policy_rate:
    case event_kind::fiscal_package:
    case event_kind::job_retention:
    case event_kind::test_trace_isolate:
        return true;
    default:
        return false; // seeds and foreign (OPEC+) supply decisions are exogenous to the slice
    }
}

} // namespace

void apply_events(game_state& state) {
    auto& w = state.world;
    const bool anchors = state.params.historical_anchors != 0.0;
    for(auto e : w.in_event) {
        if(e.get_done() || e.get_tick() > state.current_tick) {
            continue;
        }
        e.set_done(1);
        const auto kind = static_cast<event_kind>(e.get_kind());
        if(is_policy(kind) && !anchors) {
            continue;
        }
        const auto n = e.get_target();
        switch(kind) {
        case event_kind::npi_level:
            w.nation_set_npi(n, e.get_param(), e.get_value());
            w.nation_set_npi_targeted(n, e.get_param(), e.get_value2());
            break;
        case event_kind::policy_rate:
            w.nation_set_policy_rate(n, e.get_value());
            w.nation_set_rate_anchored(n, 1);
            break;
        case event_kind::fiscal_package:
            w.nation_set_fiscal_pending(n, w.nation_get_fiscal_pending(n)
                + static_cast<double>(e.get_value()) / 100.0 * w.nation_get_gdp0(n) * w.nation_get_core_price(n));
            break;
        case event_kind::epidemic_seed: {
            const auto r = capital_region(state, n);
            if(r) {
                const float s = w.region_get_ep_s(r, 0);
                const float x = std::min(s, e.get_value());
                w.region_set_ep_s(r, 0, s - x);
                w.region_set_ep_e(r, 0, w.region_get_ep_e(r, 0) + x);
            }
            break;
        }
        case event_kind::job_retention:
            w.nation_set_stw_coverage(n, e.get_value());
            w.nation_set_stw_replacement(n, e.get_value2());
            break;
        case event_kind::test_trace_isolate:
            w.nation_set_tti_strength(n, e.get_value());
            break;
        case event_kind::oil_production:
            w.nation_set_oil_target(n, e.get_value());
            w.nation_set_oil_target_set(n, 1);
            break;
        case event_kind::oil_production_delta: {
            const float base = w.nation_get_oil_target_set(n) ? w.nation_get_oil_target(n) : w.nation_get_oil_production0(n);
            w.nation_set_oil_target(n, std::max(0.0f, base + e.get_value()));
            w.nation_set_oil_target_set(n, 1);
            break;
        }
        }
    }
}

} // namespace geosim::sim
