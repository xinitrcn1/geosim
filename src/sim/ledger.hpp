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

// Sets game_state::cause for the transfers made in its scope ("Why?" cause tags).
struct cause_scope {
    cause_scope(game_state& s, uint32_t code) noexcept : state(s), previous(s.cause) { s.cause = code; }
    ~cause_scope() { state.cause = previous; }
    cause_scope(cause_scope const&) = delete;
    cause_scope& operator=(cause_scope const&) = delete;
    game_state& state;
    uint32_t previous;
};
// Tag of an agent lever decided for nation n.
[[nodiscard]] inline uint32_t lever_cause(game_state const& state, dcon::nation_id n, lever l) noexcept {
    return cause_code(cause_kind::lever, static_cast<uint32_t>(n.index()), static_cast<uint32_t>(state.derived.lever_base + static_cast<uint8_t>(l)));
}
[[nodiscard]] inline uint32_t journal_cause(dcon::nation_id n, uint8_t code) noexcept {
    return cause_code(cause_kind::journal, static_cast<uint32_t>(n.index()), code);
}

// Sum of every account balance in the world (the conserved quantity).
[[nodiscard]] double world_money(game_state const& state) noexcept;

} // namespace geosim::sim
