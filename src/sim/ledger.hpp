#pragma once
// Stock-flow-consistent ledger (docs/02-simulation-core.md#accounting-core).
//
// Every payment moves money between two accounts, so the sum of all balances
// in the world never changes. A negative balance is a net debt (firms
// borrowing, government deficits). Each payment also adds to the nation's
// year-to-date `flows[kind]`, which the "Why?" trace reads.

#include "sim/game_state.hpp"
#include "sim/model.hpp"

namespace geosim::sim {

enum class account_kind : uint8_t { pop, firms, government, finance, rest_of_world };

struct account {
    account_kind kind;
    dcon::nation_id nation; // for every kind (owner nation of a pop)
    dcon::pop_id pop;       // only for account_kind::pop

    static account of_pop(dcon::nation_id n, dcon::pop_id p) noexcept { return {account_kind::pop, n, p}; }
    static account firms(dcon::nation_id n) noexcept { return {account_kind::firms, n, {}}; }
    static account government(dcon::nation_id n) noexcept { return {account_kind::government, n, {}}; }
    static account finance(dcon::nation_id n) noexcept { return {account_kind::finance, n, {}}; }
    static account rest_of_world(dcon::nation_id n) noexcept { return {account_kind::rest_of_world, n, {}}; }
};

// Moves `amount` (USD, may be negative = reverse direction) from -> to and
// records it under `kind` on the payer's nation.
void transfer(game_state& state, account from, account to, double amount, flow kind) noexcept;

[[nodiscard]] double balance(game_state const& state, account a) noexcept;

// Sum of every account balance in the world (the conserved quantity).
[[nodiscard]] double world_money(game_state const& state) noexcept;

} // namespace geosim::sim
