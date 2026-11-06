// The in-game internet (sim/web.cpp): a directory of procedurally generated sites with
// derived audiences, search engines per sector (and a closed intranet), propaganda that
// follows press freedom, and 404s that follow instability.
#include "core/keys.hpp"
#include "sim/scenario.hpp"
#include "sim/simulation.hpp"
#include "sim/web.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

using namespace geosim;
using namespace geosim::sim::web;

static void print(page const& p) {
    std::printf("=== %s [%s] %s (run by %s; %d ads blocked)\n", p.url.c_str(), p.category.c_str(), p.title.c_str(), p.owner.c_str(), p.ads_blocked);
    int k = 0;
    for(auto const& b : p.blocks) {
        if(++k > 14) break;
        std::printf("  %d| %s%s%s\n", static_cast<int>(b.k), b.text.c_str(), b.sub.empty() ? "" : "  -- ", b.sub.c_str());
    }
}

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    auto s = std::make_unique<game_state>();
    if(!sim::build_scenario(*s, GEOSIM_ASSETS_DIR, "world2020").ok()) return EXIT_FAILURE;
    for(int d = 0; d < 120; ++d) sim::advance_day(*s);
    names nm; // codes as names
    auto dir = build_directory(*s, nm);
    CHECK(dir.sites.size() > 500);
    std::set<std::string> domains;
    for(auto const& x : dir.sites) {
        if(!domains.insert(x.domain).second) std::printf("duplicate %s (%s)\n", x.domain.c_str(), x.name.c_str());
        CHECK(domains.count(x.domain) == 1);
    }
    for(std::size_t i = 1; i < dir.sites.size(); ++i) CHECK(dir.sites[i - 1].users >= dir.sites[i].users);
    // Sectors: global, the two hubs, and a closed intranet for the DPRK.
    bool sinic = false, runet = false, closed_prk = false;
    for(auto const& sec : dir.sectors) {
        sinic = sinic || sec.key == "sinic";
        runet = runet || sec.key == "runet";
        if(sec.closed && sec.hub == sim::find_nation(*s, "PRK")) closed_prk = true;
    }
    CHECK(sinic && runet && closed_prk);
    for(auto const& x : dir.sites) {
        if(x.nation == sim::find_nation(*s, "PRK")) CHECK(x.owner && s->world.agent_get_kind(x.owner) == 0); // state-run
    }
    std::printf("top 5:");
    for(int i = 0; i < 5; ++i) std::printf(" %s (%.0fM, %s)", dir.sites[i].domain.c_str(), dir.sites[i].users / 1e6, category_name(dir.sites[i].kind));
    std::printf("\n");
    // Default engine: the player's agents' nations decide.
    const auto rus = sim::find_nation(*s, "RUS"), usa = sim::find_nation(*s, "USA"), chn = sim::find_nation(*s, "CHN");
    const std::string world = default_engine(*s, dir, {}), ru = default_engine(*s, dir, {rus, rus, sim::find_nation(*s, "BLR")}), us = default_engine(*s, dir, {usa});
    std::printf("engines: world %s, Russian agents %s, US agents %s, China %s\n", world.c_str(), ru.c_str(), us.c_str(), default_engine(*s, dir, {chn}).c_str());
    CHECK(ru != us && us == "www." + dir.sectors[0].engine);
    // Every kind renders; a site of an unfree nation spins more than one of a free nation.
    std::set<int> kinds;
    for(auto const& x : dir.sites) {
        if(kinds.insert(static_cast<int>(x.kind)).second || (verbose && x.nation && (x.nation == chn || x.nation == sim::find_nation(*s, "PRK")) && kinds.size() < 40)) {
            const auto p = render(*s, nm, dir, x.domain, world, {usa});
            CHECK(!p.blocks.empty());
            if(verbose) print(p);
        }
    }
    CHECK(kinds.size() == 15); // every site_kind, companies to anti-piracy included
    const auto home = render(*s, nm, dir, "about:home", world, {usa});
    if(verbose) print(home);
    CHECK(home.style == page::layout::home);
    const auto search = render(*s, nm, dir, world + "/search?q=chn", world);
    if(verbose) print(search);
    CHECK(search.blocks.size() > 2);
    CHECK(render(*s, nm, dir, "www.nosuchsite.xyz", world).style == page::layout::error);
    // A collapsing nation's sites go 404.
    for(auto r : s->world.in_region) {
        if(r.get_owner() == usa) r.set_unrest(4.0f);
    }
    s->world.nation_set_legitimacy(usa, 0.0f);
    auto broken = build_directory(*s, nm);
    int down = 0, all = 0;
    for(auto const& x : broken.sites) {
        if(x.nation != usa) continue;
        ++all;
        down += x.down;
    }
    std::printf("USA in collapse: %d of %d sites down\n", down, all);
    CHECK(down * 2 > all);
    // People's names follow who lives there: the USA's mix, then a USA of Lithuanians.
    {
        name_pools pools;
        std::string error;
        CHECK(load_name_pools(std::string(GEOSIM_ASSETS_DIR) + "/defs", pools, error));
        if(!error.empty()) std::printf("names: %s\n", error.c_str());
        auto s2 = std::make_unique<game_state>();
        if(!sim::build_scenario(*s2, GEOSIM_ASSETS_DIR, "2020").ok()) return EXIT_FAILURE;
        const auto us2 = sim::find_nation(*s2, "USA");
        auto mix = culture_mix(*s2, pools, us2);
        std::printf("USA names:");
        for(uint64_t k = 0; k < 6; ++k) std::printf(" %s;", person(pools, mix, k).c_str());
        std::printf("\n");
        int lit = -1;
        for(std::size_t i = 0; i < pools.cultures.size(); ++i) {
            if(pools.cultures[i] == "lithuanian") lit = static_cast<int>(i);
        }
        CHECK(lit >= 0);
        for(auto g : s2->world.in_social_group) {
            if(g.get_nation() == us2) g.set_key(core::pack_key("LTU"));
        }
        mix = culture_mix(*s2, pools, us2);
        CHECK(mix.size() == 1 && mix[0].first == lit);
        std::printf("USA of Lithuanians:");
        for(uint64_t k = 0; k < 4; ++k) std::printf(" %s;", person(pools, mix, k).c_str());
        std::printf("\n");
        domain_table domains;
        CHECK(load_domain_table(std::string(GEOSIM_ASSETS_DIR) + "/defs", domains, error));
        names nm2{nullptr, nullptr, &pools, &domains};
        auto dir2 = build_directory(*s2, nm2);
        bool byline = false;
        for(auto const& x : dir2.sites) {
            if(x.nation != us2 || (x.kind != site_kind::paper && x.kind != site_kind::state)) continue;
            const auto pg = render(*s2, nm2, dir2, x.domain, dir2.sites[0].domain);
            for(auto const& b : pg.blocks) {
                if((b.k == block::kind::headline && b.sub.find(" by ") != std::string::npos) || b.text.find("spokesperson") != std::string::npos) {
                    std::printf("%s: %s %s\n", x.domain.c_str(), b.text.c_str(), b.sub.c_str());
                    byline = true;
                    break;
                }
            }
        }
        CHECK(byline);
        // Domains follow tlds.csv.
        for(char const* iso : {"GBR", "CHN", "USA", "DEU", "JPN"}) {
            const auto n = sim::find_nation(*s2, iso);
            std::printf("%s:", iso);
            for(auto const& x : dir2.sites) {
                if(x.nation == n && (x.kind == site_kind::state || x.kind == site_kind::paper)) std::printf(" %s", x.domain.c_str());
            }
            std::printf("\n");
        }
    }
    std::printf("test_web: %s\n", failures ? "FAILED" : "ok");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
