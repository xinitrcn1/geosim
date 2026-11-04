#pragma once
// The player's action tabs (docs/08-ui.md#action-tabs): Policies, Fiscal policy, Trade,
// Diplomacy, Military, and the composition of power beside the Politics tab's
// electoral map. They act for the selected controlled agent and show only the levers
// that agent's kind owns; lever changes and actions go to the sim at its sync point.

#include "sim_runner.hpp"
#include "tab_data.hpp"
#include "ui_context.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace geosim::client {

class alert_feed;
struct hud_state;

struct tabs_state {
    uint32_t agent = 0xFFFFFFFFu; // the acting agent (a controlled one; default the first)

    // The snapshot of the open tab, refreshed a few times a second.
    std::future<tab_snapshot> pending;
    tab_snapshot snap;
    bool have = false;
    tab_request asked;     // what `snap` answers
    tab_request pending_req; // what `pending` will answer
    uint64_t serial = 0, pending_serial = 0, snap_serial = 0;
    std::chrono::steady_clock::time_point last_request{};

    // Lever changes not yet seen in a snapshot (shown meanwhile).
    struct local_override {
        uint32_t agent = 0;
        uint8_t index = 0, mode = 0;
        float value = 0;
        uint64_t serial = 0; // dropped once a snapshot requested after it arrives
    };
    std::vector<local_override> local;
    int drag = -1;         // lever whose slider is being dragged
    float scroll = 0;      // lever list (rows)

    // Diplomacy.
    int subtab = 0;        // sim::agent_kind of the counterparts, 7 = organisations
    counterpart_row target;
    bool has_target = false;
    float list_scroll = 0;
    float sanction = 0.5f, aid = 0.2f, pledge = 50.0f, lobby_value = 0.0f;
    uint8_t treaty = 1;    // sim::treaty_kind (migration)
    int lobby_lever = -1;  // index into snapshot lobby_levers
    uint32_t pending_action = 0; // seq of the action command in flight (0 = none)
    std::string pending_label;   // the action in flight
    std::string result;          // the last action's outcome, shown under the options
    bool result_ok = false;
};

// The acting agent: tabs.agent if controlled, else the first controlled one (null if none).
agent_info const* acting_agent(sim_runner const& sim, tabs_state& tabs);

// Draws the window of `open` (Policies, Fiscal policy, Trade, Diplomacy, Military)
// into `win`, polling its data; results of actions go to `alerts`.
void action_tab(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st, rect win);

// The Politics tab's right half: the composition of power of `nation` (sim index).
void power_panel(ui_context& ui, sim_runner& sim, hud_state& st, int nation, bool democracy, rect r);

} // namespace geosim::client
