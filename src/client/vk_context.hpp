#pragma once
// Vulkan 1.3 device, swapchain and frame synchronisation (docs/08-ui.md#rendering).
//
// Owns: instance (+ optional validation layer), SDL surface, one graphics+present
// queue, the swapchain, and `frames_in_flight` command buffers with their fences.
// Uses core 1.3 dynamic rendering and synchronization2; no render passes.
// Errors are reported through core::diagnostics (no exceptions).

#include "core/diagnostics.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace geosim::client {

inline constexpr uint32_t frames_in_flight = 2;

struct gpu_buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr; // host-visible buffers stay mapped
};

struct gpu_image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t mip_levels = 1;
};

struct frame_data {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore image_acquired = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
};

enum class acquire_status { ok, out_of_date, failed };

class vk_context {
public:
    vk_context() = default;
    vk_context(vk_context const&) = delete;
    vk_context& operator=(vk_context const&) = delete;
    ~vk_context() { shutdown(); }

    // Creates everything up to (not including) the swapchain.
    bool init(SDL_Window* window, bool validation, core::diagnostics& diag);
    void shutdown();

    // (Re)creates the swapchain for a framebuffer of width x height pixels.
    // Waits for the device to go idle first.
    bool create_swapchain(uint32_t width, uint32_t height, core::diagnostics& diag);

    // Frame protocol: begin_frame waits for the frame slot, acquires an image
    // and begins the command buffer; end_frame submits and presents.
    acquire_status begin_frame(core::diagnostics& diag);
    acquire_status end_frame(core::diagnostics& diag);

    [[nodiscard]] frame_data& frame() noexcept { return frames_[frame_index_]; }
    [[nodiscard]] uint32_t frame_index() const noexcept { return frame_index_; }
    [[nodiscard]] uint32_t image_index() const noexcept { return image_index_; }

    // Resource helpers.
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, gpu_buffer& out, core::diagnostics& diag);
    void destroy_buffer(gpu_buffer& b);
    bool create_image(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, gpu_image& out,
                      core::diagnostics& diag, uint32_t mip_levels = 1, uint32_t layers = 1); // layers > 1: a 2D array view
    void destroy_image(gpu_image& img);
    // Records into a temporary command buffer, submits it and waits (uploads).
    VkCommandBuffer begin_immediate();
    bool end_immediate(VkCommandBuffer cmd, core::diagnostics& diag);

    VkShaderModule create_shader(uint32_t const* code, std::size_t bytes, core::diagnostics& diag);

    // Public handles (read-only by convention).
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent{};
    bool swap_readable = false; // swapchain images allow TRANSFER_SRC (screenshots)
    bool logic_op = false;      // VkPhysicalDeviceFeatures::logicOp enabled (vector map ID pass)
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    std::string device_name;
    uint32_t validation_messages = 0; // warnings + errors reported by the validation layer
    uint32_t validation_errors = 0;

private:
    bool pick_device(core::diagnostics& diag);
    void destroy_swapchain_views();

    SDL_Window* window_ = nullptr;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkCommandPool immediate_pool_ = VK_NULL_HANDLE;
    frame_data frames_[frames_in_flight];
    std::vector<VkSemaphore> render_done_; // one per swapchain image
    uint32_t frame_index_ = 0;
    uint32_t image_index_ = 0;
};

// Synchronization2 image barrier on the colour aspect of a single-mip image.
void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

[[nodiscard]] char const* vk_result_name(VkResult r) noexcept;

} // namespace geosim::client
