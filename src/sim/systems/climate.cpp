// Climate policy, energy vintages and disasters (docs/03-economy.md#growth--long-run).
//
// Carbon price (executive lever, a bill; weekly). Pragmatic: rises with the
// world output lost to warming since the start, weighted by the public's
// environmental concern (post-materialist, self-expression values; Inglehart
// 1997); ideological: green (GAL) governing parties price carbon higher. 2019
// prices are the start (EU ETS, UK, Canada, Korea, pilots).
// Energy vintages (monthly). Energy capacity is split between fossil and clean
// vintages; clean capacity grows with renewables adoption (technology diffusion),
// which a carbon price speeds up. Above the fossil margin, a carbon price retires
// fossil capacity early - stranded assets (Mercure et al. 2018; van der Ploeg &
// Rezai 2020). The price cuts emissions, raises energy costs (CPI) and raises
// revenue.
// Disasters (monthly, region shard). Major disasters (storms, floods, droughts,
// wildfires) strike at random with a hazard that rises with warming (IPCC AR6)
// and is higher in hot regions; each destroys a lognormal share of regional
// capacity (cyclone losses persist for years; Hsiang & Jina 2014).

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace geosim::sim {

namespace {
constexpr cadence monthly_cadence{28, 28};
}

// Carbon cost per unit of energy-sector output relative to the start (USD per USD).
float carbon_cost_share(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    return std::max(0.0f, w.nation_get_carbon_price(n) - w.nation_get_carbon_price0(n)) * static_cast<float>(state.params.carbon_tonnes_per_gdp) / 1000.0f;
}

// World emissions factor from carbon prices above the start (GDP-weighted).
float carbon_emission_factor(game_state const& state) {
    auto const& w = state.world;
    double num = 0.0, den = 0.0;
    for(auto n : w.in_nation) {
        const float cut = std::min(0.8f, static_cast<float>(state.params.carbon_emission_elasticity)
                                             * std::max(0.0f, n.get_carbon_price() - n.get_carbon_price0()));
        num += n.get_oil_demand0() * (1.0f - cut) * (n.get_fossil_share0() > 0.0f ? n.get_fossil_share() / n.get_fossil_share0() : 1.0f);
        den += n.get_oil_demand0();
    }
    return den > 0.0 ? static_cast<float>(num / den) : 1.0f;
}

void decide_carbon_price(game_state& state, dcon::agent_id a, dcon::nation_id n) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const uint8_t lev = static_cast<uint8_t>(D.lever_base + static_cast<uint8_t>(lever::carbon_price));
    float damage = 0.0f;
    for(auto c : w.in_climate) damage = c.get_damage() - static_cast<float>(P.damage_quadratic * P.temperature0 * P.temperature0);
    const auto se = static_cast<uint8_t>(value_axis::self_expression);
    const float concern = std::clamp(0.5f + w.nation_get_window_center(n, se), 0.0f, 1.5f);
    const float p0 = w.nation_get_carbon_price0(n);
    const float pragmatic = p0 + static_cast<float>(P.carbon_damage_weight) * 100.0f * std::max(0.0f, damage) * concern;
    float green = 0.0f;
    if(auto inc = w.nation_get_incumbent(n)) green = w.party_get_position(inc, se) - w.nation_get_window_center(n, se);
    const float ideal = pragmatic + static_cast<float>(P.carbon_green_shift) * green;
    const float rho = w.agent_get_rho_base(a);
    const float before = w.nation_get_carbon_price(n);
    float proposed = std::max(0.0f, (1.0f - rho) * pragmatic + rho * ideal);
    if(w.agent_get_override_mode(a, lev) != 0) proposed = w.agent_get_override_value(a, lev);
    proposed = before + (proposed - before) * 0.05f; // climate policy moves slowly
    const float enacted = enact(state, n, lev, before, proposed, value_axis::self_expression, 0.002f, false);
    w.agent_set_last_pragmatic(a, lev, pragmatic);
    w.agent_set_last_ideological(a, lev, ideal);
    w.agent_set_last_choice(a, lev, enacted);
    w.nation_set_carbon_price(n, std::clamp(enacted, 0.0f, 500.0f));
}

void update_climate_policy(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    float warming = 0.0f;
    for(auto c : w.in_climate) warming = c.get_temperature() - static_cast<float>(P.temperature0);
    // Renewables technology index (if defined) for the clean vintage share.
    uint8_t ren = 0xFF;
    for(auto t : w.in_technology) {
        if(t.get_emission_cut() > 0.0f) ren = static_cast<uint8_t>(t.id.index());
    }
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        // Fossil vintages shrink as clean capacity diffuses; early retirement above the margin.
        const float progress = ren != 0xFF ? std::max(0.0f, tech_progress(state, n.id, ren)) : 0.0f;
        // Renewables mandates speed clean capacity (reforms.cpp).
        float fossil = n.get_fossil_share0() * (1.0f - std::min(1.0f, progress * (1.0f + static_cast<float>(P.mandate_diffusion) * n.get_energy_mandate())));
        const float over = std::max(0.0f, n.get_carbon_price() - static_cast<float>(P.carbon_fossil_margin)) / static_cast<float>(P.carbon_fossil_margin);
        const float retire = static_cast<float>(P.carbon_stranding) * over * years * fossil;
        n.set_stranded(n.get_stranded() + retire);
        fossil = std::max(0.0f, fossil - n.get_stranded());
        n.set_fossil_share(fossil);
        if(D.energy_sector && retire > 0.0f) {
            const auto es = static_cast<uint8_t>(D.energy_sector.index());
            for(auto own : n.get_region_ownership()) own.get_region().set_va0(es, own.get_region().get_va0(es) * (1.0f - retire));
            n.set_va0(es, n.get_va0(es) * (1.0f - retire));
        }
        // Disasters, per region.
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            const float heat = std::max(0.5f, 1.0f + 0.05f * (r.get_temp_base() - 15.0f));
            const float p = static_cast<float>(P.disaster_rate) * years * heat * std::max(0.0f, 1.0f + static_cast<float>(P.disaster_warming) * warming);
            const auto ri = static_cast<uint32_t>(r.id.index());
            if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::commodities), tick, 0x100000u + ri) < p) {
                const float u1 = std::max(1e-6f, core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::commodities), tick, 0x100000u + ri, 1));
                const float u2 = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::commodities), tick, 0x100000u + ri, 2);
                const float z = std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * std::numbers::pi_v<float> * u2);
                const float loss = std::min(0.3f, static_cast<float>(P.disaster_loss) * std::exp(0.8f * z - 0.32f));
                for(uint8_t s = 0; s < D.sector_count; ++s) r.set_va0(s, r.get_va0(s) * (1.0f - loss));
                n.set_disaster_losses(n.get_disaster_losses() + loss);
                const auto d = w.create_decision();
                w.decision_set_target(d, n.id);
                w.decision_set_tick(d, state.current_tick);
                w.decision_set_lever(d, journal_disaster);
                w.decision_set_before(d, 0.0f);
                w.decision_set_after(d, loss);
                w.decision_set_pragmatic(d, static_cast<float>(r.id.index()));
                w.decision_set_ideological(d, NAN);
                w.decision_set_prior(d, NAN);
            }
        }
    }
}

} // namespace geosim::sim
