// The map camera (src/client/map_view.hpp): oblique Mercator round trips,
// north-up and unit scale at the focus, pan and zoom-at-cursor invariants.
#include "map_view.hpp"
#include "tile_pack.hpp"

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
    // Tile cartogram: 60 regions crowded on a grid end up disjoint, near home.
    {
        std::vector<geosim::client::tile> tiles;
        std::vector<double> weight;
        for(int k = 0; k < 60; ++k) {
            geosim::client::tile t;
            t.x = (k % 10) * 0.1 + 0.03 * (k % 3);
            t.y = (k / 10) * 0.1;
            tiles.push_back(t);
            weight.push_back(1.0 + (k % 7));
        }
        geosim::client::pack_tiles(tiles, weight, 1.0 * 0.6, 0.6);
        check(geosim::client::tiles_disjoint(tiles), "packed tiles do not overlap");
        double drift = 0.0;
        for(auto const& t : tiles) drift = std::max(drift, std::hypot(t.x - t.home_x, t.y - t.home_y));
        check(drift < 1.0, "tiles stay near home");
    }
    if(failures == 0) std::printf("test_map_view: ok\n");
    return failures == 0 ? 0 : 1;
}
