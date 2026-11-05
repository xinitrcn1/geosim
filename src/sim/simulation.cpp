#include "sim/simulation.hpp"

#include "core/scheduler.hpp"
#include "sim/systems.hpp"

#include <oneapi/tbb/global_control.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdio>
#include <vector>

namespace geosim::sim {

namespace {
// Per-system wall time (headless --profile 1): off unless enabled, so ticks pay one branch.
struct system_timer {
    char const* name;
    double seconds;
    long calls;
};
bool profiling = false;
std::vector<system_timer>& timers() {
    static std::vector<system_timer> t;
    return t;
}
template<typename F>
void timed(std::size_t slot, char const* name, F&& f) {
    if(!profiling) {
        f();
        return;
    }
    auto& t = timers();
    if(t.size() <= slot) t.resize(slot + 1, {nullptr, 0.0, 0});
    const auto start = std::chrono::steady_clock::now();
    f();
    t[slot].name = name;
    t[slot].seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ++t[slot].calls;
}
} // namespace

#define GEOSIM_TIMED(fn) timed(__LINE__, #fn, [&] { fn(state); })

void set_profiling(bool on) { profiling = on; }

void set_thread_limit(int threads) {
    static std::unique_ptr<oneapi::tbb::global_control> limit;
    limit.reset(); // one control at a time: the new limit replaces the old
    limit = std::make_unique<oneapi::tbb::global_control>(oneapi::tbb::global_control::max_allowed_parallelism,
                                                          static_cast<std::size_t>(std::max(1, threads)));
}

void write_profile(std::FILE* out) {
    auto t = timers();
    t.erase(std::remove_if(t.begin(), t.end(), [](auto const& x) { return x.name == nullptr; }), t.end());
    double total = 0.0;
    for(auto const& x : t) total += x.seconds;
    std::sort(t.begin(), t.end(), [](auto const& a, auto const& b) { return a.seconds > b.seconds; });
    std::fprintf(out, "system time (total %.2f s)\n", total);
    for(auto const& x : t) {
        if(x.seconds < total * 0.002) break;
        std::fprintf(out, "  %-28s %8.3f s %5.1f%%  %8.3f ms/call\n", x.name, x.seconds, total > 0 ? 100.0 * x.seconds / total : 0.0, 1e3 * x.seconds / static_cast<double>(std::max(1L, x.calls)));
    }
}

void advance_day(game_state& state) {
    GEOSIM_TIMED(apply_events);
    GEOSIM_TIMED(update_ai);
    GEOSIM_TIMED(update_epidemic);
    GEOSIM_TIMED(update_vaccination);
    GEOSIM_TIMED(update_logistics);
    GEOSIM_TIMED(update_commodity_markets);
    GEOSIM_TIMED(update_economy);
    GEOSIM_TIMED(update_firms);
    GEOSIM_TIMED(update_oil_market);
    GEOSIM_TIMED(update_demography);
    GEOSIM_TIMED(update_migration);
    GEOSIM_TIMED(update_groups);
    GEOSIM_TIMED(update_internet);
    GEOSIM_TIMED(update_opinion);
    GEOSIM_TIMED(update_social_capital);
    GEOSIM_TIMED(update_unrest);
    GEOSIM_TIMED(update_protest);
    GEOSIM_TIMED(update_politics);
    GEOSIM_TIMED(update_institutions);
    GEOSIM_TIMED(update_geopolitics);
    GEOSIM_TIMED(update_finance);
    GEOSIM_TIMED(update_credit);
    GEOSIM_TIMED(update_bonds);
    GEOSIM_TIMED(update_labour);
    GEOSIM_TIMED(update_fiscal);
    GEOSIM_TIMED(update_assets);
    GEOSIM_TIMED(update_treaties);
    GEOSIM_TIMED(update_longrun);
    GEOSIM_TIMED(update_technology);
    GEOSIM_TIMED(update_climate_policy);
    GEOSIM_TIMED(update_policy);
    GEOSIM_TIMED(update_strategy);
    GEOSIM_TIMED(update_diplomacy);
    GEOSIM_TIMED(update_organisations);
    GEOSIM_TIMED(update_geo_organisations);
    GEOSIM_TIMED(update_mega_events);
    GEOSIM_TIMED(update_disasters);
    GEOSIM_TIMED(update_reforms);
    GEOSIM_TIMED(update_politics_ext);
    GEOSIM_TIMED(update_agreements);
    GEOSIM_TIMED(update_variants);
    GEOSIM_TIMED(update_strata);
    GEOSIM_TIMED(update_indicators);
    ++state.current_tick;
}

} // namespace geosim::sim
