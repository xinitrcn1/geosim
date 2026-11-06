// Player -> sim commands (commands.hpp).

#include "sim/commands.hpp"

#include "sim/model.hpp"

#include <algorithm>
#include <cstring>

namespace geosim::sim {

namespace {
template<typename Id>
Id id_of(uint32_t i) {
    return i == no_index ? Id{} : Id{static_cast<typename Id::value_base_t>(i)};
}
template<typename Id>
uint32_t index_of(Id id) {
    return id ? static_cast<uint32_t>(id.index()) : no_index;
}
void set_message(command_result& r, std::string const& m) {
    const auto n = std::min(m.size(), sizeof(r.message) - 1);
    std::memcpy(r.message, m.data(), n);
    r.message[n] = 0;
}
} // namespace

command_result apply_command(game_state& state, command const& c) {
    auto& w = state.world;
    command_result r;
    r.player = c.player;
    r.seq = c.seq;
    r.tag = c.tag;
    switch(c.tag) {
    case command_tag::set_override: {
        const auto a = id_of<dcon::agent_id>(c.u.set_override.agent);
        if(!a || !w.agent_is_valid(a) || c.u.set_override.lever >= state.derived.lever_count) {
            set_message(r, "No such agent or lever");
            break;
        }
        w.agent_set_override_mode(a, c.u.set_override.lever, c.u.set_override.mode);
        w.agent_set_override_value(a, c.u.set_override.lever, c.u.set_override.value);
        r.accepted = 1;
        break;
    }
    case command_tag::set_ai: {
        const auto a = id_of<dcon::agent_id>(c.u.set_ai.agent);
        if(!a || !w.agent_is_valid(a)) {
            set_message(r, "No such agent");
            break;
        }
        // AI off: every lever is directed at its current choice until the player moves it;
        // AI on: overrides are cleared and the agent decides again (the player advises).
        for(uint8_t l = 0; l < state.derived.lever_count; ++l) {
            if(c.u.set_ai.on) {
                w.agent_set_override_mode(a, l, static_cast<uint8_t>(override_mode::none));
            } else {
                w.agent_set_override_mode(a, l, static_cast<uint8_t>(override_mode::direct));
                w.agent_set_override_value(a, l, w.agent_get_last_choice(a, l));
            }
        }
        r.accepted = 1;
        break;
    }
    case command_tag::action: {
        action_request q;
        q.kind = static_cast<action_kind>(c.u.action.kind);
        q.actor = id_of<dcon::agent_id>(c.u.action.actor);
        q.target = id_of<dcon::nation_id>(c.u.action.target_nation);
        q.organisation = id_of<dcon::organisation_id>(c.u.action.organisation);
        q.treaty = static_cast<treaty_kind>(c.u.action.treaty);
        q.lever = c.u.action.lever;
        q.value = c.u.action.value;
        q.target_agent = id_of<dcon::agent_id>(c.u.action.target_agent);
        q.region = id_of<dcon::region_id>(c.u.action.region);
        q.state_to = id_of<dcon::state_id>(c.u.action.state_to);
        if((q.region && !w.region_is_valid(q.region)) || (q.state_to && !w.state_is_valid(q.state_to))) {
            set_message(r, "No such region or state");
            break;
        }
        if((q.actor && !w.agent_is_valid(q.actor)) || (q.target_agent && !w.agent_is_valid(q.target_agent))) {
            set_message(r, "No such agent");
            break;
        }
        const auto res = perform_action(state, q);
        r.accepted = res.accepted ? 1 : 0;
        set_message(r, res.message);
        break;
    }
    case command_tag::set_control: {
        const auto a = id_of<dcon::agent_id>(c.u.set_control.agent);
        if(!a || !w.agent_is_valid(a)) {
            set_message(r, "No such agent");
            break;
        }
        w.agent_set_player_controlled(a, c.u.set_control.controlled);
        w.agent_set_answer_requests(a, c.u.set_control.controlled && c.u.set_control.answer_requests ? 1 : 0);
        r.accepted = 1;
        break;
    }
    case command_tag::answer: {
        const auto q = id_of<dcon::request_id>(c.u.answer.request);
        const auto a = id_of<dcon::agent_id>(c.u.answer.agent);
        if(!q || !w.request_is_valid(q) || w.request_get_to(q) != a || !w.agent_get_player_controlled(a)) {
            set_message(r, "The request was already answered");
            break;
        }
        const auto res = answer_request(state, q, c.u.answer.accept != 0, 1);
        r.accepted = res.accepted ? 1 : 0;
        set_message(r, res.message);
        break;
    }
    default: set_message(r, "Unknown command"); break;
    }
    return r;
}

int drain_commands(game_state& state, command_queue& in, result_queue& out) {
    int n = 0;
    while(auto const* next = in.peek()) {
        if(next->tick > state.current_tick) break; // due later (lockstep input delay)
        const auto c = *in.pop();
        const auto r = apply_command(state, c);
        out.push(r); // a full result queue drops the result, never the command
        ++n;
    }
    return n;
}

command make_set_override(dcon::agent_id a, uint8_t lever, override_mode mode, float value, int32_t tick, uint32_t seq) {
    command c;
    c.tag = command_tag::set_override;
    c.seq = seq;
    c.tick = tick;
    c.u.set_override = {index_of(a), lever, static_cast<uint8_t>(mode), value};
    return c;
}
command make_set_ai(dcon::agent_id a, bool on, int32_t tick, uint32_t seq) {
    command c;
    c.tag = command_tag::set_ai;
    c.seq = seq;
    c.tick = tick;
    c.u.set_ai = {index_of(a), static_cast<uint8_t>(on ? 1 : 0)};
    return c;
}
command make_set_control(dcon::agent_id a, bool controlled, bool answer_requests, int32_t tick, uint32_t seq) {
    command c;
    c.tag = command_tag::set_control;
    c.seq = seq;
    c.tick = tick;
    c.u.set_control = {index_of(a), static_cast<uint8_t>(controlled ? 1 : 0), static_cast<uint8_t>(answer_requests ? 1 : 0)};
    return c;
}
command make_answer(dcon::request_id q, dcon::agent_id to, bool accept, int32_t tick, uint32_t seq) {
    command c;
    c.tag = command_tag::answer;
    c.seq = seq;
    c.tick = tick;
    c.u.answer = {index_of(q), index_of(to), static_cast<uint8_t>(accept ? 1 : 0)};
    return c;
}
command make_action(action_request const& q, int32_t tick, uint32_t seq) {
    command c;
    c.tag = command_tag::action;
    c.seq = seq;
    c.tick = tick;
    c.u.action = {index_of(q.actor), index_of(q.target), index_of(q.organisation), index_of(q.target_agent), index_of(q.region), index_of(q.state_to),
                  static_cast<uint8_t>(q.kind), static_cast<uint8_t>(q.treaty), q.lever, q.value};
    return c;
}

} // namespace geosim::sim
