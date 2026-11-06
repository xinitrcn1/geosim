// States and their clusters: a copy of the 2020 slice where the United States is four
// regions (clusters) in two states (states.csv). Checks the build (states, a governor per
// state shared by its clusters), the redrawing procedure by regime (decree, majority,
// supermajority and the courts), the move itself (a new state included) and that elections
// still run on the redrawn states.

#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/systems.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;
static int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

using namespace geosim;

static std::string make_assets() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "geosim_states_assets";
    fs::remove_all(root, ec);
    fs::copy(GEOSIM_ASSETS_DIR, root, fs::copy_options::recursive, ec);
    if(ec) {
        std::fprintf(stderr, "cannot copy assets: %s\n", ec.message().c_str());
        std::exit(EXIT_FAILURE);
    }
    const fs::path scen = root / "scenario" / "2020";
    std::ofstream st(scen / "states.csv");
    st << "state,iso3,name,code,lat,lon\nUSA-P,USA,Pacific,US-P,38,-110\nUSA-A,USA,Atlantic,US-A,36,-80\n";
    st.close();
    std::ofstream r(scen / "regions.csv");
    r << "region,iso3,state,name,lat,lon,area_km2,pop_share,urban_share\n"
         "USA-W,USA,USA-P,West,37,-120,1.9e6,0.24,0.9\n"
         "USA-C,USA,USA-P,Centre,40,-95,3.0e6,0.21,0.7\n"
         "USA-S,USA,USA-A,South,32,-88,2.3e6,0.38,0.75\n"
         "USA-E,USA,USA-A,Northeast,42,-74,0.5e6,0.17,0.9\n";
    r.close();
    std::ofstream a(scen / "adjacency.csv");
    a << "region_a,region_b,weight\nUSA-W,USA-C,10\nUSA-C,USA-S,12\nUSA-C,USA-E,6\nUSA-S,USA-E,8\n";
    return root.string();
}

static dcon::region_id region_named(game_state const& s, float lon) {
    for(auto r : s.world.in_region) {
        if(std::fabs(r.get_lon() - lon) < 0.01f) return r.id;
    }
    return {};
}

static int members(game_state const& s, dcon::state_id st) {
    int n = 0;
    for(auto r : s.world.in_region) n += r.get_state() == st;
    return n;
}

int main() {
    const std::string assets = make_assets();
    auto s = std::make_unique<game_state>();
    const auto report = sim::build_scenario(*s, assets, "2020");
    if(!report.error.empty()) {
        std::fprintf(stderr, "build failed: %s\n", report.error.c_str());
        return 1;
    }
    auto& w = s->world;
    const auto usa = sim::find_nation(*s, "USA");
    const auto west = region_named(*s, -120), centre = region_named(*s, -95), south = region_named(*s, -88), east = region_named(*s, -74);
    CHECK(west && centre && south && east);
    const auto pacific = w.region_get_state(west), atlantic = w.region_get_state(south);
    CHECK(pacific && atlantic && pacific != atlantic);
    CHECK(w.region_get_state(centre) == pacific && w.region_get_state(east) == atlantic);
    CHECK(w.state_get_nation(pacific) == usa);
    // One governor per state, shared by its clusters.
    CHECK(w.state_get_governor(pacific) && w.state_get_governor(atlantic));
    CHECK(w.region_get_governor(west) == w.state_get_governor(pacific) && w.region_get_governor(centre) == w.state_get_governor(pacific));
    // Every other nation's single region is its own state.
    int states = 0;
    for(auto st : w.in_state) {
        (void)st;
        ++states;
    }
    CHECK(states == 2 + static_cast<int>(w.nation_size()) - 1);

    const auto exec = w.nation_get_agent_from_executive_seat(usa);
    CHECK(exec);
    // Targets: Centre borders the Atlantic state; West only its own.
    CHECK(sim::redraw_targets(*s, centre).size() == 1 && sim::redraw_targets(*s, centre)[0] == atlantic);
    CHECK(sim::redraw_targets(*s, west).empty());
    CHECK(!sim::check_redraw(*s, exec, west, atlantic).ok); // not adjacent

    // A liberal democracy without a supermajority cannot.
    w.nation_set_liberal(usa, 0.85f);
    w.nation_set_judicial(usa, 0.8f);
    w.nation_set_gov_seat_share(usa, 0.52f);
    CHECK(!sim::check_redraw(*s, exec, centre, atlantic).ok);
    // With one, the courts still stop a large transfer (Centre is ~47% of Pacific).
    w.nation_set_gov_seat_share(usa, 0.9f);
    w.nation_set_decentralisation(usa, 0.0f);
    CHECK(!sim::check_redraw(*s, exec, centre, atlantic).ok);
    // A hybrid regime needs a majority.
    w.nation_set_liberal(usa, 0.45f);
    w.nation_set_gov_seat_share(usa, 0.4f);
    CHECK(!sim::check_redraw(*s, exec, centre, atlantic).ok);
    w.nation_set_gov_seat_share(usa, 0.6f);
    CHECK(sim::check_redraw(*s, exec, centre, atlantic).ok);
    // An autocracy redraws by decree, and the move happens.
    w.nation_set_liberal(usa, 0.1f);
    sim::action_request q;
    q.kind = sim::action_kind::redraw_state;
    q.actor = exec;
    q.region = centre;
    q.state_to = atlantic;
    CHECK(sim::perform_action(*s, q).accepted);
    CHECK(w.region_get_state(centre) == atlantic);
    CHECK(w.region_get_governor(centre) == w.state_get_governor(atlantic));
    CHECK(members(*s, pacific) == 1 && members(*s, atlantic) == 3);
    // A new state from one cluster, with its own governor.
    q.region = east;
    q.state_to = {};
    CHECK(sim::perform_action(*s, q).accepted);
    const auto fresh = w.region_get_state(east);
    CHECK(fresh && fresh != atlantic && fresh != pacific && w.state_get_nation(fresh) == usa);
    CHECK(w.state_get_governor(fresh) && w.state_get_governor(fresh) != w.state_get_governor(atlantic));
    // A region that is its own state cannot become a new one again.
    CHECK(!sim::check_redraw(*s, exec, east, dcon::state_id{}).ok);
    // Other agents cannot redraw.
    CHECK(!sim::check_redraw(*s, w.state_get_governor(pacific), west, dcon::state_id{}).ok);

    // The simulation (elections included) runs on the redrawn states.
    for(int d = 0; d < 60; ++d) sim::advance_day(*s);
    CHECK(std::isfinite(w.nation_get_unemployment(usa)) && std::isfinite(w.nation_get_approval(usa)));

    if(failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
