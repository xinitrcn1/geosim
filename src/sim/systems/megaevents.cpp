// Mega-events (FIFA World Cup, Olympic Games; docs/06-geopolitics-ai.md#mega-events).
//
// Awarding. Each edition is awarded `lead_years` ahead. Bids come from nations that can
// carry the cost: the yearly build-up cost must stay under `bid_gdp_share` of GDP, a
// threshold autocracies stretch for prestige (sportswashing; Grix & Lee 2013). Among the
// bidders the owner favours large economies, rotation away from recent hosts and
// friendly members, with an idiosyncratic term for lobbying (Feddersen, Maennig &
// Zimmermann 2008 on Olympic bid success; Poast 2007).
// Build-up. Capital spending (venues, transport) is spread evenly from the award to the
// opening and paid by the government (Baade & Matheson 2016; Flyvbjerg et al. 2021 on
// overruns). Relative to the start state only: spending already under way in 2019 is in
// the calibrated base, so its end shows as a fall in public demand.
// The event. Visitors' spending is a services export over the event's days (net
// tourism gains are small, displaced tourists offset part: Baade & Matheson 2016); the
// host's government enjoys a feel-good legitimacy gain (Kavetsos & Szymanski 2010).
// Afterwards other nations' trust in the host rises a little (soft power; Grix & Lee 2013).
// Costs amid grievance feed protests (Confederations Cup, Brazil 2013).
// Postponement. An event whose host faces a grave epidemic threat within 90 days of
// opening is postponed by a year, once (Tokyo 2020).

#include "core/calendar.hpp"
#include "core/rng.hpp"
#include "sim/model.hpp"
#include "sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace geosim::sim {

namespace {
enum : uint8_t { mega_pending = 0, mega_running = 1, mega_done = 2 };
constexpr uint8_t mega_postponed_bit = 0x80;

uint16_t ix(dcon::nation_id n) { return static_cast<uint16_t>(n.index()); }

float distance_km(game_state const& state, dcon::nation_id a, dcon::nation_id b) {
    auto const& w = state.world;
    constexpr float rad = 3.14159265f / 180.0f;
    const float la1 = w.nation_get_lat(a) * rad, la2 = w.nation_get_lat(b) * rad;
    const float dlat = la2 - la1, dlon = (w.nation_get_lon(b) - w.nation_get_lon(a)) * rad;
    const float h = std::sin(dlat / 2) * std::sin(dlat / 2) + std::cos(la1) * std::cos(la2) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2.0f * 6371.0f * std::asin(std::min(1.0f, std::sqrt(h)));
}

void log_mega(game_state& state, dcon::mega_event_id e, float phase) {
    auto& w = state.world;
    const auto n = w.mega_event_get_host(e);
    const auto d = w.create_decision();
    w.decision_set_agent(d, w.nation_get_agent_from_executive_seat(n));
    w.decision_set_target(d, n);
    w.decision_set_tick(d, state.current_tick);
    w.decision_set_lever(d, journal_mega_event);
    w.decision_set_before(d, phase);
    w.decision_set_after(d, static_cast<float>(core::date_from_tick(w.mega_event_get_start(e), state.epoch_days).year));
    w.decision_set_pragmatic(d, static_cast<float>(w.mega_event_get_organisation(e).id.index()));
    w.decision_set_ideological(d, NAN);
    w.decision_set_prior(d, NAN);
}

double nominal_gdp(game_state const& state, dcon::nation_id n) {
    auto const& w = state.world;
    return w.nation_get_gdp0(n) * w.nation_get_core_price(n) * w.nation_get_gf(n);
}

// Construction spending per year of an edition (USD).
double build_rate(game_state const& state, dcon::mega_event_id e) {
    auto const& w = state.world;
    const double years = std::max(1.0, (w.mega_event_get_start(e) - w.mega_event_get_awarded(e)) / 365.0);
    return static_cast<double>(w.mega_event_get_capex_bn(e)) * 1e9 / years;
}

// Construction share of GDP of n at `tick` (from every edition it hosts).
float build_share_at(game_state const& state, dcon::nation_id n, int32_t tick) {
    auto const& w = state.world;
    const double gdp = nominal_gdp(state, n);
    if(gdp <= 0.0) return 0.0f;
    double spend = 0.0;
    for(auto e : w.in_mega_event) {
        if(e.get_host() != n || tick < e.get_awarded() || tick >= e.get_start()) continue;
        spend += build_rate(state, e.id);
    }
    return static_cast<float>(spend / gdp);
}

// Is an edition of `o` opening in `year` already scheduled?
bool edition_exists(game_state const& state, dcon::organisation_id o, int year) {
    auto const& w = state.world;
    for(auto e : w.in_mega_event) {
        if(e.get_organisation() != o) continue;
        // Postponed editions keep their original edition year.
        const int y = core::date_from_tick(e.get_start(), state.epoch_days).year - ((e.get_status() & mega_postponed_bit) ? 1 : 0);
        if(y == year) return true;
    }
    return false;
}

void award_edition(game_state& state, dcon::organisation_id o, int year) {
    auto& w = state.world;
    auto const& P = state.params;
    const double years = std::max(1.0f, w.organisation_get_lead_years(o));
    const double cost = static_cast<double>(w.organisation_get_event_cost_bn(o)) * 1e9;
    // Recent hosts of this owner (rotation).
    std::vector<dcon::nation_id> recent;
    for(auto e : w.in_mega_event) {
        if(e.get_organisation() == o) recent.push_back(e.get_host());
    }
    dcon::nation_id best;
    float best_score = -1e9f;
    for(auto n : w.in_nation) {
        const double gdp = nominal_gdp(state, n.id);
        if(gdp <= 0.0 || n.get_population() < 1e6f) continue;
        bool war = false;
        for(auto x : w.in_nation) war = war || n.get_at_war(ix(x.id)) > 0.0f;
        if(war) continue;
        // Prestige stretches the affordable share for autocracies (up to 10x).
        const double tolerance = P.bid_gdp_share * (1.0 + 9.0 * std::clamp(1.0f - n.get_electoral(), 0.0f, 1.0f));
        if(cost / years / gdp > tolerance) continue;
        float rotation = 0.0f;
        for(std::size_t i = recent.size() > 3 ? recent.size() - 3 : 0; i < recent.size(); ++i) {
            if(recent[i] == n.id) rotation -= 2.0f;
            else if(distance_km(state, recent[i], n.id) < 3000.0f) rotation -= 0.7f;
        }
        float friends = 0.0f, k = 0.0f;
        for(auto x : w.in_nation) {
            if(x.id == n.id || x.get_gdp0() <= 0.0) continue;
            friends += x.get_trust(ix(n.id));
            k += 1.0f;
        }
        friends = k > 0.0f ? friends / k : 0.5f;
        const float u = std::max(1e-6f, core::uniform01(state.rng_seed, static_cast<uint32_t>(core::rng_stream::ai), static_cast<uint32_t>(year),
                                                        static_cast<uint32_t>(o.index()), static_cast<uint32_t>(n.id.index())));
        const float gumbel = -std::log(-std::log(u));
        const float score = 0.5f * std::log(static_cast<float>(gdp / 1e11)) + rotation + 2.0f * friends
                            + 0.5f * (1.0f - n.get_electoral()) + gumbel;
        if(score > best_score) {
            best_score = score;
            best = n.id;
        }
    }
    if(!best) return;
    const auto e = w.create_mega_event();
    const core::civil_date open{year, w.organisation_get_event_month(o), w.organisation_get_event_day(o)};
    const int32_t start = core::tick_from_date(open, state.epoch_days);
    w.mega_event_set_organisation(e, o);
    w.mega_event_set_host(e, best);
    w.mega_event_set_share(e, 1.0f);
    w.mega_event_set_start(e, start);
    w.mega_event_set_finish(e, start + static_cast<int32_t>(w.organisation_get_duration_days(o)));
    w.mega_event_set_awarded(e, state.current_tick);
    w.mega_event_set_status(e, mega_pending);
    w.mega_event_set_capex_bn(e, w.organisation_get_event_cost_bn(o));
    log_mega(state, e, 0.0f);
}
} // namespace

void init_mega_events(game_state& state) {
    auto& w = state.world;
    for(auto n : w.in_nation) {
        const float s = build_share_at(state, n.id, state.current_tick);
        n.set_mega_build_share0(s);
        n.set_mega_build_share(s);
        n.set_mega_investment(0.0f);
        n.set_mega_exports(0.0f);
        n.set_mega_glow(0.0f);
    }
}

void update_mega_events(game_state& state) {
    auto& w = state.world;
    auto const& P = state.params;
    const int32_t tick = state.current_tick;
    const auto date = core::date_from_tick(tick, state.epoch_days);

    // Awards: on 1 January, every edition opening within the lead time is awarded.
    if(date.month == 1 && date.day == 1) {
        for(auto o : w.in_organisation) {
            if(o.get_kind() != static_cast<uint8_t>(org_kind::event) || o.get_cycle_years() <= 0.0f) continue;
            const int cycle = static_cast<int>(o.get_cycle_years());
            // The edition opening `lead_years` ahead (editions already awarded are in the
            // scenario; one whose host is outside the modelled world is not re-awarded).
            const int y = date.year + static_cast<int>(o.get_lead_years());
            if(((y - static_cast<int>(o.get_cycle_phase())) % cycle + cycle) % cycle == 0 && !edition_exists(state, o.id, y)) {
                award_edition(state, o.id, y);
            }
        }
    }

    // Athletes and visitors come from everywhere: when nations holding most of the world's
    // population face a grave threat, teams withdraw (Canada and Australia, March 2020)
    // and the event is postponed even if the host is calm.
    const float grave = static_cast<float>(P.mega_postpone_threat);
    double threat_sum = 0.0, pop_sum = 0.0;
    for(auto n : w.in_nation) {
        n.set_mega_exports(0.0f);
        n.set_mega_glow(0.0f);
        threat_sum += n.get_threat() > grave ? n.get_population() : 0.0;
        pop_sum += n.get_population();
    }
    const bool world_grave = pop_sum > 0.0 && threat_sum / pop_sum > 0.5;
    for(auto e : w.in_mega_event) {
        const auto n = e.get_host();
        const uint8_t status = e.get_status() & 0x7F;
        if(status == mega_done) continue;
        // Postponement before opening under a grave epidemic threat (once).
        if(status == mega_pending && !(e.get_status() & mega_postponed_bit) && tick < e.get_start() && e.get_start() - tick <= 90
           && (w.nation_get_threat(n) > grave || world_grave)) {
            e.set_start(e.get_start() + 365);
            e.set_finish(e.get_finish() + 365);
            e.set_status(static_cast<uint8_t>(mega_pending | mega_postponed_bit));
            log_mega(state, e.id, 3.0f);
            continue;
        }
        if(tick >= e.get_start() && tick <= e.get_finish()) {
            if(status == mega_pending) {
                e.set_status(static_cast<uint8_t>(mega_running | (e.get_status() & mega_postponed_bit)));
                log_mega(state, e.id, 1.0f);
            }
            // Visitors' spending as a share of GDP per year (economy.cpp reads % of GDP).
            const double days = std::max(1, e.get_finish() - e.get_start());
            const auto o = e.get_organisation();
            const double spend_year = static_cast<double>(w.organisation_get_visitor_spend_bn(o)) * 1e9 * e.get_share() * 365.0 / days;
            const double gdp = nominal_gdp(state, n);
            if(gdp > 0.0) n.set_mega_exports(w.nation_get_mega_exports(n) + static_cast<float>(100.0 * spend_year / gdp));
            w.nation_set_mega_glow(n, static_cast<float>(P.mega_legitimacy));
        } else if(tick > e.get_finish()) {
            e.set_status(static_cast<uint8_t>(mega_done | (e.get_status() & mega_postponed_bit)));
            for(auto m : w.in_nation) {
                if(m.id == n) continue;
                const float t = m.get_trust(ix(n));
                m.set_trust(ix(n), std::min(1.0f, t + static_cast<float>(P.mega_trust) * e.get_share()));
            }
            log_mega(state, e.id, 2.0f);
        }
    }
    // Construction relative to the start state (% of GDP).
    for(auto n : w.in_nation) {
        const float s = build_share_at(state, n.id, tick);
        n.set_mega_build_share(s);
        n.set_mega_investment(100.0f * (s - n.get_mega_build_share0()));
    }
}

} // namespace geosim::sim
