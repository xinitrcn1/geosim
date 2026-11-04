// World crude oil market.

#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

// Oil (docs/03-economy.md#prices): demand follows activity and personal
// mobility; producers move toward event-set targets or price-elastic levels;
// inventories absorb the imbalance; the price follows flow and stock gaps.
// World oil demand on its trend path relative to 2019 (demand-weighted growth factors).
float oil_trend_index(game_state const& state) {
    auto const& w = state.world;
    double num = 0.0, den = 0.0;
    for(auto n : w.in_nation) {
        num += static_cast<double>(n.get_oil_demand0()) * growth_factor(state, n.id);
        den += n.get_oil_demand0();
    }
    return den > 0.0 ? static_cast<float>(num / den) : 1.0f;
}

void update_oil_market(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const auto oil = D.oil;
    if(!oil) {
        return;
    }
    const float price = w.commodity_get_price(oil);
    const float price0 = w.commodity_get_price0(oil);

    double demand = 0.0;
    double supply = 0.0;
    for(auto n : w.in_nation) {
        // Activity: oil-weighted output relative to potential.
        double num = 0.0;
        double den = 0.0;
        const float gf = growth_factor(state, n.id);
        for(uint8_t s = 0; s < D.sector_count; ++s) {
            const double weight = w.sector_get_oil_intensity(dcon::sector_id{s}) * n.get_va0(s) * gf;
            num += weight * (n.get_output(s) > 0.0f ? n.get_output(s) / (n.get_va0(s) * gf) : 1.0f);
            den += weight;
        }
        const float activity = den > 0.0 ? static_cast<float>(num / den) : 1.0f;
        const float stay = D.npi_stay_home != 0xFF ? npi_fraction(state, n.id, D.npi_stay_home) : 0.0f;
        const float move = D.npi_movement != 0xFF ? npi_fraction(state, n.id, D.npi_movement) : 0.0f;
        // Personal mobility: orders and fear, both fading with fatigue (Google mobility
        // recovered to -10..-20% by mid-2020 in most countries).
        const float fatigue = static_cast<float>(P.fatigue_max) * std::min(1.0f, n.get_npi_exposure() / static_cast<float>(P.fatigue_days));
        const float fear_fatigue = static_cast<float>(P.fear_fatigue_max) * std::min(1.0f, n.get_npi_exposure() / static_cast<float>(P.fatigue_days));
        const float fear = 1.0f / (1.0f + static_cast<float>(P.voluntary_mobility_k) * (1.0f - fear_fatigue)
                                              * reported_deaths_per_million_daily(state, n.id));
        const float orders = (0.3f * stay + 0.1f * move) * (1.0f - fatigue);
        const float mobility = std::max(0.2f, (1.0f - orders) * fear);
        const float mw = static_cast<float>(P.oil_mobility_weight);
        // Short-run price elasticity of demand (Hamilton 2009: about -0.05 to -0.1).
        const float price_effect = std::pow(std::max(0.05f, price / price0), static_cast<float>(P.oil_demand_elasticity));
        // Technology (EVs) lowers oil use per unit of activity (longcampaign.cpp).
        const float d = n.get_oil_demand0() * gf * ((1.0f - mw) * activity + mw * mobility) * price_effect * tech_oil_factor(state, n.id);
        n.set_oil_demand(d);
        demand += d;

        // Production.
        const float prod0 = n.get_oil_production0();
        if(prod0 <= 0.0f) {
            continue;
        }
        // Target chosen by the producer's executive (systems/ai.cpp); quota-bound producers
        // adjust fast, market-driven ones over months.
        const float target = n.get_oil_target();
        float speed = static_cast<float>(P.oil_production_speed);
        if(!n.get_oil_target_set()) {
            speed *= 0.2f;
        }
        float prod = n.get_oil_production();
        prod += (target - prod) * speed;
        n.set_oil_production(prod);
        supply += prod;
    }

    const double d0 = w.commodity_get_demand0(oil) * oil_trend_index(state);
    double inventory = w.commodity_get_inventory(oil) + (supply - demand); // mb, one day
    w.commodity_set_inventory(oil, inventory);
    w.commodity_set_supply(oil, static_cast<float>(supply));
    w.commodity_set_world_demand(oil, static_cast<float>(demand));

    const double flow_gap = (supply - demand) / d0;
    const double stock_gap = inventory / (d0 * 30.0); // in demand-months
    const double target = price0 * std::exp(-P.oil_flow_sensitivity * flow_gap - P.oil_inventory_sensitivity * stock_gap);
    const double next = price + (target - price) * P.oil_price_speed;
    w.commodity_set_price(oil, static_cast<float>(std::max(1.0, next)));
    w.commodity_set_acc_price(oil, w.commodity_get_acc_price(oil) + next);
    w.commodity_set_acc_days(oil, w.commodity_get_acc_days(oil) + 1.0);
}

} // namespace geosim::sim
