#pragma once
// The map camera (docs/07-map-and-filters.md#projection): a spherical oblique
// Mercator (Hotine) whose central line is the great circle through the focus
// point running east-west there. The sphere is rotated so the focus sits at
// (0, 0) of the oblique frame, with local north as the frame's pole, and
// ordinary Mercator is applied in that frame:
//
//   P = focus, E = local east, N = local north (unit vectors, right-handed)
//   x' = X.P, y' = X.E, z' = X.N
//   u = atan2(y', x'), v = atanh(z')           (oblique Mercator, radians)
//   screen = centre + scale * (u, -v)          (pixels, y down)
//
// The scale factor is 1 along the central line and cosh(v) away from it, so
// distortion is smallest around the screen centre whatever the focus; north
// is up at the centre. Panning moves the focus, which recentres the
// projection. The fragment shader (world_map.frag) runs the inverse per pixel.
// Header-only so tests can use it without Vulkan.

#include <algorithm>
#include <cmath>

namespace geosim::client {

struct map_vec3 {
    double x = 0, y = 0, z = 0;
};

[[nodiscard]] inline map_vec3 operator*(double s, map_vec3 a) noexcept { return {s * a.x, s * a.y, s * a.z}; }
[[nodiscard]] inline map_vec3 operator+(map_vec3 a, map_vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] inline double dot(map_vec3 a, map_vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline constexpr double map_pi = 3.14159265358979323846;
inline constexpr double map_deg = map_pi / 180.0;
inline constexpr double map_max_lat = 89.5 * map_deg; // the focus never sits on a pole (east would be undefined)

struct map_view {
    double lon0 = 0.0;   // focus longitude, radians
    double lat0 = 0.0;   // focus latitude, radians
    double scale = 300;  // pixels per radian on the central line
    double min_scale = 100, max_scale = 16000;

    struct frame {
        map_vec3 p, e, n;
    };
    [[nodiscard]] frame basis() const noexcept {
        const double cl = std::cos(lon0), sl = std::sin(lon0), cp = std::cos(lat0), sp = std::sin(lat0);
        return {{cp * cl, cp * sl, sp}, {-sl, cl, 0.0}, {-sp * cl, -sp * sl, cp}};
    }

    // Screen offset from the centre (pixels, y down) -> lon/lat (radians).
    void to_geo(double dx, double dy, double& lon, double& lat) const noexcept {
        const frame f = basis();
        const double u = dx / scale, v = -dy / scale;
        const double z = std::tanh(v), r = 1.0 / std::cosh(v);
        const map_vec3 x = (r * std::cos(u)) * f.p + (r * std::sin(u)) * f.e + z * f.n;
        lon = std::atan2(x.y, x.x);
        lat = std::asin(std::clamp(x.z, -1.0, 1.0));
    }

    // lon/lat (radians) -> screen offset from the centre (pixels, y down).
    void to_screen(double lon, double lat, double& dx, double& dy) const noexcept {
        const frame f = basis();
        const map_vec3 x{std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
        const double u = std::atan2(dot(x, f.e), dot(x, f.p));
        const double v = std::atanh(std::clamp(dot(x, f.n), -0.999999, 0.999999));
        dx = u * scale;
        dy = -v * scale;
    }

    void set_focus(double lon, double lat) noexcept {
        lon0 = std::remainder(lon, 2.0 * map_pi);
        lat0 = std::clamp(lat, -map_max_lat, map_max_lat);
    }

    // Drag by (dx, dy) pixels: the point that was at the centre moves with the
    // pointer, i.e. the new focus is what lay at centre - delta.
    void pan(double dx, double dy) noexcept {
        double lon = 0, lat = 0;
        to_geo(-dx, -dy, lon, lat);
        set_focus(lon, lat);
    }

    // Zoom by `factor` keeping the point under (mx, my) (offset from the centre) in place.
    void zoom_at(double factor, double mx, double my) noexcept {
        double lon = 0, lat = 0;
        to_geo(mx, my, lon, lat);
        scale = std::clamp(scale * factor, min_scale, max_scale);
        // The re-centred projection is not a pure scaling, so iterate the pan.
        for(int k = 0; k < 8; ++k) {
            double sx = 0, sy = 0;
            to_screen(lon, lat, sx, sy);
            pan(mx - sx, my - sy);
        }
    }

    // Zoom limits for a viewport: at the minimum, the whole oblique-longitude
    // circle (2 pi) spans the width; at the maximum, an ID-raster cell of
    // `cell_radians` is about 12 pixels.
    void fit_limits(double width, double cell_radians) noexcept {
        min_scale = width / (2.0 * map_pi);
        max_scale = std::max(min_scale * 2.0, 12.0 / cell_radians);
        scale = std::clamp(scale, min_scale, max_scale);
    }
};

} // namespace geosim::client
