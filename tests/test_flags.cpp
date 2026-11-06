// Flags (core/flags.hpp): parsing, lowering to GPU primitives, and the shipped data covering
// every nation of every scenario.

#include "core/csv.hpp"
#include "core/flags.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

using namespace geosim::core;

static bool parses(char const* emblems, char const* flags_csv, flags::book& b) {
    diagnostics diag;
    const bool ok = flags::parse_emblems(emblems, "emblems", b, diag) && flags::parse_flags(flags_csv, "flags", b, diag);
    if(!ok) std::fprintf(stderr, "  (expected or not) %s\n", diag.error().c_str());
    return ok;
}

int main() {
    const char* emblems = "name,part,points\n"
                          "shield,mirror,0 -1 -0.8 -1 -0.8 0.2 0 1\n"
                          "dot,circle,0 0 0.5\n";
    // Proportions, stripes as cascading bands, a disc in height units, an emblem, an inset.
    {
        flags::book b;
        CHECK(parses(emblems,
                     "iso3,shape,colour,args\n"
                     "AAA,flag,,2 1\n"
                     "AAA,hstripes,FF0000|FFFFFF|0000FF,1 2 1\n"
                     "AAA,circle,00FF00,0.25 0.5 0.2\n"
                     "AAA,emblem,000000,shield 0.75 0.5 0.5\n"
                     "BBB,flag,,3 2\n"
                     "BBB,field,FFFFFF,\n"
                     "BBB,inset,,AAA 0 0 0.5\n",
                     b));
        CHECK(b.flags.size() == 2);
        CHECK(std::fabs(b.flags[0].aspect - 2.0f) < 1e-6f);
        const auto c = flags::compile(b);
        CHECK(c.flags.size() == 2);
        // AAA: 3 bands + disc + shield (mirrored into one polygon).
        CHECK(c.flags[0].count == 5);
        auto const& band2 = c.prims[1];
        CHECK(band2.kind == flags::prim::poly && std::fabs(band2.box[1] - 0.25f) < 1e-5f && std::fabs(band2.box[3] - 1.0f) < 1e-5f);
        CHECK((band2.colour & 0xFFu) == 0xFF && (band2.colour >> 24) == 0xFF); // white, opaque, r in the low byte
        auto const& disc = c.prims[3];
        CHECK(disc.kind == flags::prim::disc && std::fabs(disc.a[0] - 0.5f) < 1e-5f && std::fabs(disc.a[2] - 0.2f) < 1e-5f);
        auto const& shield = c.prims[4];
        CHECK(shield.count == 6); // 4 left-half points + 2 mirrored off the axis
        CHECK(std::fabs(shield.box[0] - (1.5f - 0.2f)) < 1e-4f && std::fabs(shield.box[2] - (1.5f + 0.2f)) < 1e-4f);
        // BBB: its field + AAA's five primitives scaled by 0.5 into the canton.
        CHECK(c.flags[1].count == 6);
        auto const& inset_disc = c.prims[c.flags[1].first + 4];
        CHECK(inset_disc.kind == flags::prim::disc && std::fabs(inset_disc.a[0] - 0.25f) < 1e-5f && std::fabs(inset_disc.a[2] - 0.1f) < 1e-5f);
    }
    // Errors: no flag row, unknown emblem, bad colour, wrong weights, inset of itself.
    {
        flags::book b;
        CHECK(!parses(emblems, "iso3,shape,colour,args\nAAA,field,FFFFFF,\n", b));
        flags::book b2;
        CHECK(!parses(emblems, "iso3,shape,colour,args\nAAA,flag,,3 2\nAAA,emblem,000000,eagle 0.5 0.5 0.5\n", b2));
        flags::book b3;
        CHECK(!parses(emblems, "iso3,shape,colour,args\nAAA,flag,,3 2\nAAA,field,GG0000,\n", b3));
        flags::book b4;
        CHECK(!parses(emblems, "iso3,shape,colour,args\nAAA,flag,,3 2\nAAA,hstripes,FF0000|FFFFFF,1 2 3\n", b4));
        flags::book b5;
        CHECK(!parses(emblems, "iso3,shape,colour,args\nAAA,flag,,3 2\nAAA,inset,,AAA 0 0 0.5\n", b5));
    }
    // The shipped flags: they load, compile, and every scenario nation has one.
    {
        flags::book b;
        diagnostics diag;
        CHECK(flags::load(GEOSIM_ASSETS_DIR "/defs", b, diag));
        if(diag.failed()) std::fprintf(stderr, "%s\n", diag.error().c_str());
        const auto c = flags::compile(b);
        CHECK(c.flags.size() == b.flags.size());
        for(std::size_t i = 0; i < c.flags.size(); ++i) {
            if(c.flags[i].count == 0) std::fprintf(stderr, "flag %s draws nothing\n", b.flags[i].key.c_str());
            CHECK(c.flags[i].count > 0);
        }
        for(char const* scenario : {"world2020", "2020", "2008"}) {
            diagnostics d;
            const auto nations = csv_table::load(std::string(GEOSIM_ASSETS_DIR "/scenario/") + scenario + "/nations.csv", d);
            for(std::size_t r = 0; r < nations.rows(); ++r) {
                const std::string iso(nations.get(r, "iso3"));
                if(!b.find(iso)) {
                    std::fprintf(stderr, "%s: no flag for %s\n", scenario, iso.c_str());
                    ++failures;
                }
            }
        }
    }
    if(failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
