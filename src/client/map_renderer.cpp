#include "map_renderer.hpp"

#include "fullscreen.vert.h"
#include "world_map.frag.h"

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
                 VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
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

} // namespace

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
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if(!vk_ok(vkCreateSampler(dev, &sci, nullptr, &nearest_), "vkCreateSampler", diag)) return false;

    const VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 4;
    dl.pBindings = bindings;
    if(!vk_ok(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &set_layout_), "vkCreateDescriptorSetLayout", diag)) return false;

    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * frames_in_flight},
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
    const bool ok = vk_ok(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_), "vkCreateGraphicsPipelines", diag);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    return ok;
}

bool map_renderer::upload(map_data const& data, core::diagnostics& diag) {
    if(data.ids.empty() || data.terrain.empty()) {
        diag.fail("map data has no images");
        return false;
    }
    vkDeviceWaitIdle(vk_->device);
    if(!upload_levels(*vk_, data.terrain, VK_FORMAT_R8G8B8A8_UNORM, terrain_, diag)
       || !upload_levels(*vk_, data.ids, VK_FORMAT_R16_UINT, ids_, diag)) {
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
    ready_ = true;
    return true;
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
    push.flags = political ? 1u : 0u;
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
    vk_->destroy_image(ids_);
    for(auto& t : tables_) vk_->destroy_buffer(t);
    for(auto& h : hazards_) vk_->destroy_buffer(h);
    if(pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if(layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if(pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if(set_layout_) vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    if(linear_) vkDestroySampler(dev, linear_, nullptr);
    if(nearest_) vkDestroySampler(dev, nearest_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    linear_ = nearest_ = VK_NULL_HANDLE;
    ready_ = false;
    vk_ = nullptr;
}

} // namespace geosim::client
