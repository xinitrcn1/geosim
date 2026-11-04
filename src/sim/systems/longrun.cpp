// Long-run growth and climate (docs/03-economy.md#growth--long-run), every 28 days.
//
// - Capital hysteresis: regional potential output grows or shrinks with the
//   investment gap (investment ~7% of the capital stock a year).
// - Trend growth responds to institutions (liberal index; Acemoglu, Johnson &
//   Robinson 2001) and human capital (tertiary share; Lucas 1988).
// - Climate (DICE-lite; Nordhaus 2017): emissions proportional to world oil
//   demand (calibrated to 36.4 Gt CO2 in 2019), airborne fraction into the
//   atmosphere, transient warming toward the equilibrium of the CO2 forcing,
//   and marginal damages to regional potential: DICE-2016R quadratic loss,
//   larger in hotter regions (Burke, Hsiang & Miguel 2015).

#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};
}

float world_oil_demand(game_state const& state) {
    return state.derived.oil ? state.world.commodity_get_world_demand(state.derived.oil) : 0.0f;
}

void update_longrun(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;

    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        // Investment gap -> capital stock -> potential.
        const float base = n.get_i0() * std::max(0.01f, n.get_gf());
        const float gap = base > 0.0f ? n.get_investment() / base - 1.0f : 0.0f;
        const float grow = std::exp(static_cast<float>(P.capital_hysteresis) * gap * years);
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            for(uint8_t s = 0; s < state.derived.sector_count; ++s) r.set_va0(s, r.get_va0(s) * grow);
        }
        // Institutions and human capital -> trend growth.
        n.set_trend_growth(n.get_trend_growth0() + static_cast<float>(P.institutions_growth) * (n.get_liberal() - n.get_liberal0())
                           + static_cast<float>(P.human_capital_growth) * (n.get_tertiary_share() - n.get_tertiary0())
                           - static_cast<float>(P.net_growth_cost) * std::max(0.0f, n.get_net_filtering() - n.get_net_filtering0()));
    }

    // Climate: global, once per 28 days.
    if(static_cast<uint32_t>(state.current_tick) % monthly_cadence.period_days != 0) {
        return;
    }
    for(auto c : w.in_climate) {
        const float oil = world_oil_demand(state);
        const float emissions = c.get_emission_intensity() * oil;
        c.set_emissions(emissions);
        const float ppm = c.get_co2_ppm() + emissions * static_cast<float>(P.airborne_fraction) / 7.8f * years;
        c.set_co2_ppm(ppm);
        const float equilibrium = static_cast<float>(P.climate_sensitivity) * std::log2(ppm / 280.0f);
        const float t0 = c.get_temperature();
        const float t1 = t0 + (equilibrium - t0) * std::min(1.0f, years / static_cast<float>(P.climate_lag_years));
        c.set_temperature(t1);
        const float d0 = static_cast<float>(P.damage_quadratic) * t0 * t0;
        const float d1 = static_cast<float>(P.damage_quadratic) * t1 * t1;
        c.set_damage(d1);
        const float marginal = d1 - d0;
        if(marginal != 0.0f) {
            for(auto r : w.in_region) {
                const float tilt = std::max(0.2f, 1.0f + static_cast<float>(P.damage_heat_tilt) * (r.get_temp_base() - 13.0f));
                const float keep = 1.0f - marginal * tilt;
                for(uint8_t s = 0; s < state.derived.sector_count; ++s) r.set_va0(s, r.get_va0(s) * keep);
            }
        }
    }
}

} // namespace geosim::sim
