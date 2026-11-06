#pragma once
// Data behind the player's action tabs (docs/08-ui.md#action-tabs): copied out of the
// simulation at its sync point (sim_runner::request_tab, sim_tabs.cpp) and drawn by
// action_tabs.cpp. Each request fills only the part its tab needs.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace geosim::client {

enum class tab_kind : uint8_t { policies, fiscal, trade, diplomacy, military, politics };

// One lever of the acting agent (sim/levers.hpp lever_table, or an NPI indicator).
struct lever_row {
    uint8_t index = 0;           // the agent's lever slot (NPIs first, then lever_base + sim::lever)
    std::string name, unit;
    float lo = 0, hi = 1, step = 0.05f;
    bool bill = false;           // passes the legislature where it is competitive
    bool npi = false;
    float current = 0;           // agent.last_choice
    float ai_choice = 0;         // agent.last_ai_choice (what the AI alone would do)
    float value = 0;             // agent.override_value
    uint8_t mode = 0;            // agent.override_mode (sim::override_mode)
};

// Budget of a nation over the run so far (% of nominal GDP over the same days).
struct fiscal_summary {
    bool valid = false;
    float days = 0;
    float revenue = 0;   // taxes (households, indirect, firms, resources, tariffs, carbon, wealth)
    float spending = 0;  // revenue minus net lending
    float balance = 0;   // net lending: change in the government balance
    float debt = 0;      // gross debt now, % of annual GDP
    float debt0 = 0;     // at the start
    float yield10 = 0, spread = 0;
    float tax_scale = 1; // household tax rate, x start
};

struct party_row {
    std::string key;
    uint8_t family = 0;
    float vote = 0, seat = 0;
    bool in_government = false, incumbent = false;
    float position[3] = {}; // sim::value_axis: secular, self-expression, economic right
};

// A power figure: an agent of the nation with its indicators.
struct figure_row {
    std::string icon, title;
    std::vector<std::pair<std::string, std::string>> items; // label, value
    std::string tip;
};

// Value-axis means (sim::value_axis) of a slice of the population.
struct leaning_row {
    std::string label;
    float share = 0;        // of the nation's population
    float values[3] = {};
};

struct power_snapshot {
    bool valid = false;
    uint32_t nation = 0xFFFFFFFFu;
    std::string iso3, name;
    bool democracy = true;
    uint8_t electoral_system = 0;
    float electoral = 0, liberal = 0, legitimacy = 0, approval = 0, capture = 0, winning_coalition = 0, gov_seat_share = 0;
    int chamber = 0;                    // seats in the lower chamber (parliament.hpp chamber_size)
    std::vector<party_row> parties;     // by seat share, largest first
    std::vector<figure_row> figures;    // autocracies (and always the main agents)
    std::vector<figure_row> board;      // the acting board and its firm
    std::vector<leaning_row> leanings;  // the elite strata vs everyone
};

struct trade_slice {
    std::string label;
    float value = 0;     // model units (share of the total when drawn)
    uint32_t nation = 0xFFFFFFFFu;
};

struct trade_snapshot {
    bool valid = false;
    std::vector<trade_slice> exports, imports, sectors; // top 8 + "Other"
    float exports_pct = 0, imports_pct = 0, exports0_pct = 0, imports0_pct = 0;
    float tariff = 0, tariff0 = 0;
    std::vector<std::string> sanctions_by;   // nations this one sanctions (name level)
    std::vector<std::string> sanctions_on;   // nations sanctioning it
};

// A diplomatic counterpart (Diplomacy subtabs).
struct counterpart_row {
    uint32_t nation = 0xFFFFFFFFu;
    uint32_t agent = 0xFFFFFFFFu;
    uint32_t organisation = 0xFFFFFFFFu;
    std::string label, detail;
};

// The value an action takes (Diplomacy tab inputs).
// lobby: a lever of the pressed agent and a value (snapshot lobby_levers); amount: a value
// in [lo, hi] described by the sim (sim::action_option).
enum class action_input : uint8_t { none, sanction_level, aid_share, pledge, treaty, lobby, amount };

struct action_row {
    uint8_t kind = 0; // sim::action_kind
    action_input input = action_input::none;
    std::string label;
    bool available = false;
    std::string reason;
    std::string unit;                       // amount inputs
    float lo = 0, hi = 0, step = 0, initial = 0;
};

struct diplomacy_snapshot {
    bool valid = false;
    std::vector<counterpart_row> counterparts;
    std::vector<action_row> actions; // toward the selected counterpart
    std::vector<lever_row> lobby_levers; // the levers a lobby / press acts on: the pressed agent's (or the government's)
};

struct military_snapshot {
    bool valid = false;
    float loyalty = 0, defence = 0, defence0 = 0, power = 0, cinc = 0, nuclear = 0, war_effort = 0, threat = 0;
    std::vector<std::string> wars, disputes, allies;
};

struct tab_snapshot {
    int32_t tick = 0;
    uint32_t agent = 0xFFFFFFFFu;
    uint8_t kind = 0;               // sim::agent_kind of the acting agent
    uint32_t nation = 0xFFFFFFFFu;  // its nation
    std::string nation_name;
    std::vector<lever_row> levers;
    fiscal_summary fiscal;
    trade_snapshot trade;
    power_snapshot power;
    diplomacy_snapshot diplomacy;
    military_snapshot military;
};

// The in-game internet (sim/web.hpp; the Internet window): one rendered page, copied at
// the sync point, with the browser chrome's data.
struct web_block {
    uint8_t kind = 0; // sim::web::block::kind
    std::string text, sub, url;
    int8_t tone = 0;
    std::string author, flag; // posts: the poster and its nation's flag (flag_set token)
    std::string slang, gloss; // posts: a slang term (shown highlighted) and its meaning (tooltip)
    int replies = 0;          // posts: replies packed under it ("View replies (N)")
    bool reply = false;       // posts: a reply to the last post without `reply`
};
struct web_bookmark {
    std::string name, domain;
};
struct web_view {
    std::string url, title, host, category, owner;
    uint32_t accent = 0x3366CC;
    uint8_t style = 0; // sim::web::page::layout
    int ads_blocked = 0;
    std::vector<web_block> blocks;
    std::vector<web_bookmark> bookmarks; // the top 5 sites worldwide by active users
    std::string engine, engine_name;     // the default search engine (domain, name)
};

// What a tab asks of the sim.
struct tab_request {
    tab_kind tab = tab_kind::policies;
    uint32_t agent = 0xFFFFFFFFu;   // the acting agent (controlled)
    uint32_t home = 0xFFFFFFFFu;    // its nation
    uint32_t nation = 0xFFFFFFFFu;  // politics: the nation shown (else the agent's)
    int subtab = 0;                 // diplomacy: counterpart kind (sim::agent_kind) or 7 = organisations
    counterpart_row target;         // diplomacy: the selected counterpart
};

// A player action, performed on the sim thread (sim::perform_action).
struct action_order {
    uint8_t kind = 0;
    uint32_t agent = 0xFFFFFFFFu;
    uint32_t nation = 0xFFFFFFFFu;
    uint32_t organisation = 0xFFFFFFFFu;
    uint32_t target_agent = 0xFFFFFFFFu; // actions toward another agent
    uint32_t region = 0xFFFFFFFFu;       // redraw_state: the cluster (sim region index)
    uint32_t state_to = 0xFFFFFFFFu;     // redraw_state: its new state (sim state index; none: a new state)
    uint8_t treaty = 0xFF;
    uint8_t lever = 0;
    float value = 0;
};

// Redrawing state borders from the map tooltip (sim_runner::request_redraw).
struct redraw_option {
    uint32_t state = 0xFFFFFFFFu; // sim state index; none: a new state
    std::string label;
    bool ok = false;
    std::string reason;
};
struct redraw_view {
    uint32_t region = 0xFFFFFFFFu;
    std::string state; // the region's state now
    std::vector<redraw_option> options;
};

struct action_outcome {
    bool accepted = false;
    std::string message;
};

} // namespace geosim::client
