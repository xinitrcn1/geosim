#pragma once
// Parliament diagram (docs/08-ui.md#controlling-agents, Politics tab): one dot per
// representative on concentric semicircular rows, parties in wedges from left to right.
// Pure geometry, no rendering, so it is unit-tested without the client.
//
// Layout (the usual "arch" parliament diagram): rows are spaced evenly between an inner
// radius and the outer one; a row holds as many seats as its arc length allows at the row
// spacing, so outer rows hold more. The smallest number of rows that seats everyone is
// used, seats are spread over rows in proportion to their radius, and every seat is
// sorted by its angle (left first, inner rows first on ties) before the parties take
// their seats in order. Each party therefore gets a wedge of the hemicycle.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace geosim::client {

struct seat_dot {
    float x = 0, y = 0;   // unit hemicycle: centre at the origin, x in [-1, 1], y in [0, 1] (up)
    float r = 0;          // dot radius, same units
    uint32_t group = 0;   // index of the seat group (party) in the order given
};

// Integer seats from shares by largest remainder (Hamilton). Shares need not sum to 1.
inline std::vector<int> apportion(std::vector<float> const& shares, int seats) {
    std::vector<int> out(shares.size(), 0);
    double total = 0.0;
    for(float s : shares) total += std::max(0.0f, s);
    if(total <= 0.0 || seats <= 0) return out;
    std::vector<std::pair<double, std::size_t>> rest;
    int given = 0;
    for(std::size_t i = 0; i < shares.size(); ++i) {
        const double q = std::max(0.0f, shares[i]) / total * seats;
        out[i] = static_cast<int>(std::floor(q));
        given += out[i];
        rest.push_back({q - out[i], i});
    }
    std::stable_sort(rest.begin(), rest.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    for(std::size_t k = 0; given < seats && k < rest.size(); ++k, ++given) ++out[rest[k].second];
    return out;
}

// Seats per row for `rows` rows between radius `inner` and 1.
inline int row_capacity(int row, int rows, float inner) {
    const double spacing = (1.0 - inner) / rows;
    const double radius = inner + spacing * (row + 0.5);
    return static_cast<int>(std::floor(3.14159265358979 * radius / spacing)) + 1;
}

// Dots for `seats[i]` representatives of group i, groups in left-to-right order.
inline std::vector<seat_dot> parliament_layout(std::vector<int> const& seats, float inner = 0.35f) {
    std::vector<seat_dot> dots;
    int total = 0;
    for(int s : seats) total += std::max(0, s);
    if(total <= 0) return dots;
    int rows = 1;
    for(;; ++rows) {
        int cap = 0;
        for(int i = 0; i < rows; ++i) cap += row_capacity(i, rows, inner);
        if(cap >= total) break;
    }
    const double spacing = (1.0 - inner) / rows;
    // Seats per row in proportion to the radius, never above a row's capacity.
    std::vector<float> weight(static_cast<std::size_t>(rows));
    for(int i = 0; i < rows; ++i) weight[static_cast<std::size_t>(i)] = static_cast<float>(inner + spacing * (i + 0.5));
    std::vector<int> per_row = apportion(weight, total);
    int overflow = 0;
    for(int i = 0; i < rows; ++i) {
        const int cap = row_capacity(i, rows, inner);
        if(per_row[static_cast<std::size_t>(i)] > cap) {
            overflow += per_row[static_cast<std::size_t>(i)] - cap;
            per_row[static_cast<std::size_t>(i)] = cap;
        }
    }
    for(int i = rows; overflow > 0 && i-- > 0;) { // the outer rows have room left
        const int room = row_capacity(i, rows, inner) - per_row[static_cast<std::size_t>(i)];
        const int take = std::min(room, overflow);
        per_row[static_cast<std::size_t>(i)] += take;
        overflow -= take;
    }
    struct slot {
        double angle;
        int row;
        float x, y;
    };
    std::vector<slot> slots;
    slots.reserve(static_cast<std::size_t>(total));
    for(int i = 0; i < rows; ++i) {
        const int n = per_row[static_cast<std::size_t>(i)];
        const double radius = inner + spacing * (i + 0.5);
        for(int k = 0; k < n; ++k) {
            const double a = n == 1 ? 3.14159265358979 * 0.5 : 3.14159265358979 * (1.0 - static_cast<double>(k) / (n - 1));
            slots.push_back({a, i, static_cast<float>(radius * std::cos(a)), static_cast<float>(radius * std::sin(a))});
        }
    }
    std::stable_sort(slots.begin(), slots.end(), [](slot const& a, slot const& b) {
        if(std::abs(a.angle - b.angle) > 1e-9) return a.angle > b.angle; // left (pi) first
        return a.row < b.row;
    });
    const float r = static_cast<float>(spacing * 0.4);
    std::size_t at = 0;
    for(std::size_t g = 0; g < seats.size(); ++g) {
        for(int k = 0; k < seats[g] && at < slots.size(); ++k, ++at) {
            dots.push_back({slots[at].x, slots[at].y, r, static_cast<uint32_t>(g)});
        }
    }
    return dots;
}

// Seats in the lower (or only) chamber: the real size for the larger democracies, else
// the cube-root law of assembly size (Taagepera 1972: seats ~ population^(1/3)).
inline int chamber_size(std::string_view iso3, double population) {
    struct entry {
        char const* iso;
        int seats;
    };
    static constexpr entry known[] = {
        {"USA", 435}, {"GBR", 650}, {"DEU", 709}, {"FRA", 577}, {"ITA", 630}, {"ESP", 350}, {"CAN", 338}, {"AUS", 151},
        {"JPN", 465}, {"IND", 543}, {"BRA", 513}, {"MEX", 500}, {"ARG", 257}, {"ZAF", 400}, {"KOR", 300}, {"TUR", 600},
        {"POL", 460}, {"NLD", 150}, {"SWE", 349}, {"NOR", 169}, {"DNK", 179}, {"FIN", 200}, {"BEL", 150}, {"AUT", 183},
        {"CHE", 200}, {"PRT", 230}, {"GRC", 300}, {"IRL", 160}, {"NZL", 120}, {"ISR", 120}, {"IDN", 575}, {"PHL", 304},
        {"NGA", 360}, {"KEN", 349}, {"PAK", 342}, {"BGD", 350}, {"CHL", 155}, {"COL", 172}, {"PER", 130}, {"UKR", 450},
        {"HUN", 199}, {"CZE", 200}, {"ROU", 330}, {"MYS", 222}, {"LKA", 225}, {"TWN", 113}, {"SGP", 93},  {"ISL", 63},
    };
    for(auto const& e : known) {
        if(iso3 == e.iso) return e.seats;
    }
    const double s = std::cbrt(std::max(0.0, population));
    return std::clamp(static_cast<int>(std::lround(s)), 60, 650);
}

} // namespace geosim::client
