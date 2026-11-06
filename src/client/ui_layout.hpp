#pragma once
// The player HUD (docs/08-ui.md#layout), rebuilt every frame:
//   top bar (opaque, top-left): flag + nation, date, time controls, action
//     buttons, map filter buttons;
//   one translucent bar per agent the player controls, with indicators
//     coloured green / red when good / bad for the player;
//   the alert feed (translucent, dismissable), fed by the decision journal.

#include "action_tabs.hpp"
#include "client_files.hpp"
#include "sim_runner.hpp"
#include "ui_context.hpp"

#include <chrono>
#include <future>
#include <string>
#include <unordered_set>
#include <array>
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
    // Requests to the player's agents waiting for an answer (pinned on top, yellow),
    // soonest to expire first; refreshed with the journal.
    [[nodiscard]] std::vector<pending_request> const& requests() const noexcept { return requests_; }
    // Answers one on the agent's behalf; the outcome arrives as a local alert.
    void answer(sim_runner& sim, pending_request const& r, bool accept);

private:
    void add(journal_entry const& e, sim_runner const& sim); // "mine" = involves any nation the player controls

    std::vector<alert> items_; // newest first
    std::unordered_set<uint64_t> seen_;
    std::future<journal_batch> pending_;
    int32_t scanned_to_ = 0;
    std::chrono::steady_clock::time_point last_request_{};
    uint64_t local_serial_ = 0;
    std::size_t unread_ = 0;
    std::vector<pending_request> requests_;
    std::future<std::vector<pending_request>> pending_requests_;
    std::chrono::steady_clock::time_point last_requests_{};
    struct answering {
        uint32_t seq;
        std::string title;
    };
    std::vector<answering> answering_;     // answers sent, results not back yet
    std::unordered_set<uint64_t> answered_; // (request index, created tick) answered here: hidden until the sim drops it
};

// Top-bar windows, in button order (control = the '+' picker).
enum class hud_action : uint8_t { none, policy, budget, trade, diplomacy, military, politics, internet, control };

// The Internet window (browser.cpp): simple tabs, each with its history; one text field
// focused at a time (the address bar or the search box).
struct browser_tab {
    std::vector<std::string> history{"about:home"};
    std::size_t at = 0;
    float scroll = 0;
    std::string title = "Start Page";
};
struct browser_state {
    std::vector<browser_tab> tabs{browser_tab{}};
    std::size_t active = 0;
    uint8_t focus = 0;          // 0 none, 1 address bar, 2 search box
    std::string input;          // the focused field's text
    bool submit = false;        // Enter pressed this frame
    std::string search_engine;  // the search box's engine (empty: the default)
    web_view view;              // the active tab's page (when view.url == the tab's url)
    bool have = false;
    std::future<web_view> pending;
    std::string pending_url;
    std::chrono::steady_clock::time_point last{};
    std::vector<web_bookmark> bookmarks;
    std::string engine, engine_name; // the default engine
    std::vector<std::string> open_threads; // posts whose replies are shown (author + text)
};
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
struct electoral_view_info {
    bool valid = false;
    std::string nation;             // name
    std::string state;              // the state zoomed into ("" = the whole nation)
    bool democracy = true;
    std::vector<std::string> modes; // button labels
    int mode = 0;                   // active button
    float aspect = 1;               // layout width / height
    std::vector<rgba> colours;      // per local region index (the GPU draws the regions: map_renderer::set_electoral)
    std::vector<std::array<float, 4>> frames; // inset frames in the map rect, 0..1 (x0, y0, x1, y1; y down)
    int hovered = -1;               // raster region under the pointer (GPU readback), -1 none
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
    // Redrawing state borders: options for the expanded tooltip's region when the
    // player's government owns it (filled by the app), and the option clicked (-1 none).
    redraw_view redraw;
    int redraw_pick = -1;
    // Politics tab: the nation shown (sim nation index, -1 = the player's first),
    // the electoral-map mode button, the tile under the pointer, nation stepping.
    int politics_nation = -1;
    int politics_mode = 0;
    int politics_tile = -1;
    int politics_step = 0; // -1 / +1 when the nation arrows are clicked
    int politics_state = -1;        // the state the electoral map is zoomed into (sim index), -1 = the nation
    bool politics_click = false;    // a county was clicked in the electoral map this frame (zoom to its state)
    bool politics_back = false;     // the "whole nation" link was clicked
    bool tooltip_tile = false; // the tooltip describes an electoral tile, not the main map
    // Agent bars and the Control window ('+').
    int64_t toggle_ai = -1;      // agent whose AI switch was clicked this frame
    int64_t toggle_answer = -1;  // agent whose "answer requests" switch was clicked
    int64_t remove_agent = -1;   // agent whose ✖ was clicked
    int add_agent = -1;          // index into the Control window's list
    int control_kind = 0;        // sim::agent_kind shown
    float control_scroll = 0;
    std::string search;          // typed into the Control window
    tabs_state tabs;             // the action tabs: acting agent, data, diplomacy picks
    browser_state browser;       // the Internet window
};

// A window (Politics, Policies, Diplomacy...): flat white with a blue title strip and a
// close button, filling `win`. Returns the body rect.
rect window_frame(ui_context& ui, hud_state& st, rect win, std::string_view title, std::string_view subtitle);
// The Internet window (browser.cpp).
void browser_window(ui_context& ui, sim_runner& sim, hud_state& st, rect win);

// `tab` (may be null): the map mode's legend (bottom right; the alert column
// ends above it). `politics` (may be null): the Politics window's content. An
// open window (st.open) fills the space left of the alerts, below the first
// agent bar; while it is open only that agent bar shows.
// `agents`: the controllable agents for the Control window (may be null while loading).
void build_hud(ui_context& ui, sim_runner& sim, alert_feed& alerts, hud_state& st, map_tab_info const* tab = nullptr,
               electoral_view_info const* politics = nullptr, std::vector<agent_info> const* agents = nullptr);

// --- main menu (docs/08-ui.md#main-menu) ---------------------------------------

struct menu_state {
    enum class window : uint8_t { none, load, multiplayer, settings } open = window::none;
    float scroll = 0;
};

struct menu_choice {
    enum class kind : uint8_t { none, new_session, load, quit, settings_changed } what = kind::none;
    std::string path; // load
};

// The menu over the panning world: the title and a column of flat blue buttons
// on the left, and the Load / Multiplayer / Settings windows beside it.
menu_choice build_menu(ui_context& ui, menu_state& st, client_settings& settings, std::vector<save_entry> const& saves);

// A centred flat box with a message (scenario loading).
void draw_notice(ui_context& ui, std::string_view title, std::string_view text);
// The map tooltip (call after build_hud, with the region under the pointer).
void draw_map_tooltip(ui_context& ui, map_tooltip_info const& info, hud_state& st);

// "Wed 15 Jan 2020"
[[nodiscard]] std::string format_long_date(core::civil_date d);

} // namespace geosim::client
