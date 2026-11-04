// World commodity markets other than crude oil (docs/03-economy.md#prices), daily.
//
// Gas, coal, grains and metals (assets/defs/commodities.txt) clear like the oil
// market: demand follows the activity of the sectors that use each commodity
// (or population and income for food), with a short-run price elasticity; supply
// follows producers' 2019 shares with a short-run supply elasticity and, for
// grains, quarterly weather shocks; stocks absorb the gap and the price follows
// flow and stock gaps (inventory theory of storage; Working 1949, Deaton &
// Laroque 1992). Part of grain demand is biofuel and tracks the crude price.
//
// Nations feel these markets in three ways:
//  - terms of trade: net exporters gain (and net importers pay) net exports x
//    price change, a flow with the rest of the world that also moves the
//    current account and so the exchange rate (finance.cpp);
//  - resource rents: governments take a share of the production value change;
//  - prices: the domestic relative price of food passes through part of the world
//    grain price plus a distribution shock when internal freight is disrupted
//    (lockdowns on trucking and markets; logistics.cpp). Food is inelastic, so a
//    small supply cut moves prices a lot; with large CPI food weights (India ~46%)
//    this drives headline inflation where food weights are large (Anand, Prasad &
//    Zhang 2015). Prices are in world-demand units: demand0 = 1 = world 2019 use.
//    Gas reaches the CPI through household energy.

#include "core/keys.hpp"
#include "core/rng.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace geosim::sim {

namespace {

// Activity index of a commodity's demand in nation n (1 = baseline).
float demand_driver(game_state const& state, dcon::commodity_id c, dcon::nation_id n) {
    auto const& w = state.world;
    const uint8_t S = state.derived.sector_count;
    double num = 0.0, den = 0.0;
    const float gf = std::max(0.01f, w.nation_get_gf(n));
    for(uint8_t s = 0; s < S; ++s) {
        const float wt = w.commodity_get_sector_weight(c, s);
        if(wt <= 0.0f) continue;
        const double base = static_cast<double>(w.nation_get_va0(n, s)) * gf;
        if(base <= 0.0) continue;
        num += wt * w.nation_get_output(n, s) / base;
        den += wt;
    }
    if(den > 0.0) {
        return static_cast<float>(num / den);
    }
    // Food: population and real household income (Engel: low income elasticity).
    const float c0 = w.nation_get_c0(n) * gf;
    const float income = c0 > 0.0f ? w.nation_get_consumption(n) / c0 : 1.0f;
    return std::pow(std::max(0.1f, income), w.commodity_get_income_elasticity(c));
}

} // namespace

void update_commodity_markets(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    auto const& D = state.derived;
    const auto tick = static_cast<uint32_t>(state.current_tick);
    const float oil_rel = D.oil ? w.commodity_get_price(D.oil) / w.commodity_get_price0(D.oil) : 1.0f;

    for(auto c : w.in_commodity) {
        if(c.id == D.oil || c.get_world_value() <= 0.0f) {
            continue;
        }
        const auto ci = static_cast<uint8_t>(c.id.index());
        const float price = c.get_price();
        const float rel = price / std::max(1e-6f, c.get_price0());

        // Weather: a new supply shock each quarter (grains).
        if(c.get_harvest_sd() > 0.0f && tick % 91u == 0u) {
            const float u1 = std::max(1e-7f, core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::commodities), tick, ci, 0));
            const float u2 = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::commodities), tick, ci, 1);
            const float z = std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * std::numbers::pi_v<float> * u2);
            c.set_harvest(std::clamp(1.0f + c.get_harvest_sd() * z, 0.8f, 1.2f));
        }
        if(c.get_harvest() <= 0.0f) c.set_harvest(1.0f);

        double demand = 0.0, supply = 0.0, world_gf = 0.0, gf_weight = 0.0;
        const float price_effect = std::pow(std::max(0.05f, rel), c.get_demand_elasticity());
        const float biofuel = c.get_biofuel_share();
        for(auto n : w.in_nation) {
            const float gf = std::max(0.01f, n.get_gf());
            const float driver = demand_driver(state, c.id, n.id);
            const float d = n.get_commodity_cons(ci) * c.get_demand0() * gf * driver * price_effect
                            * ((1.0f - biofuel) + biofuel * std::pow(std::max(0.05f, oil_rel), 0.5f));
            n.set_commodity_demand(ci, d);
            demand += d;
            world_gf += static_cast<double>(gf) * n.get_commodity_prod(ci);
            gf_weight += n.get_commodity_prod(ci);
        }
        const float gf_w = gf_weight > 0.0 ? static_cast<float>(world_gf / gf_weight) : 1.0f;
        // Producers move toward the price-elastic level over a few months.
        float idx = c.get_supply_index() > 0.0f ? c.get_supply_index() : 1.0f;
        // Producer concentration (Herfindahl of production shares) and two strategic
        // responses: a concentrated cartel restrains supply when prices fall below 80% of
        // 2019 (Stigler 1964: cartels hold better when few and large), and food exporters
        // restrict exports in price spikes, which deepens them (Martin & Anderson 2012 on
        // 2008 and 2010-11 grain bans; Headey 2011).
        double hhi = 0.0, prod_total = 0.0;
        for(auto n : w.in_nation) prod_total += n.get_commodity_prod(ci);
        for(auto n : w.in_nation) {
            const double sh = prod_total > 0.0 ? n.get_commodity_prod(ci) / prod_total : 0.0;
            hhi += sh * sh;
        }
        const float cartel = hhi > 0.15 ? std::clamp(static_cast<float>(P.cartel_cut) * static_cast<float>(hhi) * std::max(0.0f, 0.8f - rel) * 5.0f, 0.0f, 0.3f) : 0.0f;
        const float ban = c.get_harvest_sd() > 0.0f ? std::clamp(static_cast<float>(P.export_ban) * std::max(0.0f, rel - 1.3f), 0.0f, 0.2f) : 0.0f;
        c.set_restraint(cartel + ban);
        const float target = std::pow(std::max(0.05f, rel), c.get_supply_elasticity()) * c.get_harvest() * (1.0f - cartel - ban);
        idx += (target - idx) * 0.02f;
        c.set_supply_index(idx);
        supply = static_cast<double>(c.get_demand0()) * gf_w * idx;

        const double d0 = c.get_demand0() * gf_w;
        double inventory = c.get_inventory() + (supply - demand);
        c.set_inventory(inventory);
        c.set_supply(static_cast<float>(supply));
        c.set_world_demand(static_cast<float>(demand));
        const double flow_gap = (supply - demand) / d0;
        const double stock_gap = inventory / (d0 * std::max(1.0f, c.get_inventory_days()));
        const double p_target = c.get_price0() * std::exp(-c.get_flow_sensitivity() * flow_gap - c.get_inventory_sensitivity() * stock_gap);
        const double next = price + (p_target - price) * c.get_price_speed();
        c.set_price(static_cast<float>(std::max(0.05 * c.get_price0(), next)));
        c.set_acc_price(c.get_acc_price() + next);
        c.set_acc_days(c.get_acc_days() + 1.0);
    }

    // Nation effects: terms of trade and rents (flows), domestic food price.
    dcon::commodity_id food, gas;
    for(auto c : w.in_commodity) {
        const auto key = core::unpack_key(c.get_key());
        if(key == "food") food = c.id;
        if(key == "gas") gas = c.id;
    }
    for(auto n : w.in_nation) {
        const float gf = std::max(0.01f, n.get_gf());
        double tot = 0.0, rent = 0.0;
        for(auto c : w.in_commodity) {
            if(c.id == D.oil || c.get_world_value() <= 0.0f) continue;
            const auto ci = static_cast<uint8_t>(c.id.index());
            const double dp = c.get_price() / c.get_price0() - 1.0;
            const double value = static_cast<double>(c.get_world_value()) * 1e9 / 365.0 * gf; // USD per day, world
            tot += (n.get_commodity_prod(ci) - n.get_commodity_cons(ci)) * value * dp;
            rent += n.get_commodity_take(ci) * n.get_commodity_prod(ci) * value * dp;
        }
        // Daily values; the economy posts them weekly (nation_pass) with its other flows.
        n.set_commodity_tot(static_cast<float>(tot));
        n.set_commodity_rent(static_cast<float>(rent));

        // Domestic relative food price: world pass-through + internal distribution shock.
        float target = 1.0f;
        if(food) {
            target += static_cast<float>(P.food_passthrough) * (w.commodity_get_price(food) / w.commodity_get_price0(food) - 1.0f);
        }
        target += static_cast<float>(P.food_logistics_shock) * (1.0f - n.get_domestic_logistics()) / 0.3f;
        float fp = n.get_food_price() > 0.0f ? n.get_food_price() : 1.0f;
        fp += (target - fp) / static_cast<float>(P.food_price_days);
        n.set_food_price(fp);
        (void)gas;
    }
}

float commodity_cpi_factor(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    float f = 1.0f + w.nation_get_food_cpi_share(n) * (w.nation_get_food_price(n) - 1.0f);
    for(auto c : w.in_commodity) {
        if(core::unpack_key(c.get_key()) == "gas" && c.get_price0() > 0.0f) {
            f += w.nation_get_energy_cpi_share(n) * (c.get_price() / c.get_price0() - 1.0f);
        }
    }
    return f;
}

} // namespace geosim::sim
