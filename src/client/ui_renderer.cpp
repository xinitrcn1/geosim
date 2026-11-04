#include "ui_renderer.hpp"

#include "style.hpp"

#include "blur.frag.h"
#include "fullscreen.vert.h"
#include "map.frag.h"
#include "quad.vert.h"
#include "ui.frag.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace geosim::client {

namespace {

struct quad_push {
    float viewport[2];
    float time;
    float scale;
};

struct post_push {
    float texel[2];
    float dir[2];
    float sigma;
    int32_t radius;
};

// The material table: fragment shader + whether it alpha-blends. Index = material.
struct material_def {
    uint32_t const* code;
    std::size_t bytes;
    bool blend;
};
const material_def material_table[] = {
    {spv_ui_frag, sizeof(spv_ui_frag), true},       // material::ui
    {spv_map_frag, sizeof(spv_map_frag), false},    // material::map_placeholder
};
static_assert(std::size(material_table) == static_cast<std::size_t>(material::count));

constexpr VkDeviceSize initial_instances = 8192;

// Blur strength: sigma in quarter-resolution texels (x4 in pixels).
constexpr float blur_sigma = 3.0f;
constexpr int blur_radius = 7;

bool vk_ok(VkResult r, char const* what, core::diagnostics& diag) {
    if(r == VK_SUCCESS) return true;
    diag.fail(std::string(what) + " failed: " + vk_result_name(r));
    return false;
}

} // namespace

bool ui_renderer::init(vk_context& vk, core::diagnostics& diag) {
    vk_ = &vk;
    format_ = vk.swap_format;
    VkDevice dev = vk.device;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if(!vk_ok(vkCreateSampler(dev, &sci, nullptr, &sampler_), "vkCreateSampler", diag)) return false;

    std::array<VkDescriptorSetLayoutBinding, 3> ui_bindings{};
    for(uint32_t i = 0; i < 3; ++i) {
        ui_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 3;
    dl.pBindings = ui_bindings.data();
    if(!vk_ok(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &ui_set_layout_), "vkCreateDescriptorSetLayout", diag)) return false;
    dl.bindingCount = 1;
    if(!vk_ok(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &post_set_layout_), "vkCreateDescriptorSetLayout", diag)) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 8;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if(!vk_ok(vkCreateDescriptorPool(dev, &pci, nullptr, &pool_), "vkCreateDescriptorPool", diag)) return false;

    VkPushConstantRange qpr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(quad_push)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &ui_set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &qpr;
    if(!vk_ok(vkCreatePipelineLayout(dev, &plci, nullptr, &quad_layout_), "vkCreatePipelineLayout", diag)) return false;
    VkPushConstantRange ppr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(post_push)};
    plci.pSetLayouts = &post_set_layout_;
    plci.pPushConstantRanges = &ppr;
    if(!vk_ok(vkCreatePipelineLayout(dev, &plci, nullptr, &post_layout_), "vkCreatePipelineLayout", diag)) return false;

    if(!create_pipelines(diag)) return false;

    if(!vk.create_image(text_system::atlas_size, text_system::atlas_size, VK_FORMAT_R8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, atlas_, diag)
       || !vk.create_buffer(VkDeviceSize{text_system::atlas_size} * text_system::atlas_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                            atlas_staging_, diag)) {
        return false;
    }
    for(uint32_t f = 0; f < frames_in_flight; ++f) {
        if(!ensure_instance_capacity(f, initial_instances, diag)) return false;
    }
    return resize(diag);
}

bool ui_renderer::create_pipelines(core::diagnostics& diag) {
    VkDevice dev = vk_->device;
    VkShaderModule quad_vs = vk_->create_shader(spv_quad_vert, sizeof(spv_quad_vert), diag);
    VkShaderModule full_vs = vk_->create_shader(spv_fullscreen_vert, sizeof(spv_fullscreen_vert), diag);
    VkShaderModule blur_fs = vk_->create_shader(spv_blur_frag, sizeof(spv_blur_frag), diag);
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
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format_;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, quad_vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, VK_NULL_HANDLE, "main", nullptr};

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

    bool ok = true;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attrs));
    vin.pVertexAttributeDescriptions = attrs;
    gp.layout = quad_layout_;
    for(std::size_t m = 0; m < std::size(material_table) && ok; ++m) {
        VkShaderModule fs = vk_->create_shader(material_table[m].code, material_table[m].bytes, diag);
        if(!fs) {
            ok = false;
            break;
        }
        stages[1].module = fs;
        blend.blendEnable = material_table[m].blend ? VK_TRUE : VK_FALSE;
        ok = vk_ok(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &materials_[m]), "vkCreateGraphicsPipelines", diag);
        vkDestroyShaderModule(dev, fs, nullptr);
    }

    if(ok) {
        vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        stages[0].module = full_vs;
        stages[1].module = blur_fs;
        blend.blendEnable = VK_FALSE;
        gp.layout = post_layout_;
        ok = vk_ok(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &post_pipeline_), "vkCreateGraphicsPipelines", diag);
    }
    vkDestroyShaderModule(dev, quad_vs, nullptr);
    vkDestroyShaderModule(dev, full_vs, nullptr);
    vkDestroyShaderModule(dev, blur_fs, nullptr);
    return ok;
}

void ui_renderer::destroy_targets() {
    vk_->destroy_image(scene_);
    vk_->destroy_image(half_);
    vk_->destroy_image(quarter_a_);
    vk_->destroy_image(quarter_b_);
    vk_->destroy_buffer(readback_);
}

bool ui_renderer::resize(core::diagnostics& diag) {
    vkDeviceWaitIdle(vk_->device);
    if(vk_->swap_format != format_) {
        diag.fail("swapchain format changed; pipeline rebuild is not implemented");
        return false;
    }
    destroy_targets();
    const VkExtent2D e = vk_->swap_extent;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if(!vk_->create_image(e.width, e.height, format_, usage, scene_, diag)
       || !vk_->create_image((e.width + 1) / 2, (e.height + 1) / 2, format_, usage, half_, diag)
       || !vk_->create_image((e.width + 3) / 4, (e.height + 3) / 4, format_, usage, quarter_a_, diag)
       || !vk_->create_image((e.width + 3) / 4, (e.height + 3) / 4, format_, usage, quarter_b_, diag)) {
        return false;
    }

    vkResetDescriptorPool(vk_->device, pool_, 0);
    VkDescriptorSetLayout layouts[6] = {ui_set_layout_, ui_set_layout_, post_set_layout_, post_set_layout_, post_set_layout_, post_set_layout_};
    VkDescriptorSet sets[6];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 6;
    ai.pSetLayouts = layouts;
    if(!vk_ok(vkAllocateDescriptorSets(vk_->device, &ai, sets), "vkAllocateDescriptorSets", diag)) return false;
    ui_set_ = sets[0];
    ui_scene_set_ = sets[1];
    for(int i = 0; i < 4; ++i) post_sets_[i] = sets[i + 2];

    auto info = [&](gpu_image const& img) {
        return VkDescriptorImageInfo{sampler_, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    // ui_set_: atlas, blur, scene. ui_scene_set_: atlas, blur, half_ (any image
    // in SHADER_READ layout during the scene pass). Post sets: their source.
    const VkDescriptorImageInfo images[10] = {info(atlas_), info(quarter_a_), info(scene_), info(atlas_), info(quarter_a_), info(half_),
                                              info(scene_), info(half_), info(quarter_a_), info(quarter_b_)};
    const VkDescriptorSet dst_sets[10] = {ui_set_, ui_set_, ui_set_, ui_scene_set_, ui_scene_set_, ui_scene_set_,
                                          post_sets_[0], post_sets_[1], post_sets_[2], post_sets_[3]};
    const uint32_t dst_bindings[10] = {0, 1, 2, 0, 1, 2, 0, 0, 0, 0};
    VkWriteDescriptorSet writes[10]{};
    for(int i = 0; i < 10; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dst_sets[i];
        writes[i].dstBinding = dst_bindings[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(vk_->device, 10, writes, 0, nullptr);

    // Fresh images start UNDEFINED; move them to the layout the frame expects
    // at its start (the atlas is uploaded separately).
    VkCommandBuffer cmd = vk_->begin_immediate();
    for(gpu_image const* img : {&scene_, &half_, &quarter_a_, &quarter_b_}) {
        image_barrier(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    if(!atlas_initialised_) {
        image_barrier(cmd, atlas_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        atlas_initialised_ = true;
    }
    return vk_->end_immediate(cmd, diag);
}

bool ui_renderer::upload_atlas(text_system& text, core::diagnostics& diag) {
    if(!text.dirty()) return true;
    // Rare (new glyphs): wait for the GPU so no frame in flight samples the atlas.
    vkQueueWaitIdle(vk_->queue);
    std::memcpy(atlas_staging_.mapped, text.atlas().data(), text.atlas().size());
    VkCommandBuffer cmd = vk_->begin_immediate();
    image_barrier(cmd, atlas_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {text_system::atlas_size, text_system::atlas_size, 1};
    vkCmdCopyBufferToImage(cmd, atlas_staging_.buffer, atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, atlas_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if(!vk_->end_immediate(cmd, diag)) return false;
    text.mark_uploaded();
    return true;
}

bool ui_renderer::ensure_instance_capacity(uint32_t frame, std::size_t count, core::diagnostics& diag) {
    const VkDeviceSize need = VkDeviceSize{count} * sizeof(quad_instance);
    if(instances_[frame].buffer && instances_[frame].size >= need) return true;
    // The frame slot's fence has been waited on, so its old buffer is idle.
    VkDeviceSize size = instances_[frame].size ? instances_[frame].size : initial_instances * sizeof(quad_instance);
    while(size < need) size *= 2;
    vk_->destroy_buffer(instances_[frame]);
    return vk_->create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, instances_[frame], diag);
}

void ui_renderer::draw_batches(VkCommandBuffer cmd, draw_list const& list, uint32_t base, float time, float scale) {
    const VkExtent2D e = vk_->swap_extent;
    quad_push push{{static_cast<float>(e.width), static_cast<float>(e.height)}, time, scale};
    vkCmdPushConstants(cmd, quad_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    for(auto const& b : list.batches()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, materials_[static_cast<int>(b.mat)]);
        vkCmdDraw(cmd, 6, b.count, 0, base + b.first);
    }
}

void ui_renderer::post_pass(VkCommandBuffer cmd, gpu_image const& dst, VkDescriptorSet src_set, gpu_image const& src, float dx, float dy,
                            float sigma, int radius) {
    image_barrier(cmd, dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = dst.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, dst.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0, 0, static_cast<float>(dst.extent.width), static_cast<float>(dst.extent.height), 0, 1};
    VkRect2D scissor{{0, 0}, dst.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout_, 0, 1, &src_set, 0, nullptr);
    post_push push{{1.0f / static_cast<float>(src.extent.width), 1.0f / static_cast<float>(src.extent.height)}, {dx, dy}, sigma, radius};
    vkCmdPushConstants(cmd, post_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

bool ui_renderer::record(text_system& text, draw_list const& scene, draw_list const& overlay, float time, float scale, bool capture,
                         core::diagnostics& diag, map_pass const& map) {
    if(!upload_atlas(text, diag)) return false;
    const uint32_t frame = vk_->frame_index();
    const std::size_t total = 1 + scene.quads().size() + overlay.quads().size();
    if(!ensure_instance_capacity(frame, total, diag)) return false;

    // Instance buffer: [0] scene copy, then the scene list, then the overlay list.
    const VkExtent2D e = vk_->swap_extent;
    auto* dst = static_cast<quad_instance*>(instances_[frame].mapped);
    dst[0] = {0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 0, 1, 1, 0xFFFFFFFFu, 0, 0,
              static_cast<uint32_t>(quad_mode::scene)};
    std::memcpy(dst + 1, scene.quads().data(), scene.quads().size() * sizeof(quad_instance));
    std::memcpy(dst + 1 + scene.quads().size(), overlay.quads().data(), overlay.quads().size() * sizeof(quad_instance));
    const auto scene_base = 1u;
    const auto overlay_base = static_cast<uint32_t>(1 + scene.quads().size());

    VkCommandBuffer cmd = vk_->frame().cmd;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &instances_[frame].buffer, &offset);
    VkViewport viewport{0, 0, static_cast<float>(e.width), static_cast<float>(e.height), 0, 1};
    VkRect2D scissor{{0, 0}, e};

    // 1. Scene pass.
    image_barrier(cmd, scene_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = scene_.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{style::scene_clear.r, style::scene_clear.g, style::scene_clear.b, 1.0f}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, e};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if(map.renderer && map.view && map.renderer->ready()) {
        map.renderer->draw(cmd, *map.view, static_cast<float>(e.width), static_cast<float>(e.height), scale, map.political, map.anim);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, quad_layout_, 0, 1, &ui_scene_set_, 0, nullptr);
    draw_batches(cmd, scene, scene_base, time, scale);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, scene_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // 2. Blur chain: downsample twice (single bilinear tap = 2x2 box), then separable Gaussian.
    post_pass(cmd, half_, post_sets_[0], scene_, 0, 0, 1, 0);
    post_pass(cmd, quarter_a_, post_sets_[1], half_, 0, 0, 1, 0);
    post_pass(cmd, quarter_b_, post_sets_[2], quarter_a_, 1, 0, blur_sigma, blur_radius);
    post_pass(cmd, quarter_a_, post_sets_[3], quarter_b_, 0, 1, blur_sigma, blur_radius);

    // 3. Final pass on the swapchain image.
    VkImage swap = vk_->swap_images[vk_->image_index()];
    image_barrier(cmd, swap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    att.imageView = vk_->swap_views[vk_->image_index()];
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, quad_layout_, 0, 1, &ui_set_, 0, nullptr);
    {
        quad_push push{{static_cast<float>(e.width), static_cast<float>(e.height)}, time, scale};
        vkCmdPushConstants(cmd, quad_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, materials_[static_cast<int>(material::ui)]);
        vkCmdDraw(cmd, 6, 1, 0, 0); // scene copy
    }
    draw_batches(cmd, overlay, overlay_base, time, scale);
    vkCmdEndRendering(cmd);

    if(capture && vk_->swap_readable) {
        const VkDeviceSize bytes = VkDeviceSize{e.width} * e.height * 4;
        if(!readback_.buffer || readback_.size < bytes) {
            vk_->destroy_buffer(readback_);
            if(!vk_->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback_, diag)) return false;
        }
        readback_extent_ = e;
        image_barrier(cmd, swap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {e.width, e.height, 1};
        vkCmdCopyImageToBuffer(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
        image_barrier(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                      VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        image_barrier(cmd, swap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    }
    return true;
}

bool ui_renderer::save_capture(std::string const& path, core::diagnostics& diag) {
    if(!readback_.buffer || readback_extent_.width == 0) {
        diag.fail("no captured frame (swapchain images not readable?)");
        return false;
    }
    vkDeviceWaitIdle(vk_->device);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if(!f) {
        diag.fail("cannot write " + path);
        return false;
    }
    const uint32_t w = readback_extent_.width, h = readback_extent_.height;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    const bool bgra = format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB;
    auto const* px = static_cast<uint8_t const*>(readback_.mapped);
    std::vector<uint8_t> row(std::size_t{w} * 3);
    for(uint32_t y = 0; y < h; ++y) {
        for(uint32_t x = 0; x < w; ++x) {
            uint8_t const* p = px + (std::size_t{y} * w + x) * 4;
            row[x * 3 + 0] = bgra ? p[2] : p[0];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = bgra ? p[0] : p[2];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

void ui_renderer::shutdown() {
    if(!vk_ || !vk_->device) return;
    VkDevice dev = vk_->device;
    vkDeviceWaitIdle(dev);
    destroy_targets();
    vk_->destroy_image(atlas_);
    vk_->destroy_buffer(atlas_staging_);
    for(auto& b : instances_) vk_->destroy_buffer(b);
    for(auto& p : materials_) {
        if(p) vkDestroyPipeline(dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if(post_pipeline_) vkDestroyPipeline(dev, post_pipeline_, nullptr);
    if(quad_layout_) vkDestroyPipelineLayout(dev, quad_layout_, nullptr);
    if(post_layout_) vkDestroyPipelineLayout(dev, post_layout_, nullptr);
    if(pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if(ui_set_layout_) vkDestroyDescriptorSetLayout(dev, ui_set_layout_, nullptr);
    if(post_set_layout_) vkDestroyDescriptorSetLayout(dev, post_set_layout_, nullptr);
    if(sampler_) vkDestroySampler(dev, sampler_, nullptr);
    post_pipeline_ = VK_NULL_HANDLE;
    quad_layout_ = post_layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    ui_set_layout_ = post_set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    vk_ = nullptr;
}

} // namespace geosim::client
