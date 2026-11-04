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

    // Region table for the next frame: two uint32 per raster ID (map_layer::table).
    void set_table(std::vector<uint32_t> const& table) { table_ = table; }
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
    // Raster region index (id - 1) under the last pick point, -1 for none / water.
    [[nodiscard]] int picked_region() const noexcept;
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
    std::vector<uint32_t> table_;

    // Vector regions.
    bool vector_ = false, allow_vector_ = true;
    float tol_km_ = 0;
    gpu_buffer poly_verts_, poly_idx_;
    uint32_t poly_index_count_ = 0;
    gpu_image id_img_, resolved_img_;
    gpu_buffer pick_[frames_in_flight];
    VkDescriptorSetLayout ids_set_layout_ = VK_NULL_HANDLE, resolve_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pass_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet ids_set_ = VK_NULL_HANDLE, resolve_set_ = VK_NULL_HANDLE;
    VkPipelineLayout ids_layout_ = VK_NULL_HANDLE, resolve_layout_ = VK_NULL_HANDLE;
    VkPipeline ids_pipeline_ = VK_NULL_HANDLE, resolve_pipeline_ = VK_NULL_HANDLE;
    bool create_vector_pipelines(core::diagnostics& diag);
    void write_map_sets();
};

} // namespace geosim::client
