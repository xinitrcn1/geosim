#pragma once
// The world map asset (assets/map/world.gsmap, built by tools/pipeline/build_map.py;
// format in that script and docs/07-map-and-filters.md#data) decoded into the
// CPU images the map renderer uploads:
//  - ids:     uint16 region raster (0 = water, 0xFFFF = land of no region, else
//             1 + region index), with a point-sampled mip chain
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

template <typename T>
struct map_level {
    uint32_t width = 0, height = 0;
    std::vector<T> texels;
};

struct map_data {
    std::vector<map_nation> nations;
    std::vector<map_region> regions;
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
