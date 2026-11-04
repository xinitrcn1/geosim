// The map camera (src/client/map_view.hpp): oblique Mercator round trips,
// north-up and unit scale at the focus, pan and zoom-at-cursor invariants.
#include "map_view.hpp"
#include "inset_pack.hpp"

#include <vector>

#include <cmath>
#include <cstdio>

using geosim::client::map_deg;
using geosim::client::map_view;

namespace {

int failures = 0;

void check(bool ok, char const* what) {
    if(!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

} // namespace

int main() {
    for(double lat0 : {0.0, 35.0, 60.0, -75.0, 89.0}) {
        for(double lon0 : {-179.0, 0.0, 100.0}) {
            map_view v;
            v.set_focus(lon0 * map_deg, lat0 * map_deg);
            v.scale = 2000;
            // The focus is at the centre.
            double lon = 0, lat = 0;
            v.to_geo(0, 0, lon, lat);
            check(near(lat, lat0 * map_deg, 1e-9) && near(std::remainder(lon - lon0 * map_deg, 2 * geosim::client::map_pi), 0, 1e-9),
                  "focus at centre");
            // Round trips over the screen.
            for(double dx : {-700.0, -50.0, 0.0, 300.0}) {
                for(double dy : {-400.0, 0.0, 250.0}) {
                    v.to_geo(dx, dy, lon, lat);
                    double sx = 0, sy = 0;
                    v.to_screen(lon, lat, sx, sy);
                    check(near(sx, dx, 1e-6) && near(sy, dy, 1e-6), "screen -> geo -> screen");
                }
            }
            // North is up and the scale is 1 near the centre: 1 pixel up = 1/scale radians north.
            v.to_geo(0, -1, lon, lat);
            check(near(lat - lat0 * map_deg, 1.0 / v.scale, 1e-7) || lat0 >= 89.0, "north up, unit scale");
        }
    }
    // Pan: the centre point follows a drag.
    {
        map_view v;
        v.set_focus(10 * map_deg, 45 * map_deg);
        v.scale = 3000;
        double lon = 0, lat = 0;
        v.to_geo(0, 0, lon, lat);
        v.pan(20, 10);
        double sx = 0, sy = 0;
        v.to_screen(lon, lat, sx, sy);
        check(near(sx, 20, 0.3) && near(sy, 10, 0.3), "pan moves the map with the pointer");
    }
    // Zoom at the cursor keeps the point under it.
    {
        map_view v;
        v.set_focus(-100 * map_deg, 40 * map_deg);
        v.scale = 800;
        double lon = 0, lat = 0;
        v.to_geo(400, -150, lon, lat);
        v.zoom_at(1.25, 400, -150);
        double sx = 0, sy = 0;
        v.to_screen(lon, lat, sx, sy);
        check(near(sx, 400, 0.5) && near(sy, -150, 0.5), "zoom at the cursor");
        check(near(v.scale, 1000, 1e-9), "zoom factor");
    }
    // Electoral insets, USA-like: mainland (empty lower corners), Alaska, Hawaii.
    {
        using namespace geosim::client;
        auto mx = [](double lon) { return lon * map_deg; };
        auto my = [](double lat) { return std::log(std::tan(map_pi / 4.0 + lat * map_deg / 2.0)); };
        const std::vector<merc_rect> groups = {
            {mx(-125), my(25), mx(-67), my(49)},     // mainland
            {mx(-188), my(51), mx(-130), my(71.5)},  // Alaska (across the antimeridian, unwrapped)
            {mx(-160.5), my(18.9), mx(-154.8), my(22.3)}, // Hawaii
        };
        const std::vector<double> weight = {8.0e6, 1.7e6, 2.8e4};
        occupancy land = make_occupancy(groups[0]);
        for(double lon = -125; lon <= -67; lon += 0.25) {
            for(double lat = 25; lat <= 49; lat += 0.25) {
                const bool empty = (lon < -108 && lat < 32) || (lon > -81 && lat < 30) || (lon > -95 && lat > 46);
                if(!empty) land.mark(mx(lon), my(lat));
            }
        }
        const auto placed = pack_insets(groups, weight, 0, land);
        bool disjoint = true;
        for(std::size_t i = 1; i < placed.size(); ++i) {
            disjoint = disjoint && !land.hits(placed[i].placed, 0.0);
            for(std::size_t j = i + 1; j < placed.size(); ++j) disjoint = disjoint && !placed[i].placed.overlaps(placed[j].placed);
        }
        check(disjoint, "insets overlap neither each other nor the mainland");
        merc_rect raw = groups[0];
        for(auto const& g : groups) {
            raw.x0 = std::min(raw.x0, g.x0);
            raw.y0 = std::min(raw.y0, g.y0);
            raw.x1 = std::max(raw.x1, g.x1);
            raw.y1 = std::max(raw.y1, g.y1);
        }
        const merc_rect packed = union_of(placed);
        const double ratio = packed.w() * packed.h() / (raw.w() * raw.h());
        std::printf("USA insets: packed / raw bounding area = %.2f\n", ratio);
        check(ratio < 0.5, "packed layout much smaller than the raw bounding box");
        check(placed[1].scale < 1.0 && placed[2].scale > 1.0, "Alaska shrinks, Hawaii grows");
    }
    if(failures == 0) std::printf("test_map_view: ok\n");
    return failures == 0 ? 0 : 1;
}
