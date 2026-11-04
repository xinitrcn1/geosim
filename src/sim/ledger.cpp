#include "sim/ledger.hpp"

#include <cmath>

namespace geosim::sim {

namespace {

void add(game_state& state, account a, double amount) noexcept {
    auto& w = state.world;
    switch(a.kind) {
    case account_kind::pop:
        w.pop_set_balance(a.pop, w.pop_get_balance(a.pop) + amount);
        break;
    case account_kind::firms:
        w.nation_set_bal_firms(a.nation, w.nation_get_bal_firms(a.nation) + amount);
        break;
    case account_kind::government:
        w.nation_set_bal_gov(a.nation, w.nation_get_bal_gov(a.nation) + amount);
        break;
    case account_kind::finance:
        w.nation_set_bal_fin(a.nation, w.nation_get_bal_fin(a.nation) + amount);
        break;
    case account_kind::rest_of_world:
        w.nation_set_bal_row(a.nation, w.nation_get_bal_row(a.nation) + amount);
        break;
    }
}

} // namespace

void transfer(game_state& state, account from, account to, double amount, flow kind) noexcept {
    if(amount == 0.0) {
        return;
    }
    add(state, from, -amount);
    add(state, to, amount);
    auto& w = state.world;
    const auto k = static_cast<uint8_t>(kind);
    w.nation_set_flows(from.nation, k, w.nation_get_flows(from.nation, k) + amount);
    // Cause tags: keep the top contributions per flow kind, the rest as a remainder.
    if(w.nation_get_tag_rest_size() <= k) return;
    const uint32_t code = state.cause;
    if(code == 0) {
        w.nation_set_tag_rest(from.nation, k, w.nation_get_tag_rest(from.nation, k) + amount);
        return;
    }
    const auto base = static_cast<uint8_t>(k * tags_per_flow);
    int empty = -1, smallest = 0;
    for(int i = 0; i < tags_per_flow; ++i) {
        const auto slot = static_cast<uint8_t>(base + i);
        if(w.nation_get_tag_code(from.nation, slot) == code) {
            w.nation_set_tag_amount(from.nation, slot, w.nation_get_tag_amount(from.nation, slot) + amount);
            return;
        }
        if(w.nation_get_tag_code(from.nation, slot) == 0 && empty < 0) empty = i;
        if(std::abs(w.nation_get_tag_amount(from.nation, slot)) < std::abs(w.nation_get_tag_amount(from.nation, static_cast<uint8_t>(base + smallest)))) smallest = i;
    }
    if(empty >= 0) {
        w.nation_set_tag_code(from.nation, static_cast<uint8_t>(base + empty), code);
        w.nation_set_tag_amount(from.nation, static_cast<uint8_t>(base + empty), amount);
        return;
    }
    const auto low = static_cast<uint8_t>(base + smallest);
    if(std::abs(amount) > std::abs(w.nation_get_tag_amount(from.nation, low))) {
        // Evict the smallest tag into the remainder.
        w.nation_set_tag_rest(from.nation, k, w.nation_get_tag_rest(from.nation, k) + w.nation_get_tag_amount(from.nation, low));
        w.nation_set_tag_code(from.nation, low, code);
        w.nation_set_tag_amount(from.nation, low, amount);
    } else {
        w.nation_set_tag_rest(from.nation, k, w.nation_get_tag_rest(from.nation, k) + amount);
    }
}

double balance(game_state const& state, account a) noexcept {
    auto const& w = state.world;
    switch(a.kind) {
    case account_kind::pop:
        return w.pop_get_balance(a.pop);
    case account_kind::firms:
        return w.nation_get_bal_firms(a.nation);
    case account_kind::government:
        return w.nation_get_bal_gov(a.nation);
    case account_kind::finance:
        return w.nation_get_bal_fin(a.nation);
    case account_kind::rest_of_world:
        return w.nation_get_bal_row(a.nation);
    }
    return 0.0;
}

double world_money(game_state const& state) noexcept {
    auto const& w = state.world;
    double total = 0.0;
    for(auto n : w.in_nation) {
        total += n.get_bal_firms() + n.get_bal_gov() + n.get_bal_fin() + n.get_bal_row();
    }
    for(auto p : w.in_pop) {
        total += p.get_balance();
    }
    return total;
}

} // namespace geosim::sim
