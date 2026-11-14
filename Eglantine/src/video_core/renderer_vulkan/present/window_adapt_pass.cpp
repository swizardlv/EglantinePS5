// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/frontend/framebuffer_layout.h"
#include "hud.h"
#include "vulkan_hud_shaders.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host_shaders/vulkan_present_vert_spv.h"
#include "video_core/host_shaders/present_video_key_frag_spv.h"
#include "video_core/renderer_vulkan/present/layer.h"
#include "video_core/renderer_vulkan/present/present_push_constants.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/present/window_adapt_pass.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

WindowAdaptPass::WindowAdaptPass(const Device& device, VkFormat frame_format, vk::Sampler&& sampler_, vk::ShaderModule&& fragment_shader_)
    : sampler(std::move(sampler_))
    , fragment_shader(std::move(fragment_shader_))
{
    CreateDescriptorSetLayout(device);
    CreatePipelineLayout(device);
    CreateVertexShader(device);
    CreateRenderPass(device, frame_format);
    CreatePipelines(device);
}

WindowAdaptPass::~WindowAdaptPass() = default;

void WindowAdaptPass::Draw(const Device& device, RasterizerVulkan& rasterizer, Scheduler& scheduler, size_t image_index,
                           std::list<Layer>& layers,
                           std::span<const Tegra::FramebufferConfig> configs,
                           const Layout::FramebufferLayout& layout, Frame* dst, bool draw_hud) {

    const VkFramebuffer host_framebuffer{*dst->framebuffer};
    const VkRenderPass renderpass{*render_pass};
    const VkPipelineLayout graphics_pipeline_layout{*pipeline_layout};
    const VkExtent2D render_area{
        .width = dst->width,
        .height = dst->height,
    };

    const size_t layer_count = configs.size();
    std::vector<PresentPushConstants> push_constants(layer_count);
    std::vector<VkDescriptorSet> descriptor_sets(layer_count);
    std::vector<VkPipeline> graphics_pipelines(layer_count);

    auto layer_it = layers.begin();
    for (size_t i = 0; i < layer_count; i++) {
        switch (configs[i].blending) {
        case Tegra::BlendMode::Opaque:
        default:
            graphics_pipelines[i] = *opaque_pipeline;
            break;
        case Tegra::BlendMode::Premultiplied:
            graphics_pipelines[i] = *premultiplied_pipeline;
            break;
        case Tegra::BlendMode::VideoKey:
            graphics_pipelines[i] = *video_key_pipeline;
            break;
        case Tegra::BlendMode::Coverage:
            graphics_pipelines[i] = *coverage_pipeline;
            break;
        }

        layer_it->ConfigureDraw(device, &push_constants[i], &descriptor_sets[i], rasterizer, *sampler,
                                image_index, configs[i], layout);
        layer_it++;
    }

    const auto hud = draw_hud ? Eden::GetVulkanHud() : Eden::HudSnapshot{};
    if (hud.width && !hud_pipeline) {
        const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(hud)};
        hud_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
        auto vertex = BuildShader(device, EDEN_HUD_VERT_SPV);
        auto fragment = BuildShader(device, EDEN_HUD_FRAG_SPV);
        hud_pipeline = CreateWrappedCoverageBlendingPipeline(
            device, render_pass, hud_layout, std::tie(vertex, fragment));
    }
    const VkPipeline hud_graphics_pipeline = *hud_pipeline;
    const VkPipelineLayout hud_pipeline_layout = *hud_layout;

    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const f32 bg_red = Settings::values.bg_red.GetValue() / 255.0f;
        const f32 bg_green = Settings::values.bg_green.GetValue() / 255.0f;
        const f32 bg_blue = Settings::values.bg_blue.GetValue() / 255.0f;
        const VkClearAttachment clear_attachment{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0,
            .clearValue =
                {
                    .color = {.float32 = {bg_red, bg_green, bg_blue, 1.0f}},
                },
        };
        const VkClearRect clear_rect{
            .rect =
                {
                    .offset = {0, 0},
                    .extent = render_area,
                },
            .baseArrayLayer = 0,
            .layerCount = 1,
        };

        BeginRenderPass(cmdbuf, renderpass, host_framebuffer, render_area);
        cmdbuf.ClearAttachments({clear_attachment}, {clear_rect});

        for (size_t i = 0; i < layer_count; i++) {
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipelines[i]);
            cmdbuf.PushConstants(graphics_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                                 push_constants[i]);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_layout, 0,
                                      descriptor_sets[i], {});
            cmdbuf.Draw(4, 1, 0, 0);
        }

        // The overlay's box, against the corner and at the size the settings name, laid out for a
        // picture 1080 rows high and scaled to this one (hud.h, PlaceHud).
        const auto hud_place = Eden::PlaceHud(hud, render_area.width, render_area.height);
        if (hud.width && (hud.loading || hud_place.shown)) {
            const VkRect2D scissor = hud.loading ? VkRect2D{{0, 0}, render_area} :
                VkRect2D{{static_cast<int32_t>(hud_place.box_x), static_cast<int32_t>(hud_place.box_y)},
                         {hud_place.box_w, hud_place.box_h}};
            const VkViewport viewport{static_cast<float>(scissor.offset.x), static_cast<float>(scissor.offset.y),
                static_cast<float>(scissor.extent.width), static_cast<float>(scissor.extent.height), 0, 1};
            cmdbuf.SetViewport(0, viewport);
            cmdbuf.SetScissor(0, scissor);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, hud_graphics_pipeline);
            auto constants = hud;
            if (hud.loading) {
                // The loading scene fills the picture: tell it how large that is.
                constants.x = render_area.width;
                constants.y = render_area.height;
            } else {
                // Where the text starts in this picture, and the rows its cells are laid out for.
                constants.x = hud_place.text_x;
                constants.y = hud_place.text_y;
                constants.width = hud_place.cell_rows;
            }
            cmdbuf.PushConstants(hud_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, constants);
            cmdbuf.Draw(4, 1, 0, 0);
        }

        cmdbuf.EndRenderPass();
    });
}

VkDescriptorSetLayout WindowAdaptPass::GetDescriptorSetLayout() {
    return *descriptor_set_layout;
}

VkRenderPass WindowAdaptPass::GetRenderPass() {
    return *render_pass;
}

void WindowAdaptPass::CreateDescriptorSetLayout(const Device& device) {
    descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
}

void WindowAdaptPass::CreatePipelineLayout(const Device& device) {
    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(PresentPushConstants),
    };

    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
}

void WindowAdaptPass::CreateVertexShader(const Device& device) {
    vertex_shader = BuildShader(device, VULKAN_PRESENT_VERT_SPV);
}

void WindowAdaptPass::CreateRenderPass(const Device& device, VkFormat frame_format) {
    render_pass = CreateWrappedRenderPass(device, frame_format, VK_IMAGE_LAYOUT_UNDEFINED);
}

void WindowAdaptPass::CreatePipelines(const Device& device) {
    opaque_pipeline = CreateWrappedPipeline(device, render_pass, pipeline_layout,
                                            std::tie(vertex_shader, fragment_shader));
    premultiplied_pipeline = CreateWrappedPremultipliedBlendingPipeline(
        device, render_pass, pipeline_layout, std::tie(vertex_shader, fragment_shader));
    video_key_shader = BuildShader(device, PRESENT_VIDEO_KEY_FRAG_SPV);
    video_key_pipeline = CreateWrappedPremultipliedBlendingPipeline(
        device, render_pass, pipeline_layout, std::tie(vertex_shader, video_key_shader));
    coverage_pipeline = CreateWrappedCoverageBlendingPipeline(
        device, render_pass, pipeline_layout, std::tie(vertex_shader, fragment_shader));
}

} // namespace Vulkan
