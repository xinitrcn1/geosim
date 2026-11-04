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
// - Party systems: real parties where data exists (expert-survey positions).
//   Parties adapt monthly by rule (Laver 2005; Laver & Sergenti 2011): mainstream
//   aggregators drift to their supporters' centroid, niche and ruling parties
//   stick (Adams et al. 2006), challengers hunt for votes. Challenger parties form
//   when many voters are far from every party (de Vries & Hobolt 2020; Meguid
//   2005); parties that keep under 1% dissolve.
// - Overton window per nation and value axis: centred on public opinion, wider
//   with polarisation and fragmented media, and pulled toward the governing
//   party's position - success of a once-extreme position makes it sayable
//   (Bursztyn, Egorov & Fiorin 2020; Kuran 1995; Noelle-Neumann 1974). Voters
//   stigmatise parties outside it; policy outside it costs legitimacy, and
//   liberty-restricting policy outside it feeds protest (update_unrest). Public
//   opinion is thermostatic: it moves against the direction of economic policy
//   (Wlezien 1995; Soroka & Wlezien 2010).

#include "core/keys.hpp"
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
constexpr int max_parties = 16;
constexpr uint8_t axes = static_cast<uint8_t>(value_axis::count);

// Distance of a position outside the nation's Overton window (0 inside), per axis summed.
float outside_window(game_state const& state, dcon::nation_id n, std::array<float, axes> const& pos) {
    auto const& w = state.world;
    float out = 0.0f;
    for(uint8_t a = 0; a < axes; ++a) {
        out += std::max(0.0f, std::abs(pos[a] - w.nation_get_window_center(n, a)) - w.nation_get_window_width(n, a));
    }
    return out;
}

std::array<float, axes> party_pos(game_state const& state, dcon::party_id p) {
    std::array<float, axes> x{};
    for(uint8_t a = 0; a < axes; ++a) x[a] = state.world.party_get_position(p, a);
    return x;
}

// Public opinion: size-weighted mean and standard deviation of pop values.
void public_opinion(game_state const& state, dcon::nation_id n, std::array<float, axes>& mean, std::array<float, axes>& sd) {
    auto const& w = state.world;
    std::array<double, axes> m{}, m2{};
    double total = 0.0;
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            auto p = loc.get_pop();
            const double s = p.get_size();
            for(uint8_t a = 0; a < axes; ++a) {
                m[a] += s * p.get_values(a);
                m2[a] += s * p.get_values(a) * p.get_values(a);
            }
            total += s;
        }
    }
    for(uint8_t a = 0; a < axes; ++a) {
        mean[a] = total > 0.0 ? static_cast<float>(m[a] / total) : 0.0f;
        sd[a] = total > 0.0 ? static_cast<float>(std::sqrt(std::max(0.0, m2[a] / total - (m[a] / total) * (m[a] / total)))) : 0.0f;
    }
}

void update_window(game_state& state, dcon::nation_id n, float years, bool reset) {
    auto& w = state.world;
    auto const& P = state.params;
    std::array<float, axes> mean{}, sd{};
    public_opinion(state, n, mean, sd);
    const auto inc = w.nation_get_incumbent(n);
    for(uint8_t a = 0; a < axes; ++a) {
        float c = reset ? mean[a] : w.nation_get_window_center(n, a);
        if(!reset) {
            c += (mean[a] - c) * std::min(1.0f, years / static_cast<float>(P.window_years));
            if(inc) c += static_cast<float>(P.window_normalisation) * (w.party_get_position(inc, a) - c) * years;
        }
        w.nation_set_window_center(n, a, c);
        w.nation_set_window_width(n, a, static_cast<float>(P.window_base + P.window_polarisation * sd[a] + P.window_media * w.nation_get_connectivity(n)));
    }
}

// The government's policy position: its party's position moved by what it does
// (fiscal expansion and directed credit lean left; restrictions on movement and on
// media lean toward the survival / order end of the self-expression axis).
void update_policy_position(game_state& state, dcon::nation_id n, bool baseline = false) {
    auto& w = state.world;
    auto const& D = state.derived;
    const auto inc = w.nation_get_incumbent(n);
    std::array<float, axes> pos{};
    if(inc) pos = party_pos(state, inc);
    pos[static_cast<uint8_t>(value_axis::economic_right)] -= 0.02f * w.nation_get_fiscal_announced_cum(n) + 0.03f * w.nation_get_directed_credit(n);
    float stringency = 0.0f;
    int count = 0;
    for(uint8_t k = 0; k < D.npi_count; ++k) {
        const auto role = static_cast<npi_role>(w.npi_indicator_get_role(dcon::npi_indicator_id{k}));
        if(role == npi_role::testing || role == npi_role::tracing || role == npi_role::income_support) continue;
        stringency += npi_fraction(state, n, k);
        ++count;
    }
    stringency = count > 0 ? stringency / static_cast<float>(count) : 0.0f;
    pos[static_cast<uint8_t>(value_axis::self_expression)] -= 0.6f * stringency + 1.0f * (w.nation_get_info_control(n) - w.nation_get_info_control0(n))
                                                             + 0.5f * (w.nation_get_net_filtering(n) - w.nation_get_net_filtering0(n));
    for(uint8_t a = 0; a < axes; ++a) w.nation_set_policy_position(n, a, pos[a]);
    // Only liberty-restricting policy beyond the window feeds protest.
    // Under a salient threat the public accepts restrictions it otherwise would not
    // (rally round the flag; Mueller 1970; Bol et al. 2021); the acceptance fades
    // with prolonged restrictions (fatigue).
    auto const& P = state.params;
    const float fatigue = std::min(1.0f, w.nation_get_npi_exposure(n) / static_cast<float>(P.fatigue_days));
    const float rally = static_cast<float>(P.overton_rally) * w.nation_get_threat(n) * (1.0f - fatigue);
    const float c = w.nation_get_window_center(n, static_cast<uint8_t>(value_axis::self_expression)) - rally;
    const float wd = w.nation_get_window_width(n, static_cast<uint8_t>(value_axis::self_expression));
    // Status quo bias: the public accepts the policies it already lives with; only moves
    // beyond the starting distance count (Samuelson & Zeckhauser 1988).
    const float raw = std::max(0.0f, c - wd - pos[static_cast<uint8_t>(value_axis::self_expression)]);
    if(baseline) {
        w.nation_set_policy_outside0(n, raw);
        w.nation_set_window_outside0(n, outside_window(state, n, pos));
    }
    w.nation_set_policy_outside(n, std::max(0.0f, raw - w.nation_get_policy_outside0(n)));
}

void reindex_parties(game_state& state, dcon::nation_id n);

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
                const float out = outside_window(state, n, party_pos(state, parties[j]));
                u[j] = -static_cast<float>(P.vote_distance_weight) * d2 - static_cast<float>(P.window_stigma) * out * out
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

// Party adaptation (Laver 2005): one step per month.
void adapt_parties(game_state& state, dcon::nation_id n, std::vector<dcon::party_id> const& parties, float years) {
    auto& w = state.world;
    auto const& P = state.params;
    const float step = static_cast<float>(P.party_speed) * years;
    dcon::party_id largest;
    float largest_vote = -1.0f;
    for(auto p : parties) {
        if(w.party_get_vote_share(p) > largest_vote) {
            largest_vote = w.party_get_vote_share(p);
            largest = p;
        }
    }
    for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
        const auto p = parties[j];
        const auto strategy = static_cast<party_strategy>(w.party_get_strategy(p));
        std::array<float, axes> target = party_pos(state, p);
        if(strategy == party_strategy::aggregator) {
            std::array<double, axes> sum{};
            double weight = 0.0;
            for(auto own : w.nation_get_region_ownership(n)) {
                for(auto loc : own.get_region().get_pop_location()) {
                    auto pp = loc.get_pop();
                    const double s = pp.get_size() * pp.get_party_support(static_cast<uint8_t>(j));
                    for(uint8_t a = 0; a < axes; ++a) sum[a] += s * pp.get_values(a);
                    weight += s;
                }
            }
            if(weight > 0.0) {
                for(uint8_t a = 0; a < axes; ++a) target[a] = static_cast<float>(sum[a] / weight);
            }
        } else if(strategy == party_strategy::hunter) {
            // Keep the heading while the vote grows; otherwise reverse it.
            const bool growing = w.party_get_vote_share(p) >= w.party_get_last_vote(p);
            for(uint8_t a = 0; a < axes; ++a) {
                float h = w.party_get_heading(p, a);
                if(!growing) h = -h;
                if(h == 0.0f) h = (a == 0 ? 1.0f : 0.0f);
                w.party_set_heading(p, a, h);
                target[a] += h;
            }
        } else if(strategy == party_strategy::predator && largest && largest != p) {
            target = party_pos(state, largest);
        }
        std::array<float, axes> pos = party_pos(state, p);
        float dist = 0.0f;
        for(uint8_t a = 0; a < axes; ++a) dist += (target[a] - pos[a]) * (target[a] - pos[a]);
        dist = std::sqrt(dist);
        if(dist > 1e-6f) {
            const float move = std::min(step, dist) / dist;
            for(uint8_t a = 0; a < axes; ++a) {
                float x = pos[a] + (target[a] - pos[a]) * move;
                // Mainstream parties stay (just) inside the window: crossing it costs votes.
                if(strategy == party_strategy::aggregator) {
                    const float c = w.nation_get_window_center(n, a), wd = 1.1f * w.nation_get_window_width(n, a);
                    x = std::clamp(x, c - wd, c + wd);
                }
                w.party_set_position(p, a, std::clamp(x, -1.0f, 1.0f));
            }
        }
        w.party_set_last_vote(p, w.party_get_vote_share(p));
    }
}

// Challenger entry and party dissolution (competitive systems).
bool entry_and_exit(game_state& state, dcon::nation_id n, std::vector<dcon::party_id> const& parties, float years) {
    auto& w = state.world;
    auto const& P = state.params;
    bool changed = false;
    // Exit: persistent irrelevance.
    for(auto p : parties) {
        if(w.party_get_vote_share(p) < 0.01f && !w.party_get_in_government(p) && w.nation_get_incumbent(n) != p) {
            w.party_set_low_vote_months(p, w.party_get_low_vote_months(p) + years * 12.0f);
            if(w.party_get_low_vote_months(p) >= static_cast<float>(P.party_exit_months) && parties.size() > 2) {
                log_event(state, n, journal_party_exit, static_cast<float>(w.party_get_local_index(p)), -1.0f);
                w.delete_party(p);
                changed = true;
            }
        } else {
            w.party_set_low_vote_months(p, 0.0f);
        }
    }
    if(changed || parties.size() >= static_cast<std::size_t>(max_parties)) return changed;
    // Entry: a large unrepresented constituency, at most one new party a year.
    if(state.current_tick - w.nation_get_last_party_entry(n) < 365 && w.nation_get_last_party_entry(n) != 0) return false;
    std::array<double, axes> sum{};
    double unrep = 0.0, total = 0.0;
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            auto pp = loc.get_pop();
            float nearest = 1e9f;
            for(auto p : parties) {
                float d2 = 0.0f;
                for(uint8_t a = 0; a < axes; ++a) {
                    const float d = pp.get_values(a) - w.party_get_position(p, a);
                    d2 += d * d;
                }
                nearest = std::min(nearest, std::sqrt(d2));
            }
            total += pp.get_size();
            if(nearest > static_cast<float>(P.party_entry_distance)) {
                unrep += pp.get_size();
                for(uint8_t a = 0; a < axes; ++a) sum[a] += pp.get_size() * pp.get_values(a);
            }
        }
    }
    if(total <= 0.0 || unrep / total < P.party_entry_share) return false;
    const auto p = w.create_party();
    w.party_set_key(p, core::pack_key("NEW"));
    w.party_set_family(p, static_cast<uint8_t>(party_family::challenger));
    w.party_set_strategy(p, static_cast<uint8_t>(party_strategy::hunter));
    for(uint8_t a = 0; a < axes; ++a) {
        w.party_set_position(p, a, static_cast<float>(sum[a] / unrep));
        w.party_set_heading(p, a, 0.0f);
    }
    w.party_set_vote_share(p, 0.0f);
    w.party_set_last_vote(p, 0.0f);
    w.force_create_party_membership(p, n);
    w.nation_set_last_party_entry(n, state.current_tick);
    log_event(state, n, journal_party_entry, -1.0f, static_cast<float>(unrep / total));
    return true;
}

// Local indices follow economic left-right order (coalitions are "connected" in it).
void reindex_parties(game_state& state, dcon::nation_id n) {
    auto& w = state.world;
    std::vector<dcon::party_id> list;
    for(auto m : w.nation_get_party_membership(n)) list.push_back(m.get_party().id);
    std::sort(list.begin(), list.end(), [&](auto a, auto b) {
        return w.party_get_position(a, static_cast<uint8_t>(value_axis::economic_right))
               < w.party_get_position(b, static_cast<uint8_t>(value_axis::economic_right));
    });
    for(std::size_t i = 0; i < list.size(); ++i) w.party_set_local_index(list[i], static_cast<uint8_t>(i));
}

} // namespace

void init_politics(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) {
        const auto parties = parties_of(state, n.id);
        update_window(state, n.id, 0.0f, true);
        update_policy_position(state, n.id, true);
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
        update_window(state, n.id, years, false);
        update_policy_position(state, n.id);
        auto parties = parties_of(state, n.id);
        if(parties.empty()) continue;

        vote_intention(state, n.id, parties);
        if(n.get_competitive()) {
            adapt_parties(state, n.id, parties, years);
            if(entry_and_exit(state, n.id, parties, years)) {
                reindex_parties(state, n.id);
                parties = parties_of(state, n.id);
                vote_intention(state, n.id, parties);
            } else {
                reindex_parties(state, n.id);
            }
        }
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
        // Policy beyond the Overton window erodes legitimacy (acceptance of policy).
        std::array<float, axes> policy{};
        for(uint8_t a = 0; a < axes; ++a) policy[a] = n.get_policy_position(a);
        const float outside = std::max(0.0f, outside_window(state, n.id, policy) - n.get_window_outside0());
        const float target = 0.4f * procedural + 0.4f * performance + 0.2f * 0.7f - static_cast<float>(P.overton_legitimacy) * outside;
        float legit = n.get_legitimacy();
        legit += (target - legit) * std::min(1.0f, years / static_cast<float>(P.legitimacy_years));
        legit -= static_cast<float>(P.force_legitimacy_cost) * static_cast<float>(forced) * years;
        n.set_legitimacy(std::clamp(legit, 0.0f, 1.0f));
        n.set_winning_coalition(0.05f + 0.6f * n.get_electoral());

        // Coups: small winning coalition, low legitimacy, widespread unrest.
        const float unrest_mean = size > 0 ? static_cast<float>(unrest / size) : 0.0f;
        // Coups: a disloyal military with an opportunity (Powell 2012; Svolik 2012).
        const float loyalty = n.get_military_agent() ? n.get_military_loyalty() : 1.0f;
        if(loyalty < static_cast<float>(P.coup_loyalty) && n.get_legitimacy() < 0.4f && unrest_mean > 1.5f) {
            const float p = static_cast<float>(P.coup_rate) * (unrest_mean - 1.5f) * (1.0f - n.get_winning_coalition())
                            * (static_cast<float>(P.coup_loyalty) - loyalty) * 10.0f;
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
