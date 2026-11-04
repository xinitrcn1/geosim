#pragma once
// The player HUD (docs/08-ui.md#layout), rebuilt every frame:
//   top bar (opaque, top-left): flag + nation, date, time controls, action
//     buttons, map filter buttons;
//   one translucent bar per agent the player controls, with indicators
//     coloured green / red when good / bad for the player;
//   the alert feed (translucent, dismissable), fed by the decision journal.

#include "sim_runner.hpp"
#include "ui_context.hpp"

#include <chrono>
#include <future>
#include <string>
#include <unordered_set>
#include <vector>

namespace geosim::client {

struct alert {
    uint64_t key = 0;
    int32_t tick = 0;
    std::string icon;
    std::string title;
    std::string detail;
    std::string impact; // disasters: casualties and damage (a third line on the card)
    tone t = tone::neutral;
};

// Polls the sim's journal through futures (never blocks the sim or the UI).
class alert_feed {
public:
    void poll(sim_runner& sim);
    void push_local(std::string icon, std::string title, std::string detail, tone t);
    // Adds an alert, or updates it in place when its key is already listed
    // (a disaster whose toll grows); a dismissed key is not shown again.
    void upsert(alert a);
    void dismiss(uint64_t key);
    void dismiss_all() { items_.clear(); }
    [[nodiscard]] std::vector<alert> const& items() const noexcept { return items_; }

private:
    void add(journal_entry const& e, sim_runner const& sim); // "mine" = involves any nation the player controls

    std::vector<alert> items_; // newest first
    std::unordered_set<uint64_t> seen_;
    std::future<journal_batch> pending_;
    int32_t scanned_to_ = 0;
    std::chrono::steady_clock::time_point last_request_{};
    uint64_t local_serial_ = 0;
};

enum class hud_action : uint8_t { none, policy, budget, diplomacy, military, politics };
enum class map_filter : uint8_t { political, economy, epidemic, energy, unrest, count };

struct hud_state {
    map_filter filter = map_filter::political;
    hud_action open = hud_action::none; // action panel requested (panels are not implemented yet)
    float alert_scroll = 0;             // first visible alert
    int requested_speed = -1;           // set by the time controls; -1 = no change this frame
};

void build_hud(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st);

// "Wed 15 Jan 2020"
[[nodiscard]] std::string format_long_date(core::civil_date d);

} // namespace geosim::client
