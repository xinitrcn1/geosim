#include "map_renderer.hpp"

#include "electoral.frag.h"
#include "fullscreen.vert.h"
#include "map_elect.vert.h"
#include "quad.vert.h"
#include "ui_draw.hpp"
#include "map_ids.frag.h"
#include "map_ids.vert.h"
#include "map_resolve.frag.h"
#include "world_map.frag.h"

#include <SDL3/SDL_log.h>

#include <algorithm>

#include <cstring>

namespace geosim::client {

namespace {

// Keep in sync with world_map.frag (map_block).
struct map_push {
    float viewport[2];
    float centre[2];
    float p[4]; // focus, w = scale
    float e[4]; // east, w = line scale
    float n[4]; // north
    uint32_t flags;
    uint32_t id_levels;
    uint32_t id_w, id_h;
    float anim[4];   // cloud days, seconds, cloud strength, day of year
    uint32_t counts[4]; // storms, quakes
};
static_assert(sizeof(map_push) == 112);

// std140 layout of world_map.frag hazard_block.
struct hazard_ubo {
    float storm[map_max_storms][4];
    float quake[map_max_quakes][4];
    float quake_t[map_max_quakes][4];
};

bool vk_ok(VkResult r, char const* what, core::diagnostics& diag) {
    if(r == VK_SUCCESS) return true;
    diag.fail(std::string(what) + " failed: " + vk_result_name(r));
    return false;
}

void mip_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                 VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, uint32_t layers = 1) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Uploads every mip level of `levels` (texels of T) into a new image.
template <typename T>
bool upload_levels(vk_context& vk, std::vector<map_level<T>> const& levels, VkFormat format, gpu_image& out, core::diagnostics& diag) {
    VkDeviceSize total = 0;
    for(auto const& l : levels) total += l.texels.size() * sizeof(T);
    gpu_buffer staging;
    if(!vk.create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging, diag)) return false;
    if(!vk.create_image(levels[0].width, levels[0].height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, out, diag,
                        static_cast<uint32_t>(levels.size()))) {
        vk.destroy_buffer(staging);
        return false;
    }
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize at = 0;
    for(std::size_t i = 0; i < levels.size(); ++i) {
        auto const& l = levels[i];
        std::memcpy(static_cast<uint8_t*>(staging.mapped) + at, l.texels.data(), l.texels.size() * sizeof(T));
        VkBufferImageCopy c{};
        c.bufferOffset = at;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(i), 0, 1};
        c.imageExtent = {l.width, l.height, 1};
        copies.push_back(c);
        at += l.texels.size() * sizeof(T);
    }
    VkCommandBuffer cmd = vk.begin_immediate();
    mip_barrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()),
                           copies.data());
    mip_barrier(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const bool ok = vk.end_immediate(cmd, diag);
    vk.destroy_buffer(staging);
    return ok;
}

// Push block of the ID and resolve passes (map_ids.vert, map_resolve.frag).
struct pass_push {
    float viewport[2];
    float centre[2];
    float p[4]; // focus, w = scale
    float e[4]; // east, w = tolerance (km; resolve)
    float n[4];
    uint32_t raster[4]; // raster w, h (resolve)
};
static_assert(sizeof(pass_push) == 80);

// A pipeline without vertex input for one colour attachment of `format`.
bool make_pipeline(VkDevice dev, VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, VkFormat format, bool xor_op,
                   VkPipeline& out, core::diagnostics& diag) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    cb.logicOpEnable = xor_op ? VK_TRUE : VK_FALSE;
    cb.logicOp = VK_LOGIC_OP_XOR;
    const VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rendering;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = layout;
    return vk_ok(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &out), "vkCreateGraphicsPipelines", diag);
}

// The terrain detail atlas as an R8 2D array with mips (a 2-layer 1x1 grey
// stand-in when the asset has none).
bool upload_atlas(vk_context& vk, map_data const& data, gpu_image& out, core::diagnostics& diag) {
    const bool have = data.atlas_layers > 0 && !data.atlas.empty();
    const uint32_t size = have ? data.atlas_size : 1, layers = have ? data.atlas_layers : 2, levels = have ? data.atlas_levels : 1;
    VkDeviceSize total = 0;
    if(have) {
        for(auto const& l : data.atlas) total += l.size();
    } else {
        total = 2;
    }
    gpu_buffer staging;
    if(!vk.create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging, diag)) return false;
    if(!vk.create_image(size, size, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, out, diag, levels, layers)) {
        vk.destroy_buffer(staging);
        return false;
    }
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize at = 0;
    auto* dst = static_cast<uint8_t*>(staging.mapped);
    for(uint32_t l = 0; l < layers; ++l) {
        for(uint32_t k = 0; k < levels; ++k) {
            const uint32_t s = std::max(1u, size >> k);
            VkBufferImageCopy c{};
            c.bufferOffset = at;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, k, l, 1};
            c.imageExtent = {s, s, 1};
            copies.push_back(c);
            if(have) {
                auto const& src = data.atlas[std::size_t{l} * levels + k];
                std::memcpy(dst + at, src.data(), src.size());
                at += src.size();
            } else {
                dst[at++] = 128;
            }
        }
    }
    VkCommandBuffer cmd = vk.begin_immediate();
    mip_barrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, layers);
    vkCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());
    mip_barrier(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, layers);
    const bool ok = vk.end_immediate(cmd, diag);
    vk.destroy_buffer(staging);
    return ok;
}

struct elect_push {
    float viewport[2];
    float ref_lon;
    float pad;
    float dst[4];
    float src[4];
};
static_assert(sizeof(elect_push) == 48);

} // namespace

bool map_renderer::create_electoral_pipelines(core::diagnostics& diag) {
    VkDevice dev = vk_->device;
    const VkDescriptorSetLayoutBinding ids_b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding draw_b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = ids_b;
    if(!vk_ok(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &elect_ids_set_layout_), "vkCreateDescriptorSetLayout", diag)) return false;
    dl.pBindings = draw_b;
    if(!vk_ok(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &elect_draw_set_layout_), "vkCreateDescriptorSetLayout", diag)) return false;
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 + frames_in_flight}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frames_in_flight}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1 + frames_in_flight;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if(!vk_ok(vkCreateDescriptorPool(dev, &pci, nullptr, &elect_pool_), "vkCreateDescriptorPool", diag)) return false;
    VkPushConstantRange ids_pr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(elect_push)};
    VkPushConstantRange draw_pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16}; // the UI's quad push block
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pushConstantRangeCount = 1;
    plci.pSetLayouts = &elect_ids_set_layout_;
    plci.pPushConstantRanges = &ids_pr;
    if(!vk_ok(vkCreatePipelineLayout(dev, &plci, nullptr, &elect_ids_layout_), "vkCreatePipelineLayout", diag)) return false;
    plci.pSetLayouts = &elect_draw_set_layout_;
    plci.pPushConstantRanges = &draw_pr;
    if(!vk_ok(vkCreatePipelineLayout(dev, &plci, nullptr, &elect_draw_layout_), "vkCreatePipelineLayout", diag)) return false;
    VkShaderModule ids_vs = vk_->create_shader(spv_map_elect_vert, sizeof(spv_map_elect_vert), diag);
    VkShaderModule ids_fs = vk_->create_shader(spv_map_ids_frag, sizeof(spv_map_ids_frag), diag);
    bool ok = !diag.failed() && make_pipeline(dev, ids_vs, ids_fs, elect_ids_layout_, VK_FORMAT_R32_UINT, true, elect_ids_pipeline_, diag);
    for(VkShaderModule m : {ids_vs, ids_fs}) {
        if(m) vkDestroyShaderModule(dev, m, nullptr);
    }
    if(!ok) return false;
    // The quad material pipeline: the UI's vertex shader and instance format, alpha blending.
    VkShaderModule qvs = vk_->create_shader(spv_quad_vert, sizeof(spv_quad_vert), diag);
    VkShaderModule qfs = vk_->create_shader(spv_electoral_frag, sizeof(spv_electoral_frag), diag);
    if(diag.failed()) return false;
    VkVertexInputBindingDescription binding{0, sizeof(quad_instance), VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attrs[] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(quad_instance, x)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(quad_instance, u0)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(quad_instance, color)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(quad_instance, border_color)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(quad_instance, border_width)},
        {5, 0, VK_FORMAT_R32_UINT, offsetof(quad_instance, flags)},
    };
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, qvs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, qfs, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attrs));
    vin.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;
    const VkFormat format = vk_->swap_format;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rendering;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = elect_draw_layout_;
    ok = vk_ok(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &elect_draw_pipeline_), "vkCreateGraphicsPipelines", diag);
    vkDestroyShaderModule(dev, qvs, nullptr);
    vkDestroyShaderModule(dev, qfs, nullptr);
    return ok;
}

bool map_renderer::create_vector_pipelines(core::diagnostics& diag) {
    VkDevice dev = vk_->device;
    const VkDescriptorSetLayoutBinding ids_b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
    };
    const VkDescriptorSetLayoutBinding res_b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = ids_b;
    if(!vk_ok(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &ids_set_layout_), "vkCreateDescriptorSetLayout", diag)) return false;
    dl.bindingCount = 3;
    dl.pBindings = res_b;
    if(!vk_ok(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &resolve_set_layout_), "vkCreateDescriptorSetLayout", diag)) return false;
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if(!vk_ok(vkCreateDescriptorPool(dev, &pci, nullptr, &pass_pool_), "vkCreateDescriptorPool", diag)) return false;
    VkPushConstantRange ids_pr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pass_push)};
    VkPushConstantRange res_pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pass_push)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pushConstantRangeCount = 1;
    plci.pSetLayouts = &ids_set_layout_;
    plci.pPushConstantRanges = &ids_pr;
    if(!vk_ok(vkCreatePipelineLayout(dev, &plci, nullptr, &ids_layout_), "vkCreatePipelineLayout", diag)) return false;
    plci.pSetLayouts = &resolve_set_layout_;
    plci.pPushConstantRanges = &res_pr;
    if(!vk_ok(vkCreatePipelineLayout(dev, &plci, nullptr, &resolve_layout_), "vkCreatePipelineLayout", diag)) return false;
    VkShaderModule ids_vs = vk_->create_shader(spv_map_ids_vert, sizeof(spv_map_ids_vert), diag);
    VkShaderModule ids_fs = vk_->create_shader(spv_map_ids_frag, sizeof(spv_map_ids_frag), diag);
    VkShaderModule full_vs = vk_->create_shader(spv_fullscreen_vert, sizeof(spv_fullscreen_vert), diag);
    VkShaderModule res_fs = vk_->create_shader(spv_map_resolve_frag, sizeof(spv_map_resolve_frag), diag);
    bool ok = !diag.failed() && make_pipeline(dev, ids_vs, ids_fs, ids_layout_, VK_FORMAT_R32_UINT, true, ids_pipeline_, diag)
              && make_pipeline(dev, full_vs, res_fs, resolve_layout_, VK_FORMAT_R32_UINT, false, resolve_pipeline_, diag);
    for(VkShaderModule m : {ids_vs, ids_fs, full_vs, res_fs}) {
        if(m) vkDestroyShaderModule(dev, m, nullptr);
    }
    return ok;
}

bool map_renderer::init(vk_context& vk, VkFormat format, core::diagnostics& diag) {
    vk_ = &vk;
    VkDevice dev = vk.device;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; // longitude wraps
    sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    if(!vk_ok(vkCreateSampler(dev, &sci, nullptr, &linear_), "vkCreateSampler", diag)) return false;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT; // the tileable detail atlas
    if(!vk_ok(vkCreateSampler(dev, &sci, nullptr, &repeat_), "vkCreateSampler", diag)) return false;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if(!vk_ok(vkCreateSampler(dev, &sci, nullptr, &nearest_), "vkCreateSampler", diag)) return false;

    const VkDescriptorSetLayoutBinding bindings[6] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 6;
    dl.pBindings = bindings;
    if(!vk_ok(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &set_layout_), "vkCreateDescriptorSetLayout", diag)) return false;

    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * frames_in_flight},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frames_in_flight},
                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frames_in_flight}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = frames_in_flight;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    if(!vk_ok(vkCreateDescriptorPool(dev, &pci, nullptr, &pool_), "vkCreateDescriptorPool", diag)) return false;

    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(map_push)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pr;
    if(!vk_ok(vkCreatePipelineLayout(dev, &plci, nullptr, &layout_), "vkCreatePipelineLayout", diag)) return false;

    VkShaderModule vs = vk.create_shader(spv_fullscreen_vert, sizeof(spv_fullscreen_vert), diag);
    VkShaderModule fs = vk.create_shader(spv_world_map_frag, sizeof(spv_world_map_frag), diag);
    if(diag.failed()) return false;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rendering;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = layout_;
    bool ok = vk_ok(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_), "vkCreateGraphicsPipelines", diag);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    if(ok && vk.logic_op) ok = create_vector_pipelines(diag) && create_electoral_pipelines(diag);
    return ok;
}

bool map_renderer::upload(map_data const& data, core::diagnostics& diag) {
    if(data.ids.empty() || data.terrain.empty()) {
        diag.fail("map data has no images");
        return false;
    }
    vkDeviceWaitIdle(vk_->device);
    if(!upload_levels(*vk_, data.terrain, VK_FORMAT_R8G8B8A8_UNORM, terrain_, diag)
       || !upload_levels(*vk_, data.ids, VK_FORMAT_R16_UINT, ids_, diag) || !upload_atlas(*vk_, data, atlas_, diag)) {
        return false;
    }
    id_w_ = data.ids[0].width;
    id_h_ = data.ids[0].height;
    id_levels_ = static_cast<uint32_t>(data.ids.size());

    const VkDeviceSize table_bytes = VkDeviceSize{data.regions.size() + 1} * 2 * sizeof(uint32_t);
    table_.assign((data.regions.size() + 1) * 2, 0u);
    VkDescriptorSetLayout layouts[frames_in_flight];
    for(auto& l : layouts) l = set_layout_;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = frames_in_flight;
    ai.pSetLayouts = layouts;
    if(!vk_ok(vkAllocateDescriptorSets(vk_->device, &ai, sets_), "vkAllocateDescriptorSets", diag)) return false;
    for(uint32_t f = 0; f < frames_in_flight; ++f) {
        if(!vk_->create_buffer(table_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, tables_[f], diag)
           || !vk_->create_buffer(sizeof(hazard_ubo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, hazards_[f], diag)) {
            return false;
        }
        std::memset(tables_[f].mapped, 0, table_bytes);
        std::memset(hazards_[f].mapped, 0, sizeof(hazard_ubo));
        const VkDescriptorImageInfo terrain{linear_, terrain_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo ids{nearest_, ids_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo table{tables_[f].buffer, 0, table_bytes};
        const VkDescriptorBufferInfo hazards{hazards_[f].buffer, 0, sizeof(hazard_ubo)};
        VkWriteDescriptorSet w[4]{};
        for(uint32_t i = 0; i < 4; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = sets_[f];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i == 2   ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                  : i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                           : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        w[0].pImageInfo = &terrain;
        w[1].pImageInfo = &ids;
        w[2].pBufferInfo = &table;
        w[3].pBufferInfo = &hazards;
        vkUpdateDescriptorSets(vk_->device, 4, w, 0, nullptr);
    }
    // Vector regions: the ring vertices and fans as storage buffers (vertex pulling).
    if(allow_vector_ && ids_pipeline_ && !data.poly_indices.empty()) {
        const VkDeviceSize vb = data.poly_vertices.size() * sizeof(uint32_t), ib = data.poly_indices.size() * sizeof(uint32_t);
        if(!vk_->create_buffer(vb, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, poly_verts_, diag)
           || !vk_->create_buffer(ib, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, poly_idx_, diag)) {
            return false;
        }
        std::memcpy(poly_verts_.mapped, data.poly_vertices.data(), vb);
        std::memcpy(poly_idx_.mapped, data.poly_indices.data(), ib);
        poly_index_count_ = static_cast<uint32_t>(data.poly_indices.size());
        tol_km_ = data.poly_tolerance_deg * 111.2f;
        VkDescriptorSetLayout layouts2[2] = {ids_set_layout_, resolve_set_layout_};
        VkDescriptorSet sets2[2];
        VkDescriptorSetAllocateInfo ai2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai2.descriptorPool = pass_pool_;
        ai2.descriptorSetCount = 2;
        ai2.pSetLayouts = layouts2;
        if(!vk_ok(vkAllocateDescriptorSets(vk_->device, &ai2, sets2), "vkAllocateDescriptorSets", diag)) return false;
        ids_set_ = sets2[0];
        resolve_set_ = sets2[1];
        const VkDescriptorBufferInfo vbi{poly_verts_.buffer, 0, vb}, ibi{poly_idx_.buffer, 0, ib};
        VkWriteDescriptorSet w[2]{};
        for(uint32_t i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = ids_set_;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        w[0].pBufferInfo = &vbi;
        w[1].pBufferInfo = &ibi;
        vkUpdateDescriptorSets(vk_->device, 2, w, 0, nullptr);
        for(auto& pb : pick_) {
            if(!vk_->create_buffer(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, pb, diag)) return false;
            std::memset(pb.mapped, 0, 4);
        }
        // Electoral sets: the ring vertices (shared) + the nation's fans; the ID image + colours.
        {
            VkDescriptorSetLayout el[1 + frames_in_flight];
            el[0] = elect_ids_set_layout_;
            for(uint32_t f = 0; f < frames_in_flight; ++f) el[1 + f] = elect_draw_set_layout_;
            VkDescriptorSet es[1 + frames_in_flight];
            VkDescriptorSetAllocateInfo eai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            eai.descriptorPool = elect_pool_;
            eai.descriptorSetCount = 1 + frames_in_flight;
            eai.pSetLayouts = el;
            if(!vk_ok(vkAllocateDescriptorSets(vk_->device, &eai, es), "vkAllocateDescriptorSets", diag)) return false;
            elect_ids_set_ = es[0];
            for(uint32_t f = 0; f < frames_in_flight; ++f) {
                elect_draw_sets_[f] = es[1 + f];
                const VkDeviceSize cb = VkDeviceSize{data.regions.size() + 1} * sizeof(uint32_t);
                if(!vk_->create_buffer(cb, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, elect_col_[f], diag)) return false;
                std::memset(elect_col_[f].mapped, 0, cb);
            }
            for(auto& pb : pick_) {
                vk_->destroy_buffer(pb);
                if(!vk_->create_buffer(8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, pb, diag)) return false;
                std::memset(pb.mapped, 0, 8);
            }
        }
        vector_ = true;
        SDL_Log("map: vector regions, %zu vertices, %u indices, tolerance %.1f km", data.poly_vertices.size() / 4, poly_index_count_,
                static_cast<double>(tol_km_));
    }
    ready_ = true;
    if(!resize(diag)) return false;
    return true;
}

void map_renderer::write_map_sets() {
    // Binding 4: the resolved regions (vector mode), else any usampler image (unused);
    // binding 5: the terrain detail atlas.
    const VkDescriptorImageInfo info{nearest_, vector_ && resolved_img_.view ? resolved_img_.view : ids_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo atlas{repeat_, atlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for(uint32_t f = 0; f < frames_in_flight; ++f) {
        VkWriteDescriptorSet w[2]{};
        for(uint32_t i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = sets_[f];
            w[i].dstBinding = 4 + i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        w[0].pImageInfo = &info;
        w[1].pImageInfo = &atlas;
        vkUpdateDescriptorSets(vk_->device, 2, w, 0, nullptr);
    }
}

bool map_renderer::resize(core::diagnostics& diag) {
    if(!ready_) return true;
    vkDeviceWaitIdle(vk_->device);
    if(vector_) {
        vk_->destroy_image(id_img_);
        vk_->destroy_image(resolved_img_);
        const VkExtent2D e = vk_->swap_extent;
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vk_->destroy_image(elect_img_);
        if(!vk_->create_image(e.width, e.height, VK_FORMAT_R32_UINT, usage, id_img_, diag)
           || !vk_->create_image(e.width, e.height, VK_FORMAT_R32_UINT, usage, resolved_img_, diag)
           || !vk_->create_image(e.width, e.height, VK_FORMAT_R32_UINT, usage, elect_img_, diag)) {
            return false;
        }
        VkCommandBuffer cmd = vk_->begin_immediate();
        for(gpu_image const* img : {&id_img_, &resolved_img_, &elect_img_}) {
            image_barrier(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        if(!vk_->end_immediate(cmd, diag)) return false;
        const VkDescriptorImageInfo ids{nearest_, id_img_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo raster{nearest_, ids_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo terrain{linear_, terrain_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[3]{};
        for(uint32_t i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = resolve_set_;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        w[0].pImageInfo = &ids;
        w[1].pImageInfo = &raster;
        w[2].pImageInfo = &terrain;
        vkUpdateDescriptorSets(vk_->device, 3, w, 0, nullptr);
    }
    write_map_sets();
    if(vector_) write_electoral_sets();
    return true;
}

void map_renderer::write_electoral_sets() {
    for(uint32_t f = 0; f < frames_in_flight; ++f) {
        const VkDescriptorImageInfo img{nearest_, elect_img_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo cols{elect_col_[f].buffer, 0, elect_col_[f].size};
        VkWriteDescriptorSet w[2]{};
        for(uint32_t i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = elect_draw_sets_[f];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &cols;
        vkUpdateDescriptorSets(vk_->device, 2, w, 0, nullptr);
    }
}

void map_renderer::set_electoral(std::shared_ptr<electoral_geometry const> geo, std::vector<uint32_t> colours) {
    if(!vector_) return;
    elect_colours_ = std::move(colours);
    if(geo == elect_geo_) return;
    elect_geo_ = std::move(geo);
    elect_index_count_ = 0;
    if(!elect_geo_ || elect_geo_->indices.empty()) return;
    const VkDeviceSize bytes = elect_geo_->indices.size() * sizeof(uint32_t);
    if(!elect_idx_.buffer || elect_idx_.size < bytes) {
        vkDeviceWaitIdle(vk_->device); // a frame in flight may still read the old buffer
        vk_->destroy_buffer(elect_idx_);
        core::diagnostics diag;
        if(!vk_->create_buffer(std::max<VkDeviceSize>(bytes, 1 << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, elect_idx_, diag)) {
            SDL_Log("electoral map: %s", diag.error().c_str());
            return;
        }
        const VkDescriptorBufferInfo vbi{poly_verts_.buffer, 0, poly_verts_.size}, ibi{elect_idx_.buffer, 0, elect_idx_.size};
        VkWriteDescriptorSet w[2]{};
        for(uint32_t i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = elect_ids_set_;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        w[0].pBufferInfo = &vbi;
        w[1].pBufferInfo = &ibi;
        vkUpdateDescriptorSets(vk_->device, 2, w, 0, nullptr);
    } else {
        vkDeviceWaitIdle(vk_->device);
    }
    std::memcpy(elect_idx_.mapped, elect_geo_->indices.data(), bytes);
    elect_index_count_ = static_cast<uint32_t>(elect_geo_->indices.size());
}

bool map_renderer::draw_electoral(VkCommandBuffer cmd, uint32_t first, uint32_t count, float const viewport[2], float time, float scale) {
    if(!vector_ || !elect_drawn_ || !elect_draw_pipeline_) return false;
    const uint32_t frame = vk_->frame_index();
    const float push[4] = {viewport[0], viewport[1], time, scale};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, elect_draw_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, elect_draw_layout_, 0, 1, &elect_draw_sets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, elect_draw_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 6, count, 0, first);
    return true;
}

int map_renderer::picked_electoral() const noexcept {
    if(!vector_) return -1;
    uint32_t code = 0;
    std::memcpy(&code, static_cast<uint8_t const*>(pick_[vk_->frame_index()].mapped) + 4, 4);
    const uint32_t id = code & 0xFFFFu;
    if((code & 0x80000000u) || id == 0 || code != map_region_code(id) || id > table_.size() / 2) return -1;
    return static_cast<int>(id) - 1;
}

namespace {
void fill_pass_push(pass_push& push, map_view const& view, float width, float height) {
    const map_view::frame b = view.basis();
    push.viewport[0] = width;
    push.viewport[1] = height;
    push.centre[0] = width * 0.5f;
    push.centre[1] = height * 0.5f;
    const map_vec3 axes[3] = {b.p, b.e, b.n};
    float* dst[3] = {push.p, push.e, push.n};
    for(int i = 0; i < 3; ++i) {
        dst[i][0] = static_cast<float>(axes[i].x);
        dst[i][1] = static_cast<float>(axes[i].y);
        dst[i][2] = static_cast<float>(axes[i].z);
    }
    push.p[3] = static_cast<float>(view.scale);
}
} // namespace

void map_renderer::prepass(VkCommandBuffer cmd, map_view const& view, float width, float height, int pick_x, int pick_y) {
    if(!ready_ || !vector_ || !id_img_.image) return;
    const VkExtent2D e = vk_->swap_extent;
    pass_push push{};
    fill_pass_push(push, view, width, height);
    push.e[3] = tol_km_;
    push.raster[0] = id_w_;
    push.raster[1] = id_h_;
    VkViewport viewport{0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
    VkRect2D scissor{{0, 0}, e};
    auto pass = [&](gpu_image const& target, bool clear, auto&& body) {
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = target.view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color.uint32[0] = 0;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, e};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        body();
        vkCmdEndRendering(cmd);
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    // 1. Ring fans XORed into the ID image (twice: either side of the seam).
    pass(id_img_, true, [&] {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ids_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ids_layout_, 0, 1, &ids_set_, 0, nullptr);
        vkCmdPushConstants(cmd, ids_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, poly_index_count_, 2, 0, 0);
    });
    // 2. One region per pixel.
    pass(resolved_img_, false, [&] {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_layout_, 0, 1, &resolve_set_, 0, nullptr);
        vkCmdPushConstants(cmd, resolve_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });
    // 3. The Politics window's inset map: the nation's fans once per inset, scissored.
    elect_drawn_ = false;
    if(elect_geo_ && elect_index_count_ > 0 && elect_rect_[2] > 1.0f && elect_rect_[3] > 1.0f) {
        const uint32_t frame = vk_->frame_index();
        const std::size_t n = std::min<std::size_t>(elect_colours_.size(), static_cast<std::size_t>(elect_col_[frame].size / 4));
        std::memcpy(elect_col_[frame].mapped, elect_colours_.data(), n * 4);
        pass(elect_img_, true, [&] {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, elect_ids_pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, elect_ids_layout_, 0, 1, &elect_ids_set_, 0, nullptr);
            for(auto const& in : elect_geo_->insets) {
                elect_push ep{};
                ep.viewport[0] = static_cast<float>(e.width);
                ep.viewport[1] = static_cast<float>(e.height);
                ep.ref_lon = elect_geo_->ref_lon_deg;
                ep.dst[0] = elect_rect_[0] + in.dst[0] * elect_rect_[2];
                ep.dst[1] = elect_rect_[1] + in.dst[1] * elect_rect_[3];
                ep.dst[2] = elect_rect_[0] + in.dst[2] * elect_rect_[2];
                ep.dst[3] = elect_rect_[1] + in.dst[3] * elect_rect_[3];
                std::memcpy(ep.src, in.src, sizeof(ep.src));
                const int32_t sx = std::clamp(static_cast<int32_t>(std::floor(ep.dst[0])), 0, static_cast<int32_t>(e.width));
                const int32_t sy = std::clamp(static_cast<int32_t>(std::floor(ep.dst[1])), 0, static_cast<int32_t>(e.height));
                const int32_t ex = std::clamp(static_cast<int32_t>(std::ceil(ep.dst[2])), 0, static_cast<int32_t>(e.width));
                const int32_t ey = std::clamp(static_cast<int32_t>(std::ceil(ep.dst[3])), 0, static_cast<int32_t>(e.height));
                if(ex <= sx || ey <= sy) continue;
                const VkRect2D sc{{sx, sy}, {static_cast<uint32_t>(ex - sx), static_cast<uint32_t>(ey - sy)}};
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdPushConstants(cmd, elect_ids_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ep), &ep);
                vkCmdDraw(cmd, elect_index_count_, 1, 0, 0);
            }
        });
        elect_drawn_ = true;
    }

    // 4. The region under the pick point, for picking (read when this frame slot comes round again).
    if(pick_x >= 0 && pick_y >= 0 && static_cast<uint32_t>(pick_x) < e.width && static_cast<uint32_t>(pick_y) < e.height) {
        image_barrier(cmd, resolved_img_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {pick_x, pick_y, 0};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cmd, resolved_img_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pick_[vk_->frame_index()].buffer, 1, &region);
        image_barrier(cmd, resolved_img_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        // ... and in the electoral map (raw XOR code, decoded by picked_electoral).
        image_barrier(cmd, elect_img_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        region.bufferOffset = 4;
        vkCmdCopyImageToBuffer(cmd, elect_img_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pick_[vk_->frame_index()].buffer, 1, &region);
        image_barrier(cmd, elect_img_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    } else if(pick_[vk_->frame_index()].mapped) {
        std::memset(pick_[vk_->frame_index()].mapped, 0, 8);
    }
}

int map_renderer::picked_region() const noexcept {
    if(!vector_) return -1;
    uint32_t v = 0;
    std::memcpy(&v, pick_[vk_->frame_index()].mapped, 4);
    if(v == 0 || v == 0xFFFFu || v > table_.size() / 2) return -1;
    return static_cast<int>(v) - 1;
}

void map_renderer::draw(VkCommandBuffer cmd, map_view const& view, float width, float height, float line_scale, bool political,
                        map_anim const& anim) {
    if(!ready_) return;
    const uint32_t frame = vk_->frame_index();
    // The frame slot's fence has been waited on: its table is not in use.
    const std::size_t bytes = std::min<std::size_t>(table_.size() * sizeof(uint32_t), static_cast<std::size_t>(tables_[frame].size));
    std::memcpy(tables_[frame].mapped, table_.data(), bytes);
    hazard_ubo hz{};
    const auto storms = static_cast<uint32_t>(std::min(storms_.size(), map_max_storms));
    const auto quakes = static_cast<uint32_t>(std::min(quakes_.size(), map_max_quakes));
    for(uint32_t i = 0; i < storms; ++i) {
        auto const& s = storms_[i];
        hz.storm[i][0] = s.lon;
        hz.storm[i][1] = s.lat;
        hz.storm[i][2] = s.wind;
        hz.storm[i][3] = s.radius_km;
    }
    for(uint32_t i = 0; i < quakes; ++i) {
        auto const& q = quakes_[i];
        hz.quake[i][0] = q.lon;
        hz.quake[i][1] = q.lat;
        hz.quake[i][2] = q.magnitude;
        hz.quake[i][3] = q.age_days;
        hz.quake_t[i][0] = q.shown_seconds;
    }
    std::memcpy(hazards_[frame].mapped, &hz, sizeof(hz));

    const map_view::frame b = view.basis();
    map_push push{};
    push.viewport[0] = width;
    push.viewport[1] = height;
    push.centre[0] = width * 0.5f;
    push.centre[1] = height * 0.5f;
    const map_vec3 axes[3] = {b.p, b.e, b.n};
    float* dst[3] = {push.p, push.e, push.n};
    for(int i = 0; i < 3; ++i) {
        dst[i][0] = static_cast<float>(axes[i].x);
        dst[i][1] = static_cast<float>(axes[i].y);
        dst[i][2] = static_cast<float>(axes[i].z);
    }
    push.p[3] = static_cast<float>(view.scale);
    push.e[3] = line_scale;
    push.flags = (political ? 1u : 0u) | (vector_ && resolved_img_.image ? 4u : 0u);
    push.id_levels = id_levels_;
    push.id_w = id_w_;
    push.id_h = id_h_;
    push.anim[0] = anim.cloud_days;
    push.anim[1] = anim.seconds;
    push.anim[2] = anim.cloud_strength;
    push.anim[3] = anim.day_of_year;
    push.counts[0] = storms;
    push.counts[1] = quakes;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void map_renderer::shutdown() {
    if(!vk_ || !vk_->device) return;
    VkDevice dev = vk_->device;
    vkDeviceWaitIdle(dev);
    vk_->destroy_image(terrain_);
    vk_->destroy_image(atlas_);
    vk_->destroy_image(ids_);
    for(auto& t : tables_) vk_->destroy_buffer(t);
    for(auto& h : hazards_) vk_->destroy_buffer(h);
    for(auto& pb : pick_) vk_->destroy_buffer(pb);
    for(auto& cb : elect_col_) vk_->destroy_buffer(cb);
    vk_->destroy_buffer(elect_idx_);
    vk_->destroy_image(elect_img_);
    for(VkPipeline p : {elect_ids_pipeline_, elect_draw_pipeline_}) {
        if(p) vkDestroyPipeline(dev, p, nullptr);
    }
    for(VkPipelineLayout l : {elect_ids_layout_, elect_draw_layout_}) {
        if(l) vkDestroyPipelineLayout(dev, l, nullptr);
    }
    if(elect_pool_) vkDestroyDescriptorPool(dev, elect_pool_, nullptr);
    for(VkDescriptorSetLayout l : {elect_ids_set_layout_, elect_draw_set_layout_}) {
        if(l) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    }
    elect_ids_pipeline_ = elect_draw_pipeline_ = VK_NULL_HANDLE;
    elect_ids_layout_ = elect_draw_layout_ = VK_NULL_HANDLE;
    elect_pool_ = VK_NULL_HANDLE;
    elect_ids_set_layout_ = elect_draw_set_layout_ = VK_NULL_HANDLE;
    elect_geo_.reset();
    vk_->destroy_buffer(poly_verts_);
    vk_->destroy_buffer(poly_idx_);
    vk_->destroy_image(id_img_);
    vk_->destroy_image(resolved_img_);
    for(VkPipeline p : {ids_pipeline_, resolve_pipeline_}) {
        if(p) vkDestroyPipeline(dev, p, nullptr);
    }
    for(VkPipelineLayout l : {ids_layout_, resolve_layout_}) {
        if(l) vkDestroyPipelineLayout(dev, l, nullptr);
    }
    if(pass_pool_) vkDestroyDescriptorPool(dev, pass_pool_, nullptr);
    for(VkDescriptorSetLayout l : {ids_set_layout_, resolve_set_layout_}) {
        if(l) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    }
    ids_pipeline_ = resolve_pipeline_ = VK_NULL_HANDLE;
    ids_layout_ = resolve_layout_ = VK_NULL_HANDLE;
    pass_pool_ = VK_NULL_HANDLE;
    ids_set_layout_ = resolve_set_layout_ = VK_NULL_HANDLE;
    vector_ = false;
    if(pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if(layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if(pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if(set_layout_) vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    if(linear_) vkDestroySampler(dev, linear_, nullptr);
    if(nearest_) vkDestroySampler(dev, nearest_, nullptr);
    if(repeat_) vkDestroySampler(dev, repeat_, nullptr);
    repeat_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    linear_ = nearest_ = VK_NULL_HANDLE;
    ready_ = false;
    vk_ = nullptr;
}

} // namespace geosim::client
