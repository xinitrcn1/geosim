#include "vk_context.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>

namespace geosim::client {

namespace {

bool check(VkResult r, char const* what, core::diagnostics& diag) {
    if(r == VK_SUCCESS) {
        return true;
    }
    diag.fail(std::string(what) + " failed: " + vk_result_name(r));
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              VkDebugUtilsMessengerCallbackDataEXT const* data, void* user) {
    auto* ctx = static_cast<vk_context*>(user);
    ++ctx->validation_messages;
    if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++ctx->validation_errors;
    }
    SDL_Log("[vulkan %s] %s", (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning", data->pMessage);
    return VK_FALSE;
}

bool has_layer(char const* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [&](VkLayerProperties const& l) { return std::strcmp(l.layerName, name) == 0; });
}

bool has_instance_extension(char const* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    return std::any_of(exts.begin(), exts.end(), [&](VkExtensionProperties const& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

char const* vk_result_name(VkResult r) noexcept {
    switch(r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult(other)";
    }
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool vk_context::init(SDL_Window* window, bool validation, core::diagnostics& diag) {
    window_ = window;
    uint32_t loader_version = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loader_version);
    if(loader_version < VK_API_VERSION_1_3) {
        diag.fail("Vulkan loader is older than 1.3");
        return false;
    }

    Uint32 sdl_ext_count = 0;
    char const* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if(!sdl_exts) {
        diag.fail(std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError());
        return false;
    }
    std::vector<char const*> extensions(sdl_exts, sdl_exts + sdl_ext_count);
    std::vector<char const*> layers;
    const bool use_validation = validation && has_layer("VK_LAYER_KHRONOS_validation") && has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if(validation && !use_validation) {
        SDL_Log("validation requested but VK_LAYER_KHRONOS_validation / VK_EXT_debug_utils is not available");
    }
    if(use_validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "geosim";
    app.pEngineName = "geosim";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if(!check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance", diag)) {
        return false;
    }

    if(use_validation) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                          | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debug_callback;
        mci.pUserData = this;
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if(create) {
            create(instance, &mci, nullptr, &messenger_);
        }
        SDL_Log("Vulkan validation layer enabled");
    }

    if(!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        diag.fail(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
        return false;
    }
    if(!pick_device(diag)) {
        return false;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    // Logic ops (the vector map's XOR ID pass) when the device has them.
    {
        VkPhysicalDeviceFeatures have{};
        vkGetPhysicalDeviceFeatures(physical, &have);
        logic_op = have.logicOp == VK_TRUE;
        f2.features.logicOp = have.logicOp;
    }
    char const* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    if(!check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice", diag)) {
        return false;
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    for(auto& f : frames_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queue_family;
        if(!check(vkCreateCommandPool(device, &pci, nullptr, &f.pool), "vkCreateCommandPool", diag)) {
            return false;
        }
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if(!check(vkAllocateCommandBuffers(device, &cai, &f.cmd), "vkAllocateCommandBuffers", diag)) {
            return false;
        }
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if(!check(vkCreateSemaphore(device, &sci, nullptr, &f.image_acquired), "vkCreateSemaphore", diag)
           || !check(vkCreateFence(device, &fci, nullptr, &f.in_flight), "vkCreateFence", diag)) {
            return false;
        }
    }
    VkCommandPoolCreateInfo ipci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ipci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ipci.queueFamilyIndex = queue_family;
    return check(vkCreateCommandPool(device, &ipci, nullptr, &immediate_pool_), "vkCreateCommandPool", diag);
}

bool vk_context::pick_device(core::diagnostics& diag) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if(count == 0) {
        diag.fail("no Vulkan physical device found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    int best_score = -1;
    std::string rejected;
    for(auto pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        if(props.apiVersion < VK_API_VERSION_1_3 || !f13.dynamicRendering || !f13.synchronization2) {
            rejected += std::string(" ") + props.deviceName + " (no Vulkan 1.3 / dynamic rendering);";
            continue;
        }
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());
        if(std::none_of(exts.begin(), exts.end(), [](auto const& e) { return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; })) {
            rejected += std::string(" ") + props.deviceName + " (no swapchain);";
            continue;
        }
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qprops.data());
        for(uint32_t q = 0; q < qcount; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface, &present);
            if(!(qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) {
                continue;
            }
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
            if(score > best_score) {
                best_score = score;
                physical = pd;
                queue_family = q;
                device_name = props.deviceName;
            }
            break;
        }
        if(physical != pd) {
            rejected += std::string(" ") + props.deviceName + " (no graphics+present queue);";
        }
    }
    if(best_score < 0) {
        diag.fail("no suitable Vulkan 1.3 device:" + rejected);
        return false;
    }
    return true;
}

bool vk_context::create_swapchain(uint32_t width, uint32_t height, core::diagnostics& diag) {
    vkDeviceWaitIdle(device);
    VkSurfaceCapabilitiesKHR caps;
    if(!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", diag)) {
        return false;
    }
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &fmt_count, formats.data());
    if(formats.empty()) {
        diag.fail("surface reports no formats");
        return false;
    }
    // The UI works in display (gamma) space: colours in style.hpp are sRGB
    // values written as-is, so prefer a UNORM swapchain.
    VkSurfaceFormatKHR chosen = formats[0];
    for(auto const& f : formats) {
        if((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    VkExtent2D extent = caps.currentExtent;
    if(extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if(extent.width == 0 || extent.height == 0) {
        diag.fail("swapchain extent is zero");
        return false;
    }
    uint32_t image_count = caps.minImageCount + 1;
    if(caps.maxImageCount > 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }
    swap_readable = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (swap_readable ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) : VkImageUsageFlags{0});
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                           : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available; vsync
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    if(!check(vkCreateSwapchainKHR(device, &sci, nullptr, &fresh), "vkCreateSwapchainKHR", diag)) {
        return false;
    }
    destroy_swapchain_views();
    if(swapchain) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }
    swapchain = fresh;
    swap_format = chosen.format;
    swap_extent = extent;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
    swap_images.resize(n);
    vkGetSwapchainImagesKHR(device, swapchain, &n, swap_images.data());
    swap_views.resize(n);
    render_done_.resize(n);
    for(uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = swap_images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swap_format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if(!check(vkCreateImageView(device, &vci, nullptr, &swap_views[i]), "vkCreateImageView", diag)) {
            return false;
        }
        VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if(!check(vkCreateSemaphore(device, &semi, nullptr, &render_done_[i]), "vkCreateSemaphore", diag)) {
            return false;
        }
    }
    return true;
}

void vk_context::destroy_swapchain_views() {
    for(auto v : swap_views) {
        vkDestroyImageView(device, v, nullptr);
    }
    for(auto s : render_done_) {
        vkDestroySemaphore(device, s, nullptr);
    }
    swap_views.clear();
    render_done_.clear();
}

acquire_status vk_context::begin_frame(core::diagnostics& diag) {
    auto& f = frames_[frame_index_];
    vkWaitForFences(device, 1, &f.in_flight, VK_TRUE, UINT64_MAX);
    const VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.image_acquired, VK_NULL_HANDLE, &image_index_);
    if(r == VK_ERROR_OUT_OF_DATE_KHR) {
        return acquire_status::out_of_date;
    }
    if(r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        check(r, "vkAcquireNextImageKHR", diag);
        return acquire_status::failed;
    }
    vkResetFences(device, 1, &f.in_flight);
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    return acquire_status::ok;
}

acquire_status vk_context::end_frame(core::diagnostics& diag) {
    auto& f = frames_[frame_index_];
    vkEndCommandBuffer(f.cmd);

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.image_acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = render_done_[image_index_];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    if(!check(vkQueueSubmit2(queue, 1, &si, f.in_flight), "vkQueueSubmit2", diag)) {
        return acquire_status::failed;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_done_[image_index_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &image_index_;
    const VkResult r = vkQueuePresentKHR(queue, &pi);
    frame_index_ = (frame_index_ + 1) % frames_in_flight;
    if(r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        return acquire_status::out_of_date;
    }
    if(r != VK_SUCCESS) {
        check(r, "vkQueuePresentKHR", diag);
        return acquire_status::failed;
    }
    return acquire_status::ok;
}

namespace {
bool find_memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for(uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
            out = i;
            return true;
        }
    }
    return false;
}
} // namespace

bool vk_context::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, gpu_buffer& out, core::diagnostics& diag) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if(!check(vkCreateBuffer(device, &bci, nullptr, &out.buffer), "vkCreateBuffer", diag)) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    const VkMemoryPropertyFlags want = host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    if(!find_memory_type(physical, req.memoryTypeBits, want, mai.memoryTypeIndex)) {
        diag.fail("no suitable memory type for buffer");
        return false;
    }
    if(!check(vkAllocateMemory(device, &mai, nullptr, &out.memory), "vkAllocateMemory", diag)) {
        return false;
    }
    vkBindBufferMemory(device, out.buffer, out.memory, 0);
    out.size = size;
    if(host_visible && !check(vkMapMemory(device, out.memory, 0, size, 0, &out.mapped), "vkMapMemory", diag)) {
        return false;
    }
    return true;
}

void vk_context::destroy_buffer(gpu_buffer& b) {
    if(b.buffer) {
        vkDestroyBuffer(device, b.buffer, nullptr);
    }
    if(b.memory) {
        vkFreeMemory(device, b.memory, nullptr);
    }
    b = {};
}

bool vk_context::create_image(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, gpu_image& out,
                              core::diagnostics& diag, uint32_t mip_levels, uint32_t layers) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {std::max(1u, width), std::max(1u, height), 1};
    ici.mipLevels = std::max(1u, mip_levels);
    ici.arrayLayers = std::max(1u, layers);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if(!check(vkCreateImage(device, &ici, nullptr, &out.image), "vkCreateImage", diag)) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, out.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    if(!find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mai.memoryTypeIndex)
       && !find_memory_type(physical, req.memoryTypeBits, 0, mai.memoryTypeIndex)) {
        diag.fail("no suitable memory type for image");
        return false;
    }
    if(!check(vkAllocateMemory(device, &mai, nullptr, &out.memory), "vkAllocateMemory", diag)) {
        return false;
    }
    vkBindImageMemory(device, out.image, out.memory, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out.image;
    vci.viewType = ici.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, ici.mipLevels, 0, ici.arrayLayers};
    if(!check(vkCreateImageView(device, &vci, nullptr, &out.view), "vkCreateImageView", diag)) {
        return false;
    }
    out.extent = {ici.extent.width, ici.extent.height};
    out.format = format;
    out.mip_levels = ici.mipLevels;
    return true;
}

void vk_context::destroy_image(gpu_image& img) {
    if(img.view) {
        vkDestroyImageView(device, img.view, nullptr);
    }
    if(img.image) {
        vkDestroyImage(device, img.image, nullptr);
    }
    if(img.memory) {
        vkFreeMemory(device, img.memory, nullptr);
    }
    img = {};
}

VkCommandBuffer vk_context::begin_immediate() {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = immediate_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

bool vk_context::end_immediate(VkCommandBuffer cmd, core::diagnostics& diag) {
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    const bool ok = check(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2 (immediate)", diag);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, immediate_pool_, 1, &cmd);
    return ok;
}

VkShaderModule vk_context::create_shader(uint32_t const* code, std::size_t bytes, core::diagnostics& diag) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule", diag);
    return m;
}

void vk_context::shutdown() {
    if(device) {
        vkDeviceWaitIdle(device);
        for(auto& f : frames_) {
            if(f.in_flight) vkDestroyFence(device, f.in_flight, nullptr);
            if(f.image_acquired) vkDestroySemaphore(device, f.image_acquired, nullptr);
            if(f.pool) vkDestroyCommandPool(device, f.pool, nullptr);
            f = {};
        }
        if(immediate_pool_) vkDestroyCommandPool(device, immediate_pool_, nullptr);
        immediate_pool_ = VK_NULL_HANDLE;
        destroy_swapchain_views();
        if(swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if(instance) {
        if(surface) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
        if(messenger_) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if(destroy) destroy(instance, messenger_, nullptr);
            messenger_ = VK_NULL_HANDLE;
        }
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

} // namespace geosim::client
