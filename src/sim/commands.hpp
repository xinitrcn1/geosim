// Player -> sim commands (docs/02-simulation-core.md#commands). Everything a player does to
// the simulation is a command: a tag and a payload union of plain values (raw entity
// indices, no pointers or strings), stamped with the tick it applies to. The client pushes
// commands into an SPSC queue; the sim thread drains the ones due before simulating each
// tick and applies them in order. Because commands are plain data applied at a known
// tick, a deterministic lockstep peer can apply the same stream and stay identical; the
// same stream replays a session. Results go back through a second SPSC queue.

#pragma once

#include "core/spsc.hpp"
#include "sim/game_state.hpp"
#include "sim/systems.hpp"

#include <cstdint>
#include <type_traits>

namespace geosim::sim {

inline constexpr uint32_t no_index = 0xFFFFFFFFu;

enum class command_tag : uint8_t {
    set_override, // one lever of an agent: mode (override_mode) and value
    set_ai,       // the agent's AI on (player advises / forces) or off (player directs every lever)
    action,       // a diplomatic or political move (actions.cpp)
    set_control,  // the player takes or drops an agent; whether requests to it wait for the player
    answer,       // the player answers a request to one of their agents (requests.cpp)
    count
};

struct cmd_set_override {
    uint32_t agent;
    uint8_t lever;
    uint8_t mode; // override_mode
    float value;
};
struct cmd_set_ai {
    uint32_t agent;
    uint8_t on;
};
struct cmd_action {
    uint32_t actor, target_nation, organisation; // no_index when absent
    uint32_t target_agent;                       // no_index when absent
    uint32_t region, state_to;                   // redraw_state (no_index when absent)
    uint8_t kind;                                // action_kind
    uint8_t treaty;                              // treaty_kind
    uint8_t lever;
    float value;
};

struct cmd_set_control {
    uint32_t agent;
    uint8_t controlled;      // 1: the player's agent
    uint8_t answer_requests; // 1: requests to it wait for the player (request_window_days)
};
struct cmd_answer {
    uint32_t request; // dcon::request_id index
    uint32_t agent;   // the asked agent (checked: the request must still be to it)
    uint8_t accept;
};

struct command {
    command_tag tag = command_tag::count;
    uint8_t player = 0; // issuing player (multiplayer)
    uint32_t seq = 0;   // per-player sequence number, echoed in the result
    int32_t tick = 0;   // the tick before which it applies (lockstep: current tick + input delay)
    union {
        cmd_set_override set_override;
        cmd_set_ai set_ai;
        cmd_action action;
        cmd_set_control set_control;
        cmd_answer answer;
    } u{};
};
static_assert(std::is_trivially_copyable_v<command>, "commands travel between threads and peers");

struct command_result {
    uint8_t player = 0;
    uint32_t seq = 0;
    command_tag tag = command_tag::count;
    uint8_t accepted = 0;
    char message[110] = {};
};
static_assert(std::is_trivially_copyable_v<command_result>);

using command_queue = core::spsc_queue<command, 1024>;
using result_queue = core::spsc_queue<command_result, 1024>;

// Applies one command to the state (sim thread, between ticks).
command_result apply_command(game_state& state, command const& c);
// Applies every queued command due at or before the current tick, in order, posting
// results; call before advance_day. Returns the number applied.
int drain_commands(game_state& state, command_queue& in, result_queue& out);

// Helpers for the producer side.
command make_set_override(dcon::agent_id a, uint8_t lever, override_mode mode, float value, int32_t tick, uint32_t seq);
command make_set_ai(dcon::agent_id a, bool on, int32_t tick, uint32_t seq);
command make_action(action_request const& r, int32_t tick, uint32_t seq);
command make_set_control(dcon::agent_id a, bool controlled, bool answer_requests, int32_t tick, uint32_t seq);
command make_answer(dcon::request_id r, dcon::agent_id to, bool accept, int32_t tick, uint32_t seq);

} // namespace geosim::sim
