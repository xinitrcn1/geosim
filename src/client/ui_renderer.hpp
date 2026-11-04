#pragma once
// UI renderer (docs/08-ui.md#rendering): one instanced-quad pipeline per
// material, an offscreen scene image, and a downsampled separable Gaussian
// blur of it that frosted panels sample.
//
// Per frame:
//   1. scene pass   -> scene image: world map (map_renderer; the placeholder material
//                      until the map asset is loaded) + opaque panels (+ their text)
//   2. blur chain   -> scene -> 1/2 -> 1/4 (bilinear), horizontal + vertical Gaussian at 1/4
//   3. final pass   -> swapchain: scene copy, then overlay (frosted panels sample
//                      the blurred image under their rect, text, tooltips)
// One set of offscreen images is shared by the frames in flight: queue
// submission order plus the barriers below serialise their use.

#include "map_renderer.hpp"
#include "text.hpp"
#include "ui_draw.hpp"
#include "vk_context.hpp"

#include <string>

namespace geosim::client {

// The world map, drawn first in the scene pass (beneath everything, blurred by the glass).
struct map_pass {
    map_renderer* renderer = nullptr;
    map_view const* view = nullptr;
    bool political = true;
    map_anim anim{};
    int pick_x = -1, pick_y = -1; // pixel whose region is read back (vector map picking)
};

class ui_renderer {
public:
    bool init(vk_context& vk, core::diagnostics& diag);
    void shutdown();
    // Recreates the size-dependent images and descriptor sets (after a swapchain change).
    bool resize(core::diagnostics& diag);

    // Records the whole frame into vk.frame().cmd. `capture` additionally
    // copies the final image into a readback buffer (see save_capture).
    bool record(text_system& text, draw_list const& scene, draw_list const& overlay, float time, float scale, bool capture,
                core::diagnostics& diag, map_pass const& map = {});
    // Writes the last captured frame as binary PPM (call after the frame's fence / device idle).
    bool save_capture(std::string const& path, core::diagnostics& diag);

private:
    bool create_pipelines(core::diagnostics& diag);
    bool upload_atlas(text_system& text, core::diagnostics& diag);
    bool ensure_instance_capacity(uint32_t frame, std::size_t count, core::diagnostics& diag);
    void draw_batches(VkCommandBuffer cmd, draw_list const& list, uint32_t base, float time, float scale);
    void post_pass(VkCommandBuffer cmd, gpu_image const& dst, VkDescriptorSet src_set, gpu_image const& src, float dx, float dy,
                   float sigma, int radius);
    void destroy_targets();

    vk_context* vk_ = nullptr;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout ui_set_layout_ = VK_NULL_HANDLE;   // atlas, blur, scene
    VkDescriptorSetLayout post_set_layout_ = VK_NULL_HANDLE; // one source image
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_set_ = VK_NULL_HANDLE;       // final pass: atlas, blurred, scene
    VkDescriptorSet ui_scene_set_ = VK_NULL_HANDLE; // scene pass: the scene slot holds a dummy (it is the render target)
    VkDescriptorSet post_sets_[4] = {}; // sources: scene, half, quarter_a, quarter_b

    VkPipelineLayout quad_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout post_layout_ = VK_NULL_HANDLE;
    VkPipeline materials_[static_cast<int>(material::count)] = {};
    VkPipeline post_pipeline_ = VK_NULL_HANDLE;

    gpu_image atlas_;
    gpu_buffer atlas_staging_;
    bool atlas_initialised_ = false;
    gpu_image scene_, half_, quarter_a_, quarter_b_;
    gpu_buffer instances_[frames_in_flight];
    gpu_buffer readback_;
    VkExtent2D readback_extent_{};
};

} // namespace geosim::client
