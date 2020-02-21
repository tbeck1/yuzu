// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include <cstring>
#include <optional>
#include "common/assert.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/shader_type.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/texture.h"

namespace Tegra::Engines {

using VideoCore::QueryType;

/// First register id that is actually a Macro call.
constexpr u32 MacroRegistersStart = 0xE00;

Maxwell3D::Maxwell3D(Core::System& system, VideoCore::RasterizerInterface& rasterizer,
                     MemoryManager& memory_manager)
    : system{system}, rasterizer{rasterizer}, memory_manager{memory_manager},
      macro_interpreter{*this}, upload_state{memory_manager, regs.upload} {
    InitDirtySettings();
    InitializeRegisterDefaults();
}

void Maxwell3D::InitializeRegisterDefaults() {
    // Initializes registers to their default values - what games expect them to be at boot. This is
    // for certain registers that may not be explicitly set by games.

    // Reset all registers to zero
    std::memset(&regs, 0, sizeof(regs));

    // Depth range near/far is not always set, but is expected to be the default 0.0f, 1.0f. This is
    // needed for ARMS.
    for (auto& viewport : regs.viewports) {
        viewport.depth_range_near = 0.0f;
        viewport.depth_range_far = 1.0f;
    }

    // Doom and Bomberman seems to use the uninitialized registers and just enable blend
    // so initialize blend registers with sane values
    regs.blend.equation_rgb = Regs::Blend::Equation::Add;
    regs.blend.factor_source_rgb = Regs::Blend::Factor::One;
    regs.blend.factor_dest_rgb = Regs::Blend::Factor::Zero;
    regs.blend.equation_a = Regs::Blend::Equation::Add;
    regs.blend.factor_source_a = Regs::Blend::Factor::One;
    regs.blend.factor_dest_a = Regs::Blend::Factor::Zero;
    for (auto& blend : regs.independent_blend) {
        blend.equation_rgb = Regs::Blend::Equation::Add;
        blend.factor_source_rgb = Regs::Blend::Factor::One;
        blend.factor_dest_rgb = Regs::Blend::Factor::Zero;
        blend.equation_a = Regs::Blend::Equation::Add;
        blend.factor_source_a = Regs::Blend::Factor::One;
        blend.factor_dest_a = Regs::Blend::Factor::Zero;
    }
    regs.stencil_front_op_fail = Regs::StencilOp::Keep;
    regs.stencil_front_op_zfail = Regs::StencilOp::Keep;
    regs.stencil_front_op_zpass = Regs::StencilOp::Keep;
    regs.stencil_front_func_func = Regs::ComparisonOp::Always;
    regs.stencil_front_func_mask = 0xFFFFFFFF;
    regs.stencil_front_mask = 0xFFFFFFFF;
    regs.stencil_two_side_enable = 1;
    regs.stencil_back_op_fail = Regs::StencilOp::Keep;
    regs.stencil_back_op_zfail = Regs::StencilOp::Keep;
    regs.stencil_back_op_zpass = Regs::StencilOp::Keep;
    regs.stencil_back_func_func = Regs::ComparisonOp::Always;
    regs.stencil_back_func_mask = 0xFFFFFFFF;
    regs.stencil_back_mask = 0xFFFFFFFF;

    regs.depth_test_func = Regs::ComparisonOp::Always;
    regs.cull.front_face = Regs::Cull::FrontFace::CounterClockWise;
    regs.cull.cull_face = Regs::Cull::CullFace::Back;

    // TODO(Rodrigo): Most games do not set a point size. I think this is a case of a
    // register carrying a default value. Assume it's OpenGL's default (1).
    regs.point_size = 1.0f;

    // TODO(bunnei): Some games do not initialize the color masks (e.g. Sonic Mania). Assuming a
    // default of enabled fixes rendering here.
    for (auto& color_mask : regs.color_mask) {
        color_mask.R.Assign(1);
        color_mask.G.Assign(1);
        color_mask.B.Assign(1);
        color_mask.A.Assign(1);
    }

    // NVN games expect these values to be enabled at boot
    regs.rasterize_enable = 1;
    regs.rt_separate_frag_data = 1;
    regs.framebuffer_srgb = 1;
    regs.cull.front_face = Maxwell3D::Regs::Cull::FrontFace::ClockWise;

    mme_inline[MAXWELL3D_REG_INDEX(draw.vertex_end_gl)] = true;
    mme_inline[MAXWELL3D_REG_INDEX(draw.vertex_begin_gl)] = true;
    mme_inline[MAXWELL3D_REG_INDEX(vertex_buffer.count)] = true;
    mme_inline[MAXWELL3D_REG_INDEX(index_array.count)] = true;
}

#define DIRTY_REGS_POS(field_name) static_cast<u8>(offsetof(Maxwell3D::DirtyRegs, field_name))

void Maxwell3D::InitDirtySettings() {
    const auto set_block = [this](std::size_t start, std::size_t range, u8 position) {
        const auto start_itr = dirty_pointers.begin() + start;
        const auto end_itr = start_itr + range;
        std::fill(start_itr, end_itr, position);
    };
    dirty.regs.fill(true);

    // Init Render Targets
    constexpr u32 registers_per_rt = sizeof(regs.rt[0]) / sizeof(u32);
    constexpr u32 rt_start_reg = MAXWELL3D_REG_INDEX(rt);
    constexpr u32 rt_end_reg = rt_start_reg + registers_per_rt * 8;
    u8 rt_dirty_reg = DIRTY_REGS_POS(render_target);
    for (u32 rt_reg = rt_start_reg; rt_reg < rt_end_reg; rt_reg += registers_per_rt) {
        set_block(rt_reg, registers_per_rt, rt_dirty_reg);
        ++rt_dirty_reg;
    }
    constexpr u32 depth_buffer_flag = DIRTY_REGS_POS(depth_buffer);
    dirty_pointers[MAXWELL3D_REG_INDEX(zeta_enable)] = depth_buffer_flag;
    dirty_pointers[MAXWELL3D_REG_INDEX(zeta_width)] = depth_buffer_flag;
    dirty_pointers[MAXWELL3D_REG_INDEX(zeta_height)] = depth_buffer_flag;
    constexpr u32 registers_in_zeta = sizeof(regs.zeta) / sizeof(u32);
    constexpr u32 zeta_reg = MAXWELL3D_REG_INDEX(zeta);
    set_block(zeta_reg, registers_in_zeta, depth_buffer_flag);

    // Init Vertex Arrays
    constexpr u32 vertex_array_start = MAXWELL3D_REG_INDEX(vertex_array);
    constexpr u32 vertex_array_size = sizeof(regs.vertex_array[0]) / sizeof(u32);
    constexpr u32 vertex_array_end = vertex_array_start + vertex_array_size * Regs::NumVertexArrays;
    u8 va_dirty_reg = DIRTY_REGS_POS(vertex_array);
    u8 vi_dirty_reg = DIRTY_REGS_POS(vertex_instance);
    for (u32 vertex_reg = vertex_array_start; vertex_reg < vertex_array_end;
         vertex_reg += vertex_array_size) {
        set_block(vertex_reg, 3, va_dirty_reg);
        // The divisor concerns vertex array instances
        dirty_pointers[static_cast<std::size_t>(vertex_reg) + 3] = vi_dirty_reg;
        ++va_dirty_reg;
        ++vi_dirty_reg;
    }
    constexpr u32 vertex_limit_start = MAXWELL3D_REG_INDEX(vertex_array_limit);
    constexpr u32 vertex_limit_size = sizeof(regs.vertex_array_limit[0]) / sizeof(u32);
    constexpr u32 vertex_limit_end = vertex_limit_start + vertex_limit_size * Regs::NumVertexArrays;
    va_dirty_reg = DIRTY_REGS_POS(vertex_array);
    for (u32 vertex_reg = vertex_limit_start; vertex_reg < vertex_limit_end;
         vertex_reg += vertex_limit_size) {
        set_block(vertex_reg, vertex_limit_size, va_dirty_reg);
        va_dirty_reg++;
    }
    constexpr u32 vertex_instance_start = MAXWELL3D_REG_INDEX(instanced_arrays);
    constexpr u32 vertex_instance_size =
        sizeof(regs.instanced_arrays.is_instanced[0]) / sizeof(u32);
    constexpr u32 vertex_instance_end =
        vertex_instance_start + vertex_instance_size * Regs::NumVertexArrays;
    vi_dirty_reg = DIRTY_REGS_POS(vertex_instance);
    for (u32 vertex_reg = vertex_instance_start; vertex_reg < vertex_instance_end;
         vertex_reg += vertex_instance_size) {
        set_block(vertex_reg, vertex_instance_size, vi_dirty_reg);
        vi_dirty_reg++;
    }
    set_block(MAXWELL3D_REG_INDEX(vertex_attrib_format), regs.vertex_attrib_format.size(),
              DIRTY_REGS_POS(vertex_attrib_format));

    // Init Shaders
    constexpr u32 shader_registers_count =
        sizeof(regs.shader_config[0]) * Regs::MaxShaderProgram / sizeof(u32);
    set_block(MAXWELL3D_REG_INDEX(shader_config[0]), shader_registers_count,
              DIRTY_REGS_POS(shaders));

    // State

    // Viewport
    constexpr u8 viewport_dirty_reg = DIRTY_REGS_POS(viewport);
    constexpr u32 viewport_start = MAXWELL3D_REG_INDEX(viewports);
    constexpr u32 viewport_size = sizeof(regs.viewports) / sizeof(u32);
    set_block(viewport_start, viewport_size, viewport_dirty_reg);
    constexpr u32 view_volume_start = MAXWELL3D_REG_INDEX(view_volume_clip_control);
    constexpr u32 view_volume_size = sizeof(regs.view_volume_clip_control) / sizeof(u32);
    set_block(view_volume_start, view_volume_size, viewport_dirty_reg);

    // Viewport transformation
    constexpr u32 viewport_trans_start = MAXWELL3D_REG_INDEX(viewport_transform);
    constexpr u32 viewport_trans_size = sizeof(regs.viewport_transform) / sizeof(u32);
    set_block(viewport_trans_start, viewport_trans_size, DIRTY_REGS_POS(viewport_transform));

    // Cullmode
    constexpr u32 cull_mode_start = MAXWELL3D_REG_INDEX(cull);
    constexpr u32 cull_mode_size = sizeof(regs.cull) / sizeof(u32);
    set_block(cull_mode_start, cull_mode_size, DIRTY_REGS_POS(cull_mode));

    // Screen y control
    dirty_pointers[MAXWELL3D_REG_INDEX(screen_y_control)] = DIRTY_REGS_POS(screen_y_control);

    // Primitive Restart
    constexpr u32 primitive_restart_start = MAXWELL3D_REG_INDEX(primitive_restart);
    constexpr u32 primitive_restart_size = sizeof(regs.primitive_restart) / sizeof(u32);
    set_block(primitive_restart_start, primitive_restart_size, DIRTY_REGS_POS(primitive_restart));

    // Depth Test
    constexpr u8 depth_test_dirty_reg = DIRTY_REGS_POS(depth_test);
    dirty_pointers[MAXWELL3D_REG_INDEX(depth_test_enable)] = depth_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(depth_write_enabled)] = depth_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(depth_test_func)] = depth_test_dirty_reg;

    // Stencil Test
    constexpr u32 stencil_test_dirty_reg = DIRTY_REGS_POS(stencil_test);
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_enable)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_front_func_func)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_front_func_ref)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_front_func_mask)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_front_op_fail)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_front_op_zfail)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_front_op_zpass)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_front_mask)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_two_side_enable)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_back_func_func)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_back_func_ref)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_back_func_mask)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_back_op_fail)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_back_op_zfail)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_back_op_zpass)] = stencil_test_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(stencil_back_mask)] = stencil_test_dirty_reg;

    // Color Mask
    constexpr u8 color_mask_dirty_reg = DIRTY_REGS_POS(color_mask);
    dirty_pointers[MAXWELL3D_REG_INDEX(color_mask_common)] = color_mask_dirty_reg;
    set_block(MAXWELL3D_REG_INDEX(color_mask), sizeof(regs.color_mask) / sizeof(u32),
              color_mask_dirty_reg);
    // Blend State
    constexpr u8 blend_state_dirty_reg = DIRTY_REGS_POS(blend_state);
    set_block(MAXWELL3D_REG_INDEX(blend_color), sizeof(regs.blend_color) / sizeof(u32),
              blend_state_dirty_reg);
    dirty_pointers[MAXWELL3D_REG_INDEX(independent_blend_enable)] = blend_state_dirty_reg;
    set_block(MAXWELL3D_REG_INDEX(blend), sizeof(regs.blend) / sizeof(u32), blend_state_dirty_reg);
    set_block(MAXWELL3D_REG_INDEX(independent_blend), sizeof(regs.independent_blend) / sizeof(u32),
              blend_state_dirty_reg);

    // Scissor State
    constexpr u8 scissor_test_dirty_reg = DIRTY_REGS_POS(scissor_test);
    set_block(MAXWELL3D_REG_INDEX(scissor_test), sizeof(regs.scissor_test) / sizeof(u32),
              scissor_test_dirty_reg);

    // Polygon Offset
    constexpr u8 polygon_offset_dirty_reg = DIRTY_REGS_POS(polygon_offset);
    dirty_pointers[MAXWELL3D_REG_INDEX(polygon_offset_fill_enable)] = polygon_offset_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(polygon_offset_line_enable)] = polygon_offset_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(polygon_offset_point_enable)] = polygon_offset_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(polygon_offset_units)] = polygon_offset_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(polygon_offset_factor)] = polygon_offset_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(polygon_offset_clamp)] = polygon_offset_dirty_reg;

    // Depth bounds
    constexpr u8 depth_bounds_values_dirty_reg = DIRTY_REGS_POS(depth_bounds_values);
    dirty_pointers[MAXWELL3D_REG_INDEX(depth_bounds[0])] = depth_bounds_values_dirty_reg;
    dirty_pointers[MAXWELL3D_REG_INDEX(depth_bounds[1])] = depth_bounds_values_dirty_reg;
}

void Maxwell3D::CallMacroMethod(u32 method, std::size_t num_parameters, const u32* parameters) {
    // Reset the current macro.
    executing_macro = 0;

    // Lookup the macro offset
    const u32 entry =
        ((method - MacroRegistersStart) >> 1) % static_cast<u32>(macro_positions.size());

    // Execute the current macro.
    macro_interpreter.Execute(macro_positions[entry], num_parameters, parameters);
    if (mme_draw.current_mode != MMEDrawMode::Undefined) {
        FlushMMEInlineDraw();
    }
}

void Maxwell3D::CallMethod(const GPU::MethodCall& method_call) {
    const u32 method = method_call.method;

    if (method == cb_data_state.current) {
        regs.reg_array[method] = method_call.argument;
        ProcessCBData(method_call.argument);
        return;
    } else if (cb_data_state.current != null_cb_data) {
        FinishCBData();
    }

    // It is an error to write to a register other than the current macro's ARG register before it
    // has finished execution.
    if (executing_macro != 0) {
        ASSERT(method == executing_macro + 1);
    }

    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        // We're trying to execute a macro
        if (executing_macro == 0) {
            // A macro call must begin by writing the macro method's register, not its argument.
            ASSERT_MSG((method % 2) == 0,
                       "Can't start macro execution by writing to the ARGS register");
            executing_macro = method;
        }

        macro_params.push_back(method_call.argument);

        // Call the macro when there are no more parameters in the command buffer
        if (method_call.IsLastCall()) {
            CallMacroMethod(executing_macro, macro_params.size(), macro_params.data());
            macro_params.clear();
        }
        return;
    }

    ASSERT_MSG(method < Regs::NUM_REGS,
               "Invalid Maxwell3D register, increase the size of the Regs structure");

    if (regs.reg_array[method] != method_call.argument) {
        regs.reg_array[method] = method_call.argument;
        const std::size_t dirty_reg = dirty_pointers[method];
        if (dirty_reg) {
            dirty.regs[dirty_reg] = true;
            if (dirty_reg >= DIRTY_REGS_POS(vertex_array) &&
                dirty_reg < DIRTY_REGS_POS(vertex_array_buffers)) {
                dirty.vertex_array_buffers = true;
            } else if (dirty_reg >= DIRTY_REGS_POS(vertex_instance) &&
                       dirty_reg < DIRTY_REGS_POS(vertex_instances)) {
                dirty.vertex_instances = true;
            } else if (dirty_reg >= DIRTY_REGS_POS(render_target) &&
                       dirty_reg < DIRTY_REGS_POS(render_settings)) {
                dirty.render_settings = true;
            }
        }
    }

    switch (method) {
    case MAXWELL3D_REG_INDEX(macros.data): {
        ProcessMacroUpload(method_call.argument);
        break;
    }
    case MAXWELL3D_REG_INDEX(macros.bind): {
        ProcessMacroBind(method_call.argument);
        break;
    }
    case MAXWELL3D_REG_INDEX(firmware[4]): {
        ProcessFirmwareCall4();
        break;
    }
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[0]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[1]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[2]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[3]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[4]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[5]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[6]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[7]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[8]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[9]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[10]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[11]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[12]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[13]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[14]):
    case MAXWELL3D_REG_INDEX(const_buffer.cb_data[15]): {
        StartCBData(method);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[0]): {
        ProcessCBBind(0);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[1]): {
        ProcessCBBind(1);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[2]): {
        ProcessCBBind(2);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[3]): {
        ProcessCBBind(3);
        break;
    }
    case MAXWELL3D_REG_INDEX(cb_bind[4]): {
        ProcessCBBind(4);
        break;
    }
    case MAXWELL3D_REG_INDEX(draw.vertex_end_gl): {
        DrawArrays();
        break;
    }
    case MAXWELL3D_REG_INDEX(clear_buffers): {
        ProcessClearBuffers();
        break;
    }
    case MAXWELL3D_REG_INDEX(query.query_get): {
        ProcessQueryGet();
        break;
    }
    case MAXWELL3D_REG_INDEX(condition.mode): {
        ProcessQueryCondition();
        break;
    }
    case MAXWELL3D_REG_INDEX(counter_reset): {
        ProcessCounterReset();
        break;
    }
    case MAXWELL3D_REG_INDEX(sync_info): {
        ProcessSyncPoint();
        break;
    }
    case MAXWELL3D_REG_INDEX(exec_upload): {
        upload_state.ProcessExec(regs.exec_upload.linear != 0);
        break;
    }
    case MAXWELL3D_REG_INDEX(data_upload): {
        const bool is_last_call = method_call.IsLastCall();
        upload_state.ProcessData(method_call.argument, is_last_call);
        if (is_last_call) {
            dirty.OnMemoryWrite();
        }
        break;
    }
    default:
        break;
    }
}

void Maxwell3D::StepInstance(const MMEDrawMode expected_mode, const u32 count) {
    if (mme_draw.current_mode == MMEDrawMode::Undefined) {
        if (mme_draw.gl_begin_consume) {
            mme_draw.current_mode = expected_mode;
            mme_draw.current_count = count;
            mme_draw.instance_count = 1;
            mme_draw.gl_begin_consume = false;
            mme_draw.gl_end_count = 0;
        }
        return;
    } else {
        if (mme_draw.current_mode == expected_mode && count == mme_draw.current_count &&
            mme_draw.instance_mode && mme_draw.gl_begin_consume) {
            mme_draw.instance_count++;
            mme_draw.gl_begin_consume = false;
            return;
        } else {
            FlushMMEInlineDraw();
        }
    }
    // Tail call in case it needs to retry.
    StepInstance(expected_mode, count);
}

void Maxwell3D::CallMethodFromMME(const GPU::MethodCall& method_call) {
    const u32 method = method_call.method;
    if (mme_inline[method]) {
        regs.reg_array[method] = method_call.argument;
        if (method == MAXWELL3D_REG_INDEX(vertex_buffer.count) ||
            method == MAXWELL3D_REG_INDEX(index_array.count)) {
            const MMEDrawMode expected_mode = method == MAXWELL3D_REG_INDEX(vertex_buffer.count)
                                                  ? MMEDrawMode::Array
                                                  : MMEDrawMode::Indexed;
            StepInstance(expected_mode, method_call.argument);
        } else if (method == MAXWELL3D_REG_INDEX(draw.vertex_begin_gl)) {
            mme_draw.instance_mode =
                (regs.draw.instance_next != 0) || (regs.draw.instance_cont != 0);
            mme_draw.gl_begin_consume = true;
        } else {
            mme_draw.gl_end_count++;
        }
    } else {
        if (mme_draw.current_mode != MMEDrawMode::Undefined) {
            FlushMMEInlineDraw();
        }
        CallMethod(method_call);
    }
}

void Maxwell3D::FlushMMEInlineDraw() {
    LOG_TRACE(HW_GPU, "called, topology={}, count={}", static_cast<u32>(regs.draw.topology.Value()),
              regs.vertex_buffer.count);
    ASSERT_MSG(!(regs.index_array.count && regs.vertex_buffer.count), "Both indexed and direct?");
    ASSERT(mme_draw.instance_count == mme_draw.gl_end_count);

    // Both instance configuration registers can not be set at the same time.
    ASSERT_MSG(!regs.draw.instance_next || !regs.draw.instance_cont,
               "Illegal combination of instancing parameters");

    const bool is_indexed = mme_draw.current_mode == MMEDrawMode::Indexed;
    if (ShouldExecute()) {
        rasterizer.Draw(is_indexed, true);
    }

    // TODO(bunnei): Below, we reset vertex count so that we can use these registers to determine if
    // the game is trying to draw indexed or direct mode. This needs to be verified on HW still -
    // it's possible that it is incorrect and that there is some other register used to specify the
    // drawing mode.
    if (is_indexed) {
        regs.index_array.count = 0;
    } else {
        regs.vertex_buffer.count = 0;
    }
    mme_draw.current_mode = MMEDrawMode::Undefined;
    mme_draw.current_count = 0;
    mme_draw.instance_count = 0;
    mme_draw.instance_mode = false;
    mme_draw.gl_begin_consume = false;
    mme_draw.gl_end_count = 0;
}

void Maxwell3D::ProcessMacroUpload(u32 data) {
    ASSERT_MSG(regs.macros.upload_address < macro_memory.size(),
               "upload_address exceeded macro_memory size!");
    macro_memory[regs.macros.upload_address++] = data;
}

void Maxwell3D::ProcessMacroBind(u32 data) {
    macro_positions[regs.macros.entry++] = data;
}

void Maxwell3D::ProcessFirmwareCall4() {
    LOG_WARNING(HW_GPU, "(STUBBED) called");

    // Firmware call 4 is a blob that changes some registers depending on its parameters.
    // These registers don't affect emulation and so are stubbed by setting 0xd00 to 1.
    regs.reg_array[0xd00] = 1;
}

void Maxwell3D::StampQueryResult(u64 payload, bool long_query) {
    struct LongQueryResult {
        u64_le value;
        u64_le timestamp;
    };
    static_assert(sizeof(LongQueryResult) == 16, "LongQueryResult has wrong size");
    const GPUVAddr sequence_address{regs.query.QueryAddress()};
    if (long_query) {
        // Write the 128-bit result structure in long mode. Note: We emulate an infinitely fast
        // GPU, this command may actually take a while to complete in real hardware due to GPU
        // wait queues.
        LongQueryResult query_result{payload, system.GPU().GetTicks()};
        memory_manager.WriteBlock(sequence_address, &query_result, sizeof(query_result));
    } else {
        memory_manager.Write<u32>(sequence_address, static_cast<u32>(payload));
    }
}

void Maxwell3D::ProcessQueryGet() {
    // TODO(Subv): Support the other query units.
    ASSERT_MSG(regs.query.query_get.unit == Regs::QueryUnit::Crop,
               "Units other than CROP are unimplemented");

    switch (regs.query.query_get.operation) {
    case Regs::QueryOperation::Release:
        StampQueryResult(regs.query.query_sequence, regs.query.query_get.short_query == 0);
        break;
    case Regs::QueryOperation::Acquire:
        // TODO(Blinkhawk): Under this operation, the GPU waits for the CPU to write a value that
        // matches the current payload.
        UNIMPLEMENTED_MSG("Unimplemented query operation ACQUIRE");
        break;
    case Regs::QueryOperation::Counter:
        if (const std::optional<u64> result = GetQueryResult()) {
            // If the query returns an empty optional it means it's cached and deferred.
            // In this case we have a non-empty result, so we stamp it immediately.
            StampQueryResult(*result, regs.query.query_get.short_query == 0);
        }
        break;
    case Regs::QueryOperation::Trap:
        UNIMPLEMENTED_MSG("Unimplemented query operation TRAP");
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown query operation");
        break;
    }
}

void Maxwell3D::ProcessQueryCondition() {
    const GPUVAddr condition_address{regs.condition.Address()};
    switch (regs.condition.mode) {
    case Regs::ConditionMode::Always: {
        execute_on = true;
        break;
    }
    case Regs::ConditionMode::Never: {
        execute_on = false;
        break;
    }
    case Regs::ConditionMode::ResNonZero: {
        Regs::QueryCompare cmp;
        memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
        execute_on = cmp.initial_sequence != 0U && cmp.initial_mode != 0U;
        break;
    }
    case Regs::ConditionMode::Equal: {
        Regs::QueryCompare cmp;
        memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
        execute_on =
            cmp.initial_sequence == cmp.current_sequence && cmp.initial_mode == cmp.current_mode;
        break;
    }
    case Regs::ConditionMode::NotEqual: {
        Regs::QueryCompare cmp;
        memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
        execute_on =
            cmp.initial_sequence != cmp.current_sequence || cmp.initial_mode != cmp.current_mode;
        break;
    }
    default: {
        UNIMPLEMENTED_MSG("Uninplemented Condition Mode!");
        execute_on = true;
        break;
    }
    }
}

void Maxwell3D::ProcessCounterReset() {
    switch (regs.counter_reset) {
    case Regs::CounterReset::SampleCnt:
        rasterizer.ResetCounter(QueryType::SamplesPassed);
        break;
    default:
        LOG_WARNING(Render_OpenGL, "Unimplemented counter reset={}",
                    static_cast<int>(regs.counter_reset));
        break;
    }
}

void Maxwell3D::ProcessSyncPoint() {
    const u32 sync_point = regs.sync_info.sync_point.Value();
    const u32 increment = regs.sync_info.increment.Value();
    [[maybe_unused]] const u32 cache_flush = regs.sync_info.unknown.Value();
    if (increment) {
        system.GPU().IncrementSyncPoint(sync_point);
    }
}

void Maxwell3D::DrawArrays() {
    LOG_TRACE(HW_GPU, "called, topology={}, count={}", static_cast<u32>(regs.draw.topology.Value()),
              regs.vertex_buffer.count);
    ASSERT_MSG(!(regs.index_array.count && regs.vertex_buffer.count), "Both indexed and direct?");

    // Both instance configuration registers can not be set at the same time.
    ASSERT_MSG(!regs.draw.instance_next || !regs.draw.instance_cont,
               "Illegal combination of instancing parameters");

    if (regs.draw.instance_next) {
        // Increment the current instance *before* drawing.
        state.current_instance += 1;
    } else if (!regs.draw.instance_cont) {
        // Reset the current instance to 0.
        state.current_instance = 0;
    }

    const bool is_indexed{regs.index_array.count && !regs.vertex_buffer.count};
    if (ShouldExecute()) {
        rasterizer.Draw(is_indexed, false);
    }

    // TODO(bunnei): Below, we reset vertex count so that we can use these registers to determine if
    // the game is trying to draw indexed or direct mode. This needs to be verified on HW still -
    // it's possible that it is incorrect and that there is some other register used to specify the
    // drawing mode.
    if (is_indexed) {
        regs.index_array.count = 0;
    } else {
        regs.vertex_buffer.count = 0;
    }
}

std::optional<u64> Maxwell3D::GetQueryResult() {
    switch (regs.query.query_get.select) {
    case Regs::QuerySelect::Zero:
        return 0;
    case Regs::QuerySelect::SamplesPassed:
        // Deferred.
        rasterizer.Query(regs.query.QueryAddress(), VideoCore::QueryType::SamplesPassed,
                         system.GPU().GetTicks());
        return {};
    default:
        UNIMPLEMENTED_MSG("Unimplemented query select type {}",
                          static_cast<u32>(regs.query.query_get.select.Value()));
        return 1;
    }
}

void Maxwell3D::ProcessCBBind(std::size_t stage_index) {
    // Bind the buffer currently in CB_ADDRESS to the specified index in the desired shader stage.
    auto& shader = state.shader_stages[stage_index];
    auto& bind_data = regs.cb_bind[stage_index];

    ASSERT(bind_data.index < Regs::MaxConstBuffers);
    auto& buffer = shader.const_buffers[bind_data.index];

    buffer.enabled = bind_data.valid.Value() != 0;
    buffer.address = regs.const_buffer.BufferAddress();
    buffer.size = regs.const_buffer.cb_size;
}

void Maxwell3D::ProcessCBData(u32 value) {
    const u32 id = cb_data_state.id;
    cb_data_state.buffer[id][cb_data_state.counter] = value;
    // Increment the current buffer position.
    regs.const_buffer.cb_pos = regs.const_buffer.cb_pos + 4;
    cb_data_state.counter++;
}

void Maxwell3D::StartCBData(u32 method) {
    constexpr u32 first_cb_data = MAXWELL3D_REG_INDEX(const_buffer.cb_data[0]);
    cb_data_state.start_pos = regs.const_buffer.cb_pos;
    cb_data_state.id = method - first_cb_data;
    cb_data_state.current = method;
    cb_data_state.counter = 0;
    ProcessCBData(regs.const_buffer.cb_data[cb_data_state.id]);
}

void Maxwell3D::FinishCBData() {
    // Write the input value to the current const buffer at the current position.
    const GPUVAddr buffer_address = regs.const_buffer.BufferAddress();
    ASSERT(buffer_address != 0);

    // Don't allow writing past the end of the buffer.
    ASSERT(regs.const_buffer.cb_pos <= regs.const_buffer.cb_size);

    const GPUVAddr address{buffer_address + cb_data_state.start_pos};
    const std::size_t size = regs.const_buffer.cb_pos - cb_data_state.start_pos;

    const u32 id = cb_data_state.id;
    memory_manager.WriteBlock(address, cb_data_state.buffer[id].data(), size);
    dirty.OnMemoryWrite();

    cb_data_state.id = null_cb_data;
    cb_data_state.current = null_cb_data;
}

Texture::TICEntry Maxwell3D::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tic.TICAddress() + tic_index * sizeof(Texture::TICEntry)};

    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));

    return tic_entry;
}

Texture::TSCEntry Maxwell3D::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tsc.TSCAddress() + tsc_index * sizeof(Texture::TSCEntry)};

    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

Texture::FullTextureInfo Maxwell3D::GetTextureInfo(Texture::TextureHandle tex_handle) const {
    return Texture::FullTextureInfo{GetTICEntry(tex_handle.tic_id), GetTSCEntry(tex_handle.tsc_id)};
}

Texture::FullTextureInfo Maxwell3D::GetStageTexture(ShaderType stage, std::size_t offset) const {
    const auto stage_index = static_cast<std::size_t>(stage);
    const auto& shader = state.shader_stages[stage_index];
    const auto& tex_info_buffer = shader.const_buffers[regs.tex_cb_index];
    ASSERT(tex_info_buffer.enabled && tex_info_buffer.address != 0);

    const GPUVAddr tex_info_address =
        tex_info_buffer.address + offset * sizeof(Texture::TextureHandle);

    ASSERT(tex_info_address < tex_info_buffer.address + tex_info_buffer.size);

    const Texture::TextureHandle tex_handle{memory_manager.Read<u32>(tex_info_address)};

    return GetTextureInfo(tex_handle);
}

u32 Maxwell3D::GetRegisterValue(u32 method) const {
    ASSERT_MSG(method < Regs::NUM_REGS, "Invalid Maxwell3D register");
    return regs.reg_array[method];
}

void Maxwell3D::ProcessClearBuffers() {
    ASSERT(regs.clear_buffers.R == regs.clear_buffers.G &&
           regs.clear_buffers.R == regs.clear_buffers.B &&
           regs.clear_buffers.R == regs.clear_buffers.A);

    rasterizer.Clear();
}

u32 Maxwell3D::AccessConstBuffer32(ShaderType stage, u64 const_buffer, u64 offset) const {
    ASSERT(stage != ShaderType::Compute);
    const auto& shader_stage = state.shader_stages[static_cast<std::size_t>(stage)];
    const auto& buffer = shader_stage.const_buffers[const_buffer];
    u32 result;
    std::memcpy(&result, memory_manager.GetPointer(buffer.address + offset), sizeof(u32));
    return result;
}

SamplerDescriptor Maxwell3D::AccessBoundSampler(ShaderType stage, u64 offset) const {
    return AccessBindlessSampler(stage, regs.tex_cb_index, offset * sizeof(Texture::TextureHandle));
}

SamplerDescriptor Maxwell3D::AccessBindlessSampler(ShaderType stage, u64 const_buffer,
                                                   u64 offset) const {
    ASSERT(stage != ShaderType::Compute);
    const auto& shader = state.shader_stages[static_cast<std::size_t>(stage)];
    const auto& tex_info_buffer = shader.const_buffers[const_buffer];
    const GPUVAddr tex_info_address = tex_info_buffer.address + offset;

    const Texture::TextureHandle tex_handle{memory_manager.Read<u32>(tex_info_address)};
    const Texture::FullTextureInfo tex_info = GetTextureInfo(tex_handle);
    SamplerDescriptor result = SamplerDescriptor::FromTicTexture(tex_info.tic.texture_type.Value());
    result.is_shadow.Assign(tex_info.tsc.depth_compare_enabled.Value());
    return result;
}

VideoCore::GuestDriverProfile& Maxwell3D::AccessGuestDriverProfile() {
    return rasterizer.AccessGuestDriverProfile();
}

const VideoCore::GuestDriverProfile& Maxwell3D::AccessGuestDriverProfile() const {
    return rasterizer.AccessGuestDriverProfile();
}

} // namespace Tegra::Engines
