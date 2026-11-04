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

    // Records the map into the current rendering (viewport/scissor already set).
    void draw(VkCommandBuffer cmd, map_view const& view, float width, float height, float line_scale, bool political,
              map_anim const& anim);

private:
    vk_context* vk_ = nullptr;
    bool ready_ = false;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[frames_in_flight] = {};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    gpu_image terrain_, ids_;
    gpu_buffer tables_[frames_in_flight];
    gpu_buffer hazards_[frames_in_flight];
    std::vector<map_storm> storms_;
    std::vector<map_quake> quakes_;
    uint32_t id_w_ = 0, id_h_ = 0, id_levels_ = 1;
    std::vector<uint32_t> table_;
};

} // namespace geosim::client
