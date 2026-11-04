// The multi-region path: a copy of the 2020 slice assets where the USA and
// Germany are split into regions (regions.csv, region_sectors.csv,
// adjacency.csv). Checks that the build is consistent (regional value added
// adds up to the nation's), that the no-shock baseline stays put, that money
// is conserved, and that a historical run stays finite and deterministic.

#include "sim/ledger.hpp"
#include "sim/report.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"

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
    const fs::path root = fs::temp_directory_path() / "geosim_multiregion_assets";
    fs::remove_all(root);
    fs::copy(GEOSIM_ASSETS_DIR, root, fs::copy_options::recursive);
    const fs::path scen = root / "scenario" / "2020";
    std::ofstream r(scen / "regions.csv");
    r << "region,iso3,name,lat,lon,area_km2,pop_share,gdp_share,urban_share\n"
         "USA-W,USA,West,37,-120,1.9e6,0.24,0.26,0.9\n"
         "USA-C,USA,Centre,40,-95,3.0e6,0.21,0.19,0.7\n"
         "USA-S,USA,South,32,-88,2.3e6,0.38,0.35,0.75\n"
         "USA-E,USA,Northeast,42,-74,0.5e6,0.17,0.20,0.9\n"
         "DEU-N,DEU,North,53,10,1.4e5,0.45,0.43,0.8\n"
         "DEU-S,DEU,South,48.5,10,2.2e5,0.55,0.57,0.7\n";
    r.close();
    // Sector shares: agriculture leans rural, services urban; columns sum to 1 per nation.
    std::ofstream s(scen / "region_sectors.csv");
    s << "region,sector,va_share,emp_share\n";
    const char* sectors[] = {"agr", "ene", "man", "con", "csv", "osv", "gov"};
    const double us[4][7] = {{0.20, 0.15, 0.22, 0.24, 0.25, 0.30, 0.24}, {0.40, 0.30, 0.22, 0.20, 0.19, 0.15, 0.21},
                             {0.35, 0.45, 0.38, 0.38, 0.37, 0.30, 0.38}, {0.05, 0.10, 0.18, 0.18, 0.19, 0.25, 0.17}};
    const double de[2][7] = {{0.40, 0.45, 0.38, 0.45, 0.46, 0.47, 0.45}, {0.60, 0.55, 0.62, 0.55, 0.54, 0.53, 0.55}};
    const char* us_ids[] = {"USA-W", "USA-C", "USA-S", "USA-E"};
    const char* de_ids[] = {"DEU-N", "DEU-S"};
    for(int i = 0; i < 4; ++i)
        for(int k = 0; k < 7; ++k) s << us_ids[i] << ',' << sectors[k] << ',' << us[i][k] << ',' << us[i][k] << '\n';
    for(int i = 0; i < 2; ++i)
        for(int k = 0; k < 7; ++k) s << de_ids[i] << ',' << sectors[k] << ',' << de[i][k] << ',' << de[i][k] << '\n';
    s.close();
    std::ofstream a(scen / "adjacency.csv");
    a << "region_a,region_b,weight\nUSA-W,USA-C,10\nUSA-C,USA-S,12\nUSA-C,USA-E,6\nUSA-S,USA-E,8\nDEU-N,DEU-S,9\n";
    return root.string();
}

int main() {
    const std::string assets = make_assets();

    // Consistency of the build.
    {
        auto s = std::make_unique<game_state>();
        (void)sim::build_scenario(*s, assets, "2020");
        const auto usa = sim::find_nation(*s, "USA");
        int regions = 0;
        double va = 0.0;
        for(auto own : s->world.nation_get_region_ownership(usa)) {
            ++regions;
            for(uint8_t k = 0; k < s->derived.sector_count; ++k) va += own.get_region().get_va0(k);
        }
        double nva = 0.0;
        for(uint8_t k = 0; k < s->derived.sector_count; ++k) nva += s->world.nation_get_va0(usa, k);
        CHECK(regions == 4);
        CHECK(std::abs(va / nva - 1.0) < 1e-4);
        int edges = 0;
        for(auto e : s->world.in_region_adjacency) { (void)e; ++edges; }
        CHECK(edges == 5);
    }

    // No-shock baseline stays put; money is conserved.
    {
        auto s = std::make_unique<game_state>();
        (void)sim::build_scenario(*s, assets, "2020");
        for(auto e : s->world.in_event) e.set_done(1);
        sim::begin_measurement(*s);
        const double money0 = sim::world_money(*s);
        for(int d = 0; d < 120; ++d) sim::advance_day(*s);
        for(auto const& n : sim::summarize(*s).nations) {
            const auto id = sim::find_nation(*s, n.iso3);
            if(std::abs(n.unemployment_pct - s->world.nation_get_unemployment0(id) * 100.0) > 0.5) {
                std::fprintf(stderr, "baseline drift %s: unemployment %.2f\n", n.iso3.c_str(), n.unemployment_pct);
                ++failures;
            }
        }
        CHECK(std::abs(sim::world_money(*s) - money0) <= 1e-9 * std::abs(money0) + 1.0);
    }

    // Historical run: finite and deterministic.
    {
        auto a = std::make_unique<game_state>();
        auto b = std::make_unique<game_state>();
        (void)sim::build_scenario(*a, assets, "2020");
        (void)sim::build_scenario(*b, assets, "2020");
        for(int d = 0; d < 200; ++d) {
            sim::advance_day(*a);
            sim::advance_day(*b);
        }
        bool finite = true;
        double ha = 0.0, hb = 0.0;
        for(auto r : a->world.in_region) {
            finite = finite && std::isfinite(r.get_ep_i(0)) && std::isfinite(r.get_capacity(2));
            ha += r.get_ep_i(0) + r.get_output(4);
        }
        for(auto r : b->world.in_region) hb += r.get_ep_i(0) + r.get_output(4);
        CHECK(finite);
        CHECK(ha == hb);
    }

    fs::remove_all(assets);
    if(failures == 0) {
        std::puts("test_multiregion: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_multiregion: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
