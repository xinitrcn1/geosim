#pragma once
// Atlas-style inset packing for the electoral map (docs/07-map-and-filters.md#electoral-map).
//
// A nation's territory is split into landmass groups (mainland US, Alaska,
// Hawaii; metropolitan France and its overseas regions; ...), each with its
// bounding rectangle in plain Mercator (x = lon in radians, unwrapped around
// the nation; y = ln tan(pi/4 + lat/2), up). The largest group keeps its place
// and scale; every other group becomes an inset: rescaled so its long side is
// 12-32 % of the main group's (Alaska shrinks, Hawaii grows; scale 0.15-3),
// then placed at the candidate position - inside the main rectangle's empty
// corners (no overlap with the main group's land, given as an occupancy grid)
// or alongside it - that minimises the bounding area of the whole layout, with
// a mild preference for the inset's true direction. Insets never overlap each
// other or the main land. Header-only so tests can use it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace geosim::client {

struct merc_rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] double w() const noexcept { return x1 - x0; }
    [[nodiscard]] double h() const noexcept { return y1 - y0; }
    [[nodiscard]] bool overlaps(merc_rect const& o, double gap = 0) const noexcept {
        return x0 < o.x1 + gap && o.x0 < x1 + gap && y0 < o.y1 + gap && o.y0 < y1 + gap;
    }
};

// Land cells of the main group on a grid over its rectangle.
struct occupancy {
    merc_rect box;
    int nx = 0, ny = 0;
    std::vector<uint8_t> cells; // row-major, row 0 at box.y0
    void mark(double x, double y) {
        if(nx <= 0 || ny <= 0) return;
        const int i = std::clamp(static_cast<int>((x - box.x0) / box.w() * nx), 0, nx - 1);
        const int j = std::clamp(static_cast<int>((y - box.y0) / box.h() * ny), 0, ny - 1);
        cells[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i)] = 1;
    }
    // True when any land cell of the main group lies within `r` grown by `gap`.
    [[nodiscard]] bool hits(merc_rect r, double gap) const {
        r = {r.x0 - gap, r.y0 - gap, r.x1 + gap, r.y1 + gap};
        if(!r.overlaps(box) || nx <= 0) return false;
        const int i0 = std::clamp(static_cast<int>(std::floor((r.x0 - box.x0) / box.w() * nx)), 0, nx - 1);
        const int i1 = std::clamp(static_cast<int>(std::floor((r.x1 - box.x0) / box.w() * nx)), 0, nx - 1);
        const int j0 = std::clamp(static_cast<int>(std::floor((r.y0 - box.y0) / box.h() * ny)), 0, ny - 1);
        const int j1 = std::clamp(static_cast<int>(std::floor((r.y1 - box.y0) / box.h() * ny)), 0, ny - 1);
        for(int j = j0; j <= j1; ++j) {
            for(int i = i0; i <= i1; ++i) {
                if(cells[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i)]) return true;
            }
        }
        return false;
    }
};

inline occupancy make_occupancy(merc_rect box, int long_side = 64) {
    occupancy o;
    o.box = box;
    const double l = std::max(box.w(), box.h());
    o.nx = std::max(1, static_cast<int>(std::ceil(box.w() / l * long_side)));
    o.ny = std::max(1, static_cast<int>(std::ceil(box.h() / l * long_side)));
    o.cells.assign(static_cast<std::size_t>(o.nx) * static_cast<std::size_t>(o.ny), 0);
    return o;
}

// Where a group is drawn: its source rectangle scaled by `scale` to `placed`.
struct inset_placement {
    merc_rect source;
    merc_rect placed;
    double scale = 1;
    bool inset = false;
};

inline merc_rect union_of(std::vector<inset_placement> const& p) {
    merc_rect u = p.empty() ? merc_rect{} : p[0].placed;
    for(auto const& q : p) {
        u.x0 = std::min(u.x0, q.placed.x0);
        u.y0 = std::min(u.y0, q.placed.y0);
        u.x1 = std::max(u.x1, q.placed.x1);
        u.y1 = std::max(u.y1, q.placed.y1);
    }
    return u;
}

// groups[main] stays; the others are placed largest (by `weight`) first.
inline std::vector<inset_placement> pack_insets(std::vector<merc_rect> const& groups, std::vector<double> const& weight, std::size_t main,
                                                occupancy const& land) {
    std::vector<inset_placement> out(groups.size());
    if(groups.empty()) return out;
    merc_rect const& m = groups[main];
    const double L = std::max(1e-9, std::max(m.w(), m.h()));
    const double gap = 0.025 * L;
    out[main] = {m, m, 1.0, false};
    std::vector<inset_placement> placed = {out[main]};
    std::vector<std::size_t> order;
    for(std::size_t i = 0; i < groups.size(); ++i) {
        if(i != main) order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return weight[a] > weight[b]; });
    for(std::size_t g : order) {
        merc_rect const& s = groups[g];
        const double side = std::max(1e-9, std::max(s.w(), s.h()));
        const double k = std::clamp(std::clamp(side, 0.12 * L, 0.32 * L) / side, 0.15, 3.0);
        const double iw = s.w() * k, ih = s.h() * k;
        // Preferred spot: the true centre pulled onto the main rectangle.
        const double tx = std::clamp((s.x0 + s.x1) * 0.5, m.x0, m.x1), ty = std::clamp((s.y0 + s.y1) * 0.5, m.y0, m.y1);
        const merc_rect around = union_of(placed);
        const double step = std::max(std::min(iw, ih) / 4.0, (around.w() + around.h() + 2 * (iw + ih)) / 160.0);
        double best_cost = 1e300;
        merc_rect best{};
        for(double y = around.y0 - ih - gap; y <= around.y1 + gap; y += step) {
            for(double x = around.x0 - iw - gap; x <= around.x1 + gap; x += step) {
                const merc_rect r{x, y, x + iw, y + ih};
                bool ok = !land.hits(r, gap);
                for(std::size_t p = 1; ok && p < placed.size(); ++p) ok = !r.overlaps(placed[p].placed, gap);
                if(!ok) continue;
                const double ux0 = std::min(around.x0, r.x0), uy0 = std::min(around.y0, r.y0);
                const double ux1 = std::max(around.x1, r.x1), uy1 = std::max(around.y1, r.y1);
                const double area = (ux1 - ux0) * (uy1 - uy0);
                const double d = std::hypot((r.x0 + r.x1) * 0.5 - tx, (r.y0 + r.y1) * 0.5 - ty) / L;
                const double cost = area * (1.0 + 0.25 * d);
                if(cost < best_cost) {
                    best_cost = cost;
                    best = r;
                }
            }
        }
        out[g] = {s, best, k, true};
        placed.push_back(out[g]);
    }
    return out;
}

} // namespace geosim::client
