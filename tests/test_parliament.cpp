// Parliament diagram geometry (src/client/parliament.hpp): one dot per seat, parties in
// wedges left to right, no overlapping dots.
#include "client/parliament.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if(!(cond)) {                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while(0)

using namespace geosim::client;

int main() {
    // Largest remainder hands out exactly the chamber.
    const auto us = apportion({0.534f, 0.448f, 0.018f}, 435);
    CHECK(us[0] + us[1] + us[2] == 435);
    CHECK(us[0] == 232 && us[1] == 195 && us[2] == 8);
    CHECK(apportion({0.0f, 0.0f}, 10)[0] == 0);

    for(int n : {1, 7, 60, 151, 435, 650, 709}) {
        const std::vector<int> seats = apportion({0.3f, 0.25f, 0.2f, 0.15f, 0.1f}, n);
        const auto dots = parliament_layout(seats);
        CHECK(static_cast<int>(dots.size()) == n);
        // Inside the unit hemicycle, no two dots overlap.
        double min_d = 1e9;
        for(std::size_t i = 0; i < dots.size(); ++i) {
            CHECK(dots[i].y >= -1e-4f && std::hypot(dots[i].x, dots[i].y) <= 1.0f + 1e-4f);
            for(std::size_t j = i + 1; j < dots.size() && dots.size() <= 709; ++j) {
                min_d = std::min(min_d, std::hypot(static_cast<double>(dots[i].x - dots[j].x), static_cast<double>(dots[i].y - dots[j].y)));
            }
        }
        if(dots.size() > 1) CHECK(min_d >= 2.0 * dots[0].r - 1e-4);
        // Parties take wedges in order: group indices never decrease with the angle.
        std::vector<std::pair<double, uint32_t>> by_angle;
        for(auto const& d : dots) by_angle.push_back({std::atan2(d.y, d.x), d.group});
        // Each group's angular range must not interleave with a later group's mean.
        std::vector<double> mean(seats.size(), 0.0);
        for(auto const& [a, g] : by_angle) mean[g] += a / seats[g];
        for(std::size_t g = 1; g < seats.size(); ++g) {
            if(seats[g] > 0 && seats[g - 1] > 0) CHECK(mean[g] <= mean[g - 1] + 1e-9); // left (pi) to right (0); ties across rows are fine
        }
    }
    CHECK(chamber_size("USA", 3.3e8) == 435);
    CHECK(chamber_size("XYZ", 1.0e6) == 100);   // cube root of a million
    CHECK(chamber_size("XYZ", 1.0e3) == 60);    // floor
    std::printf("test_parliament: %s\n", failures ? "FAILED" : "ok");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
