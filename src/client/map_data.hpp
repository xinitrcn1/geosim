#pragma once
// The world map asset (assets/map/world.gsmap, built by tools/pipeline/build_map.py;
// format in that script and docs/07-map-and-filters.md#data) decoded into the
// CPU images the map renderer uploads:
//  - ids:     uint16 region raster, coarse (<= 2048 wide; 0 = water, 0xFFFF =
//             land of no region, else 1 + region index), point-sampled mips
//  - terrain: RGBA8, rgb = Blue-Marble-like land colour from the Koppen-Geiger
//             class, relief and latitude (snow, ice, rock), a = relief code
//             (the shader turns it into the ocean depth ramp), with box mips
// Decoding and colouring take about a second, so map_loader runs them on a
// worker thread. No exceptions: errors go to core::diagnostics.

#include "core/diagnostics.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace geosim::client {

inline constexpr uint16_t map_water = 0;
inline constexpr uint16_t map_unclaimed = 0xFFFF;

// The ID-pass code of a raster region id: the id with a 15-bit checksum above it, so
// a pixel covered by two overlapping regions (XOR of two codes) is recognisable.
[[nodiscard]] constexpr uint32_t map_region_code(uint32_t id) noexcept {
    return (id & 0xFFFFu) | ((((id * 2654435761u) >> 17) & 0x7FFFu) << 16);
}
inline constexpr uint32_t map_lake_code = 0x80000000u;

struct map_nation {
    std::array<char, 4> iso3{}; // NUL-terminated
    uint8_t r = 0, g = 0, b = 0;
    float lat = 0, lon = 0;     // label / default-view point, degrees
};

struct map_region {
    std::array<char, 13> code{}; // regions.csv id, e.g. "AFG-003"
    uint16_t nation = 0;         // index into map_data::nations (the owner in world2020)
    float lat = 0, lon = 0;      // degrees, as in regions.csv
};

// A region's bounding box from its polygons, degrees; lon1 may exceed 180 for a
// region across the antimeridian (lon0 < lon1 always).
struct map_box {
    float lon0 = 0, lon1 = 0, lat0 = 0, lat1 = 0;
    uint32_t cells = 0; // land area in km2 (from the polygons)
};

// One ring of the vector regions: vertices [first, first + count) of poly_vertices.
struct map_ring {
    uint32_t first = 0, count = 0;
    int32_t region = -1; // region index (raster id - 1), -1 for a lake
};

template <typename T>
struct map_level {
    uint32_t width = 0, height = 0;
    std::vector<T> texels;
};

struct map_data {
    std::vector<map_nation> nations;
    std::vector<map_region> regions;
    std::vector<map_box> boxes;               // per region, from the polygons
    // Vector regions (the POLY section; empty for older assets): per vertex four
    // words {lon, lat (float bits, degrees), index of its ring's first vertex,
    // code}, and triangle fans per ring (indices into the vertices). code =
    // map_region_code(raster id) for regions, map_lake_code for lakes; drawn with
    // a XOR logic op, so even-odd filling needs no triangulation.
    std::vector<uint32_t> poly_vertices;
    std::vector<uint32_t> poly_indices;
    std::vector<map_ring> rings;
    float poly_tolerance_deg = 0;
    // Terrain detail atlas (ATLS): `atlas_layers` tileable grayscale layers of
    // atlas_size^2, mip chain per layer (level k is atlas_size >> k), all levels of
    // layer 0 first. Layers: sand, grass, forest, tundra, rock, snow, wetland, water.
    uint32_t atlas_size = 0, atlas_layers = 0, atlas_levels = 0;
    std::vector<std::vector<uint8_t>> atlas; // [layer * atlas_levels + level]
    std::vector<map_level<uint16_t>> ids;     // level 0 = full resolution
    std::vector<map_level<uint32_t>> terrain; // RGBA8 packed little-endian (r in the low byte)

    [[nodiscard]] int find_nation(std::string const& iso3) const noexcept;
};

// Reads and decodes the asset; false (with diag) when it is missing or corrupt.
bool load_map_data(std::string const& path, map_data& out, core::diagnostics& diag);

// Runs load_map_data on a worker thread; the UI thread polls ready().
class map_loader {
public:
    void start(std::string path);
    [[nodiscard]] bool ready() const noexcept { return done_.load(std::memory_order_acquire); }
    // Valid once ready(): the data (moved out once) or the error.
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::string const& error() const noexcept { return error_; }
    [[nodiscard]] std::unique_ptr<map_data> take() { return std::move(data_); }

private:
    std::unique_ptr<map_data> data_;
    std::string error_;
    bool ok_ = false;
    std::atomic<bool> done_{false};
    std::jthread thread_;
};

} // namespace geosim::client
