// The internet as a political infrastructure (docs/06-geopolitics-ai.md#information-and-the-internet).
//
// Every nation starts with the online controls its society already lives with
// (Freedom on the Net scores -> domestic information control and cross-border
// filtering; the same rule for every nation - a "great firewall" is just a high
// filtering level that a capable, order-minded state can sustain). Then:
//
// Daily
//  - Coupling between two societies = connectivity x openness at both ends x ties
//    (trade, plus a floor for language, diaspora and media links).
//  - Protest contagion: coupled foreign protest adds to the domestic drive
//    (diffusion of contention; Bamert, Gilardi & Wasserfallen 2015; Hussain &
//    Howard 2013) - filtering cuts it.
//  - Cyber damage and influence operations decay.
// Every 28 days (nation shard)
//  - Value diffusion: pops move toward the publics they are coupled to, within
//    bounded confidence (Gleditsch & Ward 2006 on the international diffusion of
//    regime preferences).
//  - Cyber operations: capable states attack rivals below the threshold of war
//    (Valeriano & Maness 2015; Buchanan 2016). Hazard rises with the attacker's
//    capability (skills, state capacity, connectivity), the pair's threat and
//    distrust, and the target's exposure (connectivity, weak capacity; filtering
//    defends a little). Effects are modest (Rid 2012; Gartzke 2013; Kostyuk &
//    Zhukov 2019): days of lost capacity in energy and business services, lower
//    trust, and an influence operation that adds to the target's protest drive.
// Costs of filtering (economy.cpp, longrun.cpp): digitally delivered services
// trade falls with filtering above the start (Freund & Weinhold 2004), and so
// does trend growth (broadband and growth; Czernich, Falck, Kretschmer &
// Woessmann 2011).

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

float national_protest(game_state const& state, dcon::nation_id n) {
    float p = 0.0f, pop = 0.0f;
    for(auto own : state.world.nation_get_region_ownership(n)) {
        p += own.get_region().get_protest() * own.get_region().get_population();
        pop += own.get_region().get_population();
    }
    return pop > 0.0f ? p / pop : 0.0f;
}

float tie(game_state const& state, dcon::nation_id n, dcon::nation_id m) {
    auto const& w = state.world;
    return w.nation_get_trade_share(n, static_cast<uint8_t>(m.index())) + w.nation_get_trade_share(m, static_cast<uint8_t>(n.index()))
           + static_cast<float>(state.params.net_tie_floor);
}

} // namespace

void update_internet(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const auto tick = static_cast<tick_t>(state.current_tick);
    const float years = static_cast<float>(monthly_cadence.period_days) / 365.0f;
    const float decay = std::exp(-1.0f / static_cast<float>(P.cyber_days));

    // Daily: coupling and contagion.
    std::vector<float> protest(w.nation_size(), 0.0f);
    for(auto n : w.in_nation) protest[n.id.index()] = national_protest(state, n.id);
    for(auto n : w.in_nation) {
        const float open_n = n.get_connectivity() * (1.0f - n.get_net_filtering());
        double contagion = 0.0, ties = 0.0;
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const auto km = static_cast<uint8_t>(m.id.index());
            const float t = tie(state, n.id, m.id);
            const float c = open_n * m.get_connectivity() * (1.0f - m.get_net_filtering()) * t;
            n.set_coupling(km, c);
            contagion += static_cast<double>(c) * protest[m.id.index()] / static_cast<float>(P.cascade_max);
            ties += t;
        }
        n.set_net_contagion(ties > 0.0 ? static_cast<float>(P.net_contagion * contagion / ties) : 0.0f);
        n.set_cyber_damage(n.get_cyber_damage() * decay);
        n.set_influence_ops(n.get_influence_ops() * decay);
        n.set_cyber_capability(n.get_state_capacity() * std::min(1.0f, n.get_tertiary_share() * 2.0f) * (0.5f + 0.5f * n.get_connectivity()));
    }

    // Monthly: value diffusion and cyber operations.
    constexpr uint8_t A = static_cast<uint8_t>(value_axis::count);
    // Every public's size-weighted value means, once a day when any nation is due (not once
    // per due nation and partner: that pass over all pops dominated the day).
    std::vector<std::array<double, A>> means;
    std::vector<uint8_t> has_mean;
    auto ensure_means = [&] {
        if(!means.empty()) return;
        means.assign(w.nation_size(), {});
        has_mean.assign(w.nation_size(), 0);
        std::vector<double> sizes(w.nation_size(), 0.0);
        for(auto r : w.in_region) { // by the region's owner (conquest moves publics)
            const auto nat = r.get_owner();
            if(!nat) continue;
            const auto i = static_cast<std::size_t>(nat.id.index());
            for(auto loc : r.get_pop_location()) {
                auto p = loc.get_pop();
                for(uint8_t a = 0; a < A; ++a) means[i][a] += p.get_size() * p.get_values(a);
                sizes[i] += p.get_size();
            }
        }
        for(std::size_t i = 0; i < means.size(); ++i) {
            if(sizes[i] <= 0.0) continue;
            for(auto& x : means[i]) x /= sizes[i];
            has_mean[i] = 1;
        }
    };
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        ensure_means();
        // Coupled foreign publics (size-weighted value means).
        std::vector<std::array<double, A>> pull;
        std::vector<double> weight;
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const float c = n.get_coupling(static_cast<uint8_t>(m.id.index()));
            if(c <= 0.0f || !has_mean[static_cast<std::size_t>(m.id.index())]) continue;
            pull.push_back(means[static_cast<std::size_t>(m.id.index())]);
            weight.push_back(c);
        }
        double wsum = 0.0;
        for(double x : weight) wsum += x;
        if(wsum > 0.0) {
            const float eps = static_cast<float>(P.opinion_eps_base + P.opinion_eps_media * n.get_media_freedom());
            const float rate = static_cast<float>(P.net_diffusion) * years * std::min(1.0f, static_cast<float>(wsum) * 10.0f);
            for(auto own : n.get_region_ownership()) {
                for(auto loc : own.get_region().get_pop_location()) {
                    auto p = loc.get_pop();
                    for(uint8_t a = 0; a < A; ++a) {
                        double target = 0.0, tw = 0.0;
                        for(std::size_t i = 0; i < pull.size(); ++i) {
                            if(std::abs(pull[i][a] - p.get_values(a)) > eps) continue; // bounded confidence
                            target += weight[i] * pull[i][a];
                            tw += weight[i];
                        }
                        if(tw > 0.0) {
                            const float x = p.get_values(a);
                            p.set_values(a, std::clamp(x + rate * (static_cast<float>(target / tw) - x), -1.0f, 1.0f));
                        }
                    }
                }
            }
        }

        // Cyber operations launched by n.
        const float capability = n.get_cyber_capability();
        if(capability <= 0.0f) continue;
        for(auto m : w.in_nation) {
            if(m.id == n.id) continue;
            const auto km = static_cast<uint8_t>(m.id.index());
            const float threat = n.get_threat_to(km);
            const float distrust = 1.0f - n.get_trust(km);
            const float exposure = m.get_connectivity() * (1.0f - 0.5f * m.get_state_capacity()) * (1.0f - 0.5f * m.get_net_filtering());
            const float hazard = static_cast<float>(P.cyber_rate) * years * capability * std::max(0.0f, threat - 0.05f) * distrust * distrust
                                 * exposure * (1.0f + 2.0f * n.get_at_war(km));
            const float u = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::cyber), static_cast<uint32_t>(state.current_tick),
                                            static_cast<uint32_t>(n.id.index()) * 1024u + static_cast<uint32_t>(m.id.index()));
            if(u < hazard) {
                const float size = core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::cyber), static_cast<uint32_t>(state.current_tick),
                                                   static_cast<uint32_t>(n.id.index()) * 1024u + static_cast<uint32_t>(m.id.index()), 1);
                const float damage = static_cast<float>(P.cyber_damage_mean) * -std::log(std::max(1e-6f, size)) * capability;
                m.set_cyber_damage(std::min(0.5f, m.get_cyber_damage() + damage));
                m.set_influence_ops(m.get_influence_ops() + static_cast<float>(P.cyber_influence) * capability);
                const auto kn = static_cast<uint8_t>(n.id.index());
                m.set_trust(kn, std::max(0.0f, m.get_trust(kn) - 0.05f));
                const auto d = w.create_decision();
                w.decision_set_agent(d, n.get_executive_seat().get_agent());
                w.decision_set_target(d, n.id);
                w.decision_set_tick(d, state.current_tick);
                w.decision_set_lever(d, journal_cyber);
                w.decision_set_before(d, 0.0f);
                w.decision_set_after(d, damage);
                w.decision_set_pragmatic(d, static_cast<float>(m.id.index()));
                w.decision_set_ideological(d, NAN);
                w.decision_set_prior(d, NAN);
            }
        }
    }
}

} // namespace geosim::sim
