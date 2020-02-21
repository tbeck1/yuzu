// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/engines/shader_bytecode.h"
#include "video_core/shader/node_helper.h"
#include "video_core/shader/shader_ir.h"

namespace VideoCommon::Shader {

using Tegra::Shader::Instruction;
using Tegra::Shader::OpCode;
using Tegra::Shader::Register;

namespace {
constexpr OperationCode GetFloatSelector(u64 selector) {
    return selector == 0 ? OperationCode::FCastHalf0 : OperationCode::FCastHalf1;
}
} // Anonymous namespace

u32 ShaderIR::DecodeConversion(NodeBlock& bb, u32 pc) {
    const Instruction instr = {program_code[pc]};
    const auto opcode = OpCode::Decode(instr);

    switch (opcode->get().GetId()) {
    case OpCode::Id::I2I_R:
    case OpCode::Id::I2I_C:
    case OpCode::Id::I2I_IMM: {
        UNIMPLEMENTED_IF(instr.conversion.int_src.selector != 0);
        UNIMPLEMENTED_IF(instr.conversion.dst_size != Register::Size::Word);
        UNIMPLEMENTED_IF(instr.alu.saturate_d);

        const bool input_signed = instr.conversion.is_input_signed;
        const bool output_signed = instr.conversion.is_output_signed;

        Node value = [&]() {
            switch (opcode->get().GetId()) {
            case OpCode::Id::I2I_R:
                return GetRegister(instr.gpr20);
            case OpCode::Id::I2I_C:
                return GetConstBuffer(instr.cbuf34.index, instr.cbuf34.GetOffset());
            case OpCode::Id::I2I_IMM:
                return Immediate(instr.alu.GetSignedImm20_20());
            default:
                UNREACHABLE();
                return Immediate(0);
            }
        }();
        value = ConvertIntegerSize(value, instr.conversion.src_size, input_signed);

        value = GetOperandAbsNegInteger(value, instr.conversion.abs_a, instr.conversion.negate_a,
                                        input_signed);
        if (input_signed != output_signed) {
            value = SignedOperation(OperationCode::ICastUnsigned, output_signed, NO_PRECISE, value);
        }

        SetInternalFlagsFromInteger(bb, value, instr.generates_cc);
        SetRegister(bb, instr.gpr0, value);
        break;
    }
    case OpCode::Id::I2F_R:
    case OpCode::Id::I2F_C:
    case OpCode::Id::I2F_IMM: {
        UNIMPLEMENTED_IF(instr.conversion.dst_size == Register::Size::Long);
        UNIMPLEMENTED_IF_MSG(instr.generates_cc,
                             "Condition codes generation in I2F is not implemented");

        Node value = [&] {
            switch (opcode->get().GetId()) {
            case OpCode::Id::I2F_R:
                return GetRegister(instr.gpr20);
            case OpCode::Id::I2F_C:
                return GetConstBuffer(instr.cbuf34.index, instr.cbuf34.GetOffset());
            case OpCode::Id::I2F_IMM:
                return Immediate(instr.alu.GetSignedImm20_20());
            default:
                UNREACHABLE();
                return Immediate(0);
            }
        }();

        const bool input_signed = instr.conversion.is_input_signed;

        if (const u32 offset = static_cast<u32>(instr.conversion.int_src.selector); offset > 0) {
            ASSERT(instr.conversion.src_size == Register::Size::Byte ||
                   instr.conversion.src_size == Register::Size::Short);
            if (instr.conversion.src_size == Register::Size::Short) {
                ASSERT(offset == 0 || offset == 2);
            }
            value = SignedOperation(OperationCode::ILogicalShiftRight, input_signed,
                                    std::move(value), Immediate(offset * 8));
        }

        value = ConvertIntegerSize(value, instr.conversion.src_size, input_signed);
        value = GetOperandAbsNegInteger(value, instr.conversion.abs_a, false, input_signed);
        value = SignedOperation(OperationCode::FCastInteger, input_signed, PRECISE, value);
        value = GetOperandAbsNegFloat(value, false, instr.conversion.negate_a);

        SetInternalFlagsFromFloat(bb, value, instr.generates_cc);

        if (instr.conversion.dst_size == Register::Size::Short) {
            value = Operation(OperationCode::HCastFloat, PRECISE, value);
        }

        SetRegister(bb, instr.gpr0, value);
        break;
    }
    case OpCode::Id::F2F_R:
    case OpCode::Id::F2F_C:
    case OpCode::Id::F2F_IMM: {
        UNIMPLEMENTED_IF(instr.conversion.dst_size == Register::Size::Long);
        UNIMPLEMENTED_IF(instr.conversion.src_size == Register::Size::Long);
        UNIMPLEMENTED_IF_MSG(instr.generates_cc,
                             "Condition codes generation in F2F is not implemented");

        Node value = [&]() {
            switch (opcode->get().GetId()) {
            case OpCode::Id::F2F_R:
                return GetRegister(instr.gpr20);
            case OpCode::Id::F2F_C:
                return GetConstBuffer(instr.cbuf34.index, instr.cbuf34.GetOffset());
            case OpCode::Id::F2F_IMM:
                return GetImmediate19(instr);
            default:
                UNREACHABLE();
                return Immediate(0);
            }
        }();

        if (instr.conversion.src_size == Register::Size::Short) {
            value = Operation(GetFloatSelector(instr.conversion.float_src.selector), NO_PRECISE,
                              std::move(value));
        } else {
            ASSERT(instr.conversion.float_src.selector == 0);
        }

        value = GetOperandAbsNegFloat(value, instr.conversion.abs_a, instr.conversion.negate_a);

        value = [&]() {
            switch (instr.conversion.f2f.GetRoundingMode()) {
            case Tegra::Shader::F2fRoundingOp::None:
                return value;
            case Tegra::Shader::F2fRoundingOp::Round:
                return Operation(OperationCode::FRoundEven, PRECISE, value);
            case Tegra::Shader::F2fRoundingOp::Floor:
                return Operation(OperationCode::FFloor, PRECISE, value);
            case Tegra::Shader::F2fRoundingOp::Ceil:
                return Operation(OperationCode::FCeil, PRECISE, value);
            case Tegra::Shader::F2fRoundingOp::Trunc:
                return Operation(OperationCode::FTrunc, PRECISE, value);
            default:
                UNIMPLEMENTED_MSG("Unimplemented F2F rounding mode {}",
                                  static_cast<u32>(instr.conversion.f2f.rounding.Value()));
                return value;
            }
        }();
        value = GetSaturatedFloat(value, instr.alu.saturate_d);

        SetInternalFlagsFromFloat(bb, value, instr.generates_cc);

        if (instr.conversion.dst_size == Register::Size::Short) {
            value = Operation(OperationCode::HCastFloat, PRECISE, value);
        }

        SetRegister(bb, instr.gpr0, value);
        break;
    }
    case OpCode::Id::F2I_R:
    case OpCode::Id::F2I_C:
    case OpCode::Id::F2I_IMM: {
        UNIMPLEMENTED_IF(instr.conversion.src_size == Register::Size::Long);
        UNIMPLEMENTED_IF_MSG(instr.generates_cc,
                             "Condition codes generation in F2I is not implemented");
        Node value = [&]() {
            switch (opcode->get().GetId()) {
            case OpCode::Id::F2I_R:
                return GetRegister(instr.gpr20);
            case OpCode::Id::F2I_C:
                return GetConstBuffer(instr.cbuf34.index, instr.cbuf34.GetOffset());
            case OpCode::Id::F2I_IMM:
                return GetImmediate19(instr);
            default:
                UNREACHABLE();
                return Immediate(0);
            }
        }();

        if (instr.conversion.src_size == Register::Size::Short) {
            value = Operation(GetFloatSelector(instr.conversion.float_src.selector), NO_PRECISE,
                              std::move(value));
        } else {
            ASSERT(instr.conversion.float_src.selector == 0);
        }

        value = GetOperandAbsNegFloat(value, instr.conversion.abs_a, instr.conversion.negate_a);

        value = [&]() {
            switch (instr.conversion.f2i.rounding) {
            case Tegra::Shader::F2iRoundingOp::RoundEven:
                return Operation(OperationCode::FRoundEven, PRECISE, value);
            case Tegra::Shader::F2iRoundingOp::Floor:
                return Operation(OperationCode::FFloor, PRECISE, value);
            case Tegra::Shader::F2iRoundingOp::Ceil:
                return Operation(OperationCode::FCeil, PRECISE, value);
            case Tegra::Shader::F2iRoundingOp::Trunc:
                return Operation(OperationCode::FTrunc, PRECISE, value);
            default:
                UNIMPLEMENTED_MSG("Unimplemented F2I rounding mode {}",
                                  static_cast<u32>(instr.conversion.f2i.rounding.Value()));
                return Immediate(0);
            }
        }();
        const bool is_signed = instr.conversion.is_output_signed;
        value = SignedOperation(OperationCode::ICastFloat, is_signed, PRECISE, value);
        value = ConvertIntegerSize(value, instr.conversion.dst_size, is_signed);

        SetRegister(bb, instr.gpr0, value);
        break;
    }
    default:
        UNIMPLEMENTED_MSG("Unhandled conversion instruction: {}", opcode->get().GetName());
    }

    return pc;
}

} // namespace VideoCommon::Shader
