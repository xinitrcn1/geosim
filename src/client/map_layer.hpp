#pragma once
// The map's thematic layer (docs/07-map-and-filters.md#views): per raster
// region, the nation key (borders) and the fill colour the shader blends over
// the terrain, for the selected view; plus picking and the text of the hover
// tooltips and the view tabs.
//
// Raster regions are the world2020 regions of the map asset. They are bound to
// the scenario's regions once (sim_runner::request_map_world): within the same
// nation (ISO3), the sim region nearest to the raster region's centroid. A
// single-region nation (the 2020 / 2008 slices) therefore takes all its raster
// regions; nations the scenario does not simulate (folded into an aggregate)
// keep their world2020 owner and colour. Until the scenario is loaded the
// asset's own owners are shown. Ownership is re-read every 250 ms (racy reads
// like the indicators), so a region that changes hands takes its new owner's
// colour and the national borders move with it.
//
// Views other than political read a region_snapshot (per sim region and per
// nation, copied at the sim's sync point every second). National values apply
// to all of a nation's regions where the sim only keeps them per nation.

#include "hazard_feed.hpp"
#include "map_data.hpp"
#include "sim_runner.hpp"
#include "ui_layout.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace geosim::client {

class map_layer {
public:
    // Takes the asset's tables and the vector rings (landmass groups); keeps a copy of
    // the coarse ID raster only when `raster_picking` (the raster mode has no GPU
    // picking); reads the world2020 region names from `assets`.
    void init(map_data& data, std::string const& assets, bool raster_picking);
    // Polls the sim; returns true when table() changed.
    bool update(sim_runner& sim, hazard_feed const& hazards, map_filter view);
    // Per region + 1: nation key, fill RGBA8, state key (map_renderer::set_table).
    [[nodiscard]] std::vector<uint32_t> const& table() const noexcept { return table_; }

    // Raster region (index into the asset's regions) at lon/lat (degrees), or -1 on water.
    [[nodiscard]] int pick(double lon_deg, double lat_deg) const noexcept;
    [[nodiscard]] map_tooltip_info describe(int region, map_filter view, sim_runner const& sim) const;
    [[nodiscard]] map_tab_info tab(map_filter view, sim_runner const& sim, hazard_feed const& hazards) const;

    // Politics tab: the nation shown (resolves -1 to the player's first nation),
    // its electoral tile map for `mode` and a tile's tooltip.
    [[nodiscard]] int resolve_nation(int nation, int step, sim_runner const& sim) const;
    void update_politics(sim_runner& sim, int nation);
    [[nodiscard]] electoral_view_info electoral(int nation, int mode, sim_runner const& sim) const;
    [[nodiscard]] map_tooltip_info describe_tile(int region, int mode, sim_runner const& sim) const;
    // Sim nation controlling a raster region (-1 = not simulated).
    [[nodiscard]] int nation_at(int region) const noexcept { return owner_of(region); }
    [[nodiscard]] int sim_region(int raster) const noexcept;
    [[nodiscard]] int owner_of(int raster) const noexcept; // sim nation index, -1 when not simulated

    // Sim nation index of an ISO3 code (-1 until bound / unknown).
    [[nodiscard]] int nation_index(std::string const& iso) const noexcept {
        for(std::size_t k = 0; k < nation_iso3_.size(); ++k) {
            if(nation_iso3_[k] == iso) return static_cast<int>(k);
        }
        return -1;
    }

    static constexpr float fill_strength = 0.72f;

    // Hazard state of a raster region in the climate view.
    enum hazard : uint8_t { none = 0, quake, storm, flood, drought };
    struct hazard_state {
        hazard kind = none;
        bool now = false; // happening (else potential)
    };

private:
    void rebuild(map_filter view, hazard_feed const& hazards);
    [[nodiscard]] std::string iso_of(int nation) const {
        return nation >= 0 && static_cast<std::size_t>(nation) < nation_iso3_.size() ? nation_iso3_[static_cast<std::size_t>(nation)] : std::string{};
    }
    [[nodiscard]] hazard_state climate_state(int raster, hazard_feed const& hazards) const;

    std::vector<map_nation> nations_;
    std::vector<map_region> regions_;
    std::vector<std::string> names_; // raster region names (world2020 regions.csv)
    std::vector<uint16_t> ids_;      // full-resolution region raster (0 water, 0xFFFF unclaimed)
    uint32_t ids_w_ = 0, ids_h_ = 0;

    std::future<sim_map_world> pending_;
    bool requested_ = false, bound_ = false;
    std::vector<int32_t> sim_region_of_;   // raster region -> sim region index (-1 = not simulated)
    std::vector<uint32_t> sim_nation_rgb_; // sim nation index -> fill colour (RGB8)
    std::vector<uint16_t> owners_;         // sim region -> owner nation index
    std::vector<std::string> nation_iso3_; // sim nation index -> ISO3
    std::vector<uint32_t> table_;
    std::chrono::steady_clock::time_point last_read_{}, last_snapshot_{};

    std::future<region_snapshot> pending_regions_;
    region_snapshot snap_;
    bool have_snap_ = false;
    std::vector<fault_row> faults_;
    std::vector<float> exports0_;         // nation exports % of GDP at the first snapshot
    std::vector<uint8_t> fault_risk_;     // raster region near a fault late in its cycle
    float grievance_q90_ = 1e9f;

    // Electoral map.
    std::vector<map_box> boxes_;
    std::future<politics_snapshot> pending_politics_;
    politics_snapshot politics_;
    int politics_for_ = -1;
    std::chrono::steady_clock::time_point last_politics_{};
    std::vector<std::vector<float>> start_votes_; // nation -> national vote shares at first sight (swing)
    std::vector<float> lonlat_;    // vector-map vertices (lon, lat degrees), for the landmass groups
    std::vector<map_ring> rings_;
    mutable std::vector<std::shared_ptr<electoral_geometry const>> insets_; // per nation, built on first use
    [[nodiscard]] std::shared_ptr<electoral_geometry const> build_insets(int nation) const;

public:
    // The Politics window's inset geometry for a nation (cached), for the renderer.
    [[nodiscard]] std::shared_ptr<electoral_geometry const> electoral_geometry_of(int nation) const { return nation < 0 ? nullptr : build_insets(nation); }

private:
    [[nodiscard]] politics_region const* politics_of(int raster) const noexcept;
    [[nodiscard]] bool democracy() const noexcept;
    map_filter last_view_ = map_filter::count;
};

} // namespace geosim::client
