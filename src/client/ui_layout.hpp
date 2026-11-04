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
#include <memory>
#include <utility>
#include <vector>

namespace geosim::client {

// Alert categories (top-bar filter icons; docs/08-ui.md#layout). `system`
// (local notices such as "scenario ready") is never filtered.
enum class alert_category : uint8_t { economic, political, security, diplomacy, disaster, health, society, system, count };
inline constexpr std::size_t alert_filter_count = static_cast<std::size_t>(alert_category::system);

struct alert {
    uint64_t key = 0;
    alert_category cat = alert_category::system;
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
    // New alerts since the bell was last opened (the badge).
    [[nodiscard]] std::size_t unread() const noexcept { return unread_; }
    void mark_seen() noexcept { unread_ = 0; }
    [[nodiscard]] std::vector<alert> const& items() const noexcept { return items_; }

private:
    void add(journal_entry const& e, sim_runner const& sim); // "mine" = involves any nation the player controls

    std::vector<alert> items_; // newest first
    std::unordered_set<uint64_t> seen_;
    std::future<journal_batch> pending_;
    int32_t scanned_to_ = 0;
    std::chrono::steady_clock::time_point last_request_{};
    uint64_t local_serial_ = 0;
    std::size_t unread_ = 0;
};

enum class hud_action : uint8_t { none, policy, budget, diplomacy, military, politics };
// Map views (top-bar filter buttons; docs/07-map-and-filters.md#views).
enum class map_filter : uint8_t { political, climate, economy, trade, energy, epidemic, unrest, count };

// Hover tooltip over a map region (flat style); `details` show when the
// pointer moves onto the tooltip (expanded panel).
struct map_tooltip_info {
    bool valid = false;
    std::string title;    // region
    std::string subtitle; // nation
    std::string label, value; // the view's headline indicator
    tone t = tone::neutral;
    std::vector<std::pair<std::string, std::string>> details; // label, value ("" value = section heading)
};

struct legend_item {
    rgba colour;
    std::string label;
};

// The Politics tab's electoral tile map (docs/07-map-and-filters.md#electoral-map).
// The packed inset map of one nation, rasterised at a fixed resolution
// (map_layer::build_insets): each cell holds the nation's local region index,
// or a code below. Drawn as run-length quads, scaled into the tab.
struct electoral_raster {
    static constexpr int32_t empty = -1, frame = -2, border = -3;
    int w = 0, h = 0;
    std::vector<int32_t> cell; // row-major, row 0 at the top
    struct run {
        uint16_t row = 0, x = 0, len = 0;
        int32_t value = empty;
    };
    std::vector<run> runs;     // non-empty runs
    std::vector<int> regions;  // local index -> raster region
};

struct electoral_view_info {
    bool valid = false;
    std::string nation;             // name
    bool democracy = true;
    std::vector<std::string> modes; // button labels
    int mode = 0;                   // active button
    float aspect = 1;               // layout width / height
    std::shared_ptr<electoral_raster const> raster;
    std::vector<rgba> colours;      // per local region index
    std::vector<legend_item> legend;
    std::vector<std::string> strip; // national result / regime indicators
};

// The map mode's legend box (title, swatches, a note; `summary` is not drawn).
struct map_tab_info {
    std::string title;
    std::vector<legend_item> legend;
    std::string note;                 // e.g. "Values are national, scaled by region"
    std::vector<std::string> summary; // lines
};

struct hud_state {
    map_filter filter = map_filter::political;
    hud_action open = hud_action::none; // action panel requested (panels are not implemented yet)
    float alert_scroll = 0;             // first visible alert
    int requested_speed = -1;           // set by the time controls; -1 = no change this frame
    bool alerts_hidden = false;          // the bell toggles the alert list
    uint32_t alert_filter = (1u << alert_filter_count) - 1; // bit per alert_category shown
    // Map tooltip: the region it describes, where it is anchored, its rect last
    // frame (the pointer on it keeps it and expands it).
    int tooltip_region = -1;
    float tooltip_x = 0, tooltip_y = 0;
    rect tooltip_rect{};
    bool tooltip_expanded = false;
    float tooltip_progress = 0; // 0..1 towards auto-expansion (2 s still pointer)
    // Politics tab: the nation shown (sim nation index, -1 = the player's first),
    // the electoral-map mode button, the tile under the pointer, nation stepping.
    int politics_nation = -1;
    int politics_mode = 0;
    int politics_tile = -1;
    int politics_step = 0; // -1 / +1 when the nation arrows are clicked
    bool tooltip_tile = false; // the tooltip describes an electoral tile, not the main map
};

// `tab` (may be null): the map mode's legend (bottom right; the alert column
// ends above it). `politics` (may be null): the Politics window's content. An
// open window (st.open) fills the space left of the alerts, below the first
// agent bar; while it is open only that agent bar shows.
void build_hud(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st, map_tab_info const* tab = nullptr,
               electoral_view_info const* politics = nullptr);
// The map tooltip (call after build_hud, with the region under the pointer).
void draw_map_tooltip(ui_context& ui, map_tooltip_info const& info, hud_state& st);

// "Wed 15 Jan 2020"
[[nodiscard]] std::string format_long_date(core::civil_date d);

} // namespace geosim::client
