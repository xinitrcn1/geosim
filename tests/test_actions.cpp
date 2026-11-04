// Player actions on the 2020 slice (actions.cpp): each is answered by the target's rules.
#include "core/keys.hpp"
#include "sim/commands.hpp"
#include "sim/levers.hpp"
#include "sim/model.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/systems.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

using namespace geosim;

namespace {
std::unique_ptr<game_state> make() {
    auto s = std::make_unique<game_state>();
    const auto report = sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "2020");
    if(!report.ok()) {
        std::fprintf(stderr, "scenario: %s\n", report.error.c_str());
        std::exit(EXIT_FAILURE);
    }
    for(auto e : s->world.in_event) e.set_done(1);
    s->params.incident_rate_scale = 0.0;
    s->params.pandemic_rate = 0.0;
    s->params.war_base_rate = 0.0;
    return s;
}
uint16_t ix(dcon::nation_id n) { return static_cast<uint16_t>(n.index()); }
bool has(std::vector<sim::action_option> const& v, sim::action_kind k) {
    for(auto const& o : v) {
        if(o.kind == k && o.available) return true;
    }
    return false;
}
} // namespace

int main() {
    auto s = make();
    auto& w = s->world;
    const auto usa = sim::find_nation(*s, "USA"), rus = sim::find_nation(*s, "RUS"), ind = sim::find_nation(*s, "IND"), can = sim::find_nation(*s, "CAN");
    const auto exec = w.nation_get_agent_from_executive_seat(usa);
    CHECK(w.agent_get_home(exec) == usa);
    CHECK(w.nation_get_sanction_set(usa, ix(rus)) < 0.0f);

    // Sanctions held by the player survive the government's monthly review.
    auto opts = sim::available_actions(*s, exec, rus, {});
    CHECK(has(opts, sim::action_kind::set_sanction));
    CHECK(!has(opts, sim::action_kind::offer_peace));
    auto r = sim::perform_action(*s, {sim::action_kind::set_sanction, exec, rus, {}, sim::treaty_kind::count, 0, 0.8f});
    CHECK(r.accepted);
    for(int d = 0; d < 60; ++d) sim::advance_day(*s);
    bool conceded = false;
    for(auto d : w.in_decision) conceded = conceded || (d.get_lever() == sim::journal_concession && d.get_target() == rus);
    std::printf("USA sanction on RUS after 60 days held at 0.8: %.2f (Russia conceded: %s)\n", w.nation_get_sanction(usa, ix(rus)), conceded ? "yes" : "no");
    // Held, unless the target conceded (which lifts the sanction and clears the hold).
    CHECK(conceded ? w.nation_get_sanction_set(usa, ix(rus)) < 0.0f : w.nation_get_sanction(usa, ix(rus)) >= 0.79f);
    r = sim::perform_action(*s, {sim::action_kind::set_sanction, exec, rus, {}, sim::treaty_kind::count, 0, 0.8f});
    r = sim::perform_action(*s, {sim::action_kind::clear_sanction, exec, rus, {}, sim::treaty_kind::count, 0, 0.0f});
    CHECK(r.accepted && w.nation_get_sanction_set(usa, ix(rus)) < 0.0f);

    // War and peace: a fresh war's peace offer is refused (nobody is losing yet).
    r = sim::perform_action(*s, {sim::action_kind::declare_war, exec, ind, {}, sim::treaty_kind::count, 0, 0.0f});
    CHECK(r.accepted && w.nation_get_at_war(usa, ix(ind)) > 0.0f);
    r = sim::perform_action(*s, {sim::action_kind::offer_peace, exec, ind, {}, sim::treaty_kind::count, 0, 0.0f});
    std::printf("peace offer on day one: %s (%s)\n", r.accepted ? "accepted" : "refused", r.message.c_str());
    // A side occupied and exhausted accepts.
    w.nation_set_control(usa, ix(ind), 0.5f);
    w.nation_set_war_cost(ind, 0.3f);
    r = sim::perform_action(*s, {sim::action_kind::offer_peace, exec, ind, {}, sim::treaty_kind::count, 0, 0.0f});
    std::printf("peace offer to a losing side: %s (%s)\n", r.accepted ? "accepted" : "refused", r.message.c_str());
    CHECK(r.accepted && w.nation_get_at_war(usa, ix(ind)) <= 0.0f);

    // Aid and a treaty with a poorer partner.
    r = sim::perform_action(*s, {sim::action_kind::send_aid, exec, ind, {}, sim::treaty_kind::count, 0, 0.1f});
    std::printf("aid: %s\n", r.message.c_str());
    CHECK(r.accepted);
    r = sim::perform_action(*s, {sim::action_kind::propose_treaty, exec, ind, {}, sim::treaty_kind::conditional_aid, 0, 0.0f});
    std::printf("conditional aid: %s\n", r.message.c_str());

    // Lobbying: a union's push enters the government's choice as advice.
    const auto uni = w.nation_get_union_agent(can);
    opts = sim::available_actions(*s, uni, can, {});
    CHECK(has(opts, sim::action_kind::lobby));
    const auto lev = static_cast<uint8_t>(s->derived.lever_base + static_cast<uint8_t>(sim::lever::min_wage));
    r = sim::perform_action(*s, {sim::action_kind::lobby, uni, can, {}, sim::treaty_kind::count, lev, 1.5f});
    std::printf("lobby: %s\n", r.message.c_str());
    CHECK(r.accepted && w.nation_get_lobby_until(can) > s->current_tick);

    // Organisations: applying to a club the members distrust fails, leaving works.
    dcon::organisation_id g7;
    for(auto o : w.in_organisation) {
        if(o.get_key() == core::pack_key("g7")) g7 = o.id;
    }
    const auto rexec = w.nation_get_agent_from_executive_seat(rus);
    r = sim::perform_action(*s, {sim::action_kind::apply_membership, rexec, {}, g7, sim::treaty_kind::count, 0, 0.0f});
    std::printf("Russia applies to the G7: %s\n", r.message.c_str());
    r = sim::perform_action(*s, {sim::action_kind::leave_membership, exec, {}, g7, sim::treaty_kind::count, 0, 0.0f});
    CHECK(r.accepted && !sim::membership_in(*s, g7, usa));

    // Commands through the SPSC queue: applied at their tick, in order, with results back.
    {
        static sim::command_queue in;
        static sim::result_queue out;
        const auto bank = w.nation_get_bank_from_monetary_authority(usa);
        const auto rate = static_cast<uint8_t>(s->derived.lever_base + static_cast<uint8_t>(sim::lever::policy_rate));
        CHECK(in.push(sim::make_set_override(bank, rate, sim::override_mode::direct, 3.0f, s->current_tick, 1)));
        CHECK(in.push(sim::make_set_ai(exec, false, s->current_tick + 5, 2))); // due later
        CHECK(sim::drain_commands(*s, in, out) == 1);
        auto r1 = out.pop();
        CHECK(r1 && r1->seq == 1 && r1->accepted);
        CHECK(w.agent_get_override_mode(bank, rate) == static_cast<uint8_t>(sim::override_mode::direct));
        for(int d = 0; d < 5; ++d) sim::advance_day(*s);
        CHECK(sim::drain_commands(*s, in, out) == 1);
        auto r2 = out.pop();
        CHECK(r2 && r2->seq == 2 && r2->accepted);
        CHECK(w.agent_get_override_mode(exec, rate) == static_cast<uint8_t>(sim::override_mode::direct));
        sim::action_request q;
        q.kind = sim::action_kind::send_aid;
        q.actor = exec;
        q.target = ind;
        q.value = 0.05f;
        CHECK(in.push(sim::make_action(q, s->current_tick, 3)));
        CHECK(sim::drain_commands(*s, in, out) == 1);
        auto r3 = out.pop();
        std::printf("aid through the command queue: %s\n", r3 ? r3->message : "(none)");
        CHECK(r3 && r3->accepted);
    }

    // Lever metadata covers every lever in order.
    CHECK(sim::lever_table.size() == static_cast<std::size_t>(sim::lever::count));
    if(failures == 0) std::printf("test_actions: ok\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
