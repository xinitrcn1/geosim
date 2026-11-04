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

struct agent_panel {
    std::string icon;
    std::string title;
    std::vector<indicator> items;
};

// One journal decision worth an alert, copied out at the sim's sync point.
struct journal_entry {
    int32_t tick = 0;
    uint8_t code = 0;     // sim::journal_* (model.hpp)
    std::string nation;   // ISO3 of decision.target
    std::string partner;  // ISO3 of the partner (pragmatic field), if any
    float before = 0, after = 0;
};

struct journal_batch {
    int32_t scanned_to = 0; // the sim's current tick when the job ran
    std::vector<journal_entry> entries;
};

class sim_runner {
public:
    sim_runner();
    sim_runner(sim_runner const&) = delete;
    sim_runner& operator=(sim_runner const&) = delete;
    ~sim_runner();

    // Starts the sim thread, which builds the scenario first (status() == loading).
    void start(std::string assets_dir, std::string scenario, std::string nation_iso3);
    void stop();

    [[nodiscard]] sim_status status() const noexcept { return status_.load(std::memory_order_acquire); }
    [[nodiscard]] std::string const& error() const noexcept { return error_; } // valid once status() == failed

    void set_speed(int s) noexcept;
    [[nodiscard]] int speed() const noexcept { return speed_.load(std::memory_order_relaxed); }
    [[nodiscard]] int32_t tick() const noexcept { return tick_.load(std::memory_order_relaxed); }
    [[nodiscard]] core::civil_date date() const noexcept;
    [[nodiscard]] int64_t epoch_days() const noexcept { return epoch_days_; } // valid once running
    [[nodiscard]] double measured_days_per_second() const noexcept { return measured_rate_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::string const& nation_iso3() const noexcept { return iso3_; }
    [[nodiscard]] std::string const& nation_flag() const noexcept { return flag_; }

    // Journal entries with tick >= since_tick, gathered at the next sync point.
    std::future<journal_batch> request_journal(int32_t since_tick);

    // Indicator panels for the agents the player controls (racy direct reads;
    // call from the UI thread once status() == running).
    std::vector<agent_panel> read_agents();

private:
    void run();
    void post(std::function<void(game_state&)> job);

    std::string assets_, scenario_, iso3_, flag_;
    std::string error_;
    std::unique_ptr<game_state> state_;
    int64_t epoch_days_ = core::epoch_days_1970;
    uint32_t nation_ = 0;    // dcon::nation_id index
    bool has_nation_ = false;
    float unemployment0_ = 0, yield0_ = 0;

    std::jthread thread_; // joins on destruction
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::function<void(game_state&)>> jobs_; // guarded by mutex_
    std::atomic<bool> quit_{false};
    std::atomic<int> speed_{0};
    std::atomic<int32_t> tick_{0};
    std::atomic<double> measured_rate_{0.0};
    std::atomic<sim_status> status_{sim_status::loading};

    // UI-thread history for trailing-year growth / inflation.
    struct sample {
        int32_t tick;
        double acc_gdp, acc_cpi, acc_days;
    };
    std::vector<sample> history_;
};

} // namespace geosim::client
