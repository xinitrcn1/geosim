#pragma once
// The political layer (docs/07-map-and-filters.md#political-view): which nation
// controls each raster region, and its fill colour, as the region table the
// map shader reads (two uint32 per raster ID: nation key, fill RGBA8).
//
// Raster regions are the world2020 regions of the map asset. They are bound to
// the scenario's regions once (sim_runner::request_map_world): within the same
// nation (ISO3), the sim region nearest to the raster region's centroid. A
// single-region nation (the 2020 / 2008 slices) therefore takes all its raster
// regions; nations the scenario does not simulate (folded into an aggregate)
// keep their world2020 owner and colour. Until the scenario is loaded the asset's own owners are shown.
// Ownership is re-read from the sim (racy, like the indicators) a few times a
// second, so a region that changes hands takes its new owner's colour and the
// national borders move with it.

#include "map_data.hpp"
#include "sim_runner.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <vector>

namespace geosim::client {

class map_layer {
public:
    // Takes the asset's region and nation tables (the images are not needed).
    void init(map_data const& data);
    // Polls the sim: binds once it is running, refreshes owners every ~250 ms.
    // Returns true when table() changed.
    bool update(sim_runner& sim);
    [[nodiscard]] std::vector<uint32_t> const& table() const noexcept { return table_; }

    static constexpr float fill_strength = 0.72f;

private:
    void rebuild();

    std::vector<map_nation> nations_;
    std::vector<map_region> regions_;
    std::future<sim_map_world> pending_;
    bool requested_ = false, bound_ = false;
    std::vector<int32_t> sim_region_of_;   // raster region -> sim region index (-1 = not simulated)
    std::vector<uint32_t> sim_nation_rgb_; // sim nation index -> fill colour (RGB8)
    std::vector<uint16_t> owners_;         // sim region -> owner nation index
    std::vector<uint32_t> table_;
    std::chrono::steady_clock::time_point last_read_{};
};

} // namespace geosim::client
