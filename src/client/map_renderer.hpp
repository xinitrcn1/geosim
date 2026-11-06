#pragma once
// The world map on the GPU (docs/07-map-and-filters.md#rendering): the terrain
// and region-ID textures of map_data, a per-frame region table (nation key +
// fill colour per raster ID, rewritten every frame from map_layer), and one
// fullscreen pipeline (fullscreen.vert + world_map.frag). ui_renderer calls
// draw() first in its scene pass, so the glass panels blur the map.

#include "map_data.hpp"
#include "map_hazards.hpp"
#include "map_view.hpp"
#include "vk_context.hpp"

#include <cstdint>
#include <vector>

namespace geosim::client {

// Animation inputs of one frame.
struct map_anim {
    float cloud_days = 0;     // cloud advection clock (days)
    float seconds = 0;        // real time
    float cloud_strength = 0; // 0 = clouds off
    float day_of_year = 1;
};

class map_renderer {
public:
    // Pipeline and samplers; `format` is the scene image format.
    bool init(vk_context& vk, VkFormat format, core::diagnostics& diag);
    void shutdown();

    // Uploads the textures (waits for the GPU once). The CPU images may be freed afterwards.
    bool upload(map_data const& data, core::diagnostics& diag);
    [[nodiscard]] bool ready() const noexcept { return ready_; }

    // Region table for the next frame: three uint32 per region + 1 (map_layer::table: nation
    // key, fill, state key), expanded to the units the map draws (adding the cluster key).
    void set_table(std::vector<uint32_t> const& region_table);
    // Storms and earthquakes for the next frame (extra entries are dropped).
    void set_hazards(std::vector<map_storm> const& storms, std::vector<map_quake> const& quakes) {
        storms_ = storms;
        quakes_ = quakes;
    }

    // Vector regions (the asset's polygons, when the device has logic ops): the ID
    // pass (vertex-pulled ring fans XORed into an R32_UINT image) and the resolve
    // pass (one region per pixel, with border detail), recorded before the scene
    // pass; then the resolved region under (pick_x, pick_y) is copied back for
    // picking (read two frames later). No-op in raster mode.
    void prepass(VkCommandBuffer cmd, map_view const& view, float width, float height, int pick_x, int pick_y);
    [[nodiscard]] bool vector() const noexcept { return vector_; }
    // Call before upload: false keeps the raster regions (comparisons, devices without logic ops).
    void allow_vector(bool on) noexcept { allow_vector_ = on; }
    // Region (cluster) index under the last pick point, -1 for none / water.
    [[nodiscard]] int picked_region() const noexcept;
    // The Politics window's inset map (vector mode): the nation's geometry and the
    // fill colour per raster region id (RGBA8, index 0 unused), the screen rect the
    // UI's material::electoral quad covers this frame (w = 0: none). prepass draws
    // the ring fans once per inset into an ID image (plain Mercator, scissored to the
    // inset); draw_electoral colours it with white region borders and dark coasts.
    void set_electoral(std::shared_ptr<electoral_geometry const> geo, std::vector<uint32_t> colours);
    void set_electoral_rect(float x, float y, float w, float h) noexcept {
        elect_rect_[0] = x;
        elect_rect_[1] = y;
        elect_rect_[2] = w;
        elect_rect_[3] = h;
    }
    // Draws `count` electoral quads at `first` (instance buffer bound); false when not available.
    bool draw_electoral(VkCommandBuffer cmd, uint32_t first, uint32_t count, float const viewport[2], float time, float scale);
    // Region (cluster) under the pick point inside the electoral map, -1 none.
    [[nodiscard]] int picked_electoral() const noexcept;

    // Screen-size images of the vector passes (after a swapchain change).
    bool resize(core::diagnostics& diag);

    // Records the map into the current rendering (viewport/scissor already set).
    void draw(VkCommandBuffer cmd, map_view const& view, float width, float height, float line_scale, bool political,
              map_anim const& anim);

private:
    vk_context* vk_ = nullptr;
    bool ready_ = false;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE, repeat_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[frames_in_flight] = {};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    gpu_image terrain_, ids_, atlas_;
    gpu_buffer tables_[frames_in_flight];
    gpu_buffer hazards_[frames_in_flight];
    std::vector<map_storm> storms_;
    std::vector<map_quake> quakes_;
    uint32_t id_w_ = 0, id_h_ = 0, id_levels_ = 1;
    std::vector<uint32_t> table_;           // per unit + 1: nation key, fill, state key, cluster key
    std::vector<uint16_t> unit_region_;     // units -> regions (clusters)

    // Vector regions.
    bool vector_ = false, allow_vector_ = true;
    float tol_km_ = 0;
    gpu_buffer poly_verts_, poly_idx_;
    uint32_t poly_index_count_ = 0;
    // Levels of detail: the fine level is drawn per visible state chunk once zoomed in to
    // about a state (fine_px_per_km), else the coarse level for the whole world.
    uint32_t fine_index_count_ = 0;
    float coarse_tol_km_ = 0;
    struct chunk_cull {
        uint32_t first = 0, count = 0;
        float x = 0, y = 0, z = 0; // centre (unit vector)
        float radius = 0;          // angular radius (radians)
    };
    std::vector<chunk_cull> chunks_;
    static constexpr float fine_px_per_km = 0.25f;
    gpu_image id_img_, resolved_img_;
    gpu_buffer pick_[frames_in_flight];
    VkDescriptorSetLayout ids_set_layout_ = VK_NULL_HANDLE, resolve_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pass_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet ids_set_ = VK_NULL_HANDLE, resolve_set_ = VK_NULL_HANDLE;
    VkPipelineLayout ids_layout_ = VK_NULL_HANDLE, resolve_layout_ = VK_NULL_HANDLE;
    VkPipeline ids_pipeline_ = VK_NULL_HANDLE, resolve_pipeline_ = VK_NULL_HANDLE;
    bool create_vector_pipelines(core::diagnostics& diag);

    // Electoral inset map.
    std::shared_ptr<electoral_geometry const> elect_geo_;
    std::vector<uint32_t> elect_colours_;
    float elect_rect_[4] = {};
    bool elect_drawn_ = false; // the ID image holds this frame's insets
    gpu_buffer elect_idx_;
    uint32_t elect_index_count_ = 0;
    gpu_buffer elect_col_[frames_in_flight];
    gpu_image elect_img_;
    VkDescriptorSetLayout elect_ids_set_layout_ = VK_NULL_HANDLE, elect_draw_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet elect_ids_set_ = VK_NULL_HANDLE, elect_draw_sets_[frames_in_flight] = {};
    VkPipelineLayout elect_ids_layout_ = VK_NULL_HANDLE, elect_draw_layout_ = VK_NULL_HANDLE;
    VkPipeline elect_ids_pipeline_ = VK_NULL_HANDLE, elect_draw_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool elect_pool_ = VK_NULL_HANDLE;
    bool create_electoral_pipelines(core::diagnostics& diag);
    void write_electoral_sets();
    void write_map_sets();
};

} // namespace geosim::client
