// Treaties (docs/06-geopolitics-ai.md#diplomacy), every 28 days per nation shard.
//
// - Alliances form against a shared threat between states that trust each other
//   and are not too far apart politically (balance of threat; Walt 1987), and
//   dissolve when trust collapses.
// - Free-trade agreements form between interdependent, trusting, politically
//   close partners; their trade gain phases in over a decade (Baier & Bergstrand
//   2007: FTAs roughly double members' trade in ten years) and enters bilateral
//   export access (geopolitics.cpp trade_access).
// - IMF programmes: when a sovereign's spread blows out, the Fund lends (from the
//   rest of the world to the financial sector that is shedding the debt),
//   spreads fall (catalytic effect; Corsetti, Guimaraes & Roubini 2006) and the
//   executive must consolidate (conditionality: a fiscal hawk; Dreher 2006).
//   Programmes last three years.

#include "core/rng.hpp"
#include "core/scheduler.hpp"
#include "sim/ledger.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>

namespace geosim::sim {

namespace {

constexpr cadence monthly_cadence{28, 28};

void log_treaty(game_state& state, dcon::nation_id n, dcon::nation_id m, float kind, float after) {
    auto& w = state.world;
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, journal_treaty);
    w.decision_set_before(d, kind);
    w.decision_set_after(d, after);
    w.decision_set_pragmatic(d, m ? static_cast<float>(m.index()) : -1.0f);
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

} // namespace

void update_treaties(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        const auto kn = static_cast<uint8_t>(n.id.index());
        for(auto m : w.in_nation) {
            if(m.id.index() <= n.id.index()) continue; // each pair once, on the lower index's shard
            const auto km = static_cast<uint8_t>(m.id.index());
            const float trust = 0.5f * (n.get_trust(km) + m.get_trust(kn));
            const float distance = std::abs(n.get_voice() - m.get_voice()) / 5.0f;
            // Shared threat: a third state threatening both.
            float shared = 0.0f;
            for(auto t : w.in_nation) {
                if(t.id == n.id || t.id == m.id) continue;
                const auto kt = static_cast<uint8_t>(t.id.index());
                shared = std::max(shared, std::min(n.get_threat_to(kt), m.get_threat_to(kt)));
            }
            const auto u = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::conflict), static_cast<uint32_t>(state.current_tick),
                                           0x60000000u + static_cast<uint32_t>(n.id.index()) * 1024u + static_cast<uint32_t>(m.id.index()));
            if(n.get_alliance(km) <= 0.0f) {
                // The deter-threat plan's last step seeks allies (strategy.cpp).
                const float seek = std::max(plan_bias(state, n.id, goal::deter_threat, 2), plan_bias(state, m.id, goal::deter_threat, 2));
                if(shared > static_cast<float>(P.alliance_threat) * (1.0f - seek) && trust > 0.5f - seek && distance < 0.3f && n.get_at_war(km) <= 0.0f && u < 0.5f * years) {
                    n.set_alliance(km, 1.0f);
                    m.set_alliance(kn, 1.0f);
                    log_treaty(state, n.id, m.id, 0.0f, 1.0f);
                }
            } else if(trust < 0.2f || n.get_at_war(km) > 0.0f) {
                n.set_alliance(km, 0.0f);
                m.set_alliance(kn, 0.0f);
                log_treaty(state, n.id, m.id, 0.0f, 0.0f);
            }
            // FTAs: start, then phase in.
            float fta = n.get_fta(km);
            if(fta <= 0.0f) {
                const float dependence = std::min(1.0f, 5.0f * (n.get_trade_share(km) + m.get_trade_share(kn)));
                const float sanction = std::max(n.get_sanction(km), m.get_sanction(kn));
                const float hazard = static_cast<float>(P.fta_rate) * years * dependence * trust * (1.0f - distance) * (1.0f - sanction);
                if(u < hazard && sanction < 0.1f) {
                    // Two-level game (Putnam 1988): each side's legislature must ratify;
                    // protectionist lobbies narrow the win-set (Milner 1997).
                    auto ratifies = [&](dcon::nation_id x) {
                        if(!w.nation_get_competitive(x)) return true;
                        const float support = w.nation_get_gov_seat_share(x) + 0.3f - static_cast<float>(P.fta_ratification) * 10.0f * w.nation_get_lobby_pressure(x);
                        return support > 0.5f;
                    };
                    if(ratifies(n.id) && ratifies(m.id)) {
                        fta = 0.01f;
                        // Level I (Putnam 1988): the depth is the Nash bargaining outcome over
                        // liberalisation, bounded by the narrower win-set; the side with the
                        // narrower win-set claims a larger share of the gains (Schelling 1960).
                        auto winset = [&](dcon::nation_id x) {
                            return std::clamp(1.0f - static_cast<float>(P.fta_ratification) * 10.0f * w.nation_get_lobby_pressure(x)
                                                  - 0.5f * std::max(0.0f, 0.5f - w.nation_get_gov_seat_share(x)), 0.2f, 1.0f);
                        };
                        const float wn = winset(n.id), wm = winset(m.id);
                        const float depth = std::min(wn, wm);
                        const float split = static_cast<float>(P.nash_split) + 0.25f * (wm - wn); // n's share of the gains
                        n.set_fta_depth(km, depth * std::clamp(2.0f * split, 0.5f, 1.5f));
                        m.set_fta_depth(kn, depth * std::clamp(2.0f * (1.0f - split), 0.5f, 1.5f));
                        log_treaty(state, n.id, m.id, 1.0f, 1.0f);
                    } else {
                        log_treaty(state, n.id, m.id, 1.0f, -1.0f); // signed, not ratified
                    }
                }
            } else {
                fta = std::min(1.0f, fta + years / static_cast<float>(P.fta_years));
                if(std::max(n.get_sanction(km), m.get_sanction(kn)) > 0.5f) fta = 0.0f; // trade war ends it
            }
            n.set_fta(km, fta);
            m.set_fta(kn, fta);
        }
        // IMF programme.
        const float stress = n.get_spread() - n.get_spread0();
        if(n.get_imf_program() <= 0.0f && stress > static_cast<float>(P.imf_spread) && n.get_reserve_currency() < 1.0f) {
            const double gdp = n.get_gdp0() * n.get_core_price() * n.get_gf();
            cause_scope c(state, journal_cause(n.id, journal_treaty));
            transfer(state, account::rest_of_world(n.id), account::finance(n.id), P.imf_loan * gdp, flow::imf_lending);
            n.set_imf_program(3.0f);
            if(auto exec = n.get_executive_seat().get_agent()) exec.set_fiscal_hawk(std::max(exec.get_fiscal_hawk(), 0.8f));
            log_treaty(state, n.id, {}, 2.0f, 1.0f);
        } else if(n.get_imf_program() > 0.0f) {
            n.set_imf_program(std::max(0.0f, n.get_imf_program() - years));
        }
    }
}

} // namespace geosim::sim
