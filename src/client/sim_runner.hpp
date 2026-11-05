#pragma once
// The simulation behind the UI (docs/08-ui.md#simulation-data).
//
// The sim runs on its own thread and never waits for the UI:
//  - Scalars (nation floats, the tick) are read by the UI thread directly from
//    the DCON SoA while the sim writes them. They are 32-bit aligned values,
//    so a read sees either the old or the new value; slightly stale or
//    mixed-day data is accepted. Entity ids are bounded by the SoA size, so a
//    stale id reads stale data, never out of bounds.
//  - Lists (anything whose storage can move, e.g. collecting journal entries
//    into a std::vector) are requested with a future: the job runs on the sim
//    thread at its sync point between two days and fulfils the promise.
// Speeds: 0 = pause, 1..4 = 1, 2, 4, 8 days per second, 5 = as fast as the
// simulation can run.

#include "core/calendar.hpp"
#include "map_hazards.hpp"
#include "tab_data.hpp"
#include "ui_context.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace geosim {
struct game_state;
}
namespace geosim::sim {
struct command;
}

namespace geosim::client {

inline constexpr int max_speed = 5;
// Days per second for speeds 0..4; speed 5 is unthrottled.
inline constexpr double days_per_second[max_speed] = {0.0, 1.0, 2.0, 4.0, 8.0};

enum class sim_status : uint8_t { loading, running, failed };

struct indicator {
    std::string label;
    std::string value;
    tone t = tone::neutral;
    std::string tip; // definition, shown on hover
};

// An agent the player can control (sim `agent`), as listed by request_agents.
struct agent_info {
    uint32_t agent = 0;              // dcon::agent_id index
    uint8_t kind = 0;                // sim::agent_kind
    uint32_t nation = 0xFFFFFFFFu;   // its nation (the first one for a shared central bank)
    uint32_t region = 0xFFFFFFFFu;   // governors
    uint32_t firm = 0xFFFFFFFFu;     // boards
    std::string iso3;                // of `nation`
    std::string label;               // "United States Government", "Texas Governor", ...
};

struct agent_panel {
    uint32_t agent = 0;  // the controlled agent (AI toggle, removal)
    bool ai = true;      // its AI keeps deciding (the player advises / forces)
    bool answer = true;  // requests to it wait for the player (else its AI answers at once)
    std::string flag;  // the agent's nation (emoji flag): several nations may be controlled
    std::string icon;
    std::string title; // "United States Government", "United States Central Bank"
    std::vector<indicator> items;
};

// One journal decision worth an alert, copied out at the sim's sync point.
struct journal_entry {
    int32_t tick = 0;
    uint8_t code = 0;     // sim::journal_* (model.hpp)
    std::string nation;   // ISO3 of decision.target
    std::string partner;  // ISO3 of the partner (pragmatic field), if any
    float before = 0, after = 0;
    float extra = 0;      // decision.prior (journal_request: who answered)
};

// A request to one of the player's agents, waiting for an answer (sim::request_kind;
// requests.cpp), copied at the sync point.
struct pending_request {
    uint32_t request = 0;  // dcon::request_id index
    uint32_t to = 0;       // the asked agent (one of the player's)
    uint8_t kind = 0;      // sim::request_kind
    std::string title, detail;
    int32_t created = 0, expires = 0;
    bool ai_accepts = false; // what the agent's AI answers when the window closes
};

// The regions of the scenario for the map (map_layer), copied at a sync point.
struct sim_map_world {
    std::vector<std::string> nation_iso3; // by nation index
    std::vector<float> lat, lon;          // by region index, degrees
    std::vector<uint16_t> owner;          // by region index: owning nation index (0xFFFF = none)
};

struct journal_batch {
    int32_t scanned_to = 0; // the sim's current tick when the job ran
    std::vector<journal_entry> entries;
};

// Every controllable agent with its nation and a label (sim thread; sim_runner.cpp).
// Names by nation / region index (empty strings fall back to codes).
std::vector<agent_info> collect_agents(game_state const& s, std::vector<std::string> const& nation_names,
                                       std::vector<std::string> const& region_names);

class sim_runner {
public:
    sim_runner();
    sim_runner(sim_runner const&) = delete;
    sim_runner& operator=(sim_runner const&) = delete;
    ~sim_runner();

    // Starts the sim thread, which builds the scenario first (status() == loading),
    // or loads `save_path` when given (a sim::save_game file of that scenario).
    // `nations` lists nations as ISO3 codes, comma-separated ("USA,CAN"): their
    // executive and central bank start controlled ("" = none).
    void start(std::string assets_dir, std::string scenario, std::string nations, std::string save_path = {});
    [[nodiscard]] bool started() const noexcept { return thread_.joinable(); }
    [[nodiscard]] std::string const& scenario() const noexcept { return scenario_; }

    // Controllable agents (all kinds), at the next sync point.
    std::future<std::vector<agent_info>> request_agents();
    // The player's agents (UI thread). Adding one gives it an agent bar; its AI
    // stays on (the player advises or forces) until set_ai(agent, false), which
    // (a sim::command_tag::set_ai command, applied by the sim before its next tick)
    // directs every lever at its current value (override_mode::direct) through a
    // job at the sync point; set_ai(agent, true) clears the overrides.
    void add_control(agent_info const& a);
    void remove_control(uint32_t agent);
    void set_ai(uint32_t agent, bool on);
    // Whether requests to the agent wait for the player's answer (default on); off,
    // its AI answers them at once (no lag).
    void set_answer_requests(uint32_t agent, bool on);
    [[nodiscard]] bool answers_requests(uint32_t agent) const noexcept {
        for(std::size_t i = 0; i < agents_.size(); ++i) {
            if(agents_[i].agent == agent) return answer_[i] != 0;
        }
        return false;
    }
    // Requests waiting for the player, at the next sync point.
    std::future<std::vector<pending_request>> request_pending();
    // Answers a request on the agent's behalf (a sim::command_tag::answer). Returns its seq.
    uint32_t answer(uint32_t request, uint32_t agent, bool accept);
    [[nodiscard]] bool controls_agent(uint32_t agent) const noexcept;
    [[nodiscard]] bool ai_on(uint32_t agent) const noexcept {
        for(std::size_t i = 0; i < agents_.size(); ++i) {
            if(agents_[i].agent == agent) return ai_[i] != 0;
        }
        return true;
    }
    [[nodiscard]] std::vector<agent_info> const& controlled_agents() const noexcept { return agents_; }
    // Writes a save at the next sync point; the future holds "" or the error.
    std::future<std::string> request_save(std::string path);
    void stop();

    [[nodiscard]] sim_status status() const noexcept { return status_.load(std::memory_order_acquire); }
    [[nodiscard]] std::string const& error() const noexcept { return error_; } // valid once status() == failed

    void set_speed(int s) noexcept;
    [[nodiscard]] int speed() const noexcept { return speed_.load(std::memory_order_relaxed); }
    [[nodiscard]] int32_t tick() const noexcept { return tick_.load(std::memory_order_relaxed); }
    [[nodiscard]] core::civil_date date() const noexcept;
    [[nodiscard]] int64_t epoch_days() const noexcept { return epoch_days_; } // valid once running
    [[nodiscard]] double measured_days_per_second() const noexcept { return measured_rate_.load(std::memory_order_relaxed); }

    // The nations of the player's agents (ISO3), in the order they were added.
    [[nodiscard]] std::vector<std::string> const& nations() const noexcept { return iso3s_; }
    // True when the player controls any agent of that nation.
    [[nodiscard]] bool controls(std::string const& iso3) const noexcept;
    // True while the nation of any agent the player controls is at war (the sim's
    // nation.at_war array; racy direct read like the indicators).
    [[nodiscard]] bool player_at_war() const noexcept;

    // Journal entries with tick >= since_tick, gathered at the next sync point.
    std::future<journal_batch> request_journal(int32_t since_tick);

    // Nations and regions for the map (at the next sync point).
    std::future<sim_map_world> request_map_world();
    // Current owner (nation index, 0xFFFF = none) of each region; racy direct
    // reads like the indicators. `out` keeps its size (from request_map_world).
    void read_region_owners(std::vector<uint16_t>& out) const noexcept;

    // Live cyclones, earthquakes of the last week and every disaster record, with
    // their notification text (sim::describe_disaster), at the next sync point.
    // Implemented in sim_hazards.cpp.
    std::future<hazard_snapshot> request_hazards();
    // Per-region and per-nation state for the map views (sim_hazards.cpp);
    // faults (static) only when asked.
    std::future<region_snapshot> request_regions(bool with_faults);
    // One nation's parties and per-region vote shares (pop-level support x size
    // x turnout) or regime-support inputs, for the Politics tab (sim_hazards.cpp).
    std::future<politics_snapshot> request_politics(uint32_t nation_index);

    // Action tabs (sim_tabs.cpp; docs/08-ui.md#action-tabs): the acting agent's levers
    // and the tab's data, gathered at the next sync point (read-only).
    std::future<tab_snapshot> request_tab(tab_request r);

    // Player -> sim writes are commands (sim/commands.hpp): the UI thread pushes them into
    // an SPSC queue, stamped with the sim's current tick and an increasing seq; the sim
    // thread drains the due ones before each tick (and while paused) and answers through
    // a result queue. A full queue keeps the command in a backlog retried by pump().
    // The player's override on one lever (mode: sim::override_mode). Returns its seq.
    uint32_t set_override(uint32_t agent, uint8_t lever, uint8_t mode, float value);
    // A diplomatic / political move (sim::perform_action). Returns its seq.
    uint32_t perform(action_order o);
    // Call once per frame (UI thread): retries the backlog, collects results.
    void pump();
    // The result of command `seq`, once the sim has applied it (taken: true once).
    bool take_result(uint32_t seq, action_outcome& out);

    // Indicator panels for the agents the player controls (racy direct reads;
    // call from the UI thread once status() == running).
    std::vector<agent_panel> read_agents();

private:
    void run();
    void post(std::function<void(game_state&)> job);
    uint32_t push_command(sim::command c); // stamps tick and seq; UI thread
    struct command_io;                      // the queues (sim_runner.cpp)
    std::unique_ptr<command_io> io_;
    uint32_t seq_ = 0;

    // UI-thread history for trailing-year growth / inflation.
    struct sample {
        int32_t tick;
        double acc_gdp, acc_cpi, acc_days;
    };
    // Indicator history of a nation with a controlled agent (UI thread; the
    // --nation ones are set up on the sim thread before status() is running).
    struct controlled {
        uint32_t index = 0; // dcon::nation_id index
        std::string iso3, flag, name;
        float unemployment0 = 0, yield0 = 0;
        std::vector<sample> history;
    };
    void read_agent(agent_info const& a, bool ai, controlled& c, std::vector<agent_panel>& out);
    controlled& track(uint32_t nation);
    void refresh_nations();

    std::string assets_, scenario_;
    std::vector<std::string> iso3s_;
    std::string error_;
    std::unique_ptr<game_state> state_;
    int64_t epoch_days_ = core::epoch_days_1970;
    std::vector<controlled> controlled_; // per nation with a controlled agent
    std::vector<agent_info> agents_;      // the player's agents (see add_control)
    std::vector<uint8_t> ai_;             // per agents_ entry: AI on
    std::vector<uint8_t> answer_;         // per agents_ entry: requests wait for the player
    std::string save_path_;
    // Display names, set on the sim thread before status() becomes running, then
    // read-only: nations.csv names (cut at a comma) by nation index, and the
    // scenario's regions.csv names by region index (empty without regions.csv).
    std::vector<std::string> nation_names_;
    std::vector<std::string> region_names_;

public:
    // "Louisiana, United States" / "Japan" (valid once running; ids out of range -> "").
    [[nodiscard]] std::string place_name(uint32_t nation_index, uint32_t region_index) const;
    [[nodiscard]] std::string nation_name(uint32_t nation_index) const {
        return nation_index < nation_names_.size() ? nation_names_[nation_index] : std::string{};
    }

    std::jthread thread_; // joins on destruction
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::function<void(game_state&)>> jobs_; // guarded by mutex_
    std::atomic<bool> quit_{false};
    std::atomic<int> speed_{0};
    std::atomic<int32_t> tick_{0};
    std::atomic<double> measured_rate_{0.0};
    std::atomic<sim_status> status_{sim_status::loading};
};

} // namespace geosim::client
