// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/container/static_vector.hpp>
#include <boost/functional/hash.hpp>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/declarations.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_device.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_resource_manager.h"
#include "video_core/renderer_vulkan/vk_sampler_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

MICROPROFILE_DEFINE(Vulkan_WaitForWorker, "Vulkan", "Wait for worker", MP_RGB(255, 192, 192));
MICROPROFILE_DEFINE(Vulkan_Drawing, "Vulkan", "Record drawing", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_Compute, "Vulkan", "Record compute", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_Clearing, "Vulkan", "Record clearing", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_Geometry, "Vulkan", "Setup geometry", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_ConstBuffers, "Vulkan", "Setup constant buffers", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_GlobalBuffers, "Vulkan", "Setup global buffers", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_RenderTargets, "Vulkan", "Setup render targets", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_Textures, "Vulkan", "Setup textures", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_Images, "Vulkan", "Setup images", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_PipelineCache, "Vulkan", "Pipeline cache", MP_RGB(192, 128, 128));

namespace {

constexpr auto ComputeShaderIndex = static_cast<std::size_t>(Tegra::Engines::ShaderType::Compute);

vk::Viewport GetViewportState(const VKDevice& device, const Maxwell& regs, std::size_t index) {
    const auto& viewport = regs.viewport_transform[index];
    const float x = viewport.translate_x - viewport.scale_x;
    const float y = viewport.translate_y - viewport.scale_y;
    const float width = viewport.scale_x * 2.0f;
    const float height = viewport.scale_y * 2.0f;

    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne;
    float near = viewport.translate_z - viewport.scale_z * reduce_z;
    float far = viewport.translate_z + viewport.scale_z;
    if (!device.IsExtDepthRangeUnrestrictedSupported()) {
        near = std::clamp(near, 0.0f, 1.0f);
        far = std::clamp(far, 0.0f, 1.0f);
    }

    return vk::Viewport(x, y, width != 0 ? width : 1.0f, height != 0 ? height : 1.0f, near, far);
}

constexpr vk::Rect2D GetScissorState(const Maxwell& regs, std::size_t index) {
    const auto& scissor = regs.scissor_test[index];
    if (!scissor.enable) {
        return {{0, 0}, {INT32_MAX, INT32_MAX}};
    }
    const u32 width = scissor.max_x - scissor.min_x;
    const u32 height = scissor.max_y - scissor.min_y;
    return {{static_cast<s32>(scissor.min_x), static_cast<s32>(scissor.min_y)}, {width, height}};
}

std::array<GPUVAddr, Maxwell::MaxShaderProgram> GetShaderAddresses(
    const std::array<Shader, Maxwell::MaxShaderProgram>& shaders) {
    std::array<GPUVAddr, Maxwell::MaxShaderProgram> addresses;
    for (std::size_t i = 0; i < std::size(addresses); ++i) {
        addresses[i] = shaders[i] ? shaders[i]->GetGpuAddr() : 0;
    }
    return addresses;
}

void TransitionImages(const std::vector<ImageView>& views, vk::PipelineStageFlags pipeline_stage,
                      vk::AccessFlags access) {
    for (auto& [view, layout] : views) {
        view->Transition(*layout, pipeline_stage, access);
    }
}

template <typename Engine, typename Entry>
Tegra::Texture::FullTextureInfo GetTextureInfo(const Engine& engine, const Entry& entry,
                                               std::size_t stage) {
    const auto stage_type = static_cast<Tegra::Engines::ShaderType>(stage);
    if (entry.IsBindless()) {
        const Tegra::Texture::TextureHandle tex_handle =
            engine.AccessConstBuffer32(stage_type, entry.GetBuffer(), entry.GetOffset());
        return engine.GetTextureInfo(tex_handle);
    }
    if constexpr (std::is_same_v<Engine, Tegra::Engines::Maxwell3D>) {
        return engine.GetStageTexture(stage_type, entry.GetOffset());
    } else {
        return engine.GetTexture(entry.GetOffset());
    }
}

} // Anonymous namespace

class BufferBindings final {
public:
    void AddVertexBinding(const vk::Buffer* buffer, vk::DeviceSize offset) {
        vertex.buffer_ptrs[vertex.num_buffers] = buffer;
        vertex.offsets[vertex.num_buffers] = offset;
        ++vertex.num_buffers;
    }

    void SetIndexBinding(const vk::Buffer* buffer, vk::DeviceSize offset, vk::IndexType type) {
        index.buffer = buffer;
        index.offset = offset;
        index.type = type;
    }

    void Bind(VKScheduler& scheduler) const {
        // Use this large switch case to avoid dispatching more memory in the record lambda than
        // what we need. It looks horrible, but it's the best we can do on standard C++.
        switch (vertex.num_buffers) {
        case 0:
            return BindStatic<0>(scheduler);
        case 1:
            return BindStatic<1>(scheduler);
        case 2:
            return BindStatic<2>(scheduler);
        case 3:
            return BindStatic<3>(scheduler);
        case 4:
            return BindStatic<4>(scheduler);
        case 5:
            return BindStatic<5>(scheduler);
        case 6:
            return BindStatic<6>(scheduler);
        case 7:
            return BindStatic<7>(scheduler);
        case 8:
            return BindStatic<8>(scheduler);
        case 9:
            return BindStatic<9>(scheduler);
        case 10:
            return BindStatic<10>(scheduler);
        case 11:
            return BindStatic<11>(scheduler);
        case 12:
            return BindStatic<12>(scheduler);
        case 13:
            return BindStatic<13>(scheduler);
        case 14:
            return BindStatic<14>(scheduler);
        case 15:
            return BindStatic<15>(scheduler);
        case 16:
            return BindStatic<16>(scheduler);
        case 17:
            return BindStatic<17>(scheduler);
        case 18:
            return BindStatic<18>(scheduler);
        case 19:
            return BindStatic<19>(scheduler);
        case 20:
            return BindStatic<20>(scheduler);
        case 21:
            return BindStatic<21>(scheduler);
        case 22:
            return BindStatic<22>(scheduler);
        case 23:
            return BindStatic<23>(scheduler);
        case 24:
            return BindStatic<24>(scheduler);
        case 25:
            return BindStatic<25>(scheduler);
        case 26:
            return BindStatic<26>(scheduler);
        case 27:
            return BindStatic<27>(scheduler);
        case 28:
            return BindStatic<28>(scheduler);
        case 29:
            return BindStatic<29>(scheduler);
        case 30:
            return BindStatic<30>(scheduler);
        case 31:
            return BindStatic<31>(scheduler);
        case 32:
            return BindStatic<32>(scheduler);
        }
        UNREACHABLE();
    }

private:
    // Some of these fields are intentionally left uninitialized to avoid initializing them twice.
    struct {
        std::size_t num_buffers = 0;
        std::array<const vk::Buffer*, Maxwell::NumVertexArrays> buffer_ptrs;
        std::array<vk::DeviceSize, Maxwell::NumVertexArrays> offsets;
    } vertex;

    struct {
        const vk::Buffer* buffer = nullptr;
        vk::DeviceSize offset;
        vk::IndexType type;
    } index;

    template <std::size_t N>
    void BindStatic(VKScheduler& scheduler) const {
        if (index.buffer != nullptr) {
            BindStatic<N, true>(scheduler);
        } else {
            BindStatic<N, false>(scheduler);
        }
    }

    template <std::size_t N, bool is_indexed>
    void BindStatic(VKScheduler& scheduler) const {
        static_assert(N <= Maxwell::NumVertexArrays);
        if constexpr (N == 0) {
            return;
        }

        std::array<vk::Buffer, N> buffers;
        std::transform(vertex.buffer_ptrs.begin(), vertex.buffer_ptrs.begin() + N, buffers.begin(),
                       [](const auto ptr) { return *ptr; });

        std::array<vk::DeviceSize, N> offsets;
        std::copy(vertex.offsets.begin(), vertex.offsets.begin() + N, offsets.begin());

        if constexpr (is_indexed) {
            // Indexed draw
            scheduler.Record([buffers, offsets, index_buffer = *index.buffer,
                              index_offset = index.offset,
                              index_type = index.type](auto cmdbuf, auto& dld) {
                cmdbuf.bindIndexBuffer(index_buffer, index_offset, index_type, dld);
                cmdbuf.bindVertexBuffers(0, static_cast<u32>(N), buffers.data(), offsets.data(),
                                         dld);
            });
        } else {
            // Array draw
            scheduler.Record([buffers, offsets](auto cmdbuf, auto& dld) {
                cmdbuf.bindVertexBuffers(0, static_cast<u32>(N), buffers.data(), offsets.data(),
                                         dld);
            });
        }
    }
};

void RasterizerVulkan::DrawParameters::Draw(vk::CommandBuffer cmdbuf,
                                            const vk::DispatchLoaderDynamic& dld) const {
    if (is_indexed) {
        cmdbuf.drawIndexed(num_vertices, num_instances, 0, base_vertex, base_instance, dld);
    } else {
        cmdbuf.draw(num_vertices, num_instances, base_vertex, base_instance, dld);
    }
}

RasterizerVulkan::RasterizerVulkan(Core::System& system, Core::Frontend::EmuWindow& renderer,
                                   VKScreenInfo& screen_info, const VKDevice& device,
                                   VKResourceManager& resource_manager,
                                   VKMemoryManager& memory_manager, VKScheduler& scheduler)
    : RasterizerAccelerated{system.Memory()}, system{system}, render_window{renderer},
      screen_info{screen_info}, device{device}, resource_manager{resource_manager},
      memory_manager{memory_manager}, scheduler{scheduler},
      staging_pool(device, memory_manager, scheduler), descriptor_pool(device),
      update_descriptor_queue(device, scheduler),
      quad_array_pass(device, scheduler, descriptor_pool, staging_pool, update_descriptor_queue),
      uint8_pass(device, scheduler, descriptor_pool, staging_pool, update_descriptor_queue),
      texture_cache(system, *this, device, resource_manager, memory_manager, scheduler,
                    staging_pool),
      pipeline_cache(system, *this, device, scheduler, descriptor_pool, update_descriptor_queue),
      buffer_cache(*this, system, device, memory_manager, scheduler, staging_pool),
      sampler_cache(device), query_cache(system, *this, device, scheduler) {
    scheduler.SetQueryCache(query_cache);
}

RasterizerVulkan::~RasterizerVulkan() = default;

void RasterizerVulkan::Draw(bool is_indexed, bool is_instanced) {
    MICROPROFILE_SCOPE(Vulkan_Drawing);

    FlushWork();

    query_cache.UpdateCounters();

    const auto& gpu = system.GPU().Maxwell3D();
    GraphicsPipelineCacheKey key{GetFixedPipelineState(gpu.regs)};

    buffer_cache.Map(CalculateGraphicsStreamBufferSize(is_indexed));

    BufferBindings buffer_bindings;
    const DrawParameters draw_params =
        SetupGeometry(key.fixed_state, buffer_bindings, is_indexed, is_instanced);

    update_descriptor_queue.Acquire();
    sampled_views.clear();
    image_views.clear();

    const auto shaders = pipeline_cache.GetShaders();
    key.shaders = GetShaderAddresses(shaders);
    SetupShaderDescriptors(shaders);

    buffer_cache.Unmap();

    const auto texceptions = UpdateAttachments();
    SetupImageTransitions(texceptions, color_attachments, zeta_attachment);

    key.renderpass_params = GetRenderPassParams(texceptions);

    auto& pipeline = pipeline_cache.GetGraphicsPipeline(key);
    scheduler.BindGraphicsPipeline(pipeline.GetHandle());

    const auto renderpass = pipeline.GetRenderPass();
    const auto [framebuffer, render_area] = ConfigureFramebuffers(renderpass);
    scheduler.RequestRenderpass({renderpass, framebuffer, {{0, 0}, render_area}, 0, nullptr});

    UpdateDynamicStates();

    buffer_bindings.Bind(scheduler);

    if (device.IsNvDeviceDiagnosticCheckpoints()) {
        scheduler.Record(
            [&pipeline](auto cmdbuf, auto& dld) { cmdbuf.setCheckpointNV(&pipeline, dld); });
    }

    const auto pipeline_layout = pipeline.GetLayout();
    const auto descriptor_set = pipeline.CommitDescriptorSet();
    scheduler.Record([pipeline_layout, descriptor_set, draw_params](auto cmdbuf, auto& dld) {
        if (descriptor_set) {
            cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout,
                                      DESCRIPTOR_SET, 1, &descriptor_set, 0, nullptr, dld);
        }
        draw_params.Draw(cmdbuf, dld);
    });
}

void RasterizerVulkan::Clear() {
    MICROPROFILE_SCOPE(Vulkan_Clearing);

    query_cache.UpdateCounters();

    const auto& gpu = system.GPU().Maxwell3D();
    if (!system.GPU().Maxwell3D().ShouldExecute()) {
        return;
    }

    const auto& regs = gpu.regs;
    const bool use_color = regs.clear_buffers.R || regs.clear_buffers.G || regs.clear_buffers.B ||
                           regs.clear_buffers.A;
    const bool use_depth = regs.clear_buffers.Z;
    const bool use_stencil = regs.clear_buffers.S;
    if (!use_color && !use_depth && !use_stencil) {
        return;
    }
    // Clearing images requires to be out of a renderpass
    scheduler.RequestOutsideRenderPassOperationContext();

    // TODO(Rodrigo): Implement clears rendering a quad or using beginning a renderpass.

    if (use_color) {
        View color_view;
        {
            MICROPROFILE_SCOPE(Vulkan_RenderTargets);
            color_view = texture_cache.GetColorBufferSurface(regs.clear_buffers.RT.Value(), false);
        }

        color_view->Transition(vk::ImageLayout::eTransferDstOptimal,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::AccessFlagBits::eTransferWrite);

        const std::array clear_color = {regs.clear_color[0], regs.clear_color[1],
                                        regs.clear_color[2], regs.clear_color[3]};
        const vk::ClearColorValue clear(clear_color);
        scheduler.Record([image = color_view->GetImage(),
                          subresource = color_view->GetImageSubresourceRange(),
                          clear](auto cmdbuf, auto& dld) {
            cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, clear, subresource,
                                   dld);
        });
    }
    if (use_depth || use_stencil) {
        View zeta_surface;
        {
            MICROPROFILE_SCOPE(Vulkan_RenderTargets);
            zeta_surface = texture_cache.GetDepthBufferSurface(false);
        }

        zeta_surface->Transition(vk::ImageLayout::eTransferDstOptimal,
                                 vk::PipelineStageFlagBits::eTransfer,
                                 vk::AccessFlagBits::eTransferWrite);

        const vk::ClearDepthStencilValue clear(regs.clear_depth,
                                               static_cast<u32>(regs.clear_stencil));
        scheduler.Record([image = zeta_surface->GetImage(),
                          subresource = zeta_surface->GetImageSubresourceRange(),
                          clear](auto cmdbuf, auto& dld) {
            cmdbuf.clearDepthStencilImage(image, vk::ImageLayout::eTransferDstOptimal, clear,
                                          subresource, dld);
        });
    }
}

void RasterizerVulkan::DispatchCompute(GPUVAddr code_addr) {
    MICROPROFILE_SCOPE(Vulkan_Compute);
    update_descriptor_queue.Acquire();
    sampled_views.clear();
    image_views.clear();

    query_cache.UpdateCounters();

    const auto& launch_desc = system.GPU().KeplerCompute().launch_description;
    const ComputePipelineCacheKey key{
        code_addr,
        launch_desc.shared_alloc,
        {launch_desc.block_dim_x, launch_desc.block_dim_y, launch_desc.block_dim_z}};
    auto& pipeline = pipeline_cache.GetComputePipeline(key);

    // Compute dispatches can't be executed inside a renderpass
    scheduler.RequestOutsideRenderPassOperationContext();

    buffer_cache.Map(CalculateComputeStreamBufferSize());

    const auto& entries = pipeline.GetEntries();
    SetupComputeConstBuffers(entries);
    SetupComputeGlobalBuffers(entries);
    SetupComputeTexelBuffers(entries);
    SetupComputeTextures(entries);
    SetupComputeImages(entries);

    buffer_cache.Unmap();

    TransitionImages(sampled_views, vk::PipelineStageFlagBits::eComputeShader,
                     vk::AccessFlagBits::eShaderRead);
    TransitionImages(image_views, vk::PipelineStageFlagBits::eComputeShader,
                     vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);

    if (device.IsNvDeviceDiagnosticCheckpoints()) {
        scheduler.Record(
            [&pipeline](auto cmdbuf, auto& dld) { cmdbuf.setCheckpointNV(nullptr, dld); });
    }

    scheduler.Record([grid_x = launch_desc.grid_dim_x, grid_y = launch_desc.grid_dim_y,
                      grid_z = launch_desc.grid_dim_z, pipeline_handle = pipeline.GetHandle(),
                      layout = pipeline.GetLayout(),
                      descriptor_set = pipeline.CommitDescriptorSet()](auto cmdbuf, auto& dld) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_handle, dld);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, DESCRIPTOR_SET, 1,
                                  &descriptor_set, 0, nullptr, dld);
        cmdbuf.dispatch(grid_x, grid_y, grid_z, dld);
    });
}

void RasterizerVulkan::ResetCounter(VideoCore::QueryType type) {
    query_cache.ResetCounter(type);
}

void RasterizerVulkan::Query(GPUVAddr gpu_addr, VideoCore::QueryType type,
                             std::optional<u64> timestamp) {
    query_cache.Query(gpu_addr, type, timestamp);
}

void RasterizerVulkan::FlushAll() {}

void RasterizerVulkan::FlushRegion(CacheAddr addr, u64 size) {
    texture_cache.FlushRegion(addr, size);
    buffer_cache.FlushRegion(addr, size);
    query_cache.FlushRegion(addr, size);
}

void RasterizerVulkan::InvalidateRegion(CacheAddr addr, u64 size) {
    texture_cache.InvalidateRegion(addr, size);
    pipeline_cache.InvalidateRegion(addr, size);
    buffer_cache.InvalidateRegion(addr, size);
    query_cache.InvalidateRegion(addr, size);
}

void RasterizerVulkan::FlushAndInvalidateRegion(CacheAddr addr, u64 size) {
    FlushRegion(addr, size);
    InvalidateRegion(addr, size);
}

void RasterizerVulkan::FlushCommands() {
    if (draw_counter > 0) {
        draw_counter = 0;
        scheduler.Flush();
    }
}

void RasterizerVulkan::TickFrame() {
    draw_counter = 0;
    update_descriptor_queue.TickFrame();
    buffer_cache.TickFrame();
    staging_pool.TickFrame();
}

bool RasterizerVulkan::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Regs::Surface& src,
                                             const Tegra::Engines::Fermi2D::Regs::Surface& dst,
                                             const Tegra::Engines::Fermi2D::Config& copy_config) {
    texture_cache.DoFermiCopy(src, dst, copy_config);
    return true;
}

bool RasterizerVulkan::AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                         VAddr framebuffer_addr, u32 pixel_stride) {
    if (!framebuffer_addr) {
        return false;
    }

    const u8* host_ptr{system.Memory().GetPointer(framebuffer_addr)};
    const auto surface{texture_cache.TryFindFramebufferSurface(host_ptr)};
    if (!surface) {
        return false;
    }

    // Verify that the cached surface is the same size and format as the requested framebuffer
    const auto& params{surface->GetSurfaceParams()};
    const auto& pixel_format{
        VideoCore::Surface::PixelFormatFromGPUPixelFormat(config.pixel_format)};
    ASSERT_MSG(params.width == config.width, "Framebuffer width is different");
    ASSERT_MSG(params.height == config.height, "Framebuffer height is different");

    screen_info.image = &surface->GetImage();
    screen_info.width = params.width;
    screen_info.height = params.height;
    screen_info.is_srgb = surface->GetSurfaceParams().srgb_conversion;
    return true;
}

void RasterizerVulkan::FlushWork() {
    static constexpr u32 DRAWS_TO_DISPATCH = 4096;

    // Only check multiples of 8 draws
    static_assert(DRAWS_TO_DISPATCH % 8 == 0);
    if ((++draw_counter & 7) != 7) {
        return;
    }

    if (draw_counter < DRAWS_TO_DISPATCH) {
        // Send recorded tasks to the worker thread
        scheduler.DispatchWork();
        return;
    }

    // Otherwise (every certain number of draws) flush execution.
    // This submits commands to the Vulkan driver.
    scheduler.Flush();
    draw_counter = 0;
}

RasterizerVulkan::Texceptions RasterizerVulkan::UpdateAttachments() {
    MICROPROFILE_SCOPE(Vulkan_RenderTargets);
    auto& dirty = system.GPU().Maxwell3D().dirty;
    const bool update_rendertargets = dirty.render_settings;
    dirty.render_settings = false;

    texture_cache.GuardRenderTargets(true);

    Texceptions texceptions;
    for (std::size_t rt = 0; rt < Maxwell::NumRenderTargets; ++rt) {
        if (update_rendertargets) {
            color_attachments[rt] = texture_cache.GetColorBufferSurface(rt, true);
        }
        if (color_attachments[rt] && WalkAttachmentOverlaps(*color_attachments[rt])) {
            texceptions[rt] = true;
        }
    }

    if (update_rendertargets) {
        zeta_attachment = texture_cache.GetDepthBufferSurface(true);
    }
    if (zeta_attachment && WalkAttachmentOverlaps(*zeta_attachment)) {
        texceptions[ZETA_TEXCEPTION_INDEX] = true;
    }

    texture_cache.GuardRenderTargets(false);

    return texceptions;
}

bool RasterizerVulkan::WalkAttachmentOverlaps(const CachedSurfaceView& attachment) {
    bool overlap = false;
    for (auto& [view, layout] : sampled_views) {
        if (!attachment.IsSameSurface(*view)) {
            continue;
        }
        overlap = true;
        *layout = vk::ImageLayout::eGeneral;
    }
    return overlap;
}

std::tuple<vk::Framebuffer, vk::Extent2D> RasterizerVulkan::ConfigureFramebuffers(
    vk::RenderPass renderpass) {
    FramebufferCacheKey key{renderpass, std::numeric_limits<u32>::max(),
                            std::numeric_limits<u32>::max()};

    const auto MarkAsModifiedAndPush = [&](const View& view) {
        if (view == nullptr) {
            return false;
        }
        key.views.push_back(view->GetHandle());
        key.width = std::min(key.width, view->GetWidth());
        key.height = std::min(key.height, view->GetHeight());
        return true;
    };

    for (std::size_t index = 0; index < std::size(color_attachments); ++index) {
        if (MarkAsModifiedAndPush(color_attachments[index])) {
            texture_cache.MarkColorBufferInUse(index);
        }
    }
    if (MarkAsModifiedAndPush(zeta_attachment)) {
        texture_cache.MarkDepthBufferInUse();
    }

    const auto [fbentry, is_cache_miss] = framebuffer_cache.try_emplace(key);
    auto& framebuffer = fbentry->second;
    if (is_cache_miss) {
        const vk::FramebufferCreateInfo framebuffer_ci({}, key.renderpass,
                                                       static_cast<u32>(key.views.size()),
                                                       key.views.data(), key.width, key.height, 1);
        const auto dev = device.GetLogical();
        const auto& dld = device.GetDispatchLoader();
        framebuffer = dev.createFramebufferUnique(framebuffer_ci, nullptr, dld);
    }

    return {*framebuffer, vk::Extent2D{key.width, key.height}};
}

RasterizerVulkan::DrawParameters RasterizerVulkan::SetupGeometry(FixedPipelineState& fixed_state,
                                                                 BufferBindings& buffer_bindings,
                                                                 bool is_indexed,
                                                                 bool is_instanced) {
    MICROPROFILE_SCOPE(Vulkan_Geometry);

    const auto& gpu = system.GPU().Maxwell3D();
    const auto& regs = gpu.regs;

    SetupVertexArrays(fixed_state.vertex_input, buffer_bindings);

    const u32 base_instance = regs.vb_base_instance;
    const u32 num_instances = is_instanced ? gpu.mme_draw.instance_count : 1;
    const u32 base_vertex = is_indexed ? regs.vb_element_base : regs.vertex_buffer.first;
    const u32 num_vertices = is_indexed ? regs.index_array.count : regs.vertex_buffer.count;

    DrawParameters params{base_instance, num_instances, base_vertex, num_vertices, is_indexed};
    SetupIndexBuffer(buffer_bindings, params, is_indexed);

    return params;
}

void RasterizerVulkan::SetupShaderDescriptors(
    const std::array<Shader, Maxwell::MaxShaderProgram>& shaders) {
    texture_cache.GuardSamplers(true);

    for (std::size_t stage = 0; stage < Maxwell::MaxShaderStage; ++stage) {
        // Skip VertexA stage
        const auto& shader = shaders[stage + 1];
        if (!shader) {
            continue;
        }
        const auto& entries = shader->GetEntries();
        SetupGraphicsConstBuffers(entries, stage);
        SetupGraphicsGlobalBuffers(entries, stage);
        SetupGraphicsTexelBuffers(entries, stage);
        SetupGraphicsTextures(entries, stage);
        SetupGraphicsImages(entries, stage);
    }
    texture_cache.GuardSamplers(false);
}

void RasterizerVulkan::SetupImageTransitions(
    Texceptions texceptions, const std::array<View, Maxwell::NumRenderTargets>& color_attachments,
    const View& zeta_attachment) {
    TransitionImages(sampled_views, vk::PipelineStageFlagBits::eAllGraphics,
                     vk::AccessFlagBits::eShaderRead);
    TransitionImages(image_views, vk::PipelineStageFlagBits::eAllGraphics,
                     vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);

    for (std::size_t rt = 0; rt < std::size(color_attachments); ++rt) {
        const auto color_attachment = color_attachments[rt];
        if (color_attachment == nullptr) {
            continue;
        }
        const auto image_layout =
            texceptions[rt] ? vk::ImageLayout::eGeneral : vk::ImageLayout::eColorAttachmentOptimal;
        color_attachment->Transition(
            image_layout, vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
    }

    if (zeta_attachment != nullptr) {
        const auto image_layout = texceptions[ZETA_TEXCEPTION_INDEX]
                                      ? vk::ImageLayout::eGeneral
                                      : vk::ImageLayout::eDepthStencilAttachmentOptimal;
        zeta_attachment->Transition(image_layout, vk::PipelineStageFlagBits::eLateFragmentTests,
                                    vk::AccessFlagBits::eDepthStencilAttachmentRead |
                                        vk::AccessFlagBits::eDepthStencilAttachmentWrite);
    }
}

void RasterizerVulkan::UpdateDynamicStates() {
    auto& gpu = system.GPU().Maxwell3D();
    UpdateViewportsState(gpu);
    UpdateScissorsState(gpu);
    UpdateDepthBias(gpu);
    UpdateBlendConstants(gpu);
    UpdateDepthBounds(gpu);
    UpdateStencilFaces(gpu);
}

void RasterizerVulkan::SetupVertexArrays(FixedPipelineState::VertexInput& vertex_input,
                                         BufferBindings& buffer_bindings) {
    const auto& regs = system.GPU().Maxwell3D().regs;

    for (u32 index = 0; index < static_cast<u32>(Maxwell::NumVertexAttributes); ++index) {
        const auto& attrib = regs.vertex_attrib_format[index];
        if (!attrib.IsValid()) {
            continue;
        }

        const auto& buffer = regs.vertex_array[attrib.buffer];
        ASSERT(buffer.IsEnabled());

        vertex_input.attributes[vertex_input.num_attributes++] =
            FixedPipelineState::VertexAttribute(index, attrib.buffer, attrib.type, attrib.size,
                                                attrib.offset);
    }

    for (u32 index = 0; index < static_cast<u32>(Maxwell::NumVertexArrays); ++index) {
        const auto& vertex_array = regs.vertex_array[index];
        if (!vertex_array.IsEnabled()) {
            continue;
        }

        const GPUVAddr start{vertex_array.StartAddress()};
        const GPUVAddr end{regs.vertex_array_limit[index].LimitAddress()};

        ASSERT(end > start);
        const std::size_t size{end - start + 1};
        const auto [buffer, offset] = buffer_cache.UploadMemory(start, size);

        vertex_input.bindings[vertex_input.num_bindings++] = FixedPipelineState::VertexBinding(
            index, vertex_array.stride,
            regs.instanced_arrays.IsInstancingEnabled(index) ? vertex_array.divisor : 0);
        buffer_bindings.AddVertexBinding(buffer, offset);
    }
}

void RasterizerVulkan::SetupIndexBuffer(BufferBindings& buffer_bindings, DrawParameters& params,
                                        bool is_indexed) {
    const auto& regs = system.GPU().Maxwell3D().regs;
    switch (regs.draw.topology) {
    case Maxwell::PrimitiveTopology::Quads:
        if (params.is_indexed) {
            UNIMPLEMENTED();
        } else {
            const auto [buffer, offset] =
                quad_array_pass.Assemble(params.num_vertices, params.base_vertex);
            buffer_bindings.SetIndexBinding(&buffer, offset, vk::IndexType::eUint32);
            params.base_vertex = 0;
            params.num_vertices = params.num_vertices * 6 / 4;
            params.is_indexed = true;
        }
        break;
    default: {
        if (!is_indexed) {
            break;
        }
        const GPUVAddr gpu_addr = regs.index_array.IndexStart();
        auto [buffer, offset] = buffer_cache.UploadMemory(gpu_addr, CalculateIndexBufferSize());

        auto format = regs.index_array.format;
        const bool is_uint8 = format == Maxwell::IndexFormat::UnsignedByte;
        if (is_uint8 && !device.IsExtIndexTypeUint8Supported()) {
            std::tie(buffer, offset) = uint8_pass.Assemble(params.num_vertices, *buffer, offset);
            format = Maxwell::IndexFormat::UnsignedShort;
        }

        buffer_bindings.SetIndexBinding(buffer, offset, MaxwellToVK::IndexFormat(device, format));
        break;
    }
    }
}

void RasterizerVulkan::SetupGraphicsConstBuffers(const ShaderEntries& entries, std::size_t stage) {
    MICROPROFILE_SCOPE(Vulkan_ConstBuffers);
    const auto& gpu = system.GPU().Maxwell3D();
    const auto& shader_stage = gpu.state.shader_stages[stage];
    for (const auto& entry : entries.const_buffers) {
        SetupConstBuffer(entry, shader_stage.const_buffers[entry.GetIndex()]);
    }
}

void RasterizerVulkan::SetupGraphicsGlobalBuffers(const ShaderEntries& entries, std::size_t stage) {
    MICROPROFILE_SCOPE(Vulkan_GlobalBuffers);
    auto& gpu{system.GPU()};
    const auto cbufs{gpu.Maxwell3D().state.shader_stages[stage]};

    for (const auto& entry : entries.global_buffers) {
        const auto addr = cbufs.const_buffers[entry.GetCbufIndex()].address + entry.GetCbufOffset();
        SetupGlobalBuffer(entry, addr);
    }
}

void RasterizerVulkan::SetupGraphicsTexelBuffers(const ShaderEntries& entries, std::size_t stage) {
    MICROPROFILE_SCOPE(Vulkan_Textures);
    const auto& gpu = system.GPU().Maxwell3D();
    for (const auto& entry : entries.texel_buffers) {
        const auto image = GetTextureInfo(gpu, entry, stage).tic;
        SetupTexelBuffer(image, entry);
    }
}

void RasterizerVulkan::SetupGraphicsTextures(const ShaderEntries& entries, std::size_t stage) {
    MICROPROFILE_SCOPE(Vulkan_Textures);
    const auto& gpu = system.GPU().Maxwell3D();
    for (const auto& entry : entries.samplers) {
        const auto texture = GetTextureInfo(gpu, entry, stage);
        SetupTexture(texture, entry);
    }
}

void RasterizerVulkan::SetupGraphicsImages(const ShaderEntries& entries, std::size_t stage) {
    MICROPROFILE_SCOPE(Vulkan_Images);
    const auto& gpu = system.GPU().KeplerCompute();
    for (const auto& entry : entries.images) {
        const auto tic = GetTextureInfo(gpu, entry, stage).tic;
        SetupImage(tic, entry);
    }
}

void RasterizerVulkan::SetupComputeConstBuffers(const ShaderEntries& entries) {
    MICROPROFILE_SCOPE(Vulkan_ConstBuffers);
    const auto& launch_desc = system.GPU().KeplerCompute().launch_description;
    for (const auto& entry : entries.const_buffers) {
        const auto& config = launch_desc.const_buffer_config[entry.GetIndex()];
        const std::bitset<8> mask = launch_desc.const_buffer_enable_mask.Value();
        Tegra::Engines::ConstBufferInfo buffer;
        buffer.address = config.Address();
        buffer.size = config.size;
        buffer.enabled = mask[entry.GetIndex()];
        SetupConstBuffer(entry, buffer);
    }
}

void RasterizerVulkan::SetupComputeGlobalBuffers(const ShaderEntries& entries) {
    MICROPROFILE_SCOPE(Vulkan_GlobalBuffers);
    const auto cbufs{system.GPU().KeplerCompute().launch_description.const_buffer_config};
    for (const auto& entry : entries.global_buffers) {
        const auto addr{cbufs[entry.GetCbufIndex()].Address() + entry.GetCbufOffset()};
        SetupGlobalBuffer(entry, addr);
    }
}

void RasterizerVulkan::SetupComputeTexelBuffers(const ShaderEntries& entries) {
    MICROPROFILE_SCOPE(Vulkan_Textures);
    const auto& gpu = system.GPU().KeplerCompute();
    for (const auto& entry : entries.texel_buffers) {
        const auto image = GetTextureInfo(gpu, entry, ComputeShaderIndex).tic;
        SetupTexelBuffer(image, entry);
    }
}

void RasterizerVulkan::SetupComputeTextures(const ShaderEntries& entries) {
    MICROPROFILE_SCOPE(Vulkan_Textures);
    const auto& gpu = system.GPU().KeplerCompute();
    for (const auto& entry : entries.samplers) {
        const auto texture = GetTextureInfo(gpu, entry, ComputeShaderIndex);
        SetupTexture(texture, entry);
    }
}

void RasterizerVulkan::SetupComputeImages(const ShaderEntries& entries) {
    MICROPROFILE_SCOPE(Vulkan_Images);
    const auto& gpu = system.GPU().KeplerCompute();
    for (const auto& entry : entries.images) {
        const auto tic = GetTextureInfo(gpu, entry, ComputeShaderIndex).tic;
        SetupImage(tic, entry);
    }
}

void RasterizerVulkan::SetupConstBuffer(const ConstBufferEntry& entry,
                                        const Tegra::Engines::ConstBufferInfo& buffer) {
    // Align the size to avoid bad std140 interactions
    const std::size_t size =
        Common::AlignUp(CalculateConstBufferSize(entry, buffer), 4 * sizeof(float));
    ASSERT(size <= MaxConstbufferSize);

    const auto [buffer_handle, offset] =
        buffer_cache.UploadMemory(buffer.address, size, device.GetUniformBufferAlignment());

    update_descriptor_queue.AddBuffer(buffer_handle, offset, size);
}

void RasterizerVulkan::SetupGlobalBuffer(const GlobalBufferEntry& entry, GPUVAddr address) {
    auto& memory_manager{system.GPU().MemoryManager()};
    const auto actual_addr = memory_manager.Read<u64>(address);
    const auto size = memory_manager.Read<u32>(address + 8);

    if (size == 0) {
        // Sometimes global memory pointers don't have a proper size. Upload a dummy entry because
        // Vulkan doesn't like empty buffers.
        constexpr std::size_t dummy_size = 4;
        const auto buffer = buffer_cache.GetEmptyBuffer(dummy_size);
        update_descriptor_queue.AddBuffer(buffer, 0, dummy_size);
        return;
    }

    const auto [buffer, offset] = buffer_cache.UploadMemory(
        actual_addr, size, device.GetStorageBufferAlignment(), entry.IsWritten());
    update_descriptor_queue.AddBuffer(buffer, offset, size);
}

void RasterizerVulkan::SetupTexelBuffer(const Tegra::Texture::TICEntry& tic,
                                        const TexelBufferEntry& entry) {
    const auto view = texture_cache.GetTextureSurface(tic, entry);
    ASSERT(view->IsBufferView());

    update_descriptor_queue.AddTexelBuffer(view->GetBufferView());
}

void RasterizerVulkan::SetupTexture(const Tegra::Texture::FullTextureInfo& texture,
                                    const SamplerEntry& entry) {
    auto view = texture_cache.GetTextureSurface(texture.tic, entry);
    ASSERT(!view->IsBufferView());

    const auto image_view = view->GetHandle(texture.tic.x_source, texture.tic.y_source,
                                            texture.tic.z_source, texture.tic.w_source);
    const auto sampler = sampler_cache.GetSampler(texture.tsc);
    update_descriptor_queue.AddSampledImage(sampler, image_view);

    const auto image_layout = update_descriptor_queue.GetLastImageLayout();
    *image_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    sampled_views.push_back(ImageView{std::move(view), image_layout});
}

void RasterizerVulkan::SetupImage(const Tegra::Texture::TICEntry& tic, const ImageEntry& entry) {
    auto view = texture_cache.GetImageSurface(tic, entry);

    if (entry.IsWritten()) {
        view->MarkAsModified(texture_cache.Tick());
    }

    UNIMPLEMENTED_IF(tic.IsBuffer());

    const auto image_view = view->GetHandle(tic.x_source, tic.y_source, tic.z_source, tic.w_source);
    update_descriptor_queue.AddImage(image_view);

    const auto image_layout = update_descriptor_queue.GetLastImageLayout();
    *image_layout = vk::ImageLayout::eGeneral;
    image_views.push_back(ImageView{std::move(view), image_layout});
}

void RasterizerVulkan::UpdateViewportsState(Tegra::Engines::Maxwell3D& gpu) {
    if (!gpu.dirty.viewport_transform && scheduler.TouchViewports()) {
        return;
    }
    gpu.dirty.viewport_transform = false;
    const auto& regs = gpu.regs;
    const std::array viewports{
        GetViewportState(device, regs, 0),  GetViewportState(device, regs, 1),
        GetViewportState(device, regs, 2),  GetViewportState(device, regs, 3),
        GetViewportState(device, regs, 4),  GetViewportState(device, regs, 5),
        GetViewportState(device, regs, 6),  GetViewportState(device, regs, 7),
        GetViewportState(device, regs, 8),  GetViewportState(device, regs, 9),
        GetViewportState(device, regs, 10), GetViewportState(device, regs, 11),
        GetViewportState(device, regs, 12), GetViewportState(device, regs, 13),
        GetViewportState(device, regs, 14), GetViewportState(device, regs, 15)};
    scheduler.Record([viewports](auto cmdbuf, auto& dld) {
        cmdbuf.setViewport(0, static_cast<u32>(viewports.size()), viewports.data(), dld);
    });
}

void RasterizerVulkan::UpdateScissorsState(Tegra::Engines::Maxwell3D& gpu) {
    if (!gpu.dirty.scissor_test && scheduler.TouchScissors()) {
        return;
    }
    gpu.dirty.scissor_test = false;
    const auto& regs = gpu.regs;
    const std::array scissors = {
        GetScissorState(regs, 0),  GetScissorState(regs, 1),  GetScissorState(regs, 2),
        GetScissorState(regs, 3),  GetScissorState(regs, 4),  GetScissorState(regs, 5),
        GetScissorState(regs, 6),  GetScissorState(regs, 7),  GetScissorState(regs, 8),
        GetScissorState(regs, 9),  GetScissorState(regs, 10), GetScissorState(regs, 11),
        GetScissorState(regs, 12), GetScissorState(regs, 13), GetScissorState(regs, 14),
        GetScissorState(regs, 15)};
    scheduler.Record([scissors](auto cmdbuf, auto& dld) {
        cmdbuf.setScissor(0, static_cast<u32>(scissors.size()), scissors.data(), dld);
    });
}

void RasterizerVulkan::UpdateDepthBias(Tegra::Engines::Maxwell3D& gpu) {
    if (!gpu.dirty.polygon_offset && scheduler.TouchDepthBias()) {
        return;
    }
    gpu.dirty.polygon_offset = false;
    const auto& regs = gpu.regs;
    scheduler.Record([constant = regs.polygon_offset_units, clamp = regs.polygon_offset_clamp,
                      factor = regs.polygon_offset_factor](auto cmdbuf, auto& dld) {
        cmdbuf.setDepthBias(constant, clamp, factor / 2.0f, dld);
    });
}

void RasterizerVulkan::UpdateBlendConstants(Tegra::Engines::Maxwell3D& gpu) {
    if (!gpu.dirty.blend_state && scheduler.TouchBlendConstants()) {
        return;
    }
    gpu.dirty.blend_state = false;
    const std::array blend_color = {gpu.regs.blend_color.r, gpu.regs.blend_color.g,
                                    gpu.regs.blend_color.b, gpu.regs.blend_color.a};
    scheduler.Record([blend_color](auto cmdbuf, auto& dld) {
        cmdbuf.setBlendConstants(blend_color.data(), dld);
    });
}

void RasterizerVulkan::UpdateDepthBounds(Tegra::Engines::Maxwell3D& gpu) {
    if (!gpu.dirty.depth_bounds_values && scheduler.TouchDepthBounds()) {
        return;
    }
    gpu.dirty.depth_bounds_values = false;
    const auto& regs = gpu.regs;
    scheduler.Record([min = regs.depth_bounds[0], max = regs.depth_bounds[1]](
                         auto cmdbuf, auto& dld) { cmdbuf.setDepthBounds(min, max, dld); });
}

void RasterizerVulkan::UpdateStencilFaces(Tegra::Engines::Maxwell3D& gpu) {
    if (!gpu.dirty.stencil_test && scheduler.TouchStencilValues()) {
        return;
    }
    gpu.dirty.stencil_test = false;
    const auto& regs = gpu.regs;
    if (regs.stencil_two_side_enable) {
        // Separate values per face
        scheduler.Record(
            [front_ref = regs.stencil_front_func_ref, front_write_mask = regs.stencil_front_mask,
             front_test_mask = regs.stencil_front_func_mask, back_ref = regs.stencil_back_func_ref,
             back_write_mask = regs.stencil_back_mask,
             back_test_mask = regs.stencil_back_func_mask](auto cmdbuf, auto& dld) {
                // Front face
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront, front_ref, dld);
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront, front_write_mask, dld);
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront, front_test_mask, dld);

                // Back face
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, back_ref, dld);
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, back_write_mask, dld);
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack, back_test_mask, dld);
            });
    } else {
        // Front face defines both faces
        scheduler.Record([ref = regs.stencil_back_func_ref, write_mask = regs.stencil_back_mask,
                          test_mask = regs.stencil_back_func_mask](auto cmdbuf, auto& dld) {
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, ref, dld);
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, write_mask, dld);
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack, test_mask, dld);
        });
    }
}

std::size_t RasterizerVulkan::CalculateGraphicsStreamBufferSize(bool is_indexed) const {
    std::size_t size = CalculateVertexArraysSize();
    if (is_indexed) {
        size = Common::AlignUp(size, 4) + CalculateIndexBufferSize();
    }
    size += Maxwell::MaxConstBuffers * (MaxConstbufferSize + device.GetUniformBufferAlignment());
    return size;
}

std::size_t RasterizerVulkan::CalculateComputeStreamBufferSize() const {
    return Tegra::Engines::KeplerCompute::NumConstBuffers *
           (Maxwell::MaxConstBufferSize + device.GetUniformBufferAlignment());
}

std::size_t RasterizerVulkan::CalculateVertexArraysSize() const {
    const auto& regs = system.GPU().Maxwell3D().regs;

    std::size_t size = 0;
    for (u32 index = 0; index < Maxwell::NumVertexArrays; ++index) {
        // This implementation assumes that all attributes are used in the shader.
        const GPUVAddr start{regs.vertex_array[index].StartAddress()};
        const GPUVAddr end{regs.vertex_array_limit[index].LimitAddress()};
        DEBUG_ASSERT(end > start);

        size += (end - start + 1) * regs.vertex_array[index].enable;
    }
    return size;
}

std::size_t RasterizerVulkan::CalculateIndexBufferSize() const {
    const auto& regs = system.GPU().Maxwell3D().regs;
    return static_cast<std::size_t>(regs.index_array.count) *
           static_cast<std::size_t>(regs.index_array.FormatSizeInBytes());
}

std::size_t RasterizerVulkan::CalculateConstBufferSize(
    const ConstBufferEntry& entry, const Tegra::Engines::ConstBufferInfo& buffer) const {
    if (entry.IsIndirect()) {
        // Buffer is accessed indirectly, so upload the entire thing
        return buffer.size;
    } else {
        // Buffer is accessed directly, upload just what we use
        return entry.GetSize();
    }
}

RenderPassParams RasterizerVulkan::GetRenderPassParams(Texceptions texceptions) const {
    using namespace VideoCore::Surface;

    const auto& regs = system.GPU().Maxwell3D().regs;
    RenderPassParams renderpass_params;

    for (std::size_t rt = 0; rt < static_cast<std::size_t>(regs.rt_control.count); ++rt) {
        const auto& rendertarget = regs.rt[rt];
        if (rendertarget.Address() == 0 || rendertarget.format == Tegra::RenderTargetFormat::NONE) {
            continue;
        }
        renderpass_params.color_attachments.push_back(RenderPassParams::ColorAttachment{
            static_cast<u32>(rt), PixelFormatFromRenderTargetFormat(rendertarget.format),
            texceptions[rt]});
    }

    renderpass_params.has_zeta = regs.zeta_enable;
    if (renderpass_params.has_zeta) {
        renderpass_params.zeta_pixel_format = PixelFormatFromDepthFormat(regs.zeta.format);
        renderpass_params.zeta_texception = texceptions[ZETA_TEXCEPTION_INDEX];
    }

    return renderpass_params;
}

} // namespace Vulkan
