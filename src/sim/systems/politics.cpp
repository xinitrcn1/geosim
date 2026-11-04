// Politics (docs/05-politics-institutions.md): every 28 days per nation shard.
//
// - Vote intention per pop: salience-weighted spatial distance to each party
//   (Downs 1957) plus retrospective economic and health voting for the
//   incumbent (Lewis-Beck & Stegmaier 2000), logit choice; turnout by kind.
// - Elections on the calendar: proportional seats (pr) or regional pluralities
//   (fptp); government = minimal connected winning coalition in left-right
//   order (Axelrod 1970). The winner's position reshapes the executive's ideology.
// - Legitimacy: procedural (electoral + liberal) and performance (grievance)
//   components; forced player overrides erode legitimacy and the liberal index
//   and raise the capture index.
// - Selectorate (Bueno de Mesquita et al. 2003): winning-coalition size W.
// - Coups: rare, drawn from the counter-based RNG under low legitimacy, high
//   unrest and a small winning coalition.

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};
constexpr int max_parties = 8;

std::vector<dcon::party_id> parties_of(game_state const& state, dcon::nation_id n) {
    std::vector<dcon::party_id> out;
    for(auto m : state.world.nation_get_party_membership(n)) {
        out.push_back(m.get_party().id);
    }
    std::sort(out.begin(), out.end(), [&](auto a, auto b) {
        return state.world.party_get_local_index(a) < state.world.party_get_local_index(b);
    });
    return out;
}

void log_event(game_state& state, dcon::nation_id n, uint8_t code, float before, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, code);
    w.decision_set_before(d, before);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, NAN);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

// Recomputes every pop's party support and the national vote shares.
void vote_intention(game_state& state, dcon::nation_id n, std::vector<dcon::party_id> const& parties) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto incumbent = w.nation_get_incumbent(n);
    const float deaths_pm = reported_deaths_per_million_daily(state, n);
    std::array<double, max_parties> votes{};
    double total = 0.0;
    constexpr std::array<float, 4> turnout = {0.5f, 0.6f, 0.75f, 0.7f};
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            auto p = loc.get_pop();
            const float econ_w = 0.3f + p.get_salience(static_cast<uint8_t>(issue::economy));
            const float values_w = 0.3f + p.get_salience(static_cast<uint8_t>(issue::identity));
            // Retrospective performance felt by this pop (income, jobs, health).
            const float performance = -2.0f * p.get_grievance() - static_cast<float>(P.vote_health_weight) * deaths_pm;
            std::array<float, max_parties> u{};
            float umax = -1e30f;
            for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
                float d2 = 0.0f;
                for(uint8_t a = 0; a < static_cast<uint8_t>(value_axis::count); ++a) {
                    const float d = p.get_values(a) - w.party_get_position(parties[j], a);
                    d2 += (a == static_cast<uint8_t>(value_axis::economic_right) ? econ_w : values_w) * d * d;
                }
                u[j] = -static_cast<float>(P.vote_distance_weight) * d2
                       + (parties[j] == incumbent ? static_cast<float>(P.vote_retrospective) * performance : 0.0f);
                umax = std::max(umax, u[j]);
            }
            float z = 0.0f;
            for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
                u[j] = std::exp((u[j] - umax) / static_cast<float>(P.vote_temperature));
                z += u[j];
            }
            const double weight = static_cast<double>(p.get_size()) * turnout[p.get_kind() & 3];
            for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
                const float share = z > 0.0f ? u[j] / z : 0.0f;
                p.set_party_support(static_cast<uint8_t>(j), share);
                votes[j] += weight * share;
            }
            total += weight;
        }
    }
    for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
        w.party_set_vote_share(parties[j], total > 0.0 ? static_cast<float>(votes[j] / total) : 0.0f);
    }
}

void hold_election(game_state& state, dcon::nation_id n, std::vector<dcon::party_id> const& parties) {
    auto& w = state.world;
    const std::size_t np = std::min<std::size_t>(parties.size(), max_parties);
    std::array<double, max_parties> seats{};
    if(w.nation_get_electoral_system(n) == static_cast<uint8_t>(electoral_system::fptp)) {
        // Regional pluralities; each region's seats proportional to its population.
        double total = 0.0;
        for(auto own : w.nation_get_region_ownership(n)) {
            auto r = own.get_region();
            std::array<double, max_parties> regional{};
            for(auto loc : r.get_pop_location()) {
                for(std::size_t j = 0; j < np; ++j) regional[j] += loc.get_pop().get_size() * loc.get_pop().get_party_support(static_cast<uint8_t>(j));
            }
            const auto winner = static_cast<std::size_t>(std::max_element(regional.begin(), regional.begin() + static_cast<std::ptrdiff_t>(np)) - regional.begin());
            seats[winner] += r.get_population();
            total += r.get_population();
        }
        for(std::size_t j = 0; j < np; ++j) seats[j] = total > 0.0 ? seats[j] / total : 0.0;
    } else {
        for(std::size_t j = 0; j < np; ++j) seats[j] = w.party_get_vote_share(parties[j]);
    }
    for(std::size_t j = 0; j < np; ++j) w.party_set_seat_share(parties[j], static_cast<float>(seats[j]));

    // Minimal connected winning coalition (parties are ordered left to right).
    const auto largest = static_cast<std::size_t>(std::max_element(seats.begin(), seats.begin() + static_cast<std::ptrdiff_t>(np)) - seats.begin());
    std::size_t best_lo = largest, best_hi = largest;
    double best_seats = seats[largest];
    for(std::size_t width = 1; width <= np && best_seats <= 0.5; ++width) {
        for(std::size_t lo = (largest + 1 >= width ? largest + 1 - width : 0); lo <= largest && lo + width <= np; ++lo) {
            double s = 0.0;
            for(std::size_t j = lo; j < lo + width; ++j) s += seats[j];
            if(s > 0.5 && (best_seats <= 0.5 || s > best_seats)) {
                best_lo = lo;
                best_hi = lo + width - 1;
                best_seats = s;
            }
        }
    }
    const auto before = w.nation_get_incumbent(n);
    for(std::size_t j = 0; j < np; ++j) w.party_set_in_government(parties[j], j >= best_lo && j <= best_hi ? 1 : 0);
    w.nation_set_incumbent(n, parties[largest]);
    w.nation_set_gov_seat_share(n, static_cast<float>(best_seats));

    // The new government's position reshapes the executive.
    if(auto exec = w.nation_get_agent_from_executive_seat(n)) {
        const float econ = w.party_get_position(parties[largest], static_cast<uint8_t>(value_axis::economic_right));
        const float secular = w.party_get_position(parties[largest], static_cast<uint8_t>(value_axis::secular));
        w.agent_set_fiscal_hawk(exec, std::clamp(0.3f + 0.5f * econ, 0.0f, 1.0f));
        w.agent_set_order_ideal(exec, std::clamp(0.2f - 0.12f * w.nation_get_voice(n) - 0.2f * secular, 0.0f, 0.8f));
    }
    log_event(state, n, journal_election, before ? static_cast<float>(w.party_get_local_index(before)) : -1.0f,
              static_cast<float>(w.party_get_local_index(parties[largest])));
}

} // namespace

void init_politics(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) {
        const auto parties = parties_of(state, n.id);
        if(parties.empty()) continue;
        vote_intention(state, n.id, parties);
        const auto inc = n.get_incumbent();
        n.set_approval0(inc ? std::max(0.05f, w.party_get_vote_share(inc)) : 0.5f);
        n.set_approval(0.5f);
    }
}

void update_politics(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    // Elections are checked daily so they happen on their date.
    for(auto n : w.in_nation) {
        if(n.get_competitive() && n.get_next_election() > 0 && state.current_tick >= n.get_next_election()) {
            const auto parties = parties_of(state, n.id);
            if(!parties.empty()) {
                vote_intention(state, n.id, parties);
                hold_election(state, n.id, parties);
            }
            n.set_next_election(n.get_next_election() + std::max(365, n.get_term_days()));
        }
    }
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const auto parties = parties_of(state, n.id);
        if(parties.empty()) continue;

        vote_intention(state, n.id, parties);
        const auto inc = n.get_incumbent();
        n.set_approval(inc ? std::clamp(0.5f * w.party_get_vote_share(inc) / n.get_approval0(), 0.0f, 1.0f) : 0.5f);

        // Forced overrides: count this nation's agents' levers under force.
        int forced = 0, overridden = 0, levers = 0;
        auto count_agent = [&](dcon::agent_id a) {
            if(!a) return;
            for(uint8_t l = 0; l < state.derived.lever_count; ++l) {
                const auto mode = static_cast<override_mode>(w.agent_get_override_mode(a, l));
                forced += mode == override_mode::force ? 1 : 0;
                overridden += mode != override_mode::none ? 1 : 0;
                ++levers;
            }
        };
        count_agent(n.get_executive_seat().get_agent());
        count_agent(w.nation_get_bank_from_monetary_authority(n));
        n.set_capture_index(levers > 0 ? static_cast<float>(overridden) / static_cast<float>(levers) : 0.0f);
        n.set_liberal(std::max(0.0f, n.get_liberal() - static_cast<float>(P.force_liberal_cost) * static_cast<float>(forced) * years));

        // Legitimacy: procedural + performance + identity (constant until identity data).
        double g = 0.0, size = 0.0, unrest = 0.0;
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            g += r.get_grievance() * r.get_population();
            unrest += r.get_unrest() * r.get_population();
            size += r.get_population();
        }
        const float grievance = size > 0 ? static_cast<float>(g / size) : 0.0f;
        const float procedural = 0.5f * n.get_electoral() + 0.5f * n.get_liberal();
        const float performance = std::clamp(1.0f - 2.0f * grievance, 0.0f, 1.0f);
        const float target = 0.4f * procedural + 0.4f * performance + 0.2f * 0.7f;
        float legit = n.get_legitimacy();
        legit += (target - legit) * std::min(1.0f, years / static_cast<float>(P.legitimacy_years));
        legit -= static_cast<float>(P.force_legitimacy_cost) * static_cast<float>(forced) * years;
        n.set_legitimacy(std::clamp(legit, 0.0f, 1.0f));
        n.set_winning_coalition(0.05f + 0.6f * n.get_electoral());

        // Coups: small winning coalition, low legitimacy, widespread unrest.
        const float unrest_mean = size > 0 ? static_cast<float>(unrest / size) : 0.0f;
        if(n.get_winning_coalition() < 0.3f && n.get_legitimacy() < 0.3f && unrest_mean > 2.0f) {
            const float p = static_cast<float>(P.coup_rate) * (unrest_mean - 2.0f);
            if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::elections), static_cast<uint32_t>(state.current_tick),
                               static_cast<uint32_t>(n.id.index())) < p) {
                n.set_electoral(n.get_electoral() * 0.5f);
                n.set_liberal(n.get_liberal() * 0.7f);
                n.set_legitimacy(0.4f);
                if(auto exec = n.get_executive_seat().get_agent()) {
                    exec.set_order_ideal(std::min(0.8f, exec.get_order_ideal() + 0.2f));
                    exec.set_rho_base(std::min(0.8f, exec.get_rho_base() + 0.1f));
                }
                log_event(state, n.id, journal_coup, 0.0f, 1.0f);
            }
        }
    }
}

} // namespace geosim::sim
