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
#include <map>
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
// Fiscal packages count while they are recent: a memory of announcements that fades over
// stimulus_memory_years (the cumulative total never fell, so a 2020 package kept a
// government "left" for ever).
void update_policy_position(game_state& state, dcon::nation_id n, bool baseline = false, float years = 0.0f) {
    auto& w = state.world;
    auto const& D = state.derived;
    const auto inc = w.nation_get_incumbent(n);
    std::array<float, axes> pos{};
    if(inc) pos = party_pos(state, inc);
    if(baseline) {
        w.nation_set_fiscal_seen(n, w.nation_get_fiscal_announced_cum(n));
        w.nation_set_stimulus_memory(n, 0.0f);
    } else if(years > 0.0f) {
        const float fresh = w.nation_get_fiscal_announced_cum(n) - w.nation_get_fiscal_seen(n);
        w.nation_set_fiscal_seen(n, w.nation_get_fiscal_announced_cum(n));
        w.nation_set_stimulus_memory(n, w.nation_get_stimulus_memory(n) * std::exp(-years / static_cast<float>(state.params.stimulus_memory_years)) + fresh);
    }
    pos[static_cast<uint8_t>(value_axis::economic_right)] -= 0.02f * w.nation_get_stimulus_memory(n) + 0.03f * w.nation_get_directed_credit(n);
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

void reindex_parties(game_state& state, dcon::nation_id n, std::vector<dcon::party_id> const& before);

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

// Retrospective performance as one pop feels it: income and jobs (grievance), deaths,
// and corruption made salient by hard times.
float pop_performance(game_state const& state, dcon::pop_id pid, float deaths_pm) {
    auto const& w = state.world;
    auto const& P = state.params;
    return -2.0f * w.pop_get_grievance(pid) - static_cast<float>(P.vote_health_weight) * deaths_pm
           - static_cast<float>(P.corruption_vote) * std::max(0.0f, w.pop_get_salience(pid, static_cast<uint8_t>(issue::corruption)) - 0.05f);
}

// A new government starts with a clean slate: voters judge it on what changes on its
// watch (the honeymoon; blame does not pass to the successor of those who caused it).
void reset_performance(game_state& state, dcon::nation_id n) {
    auto& w = state.world;
    const float deaths_pm = reported_deaths_per_million_daily(state, n);
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) loc.get_pop().set_perf_ref(pop_performance(state, loc.get_pop().id, deaths_pm));
    }
}

// Recomputes every pop's party support and the national vote shares.
// Party identification (Green, Palmquist & Schickler 2002): pops keep a share
// `inertia` of their previous support, so vote intention moves gradually.
// Retrospective voting judges the incumbent on performance relative to a reference
// that adapts over `vote_memory_years` (voters are myopic and reward change, not levels;
// Healy & Lenz 2014; Achen & Bartels 2016), and its effect saturates
// (`vote_retrospective_cap`): partisans discount bad news about their own side
// (Bartels 2002), so no performance drives a major party to zero. `years` > 0 moves
// the reference (the monthly update).
void vote_intention(game_state& state, dcon::nation_id n, std::vector<dcon::party_id> const& parties, float inertia, float years = 0.0f) {
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
            const float values_w = 0.3f + p.get_salience(static_cast<uint8_t>(issue::identity))
                                   + p.get_salience(static_cast<uint8_t>(issue::immigration))
                                   + p.get_salience(static_cast<uint8_t>(issue::environment));
            const float felt = pop_performance(state, p.id, deaths_pm);
            const float cap = std::max(1e-3f, static_cast<float>(P.vote_retrospective_cap));
            const float performance = cap * std::tanh((felt - p.get_perf_ref()) / cap);
            if(years > 0.0f) p.set_perf_ref(p.get_perf_ref() + (felt - p.get_perf_ref()) * std::min(1.0f, years / static_cast<float>(P.vote_memory_years)));
            std::array<float, max_parties> u{};
            float umax = -1e30f;
            for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
                float d2 = 0.0f;
                for(uint8_t a = 0; a < static_cast<uint8_t>(value_axis::count); ++a) {
                    float at = w.party_get_position(parties[j], a);
                    // The thermostatic public judges the incumbent by what it does as well
                    // as by its label (Wlezien 1995; Soroka & Wlezien 2010).
                    if(parties[j] == incumbent && a == static_cast<uint8_t>(value_axis::economic_right)) at += static_cast<float>(P.thermostat) * (w.nation_get_policy_position(n, a) - at);
                    const float d = p.get_values(a) - at;
                    d2 += (a == static_cast<uint8_t>(value_axis::economic_right) ? econ_w : values_w) * d * d;
                }
                const float out = outside_window(state, n, party_pos(state, parties[j]));
                u[j] = w.party_get_valence(parties[j]) // non-policy appeal: identification, leaders, organisation
                       - static_cast<float>(P.vote_distance_weight) * d2 - static_cast<float>(P.window_stigma) * out * out
                       + (parties[j] == incumbent ? static_cast<float>(P.vote_retrospective) * performance : 0.0f);
                umax = std::max(umax, u[j]);
            }
            float z = 0.0f;
            for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
                u[j] = std::exp((u[j] - umax) / static_cast<float>(P.vote_temperature));
                z += u[j];
            }
            const double weight = static_cast<double>(p.get_size()) * turnout[p.get_kind() & 3];
            float old_total = 0.0f;
            for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) old_total += p.get_party_support(static_cast<uint8_t>(j));
            const float keep = old_total > 0.5f ? inertia : 0.0f;
            for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
                const float share = keep * p.get_party_support(static_cast<uint8_t>(j)) + (1.0f - keep) * (z > 0.0f ? u[j] / z : 0.0f);
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
    auto const& P = state.params;
    const auto system = static_cast<electoral_system>(w.nation_get_electoral_system(n));
    // Single-member districts. A region (a state, a province) holds many districts, so
    // its seats - in proportion to its population - split by the seat-vote curve of
    // single-member districts, the cube law (Kendall & Stuart 1950; Taagepera 1986), on
    // the region's vote, softer under two-round runoffs (Duverger 1954; Blais & Loewen
    // 2009 put runoff disproportionality between FPTP and PR). Giving each region's seats
    // to its plurality winner made a 54% national vote 97% of the US House.
    auto districts = [&](double exponent) {
        std::array<double, max_parties> out{};
        // Seats are apportioned per state (its clusters' votes summed): redrawing state
        // borders moves votes between seat pools.
        std::map<uint32_t, std::array<double, max_parties>> votes_of;
        std::map<uint32_t, double> pop_of;
        for(auto own : w.nation_get_region_ownership(n)) {
            auto r = own.get_region();
            const uint32_t key = r.get_state() ? static_cast<uint32_t>(r.get_state().id.index()) : 0x80000000u + static_cast<uint32_t>(r.id.index());
            auto& v = votes_of[key];
            for(auto loc : r.get_pop_location()) {
                for(std::size_t j = 0; j < np; ++j) v[j] += loc.get_pop().get_size() * loc.get_pop().get_party_support(static_cast<uint8_t>(j));
            }
            pop_of[key] += r.get_population();
        }
        double total = 0.0;
        if(votes_of.size() > 1) {
            for(auto& [key, regional] : votes_of) {
                double sum = 0.0;
                // Within a region the districts are already sorted by geography (urban
                // clustering, districting), so the curve is flatter than the national cube
                // law: scaled by district_exponent / cube_law.
                const double e = std::max(1.0, exponent * static_cast<double>(P.district_exponent) / std::max(1.0, static_cast<double>(P.cube_law)));
                for(std::size_t j = 0; j < np; ++j) {
                    regional[j] = std::pow(std::max(0.0, regional[j]), e);
                    sum += regional[j];
                }
                if(sum <= 0.0) continue;
                for(std::size_t j = 0; j < np; ++j) out[j] += pop_of[key] * regional[j] / sum;
                total += pop_of[key];
            }
        } else {
            for(std::size_t j = 0; j < np; ++j) {
                out[j] = std::pow(std::max(0.0, static_cast<double>(w.party_get_vote_share(parties[j]))), exponent);
                total += out[j];
            }
        }
        for(std::size_t j = 0; j < np; ++j) out[j] = total > 0.0 ? out[j] / total : 0.0;
        return out;
    };
    // List PR: D'Hondt highest averages over a 500-seat chamber, among parties above
    // the legal threshold (Lijphart 1994; Gallagher 1991 on D'Hondt's large-party bias).
    auto list_pr = [&]() {
        std::array<double, max_parties> out{};
        std::array<double, max_parties> v{};
        const float threshold = w.nation_get_pr_threshold(n);
        bool any = false;
        for(std::size_t j = 0; j < np; ++j) {
            const float share = w.party_get_vote_share(parties[j]);
            v[j] = share >= threshold ? share : 0.0;
            any = any || v[j] > 0.0;
        }
        if(!any) {
            for(std::size_t j = 0; j < np; ++j) v[j] = w.party_get_vote_share(parties[j]);
        }
        constexpr int chamber = 500;
        std::array<int, max_parties> won{};
        for(int seat = 0; seat < chamber; ++seat) {
            std::size_t best = 0;
            double best_q = -1.0;
            for(std::size_t j = 0; j < np; ++j) {
                const double q = v[j] / (won[j] + 1);
                if(q > best_q) {
                    best_q = q;
                    best = j;
                }
            }
            ++won[best];
        }
        for(std::size_t j = 0; j < np; ++j) out[j] = static_cast<double>(won[j]) / chamber;
        return out;
    };
    std::array<double, max_parties> seats{};
    switch(system) {
    case electoral_system::fptp: seats = districts(static_cast<double>(P.cube_law)); break;
    case electoral_system::two_round: seats = districts(0.5 * (1.0 + static_cast<double>(P.cube_law))); break;
    // (mixed: below)
    case electoral_system::pr: seats = list_pr(); break;
    case electoral_system::mixed: {
        // Parallel (mixed-member majoritarian) systems: a share smd_share of seats from
        // districts, the rest from lists. Compensatory systems (Germany) are entered
        // with smd_share 0 since the list tier restores proportionality.
        const auto d = districts(static_cast<double>(P.cube_law));
        const auto l = list_pr();
        const double k = std::clamp(static_cast<double>(w.nation_get_smd_share(n)), 0.0, 1.0);
        for(std::size_t j = 0; j < np; ++j) seats[j] = k * d[j] + (1.0 - k) * l[j];
        break;
    }
    default:
        for(std::size_t j = 0; j < np; ++j) seats[j] = w.party_get_vote_share(parties[j]);
        break;
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
    std::size_t head = largest;
    // Presidential systems elect the executive separately, by a runoff between the top
    // two first-round candidates where no one has a majority (Shugart & Carey 1992;
    // Blais, Massicotte & Dobrzynska 1997). The president's party governs, with the
    // legislature it gets (divided government).
    if(w.nation_get_presidential(n) && np >= 2) {
        std::size_t first = 0, second = 1;
        for(std::size_t j = 0; j < np; ++j) {
            if(w.party_get_vote_share(parties[j]) > w.party_get_vote_share(parties[first])) first = j;
        }
        second = first == 0 ? 1 : 0;
        for(std::size_t j = 0; j < np; ++j) {
            if(j != first && w.party_get_vote_share(parties[j]) > w.party_get_vote_share(parties[second])) second = j;
        }
        head = first;
        if(w.party_get_vote_share(parties[first]) < 0.5f) {
            double a = 0.0, b = 0.0;
            for(auto own : w.nation_get_region_ownership(n)) {
                for(auto loc : own.get_region().get_pop_location()) {
                    auto p = loc.get_pop();
                    const float sa = p.get_party_support(static_cast<uint8_t>(first)), sb = p.get_party_support(static_cast<uint8_t>(second));
                    (sa >= sb ? a : b) += p.get_size() * (sa + sb);
                }
            }
            head = a >= b ? first : second;
        }
        double gov = 0.0;
        for(std::size_t j = 0; j < np; ++j) {
            if(j == head) w.party_set_in_government(parties[j], 1);
            if(w.party_get_in_government(parties[j]) && head != largest && j != head) w.party_set_in_government(parties[j], 0); // the president forms the cabinet
            gov += w.party_get_in_government(parties[j]) ? seats[j] : 0.0;
        }
        best_seats = gov;
    }
    term_limit_check(state, n, before && before == parties[head]);
    w.nation_set_incumbent(n, parties[head]);
    if(!before || before != parties[head]) {
        w.nation_set_tenure_days(n, 0.0f);
        reset_performance(state, n);
        w.nation_set_stimulus_memory(n, 0.0f); // its predecessor's packages are not its policy
    }
    w.nation_set_gov_seat_share(n, static_cast<float>(best_seats));
    allocate_portfolios(state, n);

    // The new government's position reshapes the executive.
    if(auto exec = w.nation_get_agent_from_executive_seat(n)) {
        const float econ = w.party_get_position(parties[head], static_cast<uint8_t>(value_axis::economic_right));
        const float secular = w.party_get_position(parties[head], static_cast<uint8_t>(value_axis::secular));
        w.agent_set_fiscal_hawk(exec, std::clamp(0.3f + 0.5f * econ, 0.0f, 1.0f));
        w.agent_set_order_ideal(exec, std::clamp(0.2f - 0.12f * w.nation_get_voice(n) - 0.2f * secular, 0.0f, 0.8f));
    }
    log_event(state, n, journal_election, before ? static_cast<float>(w.party_get_local_index(before)) : -1.0f,
              static_cast<float>(w.party_get_local_index(parties[head])));
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
            // Activists, donors and primaries tie a party to its core (Aldrich 1983; Adams &
            // Merrill 2006): it moves toward its voters only part of the way from its anchor.
            // Pure vote-seeking made both US parties converge on the median and the one
            // that got there first win 80% of the vote.
            if(weight > 0.0) {
                const float k = static_cast<float>(P.party_anchor);
                for(uint8_t a = 0; a < axes; ++a) target[a] = (1.0f - k) * static_cast<float>(sum[a] / weight) + k * w.party_get_anchor(p, a);
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
    w.party_set_valence(p, 0.0f); // a new party: policy appeal only
    w.party_set_key(p, core::pack_key("NEW"));
    w.party_set_family(p, static_cast<uint8_t>(party_family::challenger));
    w.party_set_strategy(p, static_cast<uint8_t>(party_strategy::hunter));
    for(uint8_t a = 0; a < axes; ++a) {
        w.party_set_position(p, a, static_cast<float>(sum[a] / unrep));
        w.party_set_heading(p, a, 0.0f);
        w.party_set_anchor(p, a, w.party_get_position(p, a));
    }
    w.party_set_vote_share(p, 0.0f);
    w.party_set_last_vote(p, 0.0f);
    w.force_create_party_membership(p, n);
    w.nation_set_last_party_entry(n, state.current_tick);
    log_event(state, n, journal_party_entry, -1.0f, static_cast<float>(unrep / total));
    return true;
}

// Local indices follow economic left-right order (coalitions are "connected" in it).
// Pops hold their support by local index, so `before` - the party list the support
// arrays were written for (parties_of before any entry, exit or move) - lets every pop's
// support follow its party to the new index; dissolved parties' support is dropped and
// the rest renormalised, new parties start from nothing. Without this, a party that
// crossed another on the economic axis swapped voters with it.
void reindex_parties(game_state& state, dcon::nation_id n, std::vector<dcon::party_id> const& before) {
    auto& w = state.world;
    std::vector<dcon::party_id> list;
    for(auto m : w.nation_get_party_membership(n)) list.push_back(m.get_party().id);
    std::sort(list.begin(), list.end(), [&](auto a, auto b) {
        return w.party_get_position(a, static_cast<uint8_t>(value_axis::economic_right))
               < w.party_get_position(b, static_cast<uint8_t>(value_axis::economic_right));
    });
    for(std::size_t i = 0; i < list.size(); ++i) w.party_set_local_index(list[i], static_cast<uint8_t>(i));
    if(list == before) return;
    // New slot of each old slot (-1: dissolved).
    std::array<int, max_parties> to{};
    to.fill(-1);
    for(std::size_t j = 0; j < before.size() && j < max_parties; ++j) {
        for(std::size_t i = 0; i < list.size() && i < max_parties; ++i) {
            if(list[i] == before[j]) to[j] = static_cast<int>(i);
        }
    }
    for(auto own : w.nation_get_region_ownership(n)) {
        for(auto loc : own.get_region().get_pop_location()) {
            auto p = loc.get_pop();
            std::array<float, max_parties> old{}, now{};
            for(std::size_t j = 0; j < max_parties && j < p.get_party_support_size(); ++j) old[j] = p.get_party_support(static_cast<uint8_t>(j));
            float total = 0.0f;
            for(std::size_t j = 0; j < before.size() && j < max_parties; ++j) {
                if(to[j] >= 0) {
                    now[static_cast<std::size_t>(to[j])] = old[j];
                    total += old[j];
                }
            }
            for(std::size_t i = 0; i < max_parties && i < p.get_party_support_size(); ++i) {
                p.set_party_support(static_cast<uint8_t>(i), total > 0.0f ? now[i] / total : 0.0f);
            }
        }
    }
}

} // namespace

// A successful coup (here or by a player-controlled military, actions.cpp): elections and
// liberties are curtailed, the new rulers are order-minded, legitimacy resets low, and
// organisations with a democracy clause suspend the nation.
void apply_coup(game_state& state, dcon::nation_id n) {
    auto& w = state.world;
    w.nation_set_electoral(n, w.nation_get_electoral(n) * 0.5f);
    w.nation_set_liberal(n, w.nation_get_liberal(n) * 0.7f);
    w.nation_set_legitimacy(n, 0.4f);
    if(auto exec = w.nation_get_agent_from_executive_seat(n)) {
        w.agent_set_order_ideal(exec, std::min(0.8f, w.agent_get_order_ideal(exec) + 0.2f));
        w.agent_set_rho_base(exec, std::min(0.8f, w.agent_get_rho_base(exec) + 0.1f));
    }
    log_event(state, n, journal_coup, 0.0f, 1.0f);
    on_coup(state, n);
}

void init_politics(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) {
        const auto parties = parties_of(state, n.id);
        for(auto p : parties) {
            for(uint8_t a = 0; a < axes; ++a) w.party_set_anchor(p, a, w.party_get_position(p, a));
        }
        update_window(state, n.id, 0.0f, true);
        update_policy_position(state, n.id, true);
        if(parties.empty()) continue;
        // Party valence (non-policy appeal: identification, leadership, organisation; Stokes
        // 1963; Adams, Merrill & Grofman 2005) calibrated so that the spatial model
        // reproduces the observed vote shares at the start, the alternative-specific
        // constants of a conditional logit. Shares then move only with voters' values,
        // salience and performance. Without it a centrist minor party (US "others") took
        // the median voter and 70% of the vote.
        {
            std::array<float, max_parties> target{};
            float total = 0.0f;
            for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
                target[j] = w.party_get_vote_share(parties[j]);
                total += target[j];
                w.party_set_valence(parties[j], 0.0f);
            }
            if(total > 0.05f) {
                const float temp = static_cast<float>(state.params.vote_temperature);
                for(int it = 0; it < 40; ++it) {
                    vote_intention(state, n.id, parties, 0.0f);
                    for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j) {
                        const float want = std::max(1e-3f, target[j] / total), got = std::max(1e-4f, w.party_get_vote_share(parties[j]));
                        w.party_set_valence(parties[j], w.party_get_valence(parties[j]) + temp * std::log(want / got));
                    }
                }
            }
        }
        vote_intention(state, n.id, parties, 0.0f);
        const auto inc = n.get_incumbent();
        n.set_approval0(inc ? std::max(0.05f, w.party_get_vote_share(inc)) : 0.5f);
        n.set_approval(0.5f);
        if(inc && !w.party_get_in_government(inc)) w.party_set_in_government(inc, 1);
        allocate_portfolios(state, n.id);
        // Governors of multi-state (federal or devolved) nations, one per state: the state's
        // plurality party; their order ideal follows its secular-traditional position. The
        // state's clusters share it (region.governor).
        std::map<uint32_t, std::array<double, max_parties>> votes_of;
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            if(!r.get_state()) continue;
            auto& v = votes_of[static_cast<uint32_t>(r.get_state().id.index())];
            for(auto loc : r.get_pop_location()) {
                for(std::size_t j = 0; j < parties.size() && j < max_parties; ++j)
                    v[j] += loc.get_pop().get_size() * loc.get_pop().get_party_support(static_cast<uint8_t>(j));
            }
        }
        if(votes_of.size() < 2) continue;
        const auto np = std::min<std::size_t>(parties.size(), max_parties);
        for(auto& [index, regional] : votes_of) {
            const dcon::state_id st{static_cast<dcon::state_id::value_base_t>(index)};
            const auto win = static_cast<std::size_t>(std::max_element(regional.begin(), regional.begin() + static_cast<std::ptrdiff_t>(np)) - regional.begin());
            const auto g = w.create_agent();
            w.agent_set_kind(g, static_cast<uint8_t>(agent_kind::governor));
            w.agent_set_home(g, n.id);
            const float secular = w.party_get_position(parties[win], static_cast<uint8_t>(value_axis::secular));
            w.agent_set_order_ideal(g, std::clamp(0.2f - 0.12f * n.get_voice() - 0.2f * secular, 0.0f, 0.8f));
            w.agent_set_rho_base(g, 0.3f);
            w.agent_set_rho(g, 0.3f);
            w.state_set_governor(st, g);
        }
        for(auto own : n.get_region_ownership()) {
            auto r = own.get_region();
            if(!r.get_state()) continue;
            r.set_governor(r.get_state().get_governor());
            r.set_npi_offset(1.0f);
        }
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
                vote_intention(state, n.id, parties, static_cast<float>(P.party_id_inertia));
                hold_election(state, n.id, parties);
            }
            n.set_next_election(n.get_next_election() + std::max(365, n.get_term_days()));
        }
    }
    for(auto n : w.in_nation) {
        if(!is_due(static_cast<uint32_t>(n.id.index()), tick, monthly_cadence)) continue;
        update_window(state, n.id, years, false);
        update_policy_position(state, n.id, false, years);
        auto parties = parties_of(state, n.id);
        if(parties.empty()) continue;

        vote_intention(state, n.id, parties, static_cast<float>(P.party_id_inertia), years);
        if(n.get_competitive()) {
            adapt_parties(state, n.id, parties, years);
            const bool changed = entry_and_exit(state, n.id, parties, years);
            // `parties` may name a dissolved party now; only identities are compared.
            reindex_parties(state, n.id, parties);
            parties = parties_of(state, n.id);
            // New parties draw support only from the next months' intention (inertia).
            if(changed) vote_intention(state, n.id, parties, static_cast<float>(P.party_id_inertia));
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
        // Identity legitimacy (politics_ext.cpp): inclusion of identity groups (Gilley 2009).
        const float target = 0.4f * procedural + 0.4f * performance + 0.2f * n.get_identity_legitimacy() - static_cast<float>(P.overton_legitimacy) * outside
                             + n.get_mega_glow(); // hosting a mega-event (megaevents.cpp)
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
                            * (static_cast<float>(P.coup_loyalty) - loyalty) * 10.0f
                            * (1.0f - coup_deterrence(state, n.id)); // democracy clauses (Powell, Lasley & Schiel 2016)
            if(core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::elections), static_cast<uint32_t>(state.current_tick),
                               static_cast<uint32_t>(n.id.index())) < p) {
                apply_coup(state, n.id);
            }
        }
    }
}

} // namespace geosim::sim
