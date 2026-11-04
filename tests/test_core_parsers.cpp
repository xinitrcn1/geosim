// Calendar, CSV and defs-format parsing.

#include "core/calendar.hpp"
#include "core/csv.hpp"
#include "core/defs.hpp"
#include "core/keys.hpp"
#include "core/rng.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

template<typename F>
static bool throws(F&& f) {
    try {
        f();
    } catch(std::runtime_error const&) {
        return true;
    }
    return false;
}

int main() {
    using namespace geosim::core;

    // Calendar
    CHECK(tick_from_date({2020, 1, 1}) == 0);
    CHECK(tick_from_date({2020, 3, 1}) == 60); // 2020 is a leap year
    CHECK(tick_from_date({2021, 1, 1}) == 366);
    const auto d = date_from_tick(75);
    CHECK(d.year == 2020 && d.month == 3 && d.day == 16);
    CHECK(parse_date("2020-02-29").has_value());
    CHECK(!parse_date("2021-02-29").has_value());
    CHECK(!parse_date("2020-13-01").has_value());
    CHECK(format_date({2020, 4, 7}) == "2020-04-07");

    // Keys
    CHECK(unpack_key(pack_key("USA")) == "USA");
    CHECK(pack_key("C2") != pack_key("C3"));

    // CSV
    const auto t = csv_table::parse("# comment\na,b,\"c,d\"\n1,,\"x \"\"y\"\"\"\n# mid comment\n2.5,3,z\n", "test.csv");
    CHECK(t.rows() == 2);
    CHECK(t.get(0, "c,d") == "x \"y\"");
    CHECK(!t.number(0, "b").has_value());
    CHECK(t.number_or(1, "a", 0) == 2.5);
    CHECK(throws([&] { (void)t.number(1, "c,d"); }));

    // Defs
    const auto nodes = parse_defs("# top\nnpi{ key{C2} level{3} # trailing\n  sub{ x{1} } }\nflag\nname{ two words }\n", "t.txt");
    CHECK(nodes.size() == 3);
    CHECK(nodes[0].key == "npi");
    CHECK(nodes[0].req("key", "t.txt") == "C2");
    CHECK(nodes[0].num("level", "t.txt").value_or(0) == 3.0);
    CHECK(nodes[0].find("sub") && nodes[0].find("sub")->num_or("x", 0, "t.txt") == 1.0);
    CHECK(nodes[0].line == 2);
    CHECK(nodes[1].key == "flag" && nodes[1].children.empty());
    CHECK(nodes[2].text == "two words");
    CHECK(throws([] { (void)parse_defs("a{ b{1} ", "bad.txt"); }));
    CHECK(throws([] { (void)parse_defs("a{1} }", "bad.txt"); }));
    CHECK(throws([] { (void)parse_number("nan", "t", 1); }));

    // Counter-based RNG: pure function of its inputs, uniform in [0, 1).
    CHECK(uniform01(42, 1, 10, 7) == uniform01(42, 1, 10, 7));
    CHECK(uniform01(42, 1, 10, 7) != uniform01(42, 1, 10, 8));
    double sum = 0.0;
    bool in_range = true;
    for(uint32_t i = 0; i < 100000; ++i) {
        const float u = uniform01(7, 2, i / 1000, i % 1000);
        in_range = in_range && u >= 0.0f && u < 1.0f;
        sum += u;
    }
    CHECK(in_range);
    CHECK(std::abs(sum / 100000.0 - 0.5) < 0.005);

    if(failures == 0) {
        std::puts("test_core_parsers: OK");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "test_core_parsers: %d failure(s)\n", failures);
    return EXIT_FAILURE;
}
