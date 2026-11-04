#pragma once
// Rectangular tile cartogram packing for the electoral map
// (docs/07-map-and-filters.md#electoral-map). Each region becomes a square
// whose area is proportional to its weight (electorate or area), placed near
// its true position (the centre of its Mercator bounding box), then pushed
// apart until no two tiles overlap: iterative overlap removal along the axis
// of least overlap (as in force-directed rectangle layout; cf. the rectangular
// cartograms of Raisz 1934 and van Kreveld & Speckmann 2007), with a weak
// spring back to each tile's home so the layout keeps the nation's shape.
// Header-only so tests can use it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace geosim::client {

struct tile {
    double x = 0, y = 0; // centre (Mercator units: x = lon radians, y = ln tan(pi/4 + lat/2); y up)
    double side = 0;     // square side
    double home_x = 0, home_y = 0;
};

inline double mercator_y(double lat_deg) {
    const double phi = std::clamp(lat_deg, -85.0, 85.0) * 3.14159265358979 / 180.0;
    return std::log(std::tan(3.14159265358979 / 4.0 + phi / 2.0));
}

// `fill`: total tile area as a share of the bounding box of the homes' boxes
// (`box_area`). `gap`: space between tiles as a share of the median side.
inline void pack_tiles(std::vector<tile>& tiles, std::vector<double> const& weight, double box_area, double fill = 0.45,
                       double gap = 0.08, int iterations = 400) {
    const std::size_t n = tiles.size();
    if(n == 0) return;
    double total = 0.0;
    for(double w : weight) total += std::max(0.0, w);
    if(total <= 0.0) total = static_cast<double>(n);
    std::vector<double> sides(n);
    for(std::size_t i = 0; i < n; ++i) {
        const double w = total > 0.0 && weight[i] > 0.0 ? weight[i] : total / static_cast<double>(n) * 0.05;
        sides[i] = std::sqrt(std::max(1e-12, box_area * fill * w / total));
    }
    // Very small regions still get a readable tile: at least a third of the median side.
    std::vector<double> sorted = sides;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n / 2), sorted.end());
    const double median = sorted[n / 2];
    const double g = gap * median;
    for(std::size_t i = 0; i < n; ++i) {
        tiles[i].side = std::max(sides[i], median / 3.0);
        tiles[i].home_x = tiles[i].x;
        tiles[i].home_y = tiles[i].y;
    }
    for(int it = 0; it < iterations; ++it) {
        const double pull = it < iterations * 3 / 4 ? 0.02 : 0.0; // the last quarter only separates
        bool moved = false;
        for(std::size_t i = 0; i < n; ++i) {
            for(std::size_t j = i + 1; j < n; ++j) {
                tile& a = tiles[i];
                tile& b = tiles[j];
                const double dx = b.x - a.x, dy = b.y - a.y;
                const double ox = (a.side + b.side) * 0.5 + g - std::fabs(dx);
                const double oy = (a.side + b.side) * 0.5 + g - std::fabs(dy);
                if(ox <= 0.0 || oy <= 0.0) continue;
                moved = true;
                if(ox < oy) {
                    const double s = (dx > 0.0 || (dx == 0.0 && i < j)) ? 1.0 : -1.0;
                    a.x -= s * ox * 0.5;
                    b.x += s * ox * 0.5;
                } else {
                    const double s = (dy > 0.0 || (dy == 0.0 && i < j)) ? 1.0 : -1.0;
                    a.y -= s * oy * 0.5;
                    b.y += s * oy * 0.5;
                }
            }
        }
        for(auto& t : tiles) {
            t.x += pull * (t.home_x - t.x);
            t.y += pull * (t.home_y - t.y);
        }
        if(!moved && pull == 0.0) break;
    }
}

// True when no two tiles overlap (gap ignored).
inline bool tiles_disjoint(std::vector<tile> const& tiles) {
    for(std::size_t i = 0; i < tiles.size(); ++i) {
        for(std::size_t j = i + 1; j < tiles.size(); ++j) {
            const double ox = (tiles[i].side + tiles[j].side) * 0.5 - std::fabs(tiles[i].x - tiles[j].x);
            const double oy = (tiles[i].side + tiles[j].side) * 0.5 - std::fabs(tiles[i].y - tiles[j].y);
            if(ox > 1e-9 && oy > 1e-9) return false;
        }
    }
    return true;
}

} // namespace geosim::client
