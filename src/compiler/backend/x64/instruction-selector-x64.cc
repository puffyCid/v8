// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstdint>
#include <limits>

#include "src/base/iterator.h"
#include "src/base/logging.h"
#include "src/base/optional.h"
#include "src/base/overflowing-math.h"
#include "src/codegen/cpu-features.h"
#include "src/codegen/machine-type.h"
#include "src/compiler/backend/instruction-codes.h"
#include "src/compiler/backend/instruction-selector-adapter.h"
#include "src/compiler/backend/instruction-selector-impl.h"
#include "src/compiler/backend/instruction-selector.h"
#include "src/compiler/backend/instruction.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/opcodes.h"
#include "src/compiler/turboshaft/operations.h"
#include "src/compiler/turboshaft/representations.h"
#include "src/roots/roots-inl.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/simd-shuffle.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {
namespace compiler {

namespace {

bool IsCompressed(Node* const node) {
  if (node == nullptr) return false;
  const IrOpcode::Value opcode = node->opcode();
  if (opcode == IrOpcode::kLoad || opcode == IrOpcode::kProtectedLoad ||
      opcode == IrOpcode::kLoadTrapOnNull ||
      opcode == IrOpcode::kUnalignedLoad ||
      opcode == IrOpcode::kLoadImmutable) {
    LoadRepresentation load_rep = LoadRepresentationOf(node->op());
    return load_rep.IsCompressed();
  } else if (node->opcode() == IrOpcode::kPhi) {
    MachineRepresentation phi_rep = PhiRepresentationOf(node->op());
    return phi_rep == MachineRepresentation::kCompressed ||
           phi_rep == MachineRepresentation::kCompressedPointer;
  }
  return false;
}

template <typename Adapter>
bool IsCompressed(InstructionSelectorT<Adapter>* selector,
                  turboshaft::OpIndex node) {
  if (!node.valid()) return false;
  if (selector->is_load(node)) {
    auto load = selector->load_view(node);
    return load.loaded_rep().IsCompressed();
  } else if (selector->IsPhi(node)) {
    MachineRepresentation phi_rep = selector->phi_representation_of(node);
    return phi_rep == MachineRepresentation::kCompressed ||
           phi_rep == MachineRepresentation::kCompressedPointer;
  }
  return false;
}

}  // namespace

template <typename Adapter>
struct BaseWithScaledIndexAndDisplacementMatch {
  using node_t = typename Adapter::node_t;

  node_t base;
  node_t index;
  int scale;
  int64_t displacement;
  DisplacementMode displacement_mode;
};

base::Optional<BaseWithScaledIndexAndDisplacementMatch<TurbofanAdapter>>
TryMatchBaseWithScaledIndexAndDisplacement64(
    InstructionSelectorT<TurbofanAdapter>* selector, Node* node) {
  BaseWithScaledIndexAndDisplacementMatch<TurbofanAdapter> result;
  BaseWithIndexAndDisplacement64Matcher m(node);
  if (m.matches()) {
    result.base = m.base();
    result.index = m.index();
    result.scale = m.scale();
    if (m.displacement() == nullptr) {
      result.displacement = 0;
    } else {
      if (m.displacement()->opcode() == IrOpcode::kInt64Constant) {
        result.displacement = OpParameter<int64_t>(m.displacement()->op());
      } else {
        DCHECK_EQ(m.displacement()->opcode(), IrOpcode::kInt32Constant);
        result.displacement = OpParameter<int32_t>(m.displacement()->op());
      }
    }
    result.displacement_mode = m.displacement_mode();
    return result;
  }
  return base::nullopt;
}

base::Optional<BaseWithScaledIndexAndDisplacementMatch<TurboshaftAdapter>>
TryMatchBaseWithScaledIndexAndDisplacement64(
    InstructionSelectorT<TurboshaftAdapter>* selector,
    turboshaft::OpIndex node) {
  BaseWithScaledIndexAndDisplacementMatch<TurboshaftAdapter> result;

  base::Optional<turboshaft::OpIndex> base;
  base::Optional<turboshaft::OpIndex> index;
  int displacement_mode;
  if (TurboshaftAdapter::Pattern::BaseWithScaledIndexAndDisplacement(
          &base, &index, &result.scale, &result.displacement,
          &displacement_mode)
          .MatchesWith(selector->turboshaft_graph(), node)) {
    result.base = base ? *base : turboshaft::OpIndex{};
    result.index = index ? *index : turboshaft::OpIndex{};
    result.displacement_mode = static_cast<DisplacementMode>(displacement_mode);
    return result;
  }
  return base::nullopt;
}

// Adds X64-specific methods for generating operands.
template <typename Adapter>
class X64OperandGeneratorT final : public OperandGeneratorT<Adapter> {
 public:
  OPERAND_GENERATOR_T_BOILERPLATE(Adapter)

  explicit X64OperandGeneratorT(InstructionSelectorT<Adapter>* selector)
      : super(selector) {}

  template <typename T>
  bool CanBeImmediate(T*) {
    UNREACHABLE(/*REMOVE*/);
  }

  bool CanBeImmediate(node_t node) {
    if (!this->is_constant(node)) return false;
    auto constant = this->constant_view(node);
    if (constant.is_compressed_heap_object()) {
      if (!COMPRESS_POINTERS_BOOL) return false;
      // For builtin code we need static roots
      if (selector()->isolate()->bootstrapper() && !V8_STATIC_ROOTS_BOOL) {
        return false;
      }
      const RootsTable& roots_table = selector()->isolate()->roots_table();
      RootIndex root_index;
      Handle<HeapObject> value = constant.heap_object_value();
      if (roots_table.IsRootHandle(value, &root_index)) {
        return RootsTable::IsReadOnly(root_index);
      }
      return false;
    }
    if (constant.is_int32() || constant.is_relocatable_int32()) {
      const int32_t value = constant.int32_value();
      // int32_t min will overflow if displacement mode is
      // kNegativeDisplacement.
      return value != std::numeric_limits<int32_t>::min();
    }
    if (constant.is_int64()) {
      const int64_t value = constant.int64_value();
      return std::numeric_limits<int32_t>::min() < value &&
             value <= std::numeric_limits<int32_t>::max();
    }
    if (constant.is_number()) {
      const double value = constant.number_value();
      return base::bit_cast<int64_t>(value) == 0;
    }
    return false;
  }

  DECLARE_UNREACHABLE_TURBOSHAFT_FALLBACK(int32_t, GetImmediateIntegerValue)
  int32_t GetImmediateIntegerValue(node_t node) {
    DCHECK(CanBeImmediate(node));
    auto constant = this->constant_view(node);
    if (constant.is_int32()) return constant.int32_value();
    DCHECK(constant.is_int64());
    return static_cast<int32_t>(constant.int64_value());
  }

  DECLARE_UNREACHABLE_TURBOSHAFT_FALLBACK(bool, CanBeMemoryOperand)
  bool CanBeMemoryOperand(InstructionCode opcode, node_t node, node_t input,
                          int effect_level) {
    if (!this->IsLoadOrLoadImmutable(input)) return false;
    if (!selector()->CanCover(node, input)) return false;
    if (effect_level != selector()->GetEffectLevel(input)) {
      return false;
    }
    MachineRepresentation rep =
        this->load_view(input).loaded_rep().representation();
    switch (opcode) {
      case kX64And:
      case kX64Or:
      case kX64Xor:
      case kX64Add:
      case kX64Sub:
      case kX64Push:
      case kX64Cmp:
      case kX64Test:
        // When pointer compression is enabled 64-bit memory operands can't be
        // used for tagged values.
        return rep == MachineRepresentation::kWord64 ||
               (!COMPRESS_POINTERS_BOOL && IsAnyTagged(rep));
      case kX64And32:
      case kX64Or32:
      case kX64Xor32:
      case kX64Add32:
      case kX64Sub32:
      case kX64Cmp32:
      case kX64Test32:
        // When pointer compression is enabled 32-bit memory operands can be
        // used for tagged values.
        return rep == MachineRepresentation::kWord32 ||
               (COMPRESS_POINTERS_BOOL &&
                (IsAnyTagged(rep) || IsAnyCompressed(rep)));
      case kAVXFloat64Add:
      case kAVXFloat64Sub:
      case kAVXFloat64Mul:
        DCHECK_EQ(MachineRepresentation::kFloat64, rep);
        return true;
      case kAVXFloat32Add:
      case kAVXFloat32Sub:
      case kAVXFloat32Mul:
        DCHECK_EQ(MachineRepresentation::kFloat32, rep);
        return true;
      case kX64Cmp16:
      case kX64Test16:
        return rep == MachineRepresentation::kWord16;
      case kX64Cmp8:
      case kX64Test8:
        return rep == MachineRepresentation::kWord8;
      default:
        break;
    }
    return false;
  }

  bool ValueFitsIntoImmediate(int64_t value) const {
    return std::numeric_limits<int32_t>::min() < value &&
           value <= std::numeric_limits<int32_t>::max();
  }

  bool IsZeroIntConstant(node_t node) const {
    if constexpr (Adapter::IsTurboshaft) {
      if (turboshaft::ConstantOp* op =
              this->turboshaft_graph()
                  ->Get(node)
                  .template TryCast<turboshaft::ConstantOp>()) {
        switch (op->kind) {
          case turboshaft::ConstantOp::Kind::kWord32:
            return op->word32() == 0;
          case turboshaft::ConstantOp::Kind::kWord64:
            return op->word64() == 0;
          default:
            break;
        }
      }
      return false;
    } else {
      if (node->opcode() == IrOpcode::kInt32Constant) {
        return OpParameter<int32_t>(node->op()) == 0;
      } else if (node->opcode() == IrOpcode::kInt64Constant) {
        return OpParameter<int64_t>(node->op()) == 0;
      }
      return false;
    }
  }

  AddressingMode GenerateMemoryOperandInputs(
      node_t index, int scale_exponent, node_t base, int64_t displacement,
      DisplacementMode displacement_mode, InstructionOperand inputs[],
      size_t* input_count,
      RegisterUseKind reg_kind = RegisterUseKind::kUseRegister) {
    AddressingMode mode = kMode_MRI;
    node_t base_before_folding = base;
    bool fold_base_into_displacement = false;
    int64_t fold_value = 0;
    if (this->valid(base) && (this->valid(index) || displacement != 0)) {
      if (CanBeImmediate(base) && this->valid(index) &&
          ValueFitsIntoImmediate(displacement)) {
        fold_value = GetImmediateIntegerValue(base);
        if (displacement_mode == kNegativeDisplacement) {
          fold_value -= displacement;
        } else {
          fold_value += displacement;
        }
        if (V8_UNLIKELY(fold_value == 0)) {
          base = node_t{};
          displacement = 0;
        } else if (ValueFitsIntoImmediate(fold_value)) {
          base = node_t{};
          fold_base_into_displacement = true;
        }
      } else if (IsZeroIntConstant(base)) {
        base = node_t{};
      }
    }
    if (this->valid(base)) {
      inputs[(*input_count)++] = UseRegister(base, reg_kind);
      if (this->valid(index)) {
        DCHECK(scale_exponent >= 0 && scale_exponent <= 3);
        inputs[(*input_count)++] = UseRegister(index, reg_kind);
        if (displacement != 0) {
          inputs[(*input_count)++] = UseImmediate64(
              displacement_mode == kNegativeDisplacement ? -displacement
                                                         : displacement);
          static const AddressingMode kMRnI_modes[] = {kMode_MR1I, kMode_MR2I,
                                                       kMode_MR4I, kMode_MR8I};
          mode = kMRnI_modes[scale_exponent];
        } else {
          static const AddressingMode kMRn_modes[] = {kMode_MR1, kMode_MR2,
                                                      kMode_MR4, kMode_MR8};
          mode = kMRn_modes[scale_exponent];
        }
      } else {
        if (displacement == 0) {
          mode = kMode_MR;
        } else {
          inputs[(*input_count)++] = UseImmediate64(
              displacement_mode == kNegativeDisplacement ? -displacement
                                                         : displacement);
          mode = kMode_MRI;
        }
      }
    } else {
      DCHECK(scale_exponent >= 0 && scale_exponent <= 3);
      if (fold_base_into_displacement) {
        DCHECK(!this->valid(base));
        DCHECK(this->valid(index));
        DCHECK_NE(displacement, 0);
        inputs[(*input_count)++] = UseRegister(index, reg_kind);
        inputs[(*input_count)++] = UseImmediate(static_cast<int>(fold_value));
        static const AddressingMode kMnI_modes[] = {kMode_MRI, kMode_M2I,
                                                    kMode_M4I, kMode_M8I};
        mode = kMnI_modes[scale_exponent];
      } else if (displacement != 0) {
        if (!this->valid(index)) {
          // This seems to only occur in (0 + k) cases, but we don't have an
          // addressing mode for a simple constant, so we use the base in a
          // register for kMode_MRI.
          CHECK(IsZeroIntConstant(base_before_folding));
          inputs[(*input_count)++] = UseRegister(base_before_folding, reg_kind);
          inputs[(*input_count)++] = UseImmediate64(
              displacement_mode == kNegativeDisplacement ? -displacement
                                                         : displacement);
          mode = kMode_MRI;
        } else {
          inputs[(*input_count)++] = UseRegister(index, reg_kind);
          inputs[(*input_count)++] = UseImmediate64(
              displacement_mode == kNegativeDisplacement ? -displacement
                                                         : displacement);
          static const AddressingMode kMnI_modes[] = {kMode_MRI, kMode_M2I,
                                                      kMode_M4I, kMode_M8I};
          mode = kMnI_modes[scale_exponent];
        }
      } else {
        inputs[(*input_count)++] = UseRegister(index, reg_kind);
        static const AddressingMode kMn_modes[] = {kMode_MR, kMode_MR1,
                                                   kMode_M4, kMode_M8};
        mode = kMn_modes[scale_exponent];
        if (mode == kMode_MR1) {
          // [%r1 + %r1*1] has a smaller encoding than [%r1*2+0]
          inputs[(*input_count)++] = UseRegister(index, reg_kind);
        }
      }
    }
    return mode;
  }

  AddressingMode GenerateMemoryOperandInputs(
      Node* index, int scale_exponent, Node* base, Node* displacement,
      DisplacementMode displacement_mode, InstructionOperand inputs[],
      size_t* input_count,
      RegisterUseKind reg_kind = RegisterUseKind::kUseRegister) {
    if constexpr (Adapter::IsTurboshaft) {
      // Turboshaft is not using this overload.
      UNREACHABLE();
    } else {
      int64_t displacement_value;
      if (displacement == nullptr) {
        displacement_value = 0;
      } else if (displacement->opcode() == IrOpcode::kInt32Constant) {
        displacement_value = OpParameter<int32_t>(displacement->op());
      } else if (displacement->opcode() == IrOpcode::kInt64Constant) {
        displacement_value = OpParameter<int64_t>(displacement->op());
      } else {
        UNREACHABLE();
      }
      return GenerateMemoryOperandInputs(index, scale_exponent, base,
                                         displacement_value, displacement_mode,
                                         inputs, input_count, reg_kind);
    }
  }

  DECLARE_UNREACHABLE_TURBOSHAFT_FALLBACK(AddressingMode,
                                          GetEffectiveAddressMemoryOperand)

  AddressingMode GetEffectiveAddressMemoryOperand(
      node_t operand, InstructionOperand inputs[], size_t* input_count,
      RegisterUseKind reg_kind = RegisterUseKind::kUseRegister);

  InstructionOperand GetEffectiveIndexOperand(node_t index,
                                              AddressingMode* mode) {
    if (CanBeImmediate(index)) {
      *mode = kMode_MRI;
      return UseImmediate(index);
    } else {
      *mode = kMode_MR1;
      return UseUniqueRegister(index);
    }
  }

  DECLARE_UNREACHABLE_TURBOSHAFT_FALLBACK(bool, CanBeBetterLeftOperand)
  bool CanBeBetterLeftOperand(node_t node) const {
    return !selector()->IsLive(node);
  }
};

template <>
AddressingMode
X64OperandGeneratorT<TurboshaftAdapter>::GetEffectiveAddressMemoryOperand(
    turboshaft::OpIndex operand, InstructionOperand inputs[],
    size_t* input_count, RegisterUseKind reg_kind) {
  using ConstKind = turboshaft::ConstantOp::Kind;

#define __ TurboshaftAdapter::Pattern::

  turboshaft::ConstantOp::Storage storage;
  int32_t offset;

  if (__ Load(__ Constant(ConstKind::kExternal, &storage), {}, {}, {}, {}, {},
              &offset)
          .MatchesWith(selector()->turboshaft_graph(), operand)) {
    ExternalReference reference = storage.external;
    if (selector()->CanAddressRelativeToRootsRegister(reference)) {
      const ptrdiff_t delta =
          offset + MacroAssemblerBase::RootRegisterOffsetForExternalReference(
                       selector()->isolate(), reference);
      if (is_int32(delta)) {
        inputs[(*input_count)++] = TempImmediate(static_cast<int32_t>(delta));
        return kMode_Root;
      }
    }
  }

  auto m = TryMatchBaseWithScaledIndexAndDisplacement64(selector(), operand);
  DCHECK(m.has_value());
  if (IsCompressed(selector(), m->base)) {
    UNIMPLEMENTED();
  }
  if (TurboshaftAdapter::valid(m->base) &&
      this->Get(m->base).Is<turboshaft::LoadRootRegisterOp>()) {
    UNIMPLEMENTED();
  } else if (m->displacement == 0 || ValueFitsIntoImmediate(m->displacement)) {
    return GenerateMemoryOperandInputs(m->index, m->scale, m->base,
                                       m->displacement, m->displacement_mode,
                                       inputs, input_count, reg_kind);
  } else if (!TurboshaftAdapter::valid(m->base) &&
             m->displacement_mode == kPositiveDisplacement) {
    // The displacement cannot be an immediate, but we can use the
    // displacement as base instead and still benefit from addressing
    // modes for the scale.
    UNIMPLEMENTED();
  } else {
    const turboshaft::Operation& op = this->turboshaft_graph()->Get(operand);
    DCHECK_GE(op.input_count, 2);

    inputs[(*input_count)++] = UseRegister(op.input(0), reg_kind);
    inputs[(*input_count)++] = UseRegister(op.input(1), reg_kind);
    return kMode_MR1;
  }

#undef __
}

template <>
AddressingMode
X64OperandGeneratorT<TurbofanAdapter>::GetEffectiveAddressMemoryOperand(
    node_t operand, InstructionOperand inputs[], size_t* input_count,
    RegisterUseKind reg_kind) {
  {
    LoadMatcher<ExternalReferenceMatcher> m(operand);
    if (m.index().HasResolvedValue() && m.object().HasResolvedValue() &&
        selector()->CanAddressRelativeToRootsRegister(
            m.object().ResolvedValue())) {
      ptrdiff_t const delta =
          m.index().ResolvedValue() +
          MacroAssemblerBase::RootRegisterOffsetForExternalReference(
              selector()->isolate(), m.object().ResolvedValue());
      if (is_int32(delta)) {
        inputs[(*input_count)++] = TempImmediate(static_cast<int32_t>(delta));
        return kMode_Root;
      }
    }
  }
  BaseWithIndexAndDisplacement64Matcher m(operand, AddressOption::kAllowAll);
  DCHECK(m.matches());
  // Decompress pointer by complex addressing mode.
  if (IsCompressed(m.base())) {
    DCHECK(m.index() == nullptr);
    DCHECK(m.displacement() == nullptr || CanBeImmediate(m.displacement()));
    AddressingMode mode = kMode_MCR;
    inputs[(*input_count)++] = UseRegister(m.base(), reg_kind);
    if (m.displacement() != nullptr) {
      inputs[(*input_count)++] = m.displacement_mode() == kNegativeDisplacement
                                     ? UseNegatedImmediate(m.displacement())
                                     : UseImmediate(m.displacement());
      mode = kMode_MCRI;
    }
    return mode;
  }
  if (m.base() != nullptr &&
      m.base()->opcode() == IrOpcode::kLoadRootRegister) {
    DCHECK_EQ(m.index(), nullptr);
    DCHECK_EQ(m.scale(), 0);
    inputs[(*input_count)++] = UseImmediate(m.displacement());
    return kMode_Root;
  } else if (m.displacement() == nullptr || CanBeImmediate(m.displacement())) {
    return GenerateMemoryOperandInputs(m.index(), m.scale(), m.base(),
                                       m.displacement(), m.displacement_mode(),
                                       inputs, input_count, reg_kind);
  } else if (m.base() == nullptr &&
             m.displacement_mode() == kPositiveDisplacement) {
    // The displacement cannot be an immediate, but we can use the
    // displacement as base instead and still benefit from addressing
    // modes for the scale.
    return GenerateMemoryOperandInputs(m.index(), m.scale(), m.displacement(),
                                       nullptr, m.displacement_mode(), inputs,
                                       input_count, reg_kind);
  } else {
    inputs[(*input_count)++] = UseRegister(operand->InputAt(0), reg_kind);
    inputs[(*input_count)++] = UseRegister(operand->InputAt(1), reg_kind);
    return kMode_MR1;
  }
}

namespace {

ArchOpcode GetLoadOpcode(LoadRepresentation load_rep) {
  ArchOpcode opcode;
  switch (load_rep.representation()) {
    case MachineRepresentation::kFloat32:
      opcode = kX64Movss;
      break;
    case MachineRepresentation::kFloat64:
      opcode = kX64Movsd;
      break;
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      opcode = load_rep.IsSigned() ? kX64Movsxbl : kX64Movzxbl;
      break;
    case MachineRepresentation::kWord16:
      opcode = load_rep.IsSigned() ? kX64Movsxwl : kX64Movzxwl;
      break;
    case MachineRepresentation::kWord32:
      opcode = kX64Movl;
      break;
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
#ifdef V8_COMPRESS_POINTERS
      opcode = kX64Movl;
      break;
#else
      UNREACHABLE();
#endif
#ifdef V8_COMPRESS_POINTERS
    case MachineRepresentation::kTaggedSigned:
      opcode = kX64MovqDecompressTaggedSigned;
      break;
    case MachineRepresentation::kTaggedPointer:
    case MachineRepresentation::kTagged:
      opcode = kX64MovqDecompressTagged;
      break;
#else
    case MachineRepresentation::kTaggedSigned:   // Fall through.
    case MachineRepresentation::kTaggedPointer:  // Fall through.
    case MachineRepresentation::kTagged:         // Fall through.
#endif
    case MachineRepresentation::kWord64:
      opcode = kX64Movq;
      break;
    case MachineRepresentation::kSandboxedPointer:
      opcode = kX64MovqDecodeSandboxedPointer;
      break;
    case MachineRepresentation::kSimd128:
      opcode = kX64Movdqu;
      break;
    case MachineRepresentation::kSimd256:  // Fall through.
      opcode = kX64Movdqu256;
      break;
    case MachineRepresentation::kNone:     // Fall through.
    case MachineRepresentation::kMapWord:  // Fall through.
      UNREACHABLE();
  }
  return opcode;
}

ArchOpcode GetStoreOpcode(StoreRepresentation store_rep) {
  switch (store_rep.representation()) {
    case MachineRepresentation::kFloat32:
      return kX64Movss;
    case MachineRepresentation::kFloat64:
      return kX64Movsd;
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      return kX64Movb;
    case MachineRepresentation::kWord16:
      return kX64Movw;
    case MachineRepresentation::kWord32:
      return kX64Movl;
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
#ifdef V8_COMPRESS_POINTERS
      return kX64MovqCompressTagged;
#else
      UNREACHABLE();
#endif
    case MachineRepresentation::kTaggedSigned:   // Fall through.
    case MachineRepresentation::kTaggedPointer:  // Fall through.
    case MachineRepresentation::kTagged:
      return kX64MovqCompressTagged;
    case MachineRepresentation::kWord64:
      return kX64Movq;
    case MachineRepresentation::kSandboxedPointer:
      return kX64MovqEncodeSandboxedPointer;
    case MachineRepresentation::kSimd128:
      return kX64Movdqu;
    case MachineRepresentation::kSimd256:  // Fall through.
      return kX64Movdqu256;
    case MachineRepresentation::kNone:     // Fall through.
    case MachineRepresentation::kMapWord:  // Fall through.
      UNREACHABLE();
  }
  UNREACHABLE();
}

ArchOpcode GetSeqCstStoreOpcode(StoreRepresentation store_rep) {
  switch (store_rep.representation()) {
    case MachineRepresentation::kWord8:
      return kAtomicStoreWord8;
    case MachineRepresentation::kWord16:
      return kAtomicStoreWord16;
    case MachineRepresentation::kWord32:
      return kAtomicStoreWord32;
    case MachineRepresentation::kWord64:
      return kX64Word64AtomicStoreWord64;
    case MachineRepresentation::kTaggedSigned:   // Fall through.
    case MachineRepresentation::kTaggedPointer:  // Fall through.
    case MachineRepresentation::kTagged:
      if (COMPRESS_POINTERS_BOOL) return kAtomicStoreWord32;
      return kX64Word64AtomicStoreWord64;
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
      CHECK(COMPRESS_POINTERS_BOOL);
      return kAtomicStoreWord32;
    default:
      UNREACHABLE();
  }
}

}  // namespace

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTraceInstruction(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  uint32_t markid = OpParameter<uint32_t>(node->op());
  Emit(kX64TraceInstruction, g.Use(node), g.UseImmediate(markid));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStackSlot(Node* node) {
  StackSlotRepresentation rep = StackSlotRepresentationOf(node->op());
  int slot = frame_->AllocateSpillSlot(rep.size(), rep.alignment());
  OperandGenerator g(this);

  Emit(kArchStackSlot, g.DefineAsRegister(node),
       sequence()->AddImmediate(Constant(slot)), 0, nullptr);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitAbortCSADcheck(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kArchAbortCSADcheck, g.NoOutput(), g.UseFixed(node->InputAt(0), rdx));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitLoadLane(Node* node) {
  LoadLaneParameters params = LoadLaneParametersOf(node->op());
  InstructionCode opcode = kArchNop;
  if (params.rep == MachineType::Int8()) {
    opcode = kX64Pinsrb;
  } else if (params.rep == MachineType::Int16()) {
    opcode = kX64Pinsrw;
  } else if (params.rep == MachineType::Int32()) {
    opcode = kX64Pinsrd;
  } else if (params.rep == MachineType::Int64()) {
    opcode = kX64Pinsrq;
  } else {
    UNREACHABLE();
  }

  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand outputs[] = {g.DefineAsRegister(node)};
  // Input 0 is value node, 1 is lane idx, and GetEffectiveAddressMemoryOperand
  // uses up to 3 inputs. This ordering is consistent with other operations that
  // use the same opcode.
  InstructionOperand inputs[5];
  size_t input_count = 0;

  inputs[input_count++] = g.UseRegister(node->InputAt(2));
  inputs[input_count++] = g.UseImmediate(params.laneidx);

  AddressingMode mode =
      g.GetEffectiveAddressMemoryOperand(node, inputs, &input_count);
  opcode |= AddressingModeField::encode(mode);

  DCHECK_GE(5, input_count);

  // x64 supports unaligned loads.
  DCHECK_NE(params.kind, MemoryAccessKind::kUnaligned);
  if (params.kind == MemoryAccessKind::kProtected) {
    opcode |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  Emit(opcode, 1, outputs, input_count, inputs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitLoadTransform(Node* node) {
  LoadTransformParameters params = LoadTransformParametersOf(node->op());
  ArchOpcode opcode;
  switch (params.transformation) {
    case LoadTransformation::kS128Load8Splat:
      opcode = kX64S128Load8Splat;
      break;
    case LoadTransformation::kS128Load16Splat:
      opcode = kX64S128Load16Splat;
      break;
    case LoadTransformation::kS128Load32Splat:
      opcode = kX64S128Load32Splat;
      break;
    case LoadTransformation::kS128Load64Splat:
      opcode = kX64S128Load64Splat;
      break;
    case LoadTransformation::kS128Load8x8S:
      opcode = kX64S128Load8x8S;
      break;
    case LoadTransformation::kS128Load8x8U:
      opcode = kX64S128Load8x8U;
      break;
    case LoadTransformation::kS128Load16x4S:
      opcode = kX64S128Load16x4S;
      break;
    case LoadTransformation::kS128Load16x4U:
      opcode = kX64S128Load16x4U;
      break;
    case LoadTransformation::kS128Load32x2S:
      opcode = kX64S128Load32x2S;
      break;
    case LoadTransformation::kS128Load32x2U:
      opcode = kX64S128Load32x2U;
      break;
    case LoadTransformation::kS128Load32Zero:
      opcode = kX64Movss;
      break;
    case LoadTransformation::kS128Load64Zero:
      opcode = kX64Movsd;
      break;
    // Simd256
    case LoadTransformation::kS256Load32Splat:
      opcode = kX64S256Load32Splat;
      break;
    case LoadTransformation::kS256Load64Splat:
      opcode = kX64S256Load64Splat;
      break;
    default:
      UNREACHABLE();
  }
  // x64 supports unaligned loads
  DCHECK_NE(params.kind, MemoryAccessKind::kUnaligned);
  InstructionCode code = opcode;
  if (params.kind == MemoryAccessKind::kProtected) {
    code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  VisitLoad(node, node, code);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitLoad(node_t node, node_t value,
                                              InstructionCode opcode) {
  X64OperandGeneratorT<Adapter> g(this);
#ifdef V8_IS_TSAN
  // On TSAN builds we require one scratch register. Because of this we also
  // have to modify the inputs to take into account possible aliasing and use
  // UseUniqueRegister which is not required for non-TSAN builds.
  InstructionOperand temps[] = {g.TempRegister()};
  size_t temp_count = arraysize(temps);
  auto reg_kind = OperandGenerator::RegisterUseKind::kUseUniqueRegister;
#else
  InstructionOperand* temps = nullptr;
  size_t temp_count = 0;
  auto reg_kind = OperandGenerator::RegisterUseKind::kUseRegister;
#endif  // V8_IS_TSAN
  InstructionOperand outputs[] = {g.DefineAsRegister(node)};
  InstructionOperand inputs[3];
  size_t input_count = 0;
  AddressingMode mode =
      g.GetEffectiveAddressMemoryOperand(value, inputs, &input_count, reg_kind);
  InstructionCode code = opcode | AddressingModeField::encode(mode);
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement for Turboshaft.
  } else {
    if (node->opcode() == IrOpcode::kProtectedLoad ||
        ((node->opcode() == IrOpcode::kWord32AtomicLoad ||
          node->opcode() == IrOpcode::kWord64AtomicLoad) &&
         (AtomicLoadParametersOf(node->op()).kind() ==
          MemoryAccessKind::kProtected))) {
      code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
    } else if (node->opcode() == IrOpcode::kLoadTrapOnNull) {
      code |= AccessModeField::encode(kMemoryAccessProtectedNullDereference);
    }
  }
  Emit(code, 1, outputs, input_count, inputs, temp_count, temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitLoad(node_t node) {
  LoadRepresentation load_rep = this->load_view(node).loaded_rep();
  DCHECK(!load_rep.IsMapWord());
  VisitLoad(node, node, GetLoadOpcode(load_rep));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitProtectedLoad(Node* node) {
  VisitLoad(node);
}

namespace {

// Shared routine for Word32/Word64 Atomic Exchange
template <typename Adapter>
void VisitAtomicExchange(InstructionSelectorT<Adapter>* selector, Node* node,
                         ArchOpcode opcode, AtomicWidth width,
                         MemoryAccessKind access_kind) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    X64OperandGeneratorT<Adapter> g(selector);
    Node* base = node->InputAt(0);
    Node* index = node->InputAt(1);
    Node* value = node->InputAt(2);
    AddressingMode addressing_mode;
    InstructionOperand inputs[] = {
        g.UseUniqueRegister(value), g.UseUniqueRegister(base),
        g.GetEffectiveIndexOperand(index, &addressing_mode)};
    InstructionOperand outputs[] = {g.DefineSameAsFirst(node)};
    InstructionCode code = opcode |
                           AddressingModeField::encode(addressing_mode) |
                           AtomicWidthField::encode(width);
    if (access_kind == MemoryAccessKind::kProtected) {
      code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
    }
    selector->Emit(code, arraysize(outputs), outputs, arraysize(inputs),
                   inputs);
  }
}

template <typename Adapter>
void VisitStoreCommon(InstructionSelectorT<Adapter>* selector,
                      const typename Adapter::StoreView& store) {
  using node_t = typename Adapter::node_t;
  X64OperandGeneratorT<Adapter> g(selector);
  node_t base = store.base();
  node_t index = store.index();
  node_t value = store.value();
  int32_t displacement = store.displacement();
  uint8_t element_size_log2 = store.element_size_log2();
  base::Optional<AtomicMemoryOrder> atomic_order = store.memory_order();
  MemoryAccessKind acs_kind = store.access_kind();

  StoreRepresentation store_rep = store.stored_rep();
  DCHECK_NE(store_rep.representation(), MachineRepresentation::kMapWord);
  WriteBarrierKind write_barrier_kind = store_rep.write_barrier_kind();
  const bool is_seqcst =
      atomic_order && *atomic_order == AtomicMemoryOrder::kSeqCst;

  if (v8_flags.enable_unconditional_write_barriers &&
      CanBeTaggedOrCompressedPointer(store_rep.representation())) {
    write_barrier_kind = kFullWriteBarrier;
  }

  const auto access_mode =
      acs_kind == MemoryAccessKind::kProtected
          ? (store.is_store_trap_on_null()
                 ? kMemoryAccessProtectedNullDereference
                 : MemoryAccessMode::kMemoryAccessProtectedMemOutOfBounds)
          : MemoryAccessMode::kMemoryAccessDirect;

  if (write_barrier_kind != kNoWriteBarrier &&
      !v8_flags.disable_write_barriers) {
    DCHECK(CanBeTaggedOrCompressedPointer(store_rep.representation()));
    AddressingMode addressing_mode;
    InstructionOperand inputs[4];
    size_t input_count = 0;
    addressing_mode = g.GenerateMemoryOperandInputs(
        index, element_size_log2, base, displacement,
        DisplacementMode::kPositiveDisplacement, inputs, &input_count,
        X64OperandGeneratorT<Adapter>::RegisterUseKind::kUseUniqueRegister);
    DCHECK_LT(input_count, 4);
    inputs[input_count++] = g.UseUniqueRegister(value);
    RecordWriteMode record_write_mode =
        WriteBarrierKindToRecordWriteMode(write_barrier_kind);
    InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
    InstructionCode code = is_seqcst ? kArchAtomicStoreWithWriteBarrier
                                     : kArchStoreWithWriteBarrier;
    code |= AddressingModeField::encode(addressing_mode);
    code |= RecordWriteModeField::encode(record_write_mode);
    code |= AccessModeField::encode(access_mode);
    selector->Emit(code, 0, nullptr, input_count, inputs, arraysize(temps),
                   temps);
  } else {
#ifdef V8_IS_TSAN
    // On TSAN builds we require two scratch registers. Because of this we also
    // have to modify the inputs to take into account possible aliasing and use
    // UseUniqueRegister which is not required for non-TSAN builds.
    InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
    size_t temp_count = arraysize(temps);
    auto reg_kind =
        OperandGeneratorT<Adapter>::RegisterUseKind::kUseUniqueRegister;
#else
    InstructionOperand* temps = nullptr;
    size_t temp_count = 0;
    auto reg_kind = OperandGeneratorT<Adapter>::RegisterUseKind::kUseRegister;
#endif  // V8_IS_TSAN

    // Release and non-atomic stores emit MOV and sequentially consistent stores
    // emit XCHG.
    // https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html

    ArchOpcode opcode;
    AddressingMode addressing_mode;
    InstructionOperand inputs[4];
    size_t input_count = 0;

    if (is_seqcst) {
      if (displacement != 0 && element_size_log2 != 1) {
        UNIMPLEMENTED();
      }
      // SeqCst stores emit XCHG instead of MOV, so encode the inputs as we
      // would for XCHG. XCHG can't encode the value as an immediate and has
      // fewer addressing modes available.
      inputs[input_count++] = g.UseUniqueRegister(value);
      inputs[input_count++] = g.UseUniqueRegister(base);
      inputs[input_count++] =
          g.GetEffectiveIndexOperand(index, &addressing_mode);
      opcode = GetSeqCstStoreOpcode(store_rep);
    } else {
      if constexpr (Adapter::IsTurbofan) {
        // TODO(nicohartmann@): Turboshaft.
        if ((ElementSizeLog2Of(store_rep.representation()) <
             kSystemPointerSizeLog2) &&
            value->opcode() == IrOpcode::kTruncateInt64ToInt32) {
          value = value->InputAt(0);
        }
      }

      addressing_mode = g.GetEffectiveAddressMemoryOperand(
          store, inputs, &input_count, reg_kind);
      InstructionOperand value_operand = g.CanBeImmediate(value)
                                             ? g.UseImmediate(value)
                                             : g.UseRegister(value, reg_kind);
      inputs[input_count++] = value_operand;
      opcode = GetStoreOpcode(store_rep);
    }

    InstructionCode code = opcode
      | AddressingModeField::encode(addressing_mode)
      | AccessModeField::encode(access_mode);
    selector->Emit(code, 0, static_cast<InstructionOperand*>(nullptr),
                   input_count, inputs, temp_count, temps);
  }
}

}  // namespace

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStorePair(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStore(node_t node) {
  return VisitStoreCommon(this, this->store_view(node));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitProtectedStore(node_t node) {
  return VisitStoreCommon(this, this->store_view(node));
}

// Architecture supports unaligned access, therefore VisitLoad is used instead
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUnalignedLoad(Node* node) {
  UNREACHABLE();
}

// Architecture supports unaligned access, therefore VisitStore is used instead
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUnalignedStore(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStoreLane(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);

  StoreLaneParameters params = StoreLaneParametersOf(node->op());
  InstructionCode opcode = kArchNop;
  if (params.rep == MachineRepresentation::kWord8) {
    opcode = kX64Pextrb;
  } else if (params.rep == MachineRepresentation::kWord16) {
    opcode = kX64Pextrw;
  } else if (params.rep == MachineRepresentation::kWord32) {
    opcode = kX64S128Store32Lane;
  } else if (params.rep == MachineRepresentation::kWord64) {
    opcode = kX64S128Store64Lane;
  } else {
    UNREACHABLE();
  }

  InstructionOperand inputs[4];
  size_t input_count = 0;
  AddressingMode addressing_mode =
      g.GetEffectiveAddressMemoryOperand(node, inputs, &input_count);
  opcode |= AddressingModeField::encode(addressing_mode);

  if (params.kind == MemoryAccessKind::kProtected) {
    opcode |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }

  InstructionOperand value_operand = g.UseRegister(node->InputAt(2));
  inputs[input_count++] = value_operand;
  inputs[input_count++] = g.UseImmediate(params.laneidx);
  DCHECK_GE(4, input_count);
  Emit(opcode, 0, nullptr, input_count, inputs);
}

// Shared routine for multiple binary operations.
template <typename Adapter>
static void VisitBinop(InstructionSelectorT<Adapter>* selector,
                       typename Adapter::node_t node, InstructionCode opcode,
                       FlagsContinuationT<Adapter>* cont) {
  X64OperandGeneratorT<Adapter> g(selector);
  DCHECK_EQ(selector->value_input_count(node), 2);
  auto left = selector->input_at(node, 0);
  auto right = selector->input_at(node, 1);
  if (selector->IsCommutative(node)) {
    if (selector->is_constant(left) && !selector->is_constant(right)) {
      std::swap(left, right);
    }
  }
  InstructionOperand inputs[8];
  size_t input_count = 0;
  InstructionOperand outputs[1];
  size_t output_count = 0;

  // TODO(turbofan): match complex addressing modes.
  if (left == right) {
    // If both inputs refer to the same operand, enforce allocating a register
    // for both of them to ensure that we don't end up generating code like
    // this:
    //
    //   mov rax, [rbp-0x10]
    //   add rax, [rbp-0x10]
    //   jo label
    InstructionOperand const input = g.UseRegister(left);
    inputs[input_count++] = input;
    inputs[input_count++] = input;
  } else if (g.CanBeImmediate(right)) {
    inputs[input_count++] = g.UseRegister(left);
    inputs[input_count++] = g.UseImmediate(right);
  } else {
    int effect_level = selector->GetEffectLevel(node, cont);
    if (selector->IsCommutative(node) && g.CanBeBetterLeftOperand(right) &&
        (!g.CanBeBetterLeftOperand(left) ||
         !g.CanBeMemoryOperand(opcode, node, right, effect_level))) {
      std::swap(left, right);
    }
    if (g.CanBeMemoryOperand(opcode, node, right, effect_level)) {
      inputs[input_count++] = g.UseRegister(left);
      AddressingMode addressing_mode =
          g.GetEffectiveAddressMemoryOperand(right, inputs, &input_count);
      opcode |= AddressingModeField::encode(addressing_mode);
    } else {
      inputs[input_count++] = g.UseRegister(left);
      inputs[input_count++] = g.Use(right);
    }
  }

  if (cont->IsBranch()) {
    inputs[input_count++] = g.Label(cont->true_block());
    inputs[input_count++] = g.Label(cont->false_block());
  }

  outputs[output_count++] = g.DefineSameAsFirst(node);

  DCHECK_NE(0u, input_count);
  DCHECK_EQ(1u, output_count);
  DCHECK_GE(arraysize(inputs), input_count);
  DCHECK_GE(arraysize(outputs), output_count);

  selector->EmitWithContinuation(opcode, output_count, outputs, input_count,
                                 inputs, cont);
}

// Shared routine for multiple binary operations.
base::Optional<int32_t> GetWord32Constant(
    InstructionSelectorT<TurbofanAdapter>* selector, Node* node,
    bool allow_implicit_int64_truncation =
        TurbofanAdapter::AllowsImplicitWord64ToWord32Truncation) {
  DCHECK(!allow_implicit_int64_truncation);
  if (node->opcode() != IrOpcode::kInt32Constant) return base::nullopt;
  return OpParameter<int32_t>(node->op());
}

base::Optional<int32_t> GetWord32Constant(
    InstructionSelectorT<TurboshaftAdapter>* selector, turboshaft::OpIndex node,
    bool allow_implicit_int64_truncation =
        TurboshaftAdapter::AllowsImplicitWord64ToWord32Truncation) {
  if (auto* constant = selector->Get(node).TryCast<turboshaft::ConstantOp>()) {
    if (constant->kind == turboshaft::ConstantOp::Kind::kWord32) {
      return constant->word32();
    }
    if (allow_implicit_int64_truncation &&
        constant->kind == turboshaft::ConstantOp::Kind::kWord64) {
      return static_cast<int32_t>(constant->word64());
    }
  }
  return base::nullopt;
}

void VisitBinop(InstructionSelectorT<TurboshaftAdapter>* selector, Node* node,
                InstructionCode code) {
  // TODO(nicohartmann@): Remove once everything is ported.
  UNREACHABLE();
}

template <typename Adapter>
static void VisitBinop(InstructionSelectorT<Adapter>* selector,
                       typename Adapter::node_t node, InstructionCode opcode) {
  FlagsContinuationT<Adapter> cont;
  VisitBinop(selector, node, opcode, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32And(node_t node) {
  X64OperandGeneratorT<Adapter> g(this);
  auto binop = this->word_binop_view(node);
  if (auto c = GetWord32Constant(this, binop.right())) {
    if (*c == 0xFF) {
      Emit(kX64Movzxbl, g.DefineAsRegister(node), g.Use(binop.left()));
      return;
    } else if (*c == 0xFFFF) {
      Emit(kX64Movzxwl, g.DefineAsRegister(node), g.Use(binop.left()));
      return;
    }
  }
  VisitBinop(this, node, kX64And32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64And(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Uint64BinopMatcher m(node);
  if (m.right().Is(0xFF)) {
    Emit(kX64Movzxbq, g.DefineAsRegister(node), g.Use(m.left().node()));
  } else if (m.right().Is(0xFFFF)) {
    Emit(kX64Movzxwq, g.DefineAsRegister(node), g.Use(m.left().node()));
  } else if (m.right().Is(0xFFFFFFFF)) {
    Emit(kX64Movl, g.DefineAsRegister(node), g.Use(m.left().node()));
  } else if (m.right().IsInRange(std::numeric_limits<uint32_t>::min(),
                                 std::numeric_limits<uint32_t>::max())) {
    Emit(kX64And32, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()),
         g.UseImmediate(static_cast<int32_t>(m.right().ResolvedValue())));
  } else {
    VisitBinop(this, node, kX64And);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Or(node_t node) {
  VisitBinop(this, node, kX64Or32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Or(Node* node) {
  VisitBinop(this, node, kX64Or);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Xor(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Uint32BinopMatcher m(node);
  if (m.right().Is(-1)) {
    Emit(kX64Not32, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()));
  } else {
    VisitBinop(this, node, kX64Xor32);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Xor(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Uint64BinopMatcher m(node);
  if (m.right().Is(-1)) {
    Emit(kX64Not, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()));
  } else {
    VisitBinop(this, node, kX64Xor);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitStackPointerGreaterThan(
    node_t node, FlagsContinuation* cont) {
  StackCheckKind kind;
  if constexpr (Adapter::IsTurboshaft) {
    kind = this->turboshaft_graph()
               ->Get(node)
               .template Cast<turboshaft::StackPointerGreaterThanOp>()
               .kind;
  } else {
    kind = StackCheckKindOf(node->op());
  }
  InstructionCode opcode =
      kArchStackPointerGreaterThan | MiscField::encode(static_cast<int>(kind));

  int effect_level = GetEffectLevel(node, cont);

  X64OperandGeneratorT<Adapter> g(this);
  node_t value = this->input_at(node, 0);
  if (g.CanBeMemoryOperand(kX64Cmp, node, value, effect_level)) {
    DCHECK(this->IsLoadOrLoadImmutable(value));

    // GetEffectiveAddressMemoryOperand can create at most 3 inputs.
    static constexpr int kMaxInputCount = 3;

    size_t input_count = 0;
    InstructionOperand inputs[kMaxInputCount];
    AddressingMode addressing_mode =
        g.GetEffectiveAddressMemoryOperand(value, inputs, &input_count);
    opcode |= AddressingModeField::encode(addressing_mode);
    DCHECK_LE(input_count, kMaxInputCount);

    EmitWithContinuation(opcode, 0, nullptr, input_count, inputs, cont);
  } else {
    EmitWithContinuation(opcode, g.UseRegister(value), cont);
  }
}

namespace {

template <typename Adapter>
void TryMergeTruncateInt64ToInt32IntoLoad(
    InstructionSelectorT<Adapter>* selector, Node* node, Node* load) {
  LoadRepresentation load_rep = LoadRepresentationOf(load->op());
  MachineRepresentation rep = load_rep.representation();
  InstructionCode opcode;
  switch (rep) {
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      opcode = load_rep.IsSigned() ? kX64Movsxbl : kX64Movzxbl;
      break;
    case MachineRepresentation::kWord16:
      opcode = load_rep.IsSigned() ? kX64Movsxwl : kX64Movzxwl;
      break;
    case MachineRepresentation::kWord32:
    case MachineRepresentation::kWord64:
    case MachineRepresentation::kTaggedSigned:
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kCompressed:  // Fall through.
      opcode = kX64Movl;
      break;
    default:
      UNREACHABLE();
  }
  X64OperandGeneratorT<Adapter> g(selector);
#ifdef V8_IS_TSAN
  // On TSAN builds we require one scratch register. Because of this we also
  // have to modify the inputs to take into account possible aliasing and use
  // UseUniqueRegister which is not required for non-TSAN builds.
  InstructionOperand temps[] = {g.TempRegister()};
  size_t temp_count = arraysize(temps);
  auto reg_kind =
      OperandGeneratorT<Adapter>::RegisterUseKind::kUseUniqueRegister;
#else
  InstructionOperand* temps = nullptr;
  size_t temp_count = 0;
  auto reg_kind = OperandGeneratorT<Adapter>::RegisterUseKind::kUseRegister;
#endif  // V8_IS_TSAN
  InstructionOperand outputs[] = {g.DefineAsRegister(node)};
  size_t input_count = 0;
  InstructionOperand inputs[3];
  AddressingMode mode =
      g.GetEffectiveAddressMemoryOperand(load, inputs, &input_count, reg_kind);
  opcode |= AddressingModeField::encode(mode);

  selector->Emit(opcode, 1, outputs, input_count, inputs, temp_count, temps);
}

// Shared routine for multiple 32-bit shift operations.
// TODO(bmeurer): Merge this with VisitWord64Shift using template magic?
void VisitWord32Shift(InstructionSelectorT<TurboshaftAdapter>*, Node*,
                      ArchOpcode) {
  UNREACHABLE();
}

template <typename Adapter>
void VisitWord32Shift(InstructionSelectorT<Adapter>* selector,
                      typename Adapter::node_t node, ArchOpcode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  DCHECK_EQ(selector->value_input_count(node), 2);
  auto left = selector->input_at(node, 0);
  auto right = selector->input_at(node, 1);

  if constexpr (Adapter::IsTurboshaft) {
    // Word64 to Word32 truncation is implicit in Turboshaft.
  } else {
    if (left->opcode() == IrOpcode::kTruncateInt64ToInt32) {
      left = left->InputAt(0);
    }
  }

  if (g.CanBeImmediate(right)) {
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseImmediate(right));
  } else {
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseFixed(right, rcx));
  }
}

// Shared routine for multiple 64-bit shift operations.
// TODO(bmeurer): Merge this with VisitWord32Shift using template magic?
void VisitWord64Shift(InstructionSelectorT<TurboshaftAdapter>*, Node*,
                      ArchOpcode) {
  UNREACHABLE();
}

template <typename Adapter>
void VisitWord64Shift(InstructionSelectorT<Adapter>* selector,
                      typename Adapter::node_t node, ArchOpcode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  DCHECK_EQ(selector->value_input_count(node), 2);
  auto left = selector->input_at(node, 0);
  auto right = selector->input_at(node, 1);

  if (g.CanBeImmediate(right)) {
    if constexpr (Adapter::IsTurboshaft) {
      // TODO(nicohartmann@): Implement this for Turboshaft.
    } else {
      Int64BinopMatcher m(node);
      if (opcode == kX64Shr && m.left().IsChangeUint32ToUint64() &&
          m.right().HasResolvedValue() && m.right().ResolvedValue() < 32 &&
          m.right().ResolvedValue() >= 0) {
        opcode = kX64Shr32;
        left = left->InputAt(0);
      }
    }
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseImmediate(right));
  } else {
    if constexpr (Adapter::IsTurboshaft) {
      // TODO(nicohartmann@): Implement this for Turboshaft.
    } else {
      Int64BinopMatcher m(node);
      if (m.right().IsWord64And()) {
        Int64BinopMatcher mright(right);
        if (mright.right().Is(0x3F)) {
          right = mright.left().node();
        }
      }
    }
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseFixed(right, rcx));
  }
}

// Shared routine for multiple shift operations with continuation.
template <typename Adapter, typename BinopMatcher, int Bits>
bool TryVisitWordShift(InstructionSelectorT<Adapter>* selector, Node* node,
                       ArchOpcode opcode, FlagsContinuationT<Adapter>* cont) {
  X64OperandGeneratorT<Adapter> g(selector);
  BinopMatcher m(node);
  Node* left = m.left().node();
  Node* right = m.right().node();

  // If the shift count is 0, the flags are not affected.
  if (!g.CanBeImmediate(right) ||
      (g.GetImmediateIntegerValue(right) & (Bits - 1)) == 0) {
    return false;
  }
  InstructionOperand output = g.DefineSameAsFirst(node);
  InstructionOperand inputs[2];
  inputs[0] = g.UseRegister(left);
  inputs[1] = g.UseImmediate(right);
  selector->EmitWithContinuation(opcode, 1, &output, 2, inputs, cont);
  return true;
}

template <typename Adapter>
void EmitLea(InstructionSelectorT<Adapter>* selector, InstructionCode opcode,
             typename Adapter::node_t result, typename Adapter::node_t index,
             int scale, typename Adapter::node_t base, int64_t displacement,
             DisplacementMode displacement_mode) {
  X64OperandGeneratorT<Adapter> g(selector);

  InstructionOperand inputs[4];
  size_t input_count = 0;
  AddressingMode mode =
      g.GenerateMemoryOperandInputs(index, scale, base, displacement,
                                    displacement_mode, inputs, &input_count);

  DCHECK_NE(0u, input_count);
  DCHECK_GE(arraysize(inputs), input_count);

  InstructionOperand outputs[1];
  outputs[0] = g.DefineAsRegister(result);

  opcode = AddressingModeField::encode(mode) | opcode;

  selector->Emit(opcode, 1, outputs, input_count, inputs);
}

}  // namespace

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitWord32Shl(Node* node) {
  Int32ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, kX64Lea32, node, index, m.scale(), base, 0,
            kPositiveDisplacement);
    return;
  }
  VisitWord32Shift(this, node, kX64Shl32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Shl(node_t node) {
  X64OperandGeneratorT<Adapter> g(this);
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Support the same cases as Turbofan.
  } else {
    Int64ScaleMatcher m(node, true);
    if (m.matches()) {
      Node* index = node->InputAt(0);
      Node* base = m.power_of_two_plus_one() ? index : nullptr;
      EmitLea(this, kX64Lea, node, index, m.scale(), base, 0,
              kPositiveDisplacement);
      return;
    } else {
      Int64BinopMatcher bm(node);
      if ((bm.left().IsChangeInt32ToInt64() ||
           bm.left().IsChangeUint32ToUint64()) &&
          bm.right().IsInRange(32, 63)) {
        // There's no need to sign/zero-extend to 64-bit if we shift out the
        // upper 32 bits anyway.
        Emit(kX64Shl, g.DefineSameAsFirst(node),
             g.UseRegister(bm.left().node()->InputAt(0)),
             g.UseImmediate(bm.right().node()));
        return;
      }
    }
  }
  VisitWord64Shift(this, node, kX64Shl);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Shr(Node* node) {
  VisitWord32Shift(this, node, kX64Shr32);
}

namespace {

inline AddressingMode AddDisplacementToAddressingMode(AddressingMode mode) {
  switch (mode) {
    case kMode_MR:
      return kMode_MRI;
    case kMode_MR1:
      return kMode_MR1I;
    case kMode_MR2:
      return kMode_MR2I;
    case kMode_MR4:
      return kMode_MR4I;
    case kMode_MR8:
      return kMode_MR8I;
    case kMode_M1:
      return kMode_M1I;
    case kMode_M2:
      return kMode_M2I;
    case kMode_M4:
      return kMode_M4I;
    case kMode_M8:
      return kMode_M8I;
    case kMode_None:
    case kMode_MRI:
    case kMode_MR1I:
    case kMode_MR2I:
    case kMode_MR4I:
    case kMode_MR8I:
    case kMode_M1I:
    case kMode_M2I:
    case kMode_M4I:
    case kMode_M8I:
    case kMode_Root:
    case kMode_MCR:
    case kMode_MCRI:
      UNREACHABLE();
  }
  UNREACHABLE();
}

template <typename T>
bool TryMatchLoadWord64AndShiftRight(
    InstructionSelectorT<TurboshaftAdapter>* selector, T node,
    InstructionCode opcode) {
  // TODO(nicohartmann@): Implement for Turboshaft.
  return false;
}

bool TryMatchLoadWord64AndShiftRight(
    InstructionSelectorT<TurbofanAdapter>* selector, Node* node,
    InstructionCode opcode) {
  DCHECK(IrOpcode::kWord64Sar == node->opcode() ||
         IrOpcode::kWord64Shr == node->opcode());
  X64OperandGeneratorT<TurbofanAdapter> g(selector);
  Int64BinopMatcher m(node);
  if (selector->CanCover(m.node(), m.left().node()) && m.left().IsLoad() &&
      m.right().Is(32)) {
    DCHECK_EQ(selector->GetEffectLevel(node),
              selector->GetEffectLevel(m.left().node()));
    // Just load and sign-extend the interesting 4 bytes instead. This happens,
    // for example, when we're loading and untagging SMIs.
    BaseWithIndexAndDisplacement64Matcher mleft(m.left().node(),
                                                AddressOption::kAllowAll);
    if (mleft.matches() && (mleft.displacement() == nullptr ||
                            g.CanBeImmediate(mleft.displacement()))) {
#ifdef V8_IS_TSAN
      // On TSAN builds we require one scratch register. Because of this we also
      // have to modify the inputs to take into account possible aliasing and
      // use UseUniqueRegister which is not required for non-TSAN builds.
      InstructionOperand temps[] = {g.TempRegister()};
      size_t temp_count = arraysize(temps);
      auto reg_kind = OperandGeneratorT<
          TurbofanAdapter>::RegisterUseKind::kUseUniqueRegister;
#else
      InstructionOperand* temps = nullptr;
      size_t temp_count = 0;
      auto reg_kind =
          OperandGeneratorT<TurbofanAdapter>::RegisterUseKind::kUseRegister;
#endif  // V8_IS_TSAN
      size_t input_count = 0;
      InstructionOperand inputs[3];
      AddressingMode mode = g.GetEffectiveAddressMemoryOperand(
          m.left().node(), inputs, &input_count, reg_kind);
      if (mleft.displacement() == nullptr) {
        // Make sure that the addressing mode indicates the presence of an
        // immediate displacement. It seems that we never use M1 and M2, but we
        // handle them here anyways.
        mode = AddDisplacementToAddressingMode(mode);
        inputs[input_count++] =
            ImmediateOperand(ImmediateOperand::INLINE_INT32, 4);
      } else {
        // In the case that the base address was zero, the displacement will be
        // in a register and replacing it with an immediate is not allowed. This
        // usually only happens in dead code anyway.
        if (!inputs[input_count - 1].IsImmediate()) return false;
        int32_t displacement = g.GetImmediateIntegerValue(mleft.displacement());
        inputs[input_count - 1] =
            ImmediateOperand(ImmediateOperand::INLINE_INT32, displacement + 4);
      }
      InstructionOperand outputs[] = {g.DefineAsRegister(node)};
      InstructionCode code = opcode | AddressingModeField::encode(mode);
      selector->Emit(code, 1, outputs, input_count, inputs, temp_count, temps);
      return true;
    }
  }
  return false;
}

}  // namespace

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Shr(Node* node) {
  if (TryMatchLoadWord64AndShiftRight(this, node, kX64Movl)) return;
  VisitWord64Shift(this, node, kX64Shr);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Sar(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Add this optimization for Turboshaft.
  } else {
    X64OperandGeneratorT<Adapter> g(this);
    Int32BinopMatcher m(node);
    if (CanCover(m.node(), m.left().node()) && m.left().IsWord32Shl()) {
      Int32BinopMatcher mleft(m.left().node());
      if (mleft.right().Is(16) && m.right().Is(16)) {
        Emit(kX64Movsxwl, g.DefineAsRegister(node), g.Use(mleft.left().node()));
        return;
      } else if (mleft.right().Is(24) && m.right().Is(24)) {
        Emit(kX64Movsxbl, g.DefineAsRegister(node), g.Use(mleft.left().node()));
        return;
      }
    }
  }
  VisitWord32Shift(this, node, kX64Sar32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Sar(node_t node) {
  if (TryMatchLoadWord64AndShiftRight(this, node, kX64Movsxlq)) return;
  VisitWord64Shift(this, node, kX64Sar);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Rol(Node* node) {
  VisitWord32Shift(this, node, kX64Rol32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Rol(Node* node) {
  VisitWord64Shift(this, node, kX64Rol);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Ror(Node* node) {
  VisitWord32Shift(this, node, kX64Ror32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Ror(Node* node) {
  VisitWord64Shift(this, node, kX64Ror);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32ReverseBits(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64ReverseBits(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64ReverseBytes(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64Bswap, g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32ReverseBytes(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64Bswap32, g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSimd128ReverseBytes(Node* node) {
  UNREACHABLE();
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitInt32Add(Node* node) {
  X64OperandGeneratorT<TurbofanAdapter> g(this);

  // No need to truncate the values before Int32Add.
  DCHECK_EQ(node->InputCount(), 2);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  if (left->opcode() == IrOpcode::kTruncateInt64ToInt32) {
    node->ReplaceInput(0, left->InputAt(0));
  }
  if (right->opcode() == IrOpcode::kTruncateInt64ToInt32) {
    node->ReplaceInput(1, right->InputAt(0));
  }

  // Try to match the Add to a leal pattern
  BaseWithIndexAndDisplacement32Matcher m(node);
  if (m.matches() &&
      (m.displacement() == nullptr || g.CanBeImmediate(m.displacement()))) {
    int64_t disp = 0;
    if (m.displacement() == nullptr) {
    } else if (m.displacement()->opcode() == IrOpcode::kInt32Constant) {
      disp = OpParameter<int32_t>(m.displacement()->op());
    } else {
      DCHECK_EQ(m.displacement()->opcode(), IrOpcode::kInt64Constant);
      disp = OpParameter<int64_t>(m.displacement()->op());
    }
    EmitLea(this, kX64Lea32, node, m.index(), m.scale(), m.base(), disp,
            m.displacement_mode());
    return;
  }

  // No leal pattern match, use addl
  VisitBinop(this, node, kX64Add32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Add(node_t node) {
  X64OperandGeneratorT<Adapter> g(this);
  // Try to match the Add to a leaq pattern
  if (auto match = TryMatchBaseWithScaledIndexAndDisplacement64(this, node)) {
    if (g.ValueFitsIntoImmediate(match->displacement)) {
      EmitLea(this, kX64Lea, node, match->index, match->scale, match->base,
              match->displacement, match->displacement_mode);
      return;
    }
  }

  // No leal pattern match, use addq
  VisitBinop(this, node, kX64Add);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64AddWithOverflow(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kX64Add, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kX64Add, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32Sub(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  DCHECK_EQ(node->InputCount(), 2);
  Node* input1 = node->InputAt(0);
  Node* input2 = node->InputAt(1);
  if (input1->opcode() == IrOpcode::kTruncateInt64ToInt32 &&
      g.CanBeImmediate(input2)) {
    int32_t imm = g.GetImmediateIntegerValue(input2);
    InstructionOperand int64_input = g.UseRegister(input1->InputAt(0));
    if (imm == 0) {
      // Emit "movl" for subtraction of 0.
      Emit(kX64Movl, g.DefineAsRegister(node), int64_input);
    } else {
      // Omit truncation and turn subtractions of constant values into immediate
      // "leal" instructions by negating the value.
      Emit(kX64Lea32 | AddressingModeField::encode(kMode_MRI),
           g.DefineAsRegister(node), int64_input,
           g.TempImmediate(base::NegateWithWraparound(imm)));
    }
    return;
  }

  Int32BinopMatcher m(node);
  if (m.left().Is(0)) {
    Emit(kX64Neg32, g.DefineSameAsFirst(node), g.UseRegister(m.right().node()));
  } else if (m.right().Is(0)) {
    // {EmitIdentity} reuses the virtual register of the first input
    // for the output. This is exactly what we want here.
    EmitIdentity(node);
  } else if (m.right().HasResolvedValue() &&
             g.CanBeImmediate(m.right().node())) {
    // Turn subtractions of constant values into immediate "leal" instructions
    // by negating the value.
    Emit(
        kX64Lea32 | AddressingModeField::encode(kMode_MRI),
        g.DefineAsRegister(node), g.UseRegister(m.left().node()),
        g.TempImmediate(base::NegateWithWraparound(m.right().ResolvedValue())));
  } else {
    VisitBinop(this, node, kX64Sub32);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Sub(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Int64BinopMatcher m(node);
  if (m.left().Is(0)) {
    Emit(kX64Neg, g.DefineSameAsFirst(node), g.UseRegister(m.right().node()));
  } else {
    if (m.right().HasResolvedValue() && g.CanBeImmediate(m.right().node())) {
      // Turn subtractions of constant values into immediate "leaq" instructions
      // by negating the value.
      Emit(kX64Lea | AddressingModeField::encode(kMode_MRI),
           g.DefineAsRegister(node), g.UseRegister(m.left().node()),
           g.TempImmediate(-static_cast<int32_t>(m.right().ResolvedValue())));
      return;
    }
    VisitBinop(this, node, kX64Sub);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64SubWithOverflow(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kX64Sub, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kX64Sub, &cont);
  }
}

namespace {

template <typename Adapter>
void VisitMul(InstructionSelectorT<Adapter>* selector, Node* node,
              ArchOpcode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  Int32BinopMatcher m(node);
  Node* left = m.left().node();
  Node* right = m.right().node();
  if (g.CanBeImmediate(right)) {
    selector->Emit(opcode, g.DefineAsRegister(node), g.Use(left),
                   g.UseImmediate(right));
  } else {
    if (g.CanBeBetterLeftOperand(right)) {
      std::swap(left, right);
    }
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.Use(right));
  }
}

template <typename Adapter>
void VisitMulHigh(InstructionSelectorT<Adapter>* selector, Node* node,
                  ArchOpcode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  if (selector->IsLive(left) && !selector->IsLive(right)) {
    std::swap(left, right);
  }
  InstructionOperand temps[] = {g.TempRegister(rax)};
  // TODO(turbofan): We use UseUniqueRegister here to improve register
  // allocation.
  selector->Emit(opcode, g.DefineAsFixed(node, rdx), g.UseFixed(left, rax),
                 g.UseUniqueRegister(right), arraysize(temps), temps);
}

template <typename Adapter>
void VisitDiv(InstructionSelectorT<Adapter>* selector, Node* node,
              ArchOpcode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  InstructionOperand temps[] = {g.TempRegister(rdx)};
  selector->Emit(
      opcode, g.DefineAsFixed(node, rax), g.UseFixed(node->InputAt(0), rax),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

template <typename Adapter>
void VisitMod(InstructionSelectorT<Adapter>* selector, Node* node,
              ArchOpcode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  InstructionOperand temps[] = {g.TempRegister(rax)};
  selector->Emit(
      opcode, g.DefineAsFixed(node, rdx), g.UseFixed(node->InputAt(0), rax),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

}  // namespace

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitInt32Mul(Node* node) {
  Int32ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, kX64Lea32, node, index, m.scale(), base, 0,
            kPositiveDisplacement);
    return;
  }
  VisitMul(this, node, kX64Imul32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32MulWithOverflow(node_t node) {
  node_t ovf = FindProjection(node, 1);
  if (Adapter::valid(ovf)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX64Imul32, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX64Imul32, &cont);
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitInt64Mul(Node* node) {
  Int64ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, kX64Lea, node, index, m.scale(), base, 0,
            kPositiveDisplacement);
    return;
  }
  VisitMul(this, node, kX64Imul);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64MulWithOverflow(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kX64Imul, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kX64Imul, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32MulHigh(Node* node) {
  VisitMulHigh(this, node, kX64ImulHigh32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64MulHigh(Node* node) {
  VisitMulHigh(this, node, kX64ImulHigh64);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32Div(Node* node) {
  VisitDiv(this, node, kX64Idiv32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Div(Node* node) {
  VisitDiv(this, node, kX64Idiv);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32Div(Node* node) {
  VisitDiv(this, node, kX64Udiv32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64Div(Node* node) {
  VisitDiv(this, node, kX64Udiv);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32Mod(Node* node) {
  VisitMod(this, node, kX64Idiv32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64Mod(Node* node) {
  VisitMod(this, node, kX64Idiv);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32Mod(Node* node) {
  VisitMod(this, node, kX64Udiv32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64Mod(Node* node) {
  VisitMod(this, node, kX64Udiv);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32MulHigh(Node* node) {
  VisitMulHigh(this, node, kX64UmulHigh32);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64MulHigh(Node* node) {
  VisitMulHigh(this, node, kX64UmulHigh64);
}

// TryTruncateFloat32ToInt64 and TryTruncateFloat64ToInt64 operations attempt
// truncation from 32|64-bit float to 64-bit integer by performing roughly the
// following steps:
// 1. Round the original FP value to zero, store in `rounded`;
// 2. Convert the original FP value to integer;
// 3. Convert the integer value back to floating point, store in
// `converted_back`;
// 4. If `rounded` == `converted_back`:
//      Set Projection(1) := 1;   -- the value was in range
//    Else:
//      Set Projection(1) := 0;   -- the value was out of range
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat32ToInt64(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  InstructionOperand temps[1];
  size_t output_count = 0;
  size_t temp_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
    temps[temp_count++] = g.TempSimd128Register();
  }

  Emit(kSSEFloat32ToInt64, output_count, outputs, 1, inputs, temp_count, temps);
}

// TryTruncateFloatNNToUintDD operations attempt truncation from NN-bit
// float to DD-bit integer by using ConvertFloatToUintDD macro instructions.
// It performs a float-to-int instruction, rounding to zero and tests whether
// the result is positive integer (the default, fast case), which means the
// value is in range. Then, we set Projection(1) := 1. Else, we perform
// additional subtraction, conversion and (in case the value was originally
// negative, but still within range) we restore it and set Projection(1) := 1.
// In all other cases we set Projection(1) := 0, denoting value out of range.
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat64ToUint32(
    Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kSSEFloat64ToUint32, output_count, outputs, 1, inputs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat32ToUint64(
    Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kSSEFloat32ToUint64, output_count, outputs, 1, inputs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat64ToUint64(
    Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kSSEFloat64ToUint64, output_count, outputs, 1, inputs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat64ToInt64(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  InstructionOperand temps[1];
  size_t output_count = 0;
  size_t temp_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
    temps[temp_count++] = g.TempSimd128Register();
  }

  Emit(kSSEFloat64ToInt64, output_count, outputs, 1, inputs, temp_count, temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTryTruncateFloat64ToInt32(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  InstructionOperand temps[1];
  size_t output_count = 0;
  size_t temp_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
    temps[temp_count++] = g.TempSimd128Register();
  }

  Emit(kSSEFloat64ToInt32, output_count, outputs, 1, inputs, temp_count, temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitBitcastWord32ToWord64(Node* node) {
  DCHECK(SmiValuesAre31Bits());
  DCHECK(COMPRESS_POINTERS_BOOL);
  EmitIdentity(node);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeInt32ToInt64(node_t node) {
  DCHECK_EQ(this->value_input_count(node), 1);

  X64OperandGeneratorT<Adapter> g(this);
  auto value = this->input_at(node, 0);
  if (this->IsLoadOrLoadImmutable(value) && CanCover(node, value)) {
    LoadRepresentation load_rep = this->load_view(value).loaded_rep();
    MachineRepresentation rep = load_rep.representation();
    InstructionCode opcode;
    switch (rep) {
      case MachineRepresentation::kBit:  // Fall through.
      case MachineRepresentation::kWord8:
        opcode = load_rep.IsSigned() ? kX64Movsxbq : kX64Movzxbq;
        break;
      case MachineRepresentation::kWord16:
        opcode = load_rep.IsSigned() ? kX64Movsxwq : kX64Movzxwq;
        break;
      case MachineRepresentation::kWord32:
      case MachineRepresentation::kWord64:
        // Since BitcastElider may remove nodes of
        // IrOpcode::kTruncateInt64ToInt32 and directly use the inputs, values
        // with kWord64 can also reach this line.
      case MachineRepresentation::kTaggedSigned:
      case MachineRepresentation::kTagged:
        // ChangeInt32ToInt64 must interpret its input as a _signed_ 32-bit
        // integer, so here we must sign-extend the loaded value in any case.
        opcode = kX64Movsxlq;
        break;
      default:
        UNREACHABLE();
    }
    InstructionOperand outputs[] = {g.DefineAsRegister(node)};
    size_t input_count = 0;
    InstructionOperand inputs[3];
    AddressingMode mode = g.GetEffectiveAddressMemoryOperand(
        this->input_at(node, 0), inputs, &input_count);
    opcode |= AddressingModeField::encode(mode);
    Emit(opcode, 1, outputs, input_count, inputs);
  } else {
    Emit(kX64Movsxlq, g.DefineAsRegister(node), g.Use(this->input_at(node, 0)));
  }
}

template <typename Adapter>
bool InstructionSelectorT<Adapter>::ZeroExtendsWord32ToWord64NoPhis(
    Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  DCHECK_NE(node->opcode(), IrOpcode::kPhi);
  switch (node->opcode()) {
    case IrOpcode::kWord32And:
    case IrOpcode::kWord32Or:
    case IrOpcode::kWord32Xor:
    case IrOpcode::kWord32Shl:
    case IrOpcode::kWord32Shr:
    case IrOpcode::kWord32Sar:
    case IrOpcode::kWord32Rol:
    case IrOpcode::kWord32Ror:
    case IrOpcode::kWord32Equal:
    case IrOpcode::kInt32Add:
    case IrOpcode::kInt32Sub:
    case IrOpcode::kInt32Mul:
    case IrOpcode::kInt32MulHigh:
    case IrOpcode::kInt32Div:
    case IrOpcode::kInt32LessThan:
    case IrOpcode::kInt32LessThanOrEqual:
    case IrOpcode::kInt32Mod:
    case IrOpcode::kUint32Div:
    case IrOpcode::kUint32LessThan:
    case IrOpcode::kUint32LessThanOrEqual:
    case IrOpcode::kUint32Mod:
    case IrOpcode::kUint32MulHigh:
    case IrOpcode::kTruncateInt64ToInt32:
      // These 32-bit operations implicitly zero-extend to 64-bit on x64, so the
      // zero-extension is a no-op.
      return true;
    case IrOpcode::kProjection: {
      Node* const value = node->InputAt(0);
      switch (value->opcode()) {
        case IrOpcode::kInt32AddWithOverflow:
        case IrOpcode::kInt32SubWithOverflow:
        case IrOpcode::kInt32MulWithOverflow:
          return true;
        default:
          return false;
      }
    }
    case IrOpcode::kLoad:
    case IrOpcode::kLoadImmutable:
    case IrOpcode::kProtectedLoad:
    case IrOpcode::kLoadTrapOnNull: {
      // The movzxbl/movsxbl/movzxwl/movsxwl/movl operations implicitly
      // zero-extend to 64-bit on x64, so the zero-extension is a no-op.
      LoadRepresentation load_rep = LoadRepresentationOf(node->op());
      switch (load_rep.representation()) {
        case MachineRepresentation::kWord8:
        case MachineRepresentation::kWord16:
        case MachineRepresentation::kWord32:
          return true;
        default:
          return false;
      }
    }
    case IrOpcode::kInt32Constant:
    case IrOpcode::kInt64Constant:
      // Constants are loaded with movl or movq, or xorl for zero; see
      // CodeGenerator::AssembleMove. So any non-negative constant that fits
      // in a 32-bit signed integer is zero-extended to 64 bits.
      if (g.CanBeImmediate(node)) {
        return g.GetImmediateIntegerValue(node) >= 0;
      }
      return false;
    default:
      return false;
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitChangeUint32ToUint64(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Node* value = node->InputAt(0);
  if (ZeroExtendsWord32ToWord64(value)) {
    // These 32-bit operations implicitly zero-extend to 64-bit on x64, so the
    // zero-extension is a no-op.
    return EmitIdentity(node);
  }
  Emit(kX64Movl, g.DefineAsRegister(node), g.Use(value));
}

namespace {

void VisitRO(InstructionSelectorT<TurboshaftAdapter>*, Node*, InstructionCode) {
  UNREACHABLE();
}

template <typename Adapter>
void VisitRO(InstructionSelectorT<Adapter>* selector,
             typename Adapter::node_t node, InstructionCode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  DCHECK_EQ(selector->value_input_count(node), 1);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.Use(selector->input_at(node, 0)));
}

template <typename Adapter>
void VisitRR(InstructionSelectorT<Adapter>* selector, Node* node,
             InstructionCode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void VisitRRO(InstructionSelectorT<Adapter>* selector, Node* node,
              InstructionCode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  selector->Emit(opcode, g.DefineSameAsFirst(node),
                 g.UseRegister(node->InputAt(0)), g.Use(node->InputAt(1)));
}

void VisitFloatBinop(InstructionSelectorT<TurboshaftAdapter>*, Node*,
                     InstructionCode, InstructionCode) {
  UNREACHABLE();
}

template <typename Adapter>
void VisitFloatBinop(InstructionSelectorT<Adapter>* selector,
                     typename Adapter::node_t node, InstructionCode avx_opcode,
                     InstructionCode sse_opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  DCHECK_EQ(selector->value_input_count(node), 2);
  auto left = selector->input_at(node, 0);
  auto right = selector->input_at(node, 1);
  InstructionOperand inputs[8];
  size_t input_count = 0;
  InstructionOperand outputs[1];
  size_t output_count = 0;

  if (left == right) {
    // If both inputs refer to the same operand, enforce allocating a register
    // for both of them to ensure that we don't end up generating code like
    // this:
    //
    //   movss rax, [rbp-0x10]
    //   addss rax, [rbp-0x10]
    //   jo label
    InstructionOperand const input = g.UseRegister(left);
    inputs[input_count++] = input;
    inputs[input_count++] = input;
  } else {
    int effect_level = selector->GetEffectLevel(node);
    if (selector->IsCommutative(node) &&
        (g.CanBeBetterLeftOperand(right) ||
         g.CanBeMemoryOperand(avx_opcode, node, left, effect_level)) &&
        (!g.CanBeBetterLeftOperand(left) ||
         !g.CanBeMemoryOperand(avx_opcode, node, right, effect_level))) {
      std::swap(left, right);
    }
    if (g.CanBeMemoryOperand(avx_opcode, node, right, effect_level)) {
      inputs[input_count++] = g.UseRegister(left);
      AddressingMode addressing_mode =
          g.GetEffectiveAddressMemoryOperand(right, inputs, &input_count);
      avx_opcode |= AddressingModeField::encode(addressing_mode);
      sse_opcode |= AddressingModeField::encode(addressing_mode);
    } else {
      inputs[input_count++] = g.UseRegister(left);
      inputs[input_count++] = g.Use(right);
    }
  }

  DCHECK_NE(0u, input_count);
  DCHECK_GE(arraysize(inputs), input_count);

  if (selector->IsSupported(AVX)) {
    outputs[output_count++] = g.DefineAsRegister(node);
    DCHECK_EQ(1u, output_count);
    DCHECK_GE(arraysize(outputs), output_count);
    selector->Emit(avx_opcode, output_count, outputs, input_count, inputs);
  } else {
    outputs[output_count++] = g.DefineSameAsFirst(node);
    DCHECK_EQ(1u, output_count);
    DCHECK_GE(arraysize(outputs), output_count);
    selector->Emit(sse_opcode, output_count, outputs, input_count, inputs);
  }
}

template <typename Adapter>
void VisitFloatUnop(InstructionSelectorT<Adapter>* selector, Node* node,
                    Node* input, InstructionCode opcode) {
  X64OperandGeneratorT<Adapter> g(selector);
  if (selector->IsSupported(AVX)) {
    selector->Emit(opcode, g.DefineAsRegister(node), g.UseRegister(input));
  } else {
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(input));
  }
}

}  // namespace

#define RO_OP_T_LIST(V)                      \
  V(RoundFloat64ToInt32, kSSEFloat64ToInt32) \
  V(ChangeInt32ToFloat64, kSSEInt32ToFloat64)

#define RO_OP_LIST(V)                                                    \
  V(Word64Clz, kX64Lzcnt)                                                \
  V(Word32Clz, kX64Lzcnt32)                                              \
  V(Word64Ctz, kX64Tzcnt)                                                \
  V(Word32Ctz, kX64Tzcnt32)                                              \
  V(Word64Popcnt, kX64Popcnt)                                            \
  V(Word32Popcnt, kX64Popcnt32)                                          \
  V(Float64Sqrt, kSSEFloat64Sqrt)                                        \
  V(Float32Sqrt, kSSEFloat32Sqrt)                                        \
  V(ChangeFloat64ToInt32, kSSEFloat64ToInt32)                            \
  V(ChangeFloat64ToInt64, kSSEFloat64ToInt64)                            \
  V(ChangeFloat64ToUint32, kSSEFloat64ToUint32 | MiscField::encode(1))   \
  V(TruncateFloat64ToInt64, kSSEFloat64ToInt64)                          \
  V(TruncateFloat64ToUint32, kSSEFloat64ToUint32 | MiscField::encode(0)) \
  V(ChangeFloat64ToUint64, kSSEFloat64ToUint64)                          \
  V(TruncateFloat64ToFloat32, kSSEFloat64ToFloat32)                      \
  V(ChangeFloat32ToFloat64, kSSEFloat32ToFloat64)                        \
  V(TruncateFloat32ToInt32, kSSEFloat32ToInt32)                          \
  V(TruncateFloat32ToUint32, kSSEFloat32ToUint32)                        \
  V(ChangeInt64ToFloat64, kSSEInt64ToFloat64)                            \
  V(ChangeUint32ToFloat64, kSSEUint32ToFloat64)                          \
  V(RoundInt32ToFloat32, kSSEInt32ToFloat32)                             \
  V(RoundInt64ToFloat32, kSSEInt64ToFloat32)                             \
  V(RoundUint64ToFloat32, kSSEUint64ToFloat32)                           \
  V(RoundInt64ToFloat64, kSSEInt64ToFloat64)                             \
  V(RoundUint64ToFloat64, kSSEUint64ToFloat64)                           \
  V(RoundUint32ToFloat32, kSSEUint32ToFloat32)                           \
  V(BitcastFloat32ToInt32, kX64BitcastFI)                                \
  V(BitcastFloat64ToInt64, kX64BitcastDL)                                \
  V(BitcastInt32ToFloat32, kX64BitcastIF)                                \
  V(BitcastInt64ToFloat64, kX64BitcastLD)                                \
  V(Float64ExtractLowWord32, kSSEFloat64ExtractLowWord32)                \
  V(Float64ExtractHighWord32, kSSEFloat64ExtractHighWord32)              \
  V(SignExtendWord8ToInt32, kX64Movsxbl)                                 \
  V(SignExtendWord16ToInt32, kX64Movsxwl)                                \
  V(SignExtendWord8ToInt64, kX64Movsxbq)                                 \
  V(SignExtendWord16ToInt64, kX64Movsxwq)                                \
  V(SignExtendWord32ToInt64, kX64Movsxlq)

#define RR_OP_LIST(V)                                                         \
  V(Float32RoundDown, kSSEFloat32Round | MiscField::encode(kRoundDown))       \
  V(Float64RoundDown, kSSEFloat64Round | MiscField::encode(kRoundDown))       \
  V(Float32RoundUp, kSSEFloat32Round | MiscField::encode(kRoundUp))           \
  V(Float64RoundUp, kSSEFloat64Round | MiscField::encode(kRoundUp))           \
  V(Float32RoundTruncate, kSSEFloat32Round | MiscField::encode(kRoundToZero)) \
  V(Float64RoundTruncate, kSSEFloat64Round | MiscField::encode(kRoundToZero)) \
  V(Float32RoundTiesEven,                                                     \
    kSSEFloat32Round | MiscField::encode(kRoundToNearest))                    \
  V(Float64RoundTiesEven,                                                     \
    kSSEFloat64Round | MiscField::encode(kRoundToNearest))                    \
  V(F32x4Ceil, kX64F32x4Round | MiscField::encode(kRoundUp))                  \
  V(F32x4Floor, kX64F32x4Round | MiscField::encode(kRoundDown))               \
  V(F32x4Trunc, kX64F32x4Round | MiscField::encode(kRoundToZero))             \
  V(F32x4NearestInt, kX64F32x4Round | MiscField::encode(kRoundToNearest))     \
  V(F64x2Ceil, kX64F64x2Round | MiscField::encode(kRoundUp))                  \
  V(F64x2Floor, kX64F64x2Round | MiscField::encode(kRoundDown))               \
  V(F64x2Trunc, kX64F64x2Round | MiscField::encode(kRoundToZero))             \
  V(F64x2NearestInt, kX64F64x2Round | MiscField::encode(kRoundToNearest))

#define RO_VISITOR(Name, opcode)                                \
  template <typename Adapter>                                   \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) { \
    VisitRO(this, node, opcode);                                \
  }
RO_OP_LIST(RO_VISITOR)
#undef RO_VISITOR
#undef RO_OP_LIST

#define RO_VISITOR(Name, opcode)                                 \
  template <typename Adapter>                                    \
  void InstructionSelectorT<Adapter>::Visit##Name(node_t node) { \
    VisitRO(this, node, opcode);                                 \
  }
RO_OP_T_LIST(RO_VISITOR)
#undef RO_VIISTOR
#undef RO_OP_T_LIST

#define RR_VISITOR(Name, opcode)                                \
  template <typename Adapter>                                   \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) { \
    VisitRR(this, node, opcode);                                \
  }
RR_OP_LIST(RR_VISITOR)
#undef RR_VISITOR
#undef RR_OP_LIST

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateFloat64ToWord32(Node* node) {
  VisitRR(this, node, kArchTruncateDoubleToI);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitTruncateInt64ToInt32(Node* node) {
  // We rely on the fact that TruncateInt64ToInt32 zero extends the
  // value (see ZeroExtendsWord32ToWord64). So all code paths here
  // have to satisfy that condition.
  X64OperandGeneratorT<Adapter> g(this);
  Node* value = node->InputAt(0);
  bool can_cover = false;
  if (value->opcode() == IrOpcode::kBitcastTaggedToWordForTagAndSmiBits) {
    can_cover = CanCover(node, value) && CanCover(value, value->InputAt(0));
    value = value->InputAt(0);
  } else {
    can_cover = CanCover(node, value);
  }
  if (can_cover) {
    switch (value->opcode()) {
      case IrOpcode::kWord64Sar:
      case IrOpcode::kWord64Shr: {
        Int64BinopMatcher m(value);
        if (m.right().Is(32)) {
          if (CanCover(value, value->InputAt(0)) &&
              TryMatchLoadWord64AndShiftRight(this, value, kX64Movl)) {
            return EmitIdentity(node);
          }
          Emit(kX64Shr, g.DefineSameAsFirst(node),
               g.UseRegister(m.left().node()), g.TempImmediate(32));
          return;
        }
        break;
      }
      case IrOpcode::kLoad:
      case IrOpcode::kLoadImmutable: {
        TryMergeTruncateInt64ToInt32IntoLoad(this, node, value);
        return;
      }
      default:
        break;
    }
  }
  Emit(kX64Movl, g.DefineAsRegister(node), g.Use(value));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Add(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat32Add, kSSEFloat32Add);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Sub(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat32Sub, kSSEFloat32Sub);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Mul(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat32Mul, kSSEFloat32Mul);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Div(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat32Div, kSSEFloat32Div);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Abs(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Float32Abs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Max(Node* node) {
  VisitRRO(this, node, kSSEFloat32Max);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Min(Node* node) {
  VisitRRO(this, node, kSSEFloat32Min);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Add(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat64Add, kSSEFloat64Add);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Sub(node_t node) {
  VisitFloatBinop(this, node, kAVXFloat64Sub, kSSEFloat64Sub);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Mul(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat64Mul, kSSEFloat64Mul);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Div(node_t node) {
  VisitFloatBinop(this, node, kAVXFloat64Div, kSSEFloat64Div);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Mod(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand temps[] = {g.TempRegister(rax)};
  Emit(kSSEFloat64Mod, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)), 1,
       temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Max(Node* node) {
  VisitRRO(this, node, kSSEFloat64Max);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Min(Node* node) {
  VisitRRO(this, node, kSSEFloat64Min);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Abs(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Float64Abs);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64RoundTiesAway(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Neg(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Float32Neg);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Neg(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Float64Neg);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Ieee754Binop(
    Node* node, InstructionCode opcode) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(opcode, g.DefineAsFixed(node, xmm0), g.UseFixed(node->InputAt(0), xmm0),
       g.UseFixed(node->InputAt(1), xmm1))
      ->MarkAsCall();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Ieee754Unop(
    Node* node, InstructionCode opcode) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(opcode, g.DefineAsFixed(node, xmm0), g.UseFixed(node->InputAt(0), xmm0))
      ->MarkAsCall();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::EmitMoveParamToFPR(node_t node, int index) {
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::EmitMoveFPRToParam(
    InstructionOperand* op, LinkageLocation location) {}

template <typename Adapter>
void InstructionSelectorT<Adapter>::EmitPrepareArguments(
    ZoneVector<PushParameter>* arguments, const CallDescriptor* call_descriptor,
    node_t node) {
  X64OperandGeneratorT<Adapter> g(this);

  // Prepare for C function call.
  if (call_descriptor->IsCFunctionCall()) {
    Emit(kArchPrepareCallCFunction | MiscField::encode(static_cast<int>(
                                         call_descriptor->ParameterCount())),
         0, nullptr, 0, nullptr);

    // Poke any stack arguments.
    for (size_t n = 0; n < arguments->size(); ++n) {
      PushParameter input = (*arguments)[n];
      if (this->valid(input.node)) {
        int slot = static_cast<int>(n);
        InstructionOperand value = g.CanBeImmediate(input.node)
                                       ? g.UseImmediate(input.node)
                                       : g.UseRegister(input.node);
        Emit(kX64Poke | MiscField::encode(slot), g.NoOutput(), value);
      }
    }
  } else {
    // Push any stack arguments.
    int effect_level = GetEffectLevel(node);
    int stack_decrement = 0;
    for (PushParameter input : base::Reversed(*arguments)) {
      stack_decrement += kSystemPointerSize;
      // Skip holes in the param array. These represent both extra slots for
      // multi-slot values and padding slots for alignment.
      if (!this->valid(input.node)) continue;
      InstructionOperand decrement = g.UseImmediate(stack_decrement);
      stack_decrement = 0;
      if (g.CanBeImmediate(input.node)) {
        Emit(kX64Push, g.NoOutput(), decrement, g.UseImmediate(input.node));
      } else if (IsSupported(INTEL_ATOM) ||
                 sequence()->IsFP(GetVirtualRegister(input.node))) {
        // TODO(titzer): X64Push cannot handle stack->stack double moves
        // because there is no way to encode fixed double slots.
        Emit(kX64Push, g.NoOutput(), decrement, g.UseRegister(input.node));
      } else if (g.CanBeMemoryOperand(kX64Push, node, input.node,
                                      effect_level)) {
        InstructionOperand outputs[1];
        InstructionOperand inputs[5];
        size_t input_count = 0;
        inputs[input_count++] = decrement;
        AddressingMode mode = g.GetEffectiveAddressMemoryOperand(
            input.node, inputs, &input_count);
        InstructionCode opcode = kX64Push | AddressingModeField::encode(mode);
        Emit(opcode, 0, outputs, input_count, inputs);
      } else {
        Emit(kX64Push, g.NoOutput(), decrement, g.UseAny(input.node));
      }
    }
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::EmitPrepareResults(
    ZoneVector<PushParameter>* results, const CallDescriptor* call_descriptor,
    node_t node) {
  X64OperandGeneratorT<Adapter> g(this);
  for (PushParameter output : *results) {
    if (!output.location.IsCallerFrameSlot()) continue;
    // Skip any alignment holes in nodes.
    if (this->valid(output.node)) {
      DCHECK(!call_descriptor->IsCFunctionCall());
      if (output.location.GetType() == MachineType::Float32()) {
        MarkAsFloat32(output.node);
      } else if (output.location.GetType() == MachineType::Float64()) {
        MarkAsFloat64(output.node);
      } else if (output.location.GetType() == MachineType::Simd128()) {
        MarkAsSimd128(output.node);
      }
      InstructionOperand result = g.DefineAsRegister(output.node);
      int offset = call_descriptor->GetOffsetToReturns();
      int reverse_slot = -output.location.GetLocation() - offset;
      InstructionOperand slot = g.UseImmediate(reverse_slot);
      Emit(kX64Peek, 1, &result, 1, &slot);
    }
  }
}

template <typename Adapter>
bool InstructionSelectorT<Adapter>::IsTailCallAddressImmediate() {
  return true;
}

namespace {

template <typename Adapter>
void VisitCompareWithMemoryOperand(InstructionSelectorT<Adapter>* selector,
                                   InstructionCode opcode,
                                   typename Adapter::node_t left,
                                   InstructionOperand right,
                                   FlagsContinuationT<Adapter>* cont) {
  DCHECK(selector->IsLoadOrLoadImmutable(left));
  X64OperandGeneratorT<Adapter> g(selector);
  size_t input_count = 0;
  InstructionOperand inputs[6];
  AddressingMode addressing_mode =
      g.GetEffectiveAddressMemoryOperand(left, inputs, &input_count);
  opcode |= AddressingModeField::encode(addressing_mode);
  inputs[input_count++] = right;
  if (cont->IsSelect()) {
    if (opcode == kUnorderedEqual) {
      cont->Negate();
      inputs[input_count++] = g.UseRegister(cont->true_value());
      inputs[input_count++] = g.Use(cont->false_value());
    } else {
      inputs[input_count++] = g.UseRegister(cont->false_value());
      inputs[input_count++] = g.Use(cont->true_value());
    }
  }

  selector->EmitWithContinuation(opcode, 0, nullptr, input_count, inputs, cont);
}

// Shared routine for multiple compare operations.
template <typename Adapter>
void VisitCompare(InstructionSelectorT<Adapter>* selector,
                  InstructionCode opcode, InstructionOperand left,
                  InstructionOperand right, FlagsContinuationT<Adapter>* cont) {
  if (cont->IsSelect()) {
    X64OperandGeneratorT<Adapter> g(selector);
    InstructionOperand inputs[4] = {left, right};
    if (cont->condition() == kUnorderedEqual) {
      cont->Negate();
      inputs[2] = g.UseRegister(cont->true_value());
      inputs[3] = g.Use(cont->false_value());
    } else {
      inputs[2] = g.UseRegister(cont->false_value());
      inputs[3] = g.Use(cont->true_value());
    }
    selector->EmitWithContinuation(opcode, 0, nullptr, 4, inputs, cont);
    return;
  }
  selector->EmitWithContinuation(opcode, left, right, cont);
}

// Shared routine for multiple compare operations.
template <typename Adapter>
void VisitCompare(InstructionSelectorT<Adapter>* selector,
                  InstructionCode opcode, typename Adapter::node_t left,
                  typename Adapter::node_t right,
                  FlagsContinuationT<Adapter>* cont, bool commutative) {
  X64OperandGeneratorT<Adapter> g(selector);
  if (commutative && g.CanBeBetterLeftOperand(right)) {
    std::swap(left, right);
  }
  VisitCompare(selector, opcode, g.UseRegister(left), g.Use(right), cont);
}

template <typename Adapter>
MachineType MachineTypeForNarrow(InstructionSelectorT<Adapter>* selector,
                                 typename Adapter::node_t node,
                                 typename Adapter::node_t hint_node) {
  if (selector->IsLoadOrLoadImmutable(hint_node)) {
    MachineType hint = selector->load_view(hint_node).loaded_rep();
    if (selector->is_integer_constant(node)) {
      int64_t constant = selector->integer_constant(node);
      if (hint == MachineType::Int8()) {
        if (constant >= std::numeric_limits<int8_t>::min() &&
            constant <= std::numeric_limits<int8_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Uint8()) {
        if (constant >= std::numeric_limits<uint8_t>::min() &&
            constant <= std::numeric_limits<uint8_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Int16()) {
        if (constant >= std::numeric_limits<int16_t>::min() &&
            constant <= std::numeric_limits<int16_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Uint16()) {
        if (constant >= std::numeric_limits<uint16_t>::min() &&
            constant <= std::numeric_limits<uint16_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Int32()) {
        if (constant >= std::numeric_limits<int32_t>::min() &&
            constant <= std::numeric_limits<int32_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Uint32()) {
        if (constant >= std::numeric_limits<uint32_t>::min() &&
            constant <= std::numeric_limits<uint32_t>::max())
          return hint;
      }
    }
  }
  if (selector->IsLoadOrLoadImmutable(node)) {
    return selector->load_view(node).loaded_rep();
  }
  return MachineType::None();
}

bool IsIntConstant(InstructionSelectorT<TurbofanAdapter>*, Node* node) {
  return node->opcode() == IrOpcode::kInt32Constant ||
         node->opcode() == IrOpcode::kInt64Constant;
}
bool IsIntConstant(InstructionSelectorT<TurboshaftAdapter>* selector,
                   turboshaft::OpIndex node) {
  if (auto constant = selector->Get(node).TryCast<turboshaft::ConstantOp>()) {
    return constant->kind == turboshaft::ConstantOp::Kind::kWord32 ||
           constant->kind == turboshaft::ConstantOp::Kind::kWord64;
  }
  return false;
}
bool IsWordAnd(InstructionSelectorT<TurbofanAdapter>*, Node* node) {
  return node->opcode() == IrOpcode::kWord32And ||
         node->opcode() == IrOpcode::kWord64And;
}
bool IsWordAnd(InstructionSelectorT<TurboshaftAdapter>* selector,
               turboshaft::OpIndex node) {
  if (auto binop = selector->Get(node).TryCast<turboshaft::WordBinopOp>()) {
    return binop->kind == turboshaft::WordBinopOp::Kind::kBitwiseAnd;
  }
  return false;
}

// The result of WordAnd with a positive interger constant in X64 is known to
// be sign(zero)-extended. Comparing this result with another positive interger
// constant can have narrowed operand.
template <typename Adapter>
MachineType MachineTypeForNarrowWordAnd(
    InstructionSelectorT<Adapter>* selector, typename Adapter::node_t and_node,
    typename Adapter::node_t constant_node) {
  DCHECK_EQ(selector->value_input_count(and_node), 2);
  auto and_left = selector->input_at(and_node, 0);
  auto and_right = selector->input_at(and_node, 1);
  auto and_constant_node = IsIntConstant(selector, and_right) ? and_right
                           : IsIntConstant(selector, and_left)
                               ? and_left
                               : typename Adapter::node_t{};

  if (Adapter::valid(and_constant_node)) {
    int64_t and_constant = selector->integer_constant(and_constant_node);
    int64_t cmp_constant = selector->integer_constant(constant_node);
    if (and_constant >= 0 && cmp_constant >= 0) {
      int64_t constant =
          and_constant > cmp_constant ? and_constant : cmp_constant;
      if (constant <= std::numeric_limits<int8_t>::max()) {
        return MachineType::Int8();
      } else if (constant <= std::numeric_limits<uint8_t>::max()) {
        return MachineType::Uint8();
      } else if (constant <= std::numeric_limits<int16_t>::max()) {
        return MachineType::Int16();
      } else if (constant <= std::numeric_limits<uint16_t>::max()) {
        return MachineType::Uint16();
      } else if (constant <= std::numeric_limits<int32_t>::max()) {
        return MachineType::Int32();
      } else if (constant <= std::numeric_limits<uint32_t>::max()) {
        return MachineType::Uint32();
      }
    }
  }

  return MachineType::None();
}

// Tries to match the size of the given opcode to that of the operands, if
// possible.
template <typename Adapter>
InstructionCode TryNarrowOpcodeSize(InstructionSelectorT<Adapter>* selector,
                                    InstructionCode opcode,
                                    typename Adapter::node_t left,
                                    typename Adapter::node_t right,
                                    FlagsContinuationT<Adapter>* cont) {
  MachineType left_type = MachineType::None();
  MachineType right_type = MachineType::None();
  if (IsWordAnd(selector, left) && IsIntConstant(selector, right)) {
    left_type = MachineTypeForNarrowWordAnd(selector, left, right);
    right_type = left_type;
  } else if (IsWordAnd(selector, right) && IsIntConstant(selector, left)) {
    right_type = MachineTypeForNarrowWordAnd(selector, right, left);
    left_type = right_type;
  } else {
    // TODO(epertoso): we can probably get some size information out phi nodes.
    // If the load representations don't match, both operands will be
    // zero/sign-extended to 32bit.
    left_type = MachineTypeForNarrow(selector, left, right);
    right_type = MachineTypeForNarrow(selector, right, left);
  }
  if (left_type == right_type) {
    switch (left_type.representation()) {
      case MachineRepresentation::kBit:
      case MachineRepresentation::kWord8: {
        if (opcode == kX64Test || opcode == kX64Test32) return kX64Test8;
        if (opcode == kX64Cmp || opcode == kX64Cmp32) {
          if (left_type.semantic() == MachineSemantic::kUint32) {
            cont->OverwriteUnsignedIfSigned();
          } else {
            CHECK_EQ(MachineSemantic::kInt32, left_type.semantic());
          }
          return kX64Cmp8;
        }
        break;
      }
      // Cmp16/Test16 may introduce LCP(Length-Changing-Prefixes) stall, use
      // Cmp32/Test32 instead.
      case MachineRepresentation::kWord16:  // Fall through.
      case MachineRepresentation::kWord32:
        if (opcode == kX64Test) return kX64Test32;
        if (opcode == kX64Cmp) {
          if (left_type.semantic() == MachineSemantic::kUint32) {
            cont->OverwriteUnsignedIfSigned();
          } else {
            CHECK_EQ(MachineSemantic::kInt32, left_type.semantic());
          }
          return kX64Cmp32;
        }
        break;
#ifdef V8_COMPRESS_POINTERS
      case MachineRepresentation::kTaggedSigned:
      case MachineRepresentation::kTaggedPointer:
      case MachineRepresentation::kTagged:
        // When pointer compression is enabled the lower 32-bits uniquely
        // identify tagged value.
        if (opcode == kX64Cmp) return kX64Cmp32;
        break;
#endif
      default:
        break;
    }
  }
  return opcode;
}

/*
Remove unnecessary WordAnd
For example:
33:  IfFalse(31)
517: Int32Constant[65535]
518: Word32And(18, 517)
36:  Int32Constant[266]
37:  Int32LessThanOrEqual(36, 518)
38:  Branch[None]

If Int32LessThanOrEqual select cmp16, the above Word32And can be removed:
33:  IfFalse(31)
36:  Int32Constant[266]
37:  Int32LessThanOrEqual(36, 18)
38:  Branch[None]
*/
template <typename Adapter>
typename Adapter::node_t RemoveUnnecessaryWordAnd(
    InstructionSelectorT<Adapter>* selector, InstructionCode opcode,
    typename Adapter::node_t and_node) {
  int64_t mask = 0;

  if (opcode == kX64Cmp32 || opcode == kX64Test32) {
    mask = std::numeric_limits<uint32_t>::max();
  } else if (opcode == kX64Cmp16 || opcode == kX64Test16) {
    mask = std::numeric_limits<uint16_t>::max();
  } else if (opcode == kX64Cmp8 || opcode == kX64Test8) {
    mask = std::numeric_limits<uint8_t>::max();
  } else {
    return and_node;
  }

  DCHECK_EQ(selector->value_input_count(and_node), 2);
  auto and_left = selector->input_at(and_node, 0);
  auto and_right = selector->input_at(and_node, 1);
  auto and_constant_node = typename Adapter::node_t{};
  auto and_other_node = typename Adapter::node_t{};
  if (IsIntConstant(selector, and_left)) {
    and_constant_node = and_left;
    and_other_node = and_right;
  } else if (IsIntConstant(selector, and_right)) {
    and_constant_node = and_right;
    and_other_node = and_left;
  }

  if (Adapter::valid(and_constant_node)) {
    int64_t and_constant = selector->integer_constant(and_constant_node);
    if (and_constant == mask) return and_other_node;
  }
  return and_node;
}

// Shared routine for multiple word compare operations.
template <typename Adapter>
void VisitWordCompare(InstructionSelectorT<Adapter>* selector,
                      typename Adapter::node_t node, InstructionCode opcode,
                      FlagsContinuationT<Adapter>* cont) {
  X64OperandGeneratorT<Adapter> g(selector);
  DCHECK_EQ(selector->value_input_count(node), 2);
  auto left = selector->input_at(node, 0);
  auto right = selector->input_at(node, 1);

  if constexpr (Adapter::IsTurbofan) {
    // The 32-bit comparisons automatically truncate Word64
    // values to Word32 range, no need to do that explicitly.
    if (opcode == kX64Cmp32 || opcode == kX64Test32) {
      if (left->opcode() == IrOpcode::kTruncateInt64ToInt32) {
        left = left->InputAt(0);
      }

      if (right->opcode() == IrOpcode::kTruncateInt64ToInt32) {
        right = right->InputAt(0);
      }
    }
  }

  opcode = TryNarrowOpcodeSize(selector, opcode, left, right, cont);

  // If one of the two inputs is an immediate, make sure it's on the right, or
  // if one of the two inputs is a memory operand, make sure it's on the left.
  int effect_level = selector->GetEffectLevel(node, cont);

  if ((!g.CanBeImmediate(right) && g.CanBeImmediate(left)) ||
      (g.CanBeMemoryOperand(opcode, node, right, effect_level) &&
       !g.CanBeMemoryOperand(opcode, node, left, effect_level))) {
    if (!selector->IsCommutative(node)) cont->Commute();
    std::swap(left, right);
  }

  if (IsWordAnd(selector, left)) {
    left = RemoveUnnecessaryWordAnd(selector, opcode, left);
  }

  // Match immediates on right side of comparison.
  if (g.CanBeImmediate(right)) {
    if (g.CanBeMemoryOperand(opcode, node, left, effect_level)) {
      return VisitCompareWithMemoryOperand(selector, opcode, left,
                                           g.UseImmediate(right), cont);
    }
    return VisitCompare(selector, opcode, g.Use(left), g.UseImmediate(right),
                        cont);
  }

  // Match memory operands on left side of comparison.
  if (g.CanBeMemoryOperand(opcode, node, left, effect_level)) {
    return VisitCompareWithMemoryOperand(selector, opcode, left,
                                         g.UseRegister(right), cont);
  }

  return VisitCompare(selector, opcode, left, right, cont,
                      selector->IsCommutative(node));
}

template <typename Adapter>
void VisitWord64EqualImpl(InstructionSelectorT<Adapter>* selector, Node* node,
                          FlagsContinuationT<Adapter>* cont) {
  if (selector->CanUseRootsRegister()) {
    X64OperandGeneratorT<Adapter> g(selector);
    const RootsTable& roots_table = selector->isolate()->roots_table();
    RootIndex root_index;
    HeapObjectBinopMatcher m(node);
    if (m.right().HasResolvedValue() &&
        roots_table.IsRootHandle(m.right().ResolvedValue(), &root_index)) {
      InstructionCode opcode =
          kX64Cmp | AddressingModeField::encode(kMode_Root);
      return VisitCompare(
          selector, opcode,
          g.TempImmediate(
              MacroAssemblerBase::RootRegisterOffsetForRootIndex(root_index)),
          g.UseRegister(m.left().node()), cont);
    }
  }
  VisitWordCompare(selector, node, kX64Cmp, cont);
}

template <typename Adapter>
void VisitWord32EqualImpl(InstructionSelectorT<Adapter>* selector,
                          typename Adapter::node_t node,
                          FlagsContinuationT<Adapter>* cont) {
  if constexpr (Adapter::IsTurboshaft) {
    UNIMPLEMENTED();
  } else {
    if (COMPRESS_POINTERS_BOOL && selector->isolate()) {
      X64OperandGeneratorT<Adapter> g(selector);
      const RootsTable& roots_table = selector->isolate()->roots_table();
      RootIndex root_index;
      Node* left = nullptr;
      Handle<HeapObject> right;
      // HeapConstants and CompressedHeapConstants can be treated the same when
      // using them as an input to a 32-bit comparison. Check whether either is
      // present.
      {
        CompressedHeapObjectBinopMatcher m(node);
        if (m.right().HasResolvedValue()) {
          left = m.left().node();
          right = m.right().ResolvedValue();
        } else {
          HeapObjectBinopMatcher m2(node);
          if (m2.right().HasResolvedValue()) {
            left = m2.left().node();
            right = m2.right().ResolvedValue();
          }
        }
      }
      if (!right.is_null() && roots_table.IsRootHandle(right, &root_index)) {
        DCHECK_NE(left, nullptr);
        if (RootsTable::IsReadOnly(root_index) &&
            (V8_STATIC_ROOTS_BOOL || !selector->isolate()->bootstrapper())) {
          return VisitCompare(
              selector, kX64Cmp32, g.UseRegister(left),
              g.TempImmediate(MacroAssemblerBase::ReadOnlyRootPtr(
                  root_index, selector->isolate())),
              cont);
        }
        if (selector->CanUseRootsRegister()) {
          InstructionCode opcode =
              kX64Cmp32 | AddressingModeField::encode(kMode_Root);
          return VisitCompare(
              selector, opcode,
              g.TempImmediate(
                  MacroAssemblerBase::RootRegisterOffsetForRootIndex(
                      root_index)),
              g.UseRegister(left), cont);
        }
      }
    }
    VisitWordCompare(selector, node, kX64Cmp32, cont);
  }
}

void VisitCompareZero(InstructionSelectorT<TurboshaftAdapter>* selector,
                      turboshaft::OpIndex user, turboshaft::OpIndex node,
                      InstructionCode opcode,
                      FlagsContinuationT<TurboshaftAdapter>* cont) {
  X64OperandGeneratorT<TurboshaftAdapter> g(selector);
  const turboshaft::Operation& op = selector->turboshaft_graph()->Get(node);
  if (cont->IsBranch() &&
      (cont->condition() == kNotEqual || cont->condition() == kEqual)) {
    if (const auto binop = op.TryCast<turboshaft::WordBinopOp>()) {
      using Kind = turboshaft::WordBinopOp::Kind;
      if (selector->IsOnlyUserOfNodeInSameBlock(user, node)) {
        const bool is64 =
            binop->rep == turboshaft::WordRepresentation::Word64();
        switch (binop->kind) {
          case Kind::kAdd:
            return VisitBinop(selector, node, is64 ? kX64Add : kX64Add32, cont);
          case Kind::kSub:
            return VisitBinop(selector, node, is64 ? kX64Sub : kX64Sub32, cont);
          case Kind::kBitwiseAnd:
            return VisitBinop(selector, node, is64 ? kX64And : kX64And32, cont);
          case Kind::kBitwiseOr:
            return VisitBinop(selector, node, is64 ? kX64Or : kX64Or32, cont);
          default:
            break;
        }
      }
    } else if (const auto shift = op.TryCast<turboshaft::ShiftOp>()) {
      using Kind = turboshaft::ShiftOp::Kind;
      if (selector->IsOnlyUserOfNodeInSameBlock(user, node)) {
        switch (shift->kind) {
          case Kind::kShiftLeft:
          case Kind::kShiftRightLogical:
            UNIMPLEMENTED();
          default:
            break;
        }
      }
    }
  }

  int effect_level = selector->GetEffectLevel(node, cont);
  if (const auto load = op.TryCast<turboshaft::LoadOp>()) {
    if (load->loaded_rep == turboshaft::MemoryRepresentation::Int8()) {
      if (opcode == kX64Cmp32) {
        opcode = kX64Cmp8;
      } else if (opcode == kX64Test32) {
        opcode = kX64Test8;
      }
    } else if (load->loaded_rep == turboshaft::MemoryRepresentation::Int16()) {
      if (opcode == kX64Cmp32) {
        opcode = kX64Cmp16;
      } else if (opcode == kX64Test32) {
        opcode = kX64Test16;
      }
    }
  }
  if (g.CanBeMemoryOperand(opcode, user, node, effect_level)) {
    VisitCompareWithMemoryOperand(selector, opcode, node, g.TempImmediate(0),
                                  cont);
  } else {
    VisitCompare(selector, opcode, g.Use(node), g.TempImmediate(0), cont);
  }
}

// Shared routine for comparison with zero.
void VisitCompareZero(InstructionSelectorT<TurbofanAdapter>* selector,
                      Node* user, Node* node, InstructionCode opcode,
                      FlagsContinuationT<TurbofanAdapter>* cont) {
  X64OperandGeneratorT<TurbofanAdapter> g(selector);
  if (cont->IsBranch() &&
      (cont->condition() == kNotEqual || cont->condition() == kEqual)) {
    switch (node->opcode()) {
#define FLAGS_SET_BINOP_LIST(V)        \
  V(kInt32Add, VisitBinop, kX64Add32)  \
  V(kInt32Sub, VisitBinop, kX64Sub32)  \
  V(kWord32And, VisitBinop, kX64And32) \
  V(kWord32Or, VisitBinop, kX64Or32)   \
  V(kInt64Add, VisitBinop, kX64Add)    \
  V(kInt64Sub, VisitBinop, kX64Sub)    \
  V(kWord64And, VisitBinop, kX64And)   \
  V(kWord64Or, VisitBinop, kX64Or)
#define FLAGS_SET_BINOP(opcode, Visit, archOpcode)           \
  case IrOpcode::opcode:                                     \
    if (selector->IsOnlyUserOfNodeInSameBlock(user, node)) { \
      return Visit(selector, node, archOpcode, cont);        \
    }                                                        \
    break;
      FLAGS_SET_BINOP_LIST(FLAGS_SET_BINOP)
#undef FLAGS_SET_BINOP_LIST
#undef FLAGS_SET_BINOP

#define TRY_VISIT_WORD32_SHIFT \
  TryVisitWordShift<TurbofanAdapter, Int32BinopMatcher, 32>
#define TRY_VISIT_WORD64_SHIFT \
  TryVisitWordShift<TurbofanAdapter, Int64BinopMatcher, 64>
// Skip Word64Sar/Word32Sar since no instruction reduction in most cases.
#define FLAGS_SET_SHIFT_LIST(V)                    \
  V(kWord32Shl, TRY_VISIT_WORD32_SHIFT, kX64Shl32) \
  V(kWord32Shr, TRY_VISIT_WORD32_SHIFT, kX64Shr32) \
  V(kWord64Shl, TRY_VISIT_WORD64_SHIFT, kX64Shl)   \
  V(kWord64Shr, TRY_VISIT_WORD64_SHIFT, kX64Shr)
#define FLAGS_SET_SHIFT(opcode, TryVisit, archOpcode)         \
  case IrOpcode::opcode:                                      \
    if (selector->IsOnlyUserOfNodeInSameBlock(user, node)) {  \
      if (TryVisit(selector, node, archOpcode, cont)) return; \
    }                                                         \
    break;
      FLAGS_SET_SHIFT_LIST(FLAGS_SET_SHIFT)
#undef TRY_VISIT_WORD32_SHIFT
#undef TRY_VISIT_WORD64_SHIFT
#undef FLAGS_SET_SHIFT_LIST
#undef FLAGS_SET_SHIFT
      default:
        break;
    }
  }
  int effect_level = selector->GetEffectLevel(node, cont);
  if (node->opcode() == IrOpcode::kLoad ||
      node->opcode() == IrOpcode::kLoadImmutable) {
    switch (LoadRepresentationOf(node->op()).representation()) {
      case MachineRepresentation::kWord8:
        if (opcode == kX64Cmp32) {
          opcode = kX64Cmp8;
        } else if (opcode == kX64Test32) {
          opcode = kX64Test8;
        }
        break;
      case MachineRepresentation::kWord16:
        if (opcode == kX64Cmp32) {
          opcode = kX64Cmp16;
        } else if (opcode == kX64Test32) {
          opcode = kX64Test16;
        }
        break;
      default:
        break;
    }
  }
  if (g.CanBeMemoryOperand(opcode, user, node, effect_level)) {
    VisitCompareWithMemoryOperand(selector, opcode, node, g.TempImmediate(0),
                                  cont);
  } else {
    VisitCompare(selector, opcode, g.Use(node), g.TempImmediate(0), cont);
  }
}

// Shared routine for multiple float32 compare operations (inputs commuted).
template <typename Adapter>
void VisitFloat32Compare(InstructionSelectorT<Adapter>* selector,
                         typename Adapter::node_t node,
                         FlagsContinuationT<Adapter>* cont) {
  auto left = selector->input_at(node, 0);
  auto right = selector->input_at(node, 1);
  InstructionCode const opcode =
      selector->IsSupported(AVX) ? kAVXFloat32Cmp : kSSEFloat32Cmp;
  VisitCompare(selector, opcode, right, left, cont, false);
}

// Shared routine for multiple float64 compare operations (inputs commuted).
template <typename Adapter>
void VisitFloat64Compare(InstructionSelectorT<Adapter>* selector,
                         typename Adapter::node_t node,
                         FlagsContinuationT<Adapter>* cont) {
  auto left = selector->input_at(node, 0);
  auto right = selector->input_at(node, 1);
  InstructionCode const opcode =
      selector->IsSupported(AVX) ? kAVXFloat64Cmp : kSSEFloat64Cmp;
  VisitCompare(selector, opcode, right, left, cont, false);
}

// Shared routine for Word32/Word64 Atomic Binops
void VisitAtomicBinop(InstructionSelectorT<TurbofanAdapter>* selector,
                      Node* node, ArchOpcode opcode, AtomicWidth width,
                      MemoryAccessKind access_kind) {
  X64OperandGeneratorT<TurbofanAdapter> g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);
  AddressingMode addressing_mode;
  InstructionOperand inputs[] = {
      g.UseUniqueRegister(value), g.UseUniqueRegister(base),
      g.GetEffectiveIndexOperand(index, &addressing_mode)};
  InstructionOperand outputs[] = {g.DefineAsFixed(node, rax)};
  InstructionOperand temps[] = {g.TempRegister()};
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode) |
                         AtomicWidthField::encode(width);
  if (access_kind == MemoryAccessKind::kProtected) {
    code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  selector->Emit(code, arraysize(outputs), outputs, arraysize(inputs), inputs,
                 arraysize(temps), temps);
}

// Shared routine for Word32/Word64 Atomic CmpExchg
void VisitAtomicCompareExchange(InstructionSelectorT<TurbofanAdapter>* selector,
                                Node* node, ArchOpcode opcode,
                                AtomicWidth width,
                                MemoryAccessKind access_kind) {
  X64OperandGeneratorT<TurbofanAdapter> g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* old_value = node->InputAt(2);
  Node* new_value = node->InputAt(3);
  AddressingMode addressing_mode;
  InstructionOperand inputs[] = {
      g.UseFixed(old_value, rax), g.UseUniqueRegister(new_value),
      g.UseUniqueRegister(base),
      g.GetEffectiveIndexOperand(index, &addressing_mode)};
  InstructionOperand outputs[] = {g.DefineAsFixed(node, rax)};
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode) |
                         AtomicWidthField::encode(width);
  if (access_kind == MemoryAccessKind::kProtected) {
    code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  selector->Emit(code, arraysize(outputs), outputs, arraysize(inputs), inputs);
}

}  // namespace

// Shared routine for word comparison against zero.
template <>
void InstructionSelectorT<TurboshaftAdapter>::VisitWordCompareZero(
    node_t user, node_t value, FlagsContinuation* cont) {
  for (const turboshaft::EqualOp* equal =
           this->turboshaft_graph()->Get(value).TryCast<turboshaft::EqualOp>();
       equal != nullptr && CanCover(user, value);) {
    const turboshaft::ConstantOp* right_constant =
        this->turboshaft_graph()
            ->Get(equal->right())
            .TryCast<turboshaft::ConstantOp>();
    if (right_constant == nullptr) break;
    if (right_constant->kind != turboshaft::ConstantOp::Kind::kWord32) break;
    if (right_constant->word32() != 0) break;

    user = value;
    value = equal->left();
    cont->Negate();
  }

  if (CanCover(user, value)) {
    // TODO(nicohartmann@): Implement for Turboshaft.
  }

  // Branch could not be combined with a compare, emit compare against 0.
  VisitCompareZero(this, user, value, kX64Cmp32, cont);
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitWordCompareZero(
    Node* user, Node* value, FlagsContinuation* cont) {
  // Try to combine with comparisons against 0 by simply inverting the branch.
  while (value->opcode() == IrOpcode::kWord32Equal && CanCover(user, value)) {
    Int32BinopMatcher m(value);
    if (!m.right().Is(0)) break;

    user = value;
    value = m.left().node();
    cont->Negate();
  }

  if (CanCover(user, value)) {
    switch (value->opcode()) {
      case IrOpcode::kWord32Equal:
        cont->OverwriteAndNegateIfEqual(kEqual);
        return VisitWord32EqualImpl(this, value, cont);
      case IrOpcode::kInt32LessThan:
        cont->OverwriteAndNegateIfEqual(kSignedLessThan);
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kInt32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kSignedLessThanOrEqual);
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kUint32LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kUint32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kWord64Equal: {
        cont->OverwriteAndNegateIfEqual(kEqual);
        Int64BinopMatcher m(value);
        if (m.right().Is(0)) {
          // Try to combine the branch with a comparison.
          Node* const eq_user = m.node();
          Node* const eq_value = m.left().node();
          if (CanCover(eq_user, eq_value)) {
            switch (eq_value->opcode()) {
              case IrOpcode::kInt64Sub:
                return VisitWordCompare(this, eq_value, kX64Cmp, cont);
              case IrOpcode::kWord64And:
                return VisitWordCompare(this, eq_value, kX64Test, cont);
              default:
                break;
            }
          }
          return VisitCompareZero(this, eq_user, eq_value, kX64Cmp, cont);
        }
        return VisitWord64EqualImpl(this, value, cont);
      }
      case IrOpcode::kInt64LessThan:
        cont->OverwriteAndNegateIfEqual(kSignedLessThan);
        return VisitWordCompare(this, value, kX64Cmp, cont);
      case IrOpcode::kInt64LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kSignedLessThanOrEqual);
        return VisitWordCompare(this, value, kX64Cmp, cont);
      case IrOpcode::kUint64LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
        return VisitWordCompare(this, value, kX64Cmp, cont);
      case IrOpcode::kUint64LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
        return VisitWordCompare(this, value, kX64Cmp, cont);
      case IrOpcode::kFloat32Equal:
        cont->OverwriteAndNegateIfEqual(kUnorderedEqual);
        return VisitFloat32Compare(this, value, cont);
      case IrOpcode::kFloat32LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThan);
        return VisitFloat32Compare(this, value, cont);
      case IrOpcode::kFloat32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThanOrEqual);
        return VisitFloat32Compare(this, value, cont);
      case IrOpcode::kFloat64Equal:
        cont->OverwriteAndNegateIfEqual(kUnorderedEqual);
        return VisitFloat64Compare(this, value, cont);
      case IrOpcode::kFloat64LessThan: {
        Float64BinopMatcher m(value);
        if (m.left().Is(0.0) && m.right().IsFloat64Abs()) {
          // This matches the pattern
          //
          //   Float64LessThan(#0.0, Float64Abs(x))
          //
          // which TurboFan generates for NumberToBoolean in the general case,
          // and which evaluates to false if x is 0, -0 or NaN. We can compile
          // this to a simple (v)ucomisd using not_equal flags condition, which
          // avoids the costly Float64Abs.
          cont->OverwriteAndNegateIfEqual(kNotEqual);
          InstructionCode const opcode =
              IsSupported(AVX) ? kAVXFloat64Cmp : kSSEFloat64Cmp;
          return VisitCompare(this, opcode, m.left().node(),
                              m.right().InputAt(0), cont, false);
        }
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThan);
        return VisitFloat64Compare(this, value, cont);
      }
      case IrOpcode::kFloat64LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThanOrEqual);
        return VisitFloat64Compare(this, value, cont);
      case IrOpcode::kProjection:
        // Check if this is the overflow output projection of an
        // <Operation>WithOverflow node.
        if (ProjectionIndexOf(value->op()) == 1u) {
          // We cannot combine the <Operation>WithOverflow with this branch
          // unless the 0th projection (the use of the actual value of the
          // <Operation> is either nullptr, which means there's no use of the
          // actual value, or was already defined, which means it is scheduled
          // *AFTER* this branch).
          Node* const node = value->InputAt(0);
          Node* const result = NodeProperties::FindProjection(node, 0);
          if (result == nullptr || IsDefined(result)) {
            switch (node->opcode()) {
              case IrOpcode::kInt32AddWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Add32, cont);
              case IrOpcode::kInt32SubWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Sub32, cont);
              case IrOpcode::kInt32MulWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Imul32, cont);
              case IrOpcode::kInt64AddWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Add, cont);
              case IrOpcode::kInt64SubWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Sub, cont);
              case IrOpcode::kInt64MulWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Imul, cont);
              default:
                break;
            }
          }
        }
        break;
      case IrOpcode::kInt32Sub:
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kWord32And:
        return VisitWordCompare(this, value, kX64Test32, cont);
      case IrOpcode::kStackPointerGreaterThan:
        cont->OverwriteAndNegateIfEqual(kStackPointerGreaterThanCondition);
        return VisitStackPointerGreaterThan(value, cont);
      default:
        break;
    }
  }

  // Branch could not be combined with a compare, emit compare against 0.
  VisitCompareZero(this, user, value, kX64Cmp32, cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitSwitch(Node* node,
                                                const SwitchInfo& sw) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand value_operand = g.UseRegister(node->InputAt(0));

  // Emit either ArchTableSwitch or ArchBinarySearchSwitch.
  if (enable_switch_jump_table_ ==
      InstructionSelector::kEnableSwitchJumpTable) {
    static const size_t kMaxTableSwitchValueRange = 2 << 16;
    size_t table_space_cost = 4 + sw.value_range();
    size_t table_time_cost = 3;
    size_t lookup_space_cost = 3 + 2 * sw.case_count();
    size_t lookup_time_cost = sw.case_count();
    if (sw.case_count() > 4 &&
        table_space_cost + 3 * table_time_cost <=
            lookup_space_cost + 3 * lookup_time_cost &&
        sw.min_value() > std::numeric_limits<int32_t>::min() &&
        sw.value_range() <= kMaxTableSwitchValueRange) {
      InstructionOperand index_operand = g.TempRegister();
      if (sw.min_value()) {
        // The leal automatically zero extends, so result is a valid 64-bit
        // index.
        Emit(kX64Lea32 | AddressingModeField::encode(kMode_MRI), index_operand,
             value_operand, g.TempImmediate(-sw.min_value()));
      } else {
        // Zero extend, because we use it as 64-bit index into the jump table.
        if (ZeroExtendsWord32ToWord64(node->InputAt(0))) {
          // Input value has already been zero-extended.
          index_operand = value_operand;
        } else {
          Emit(kX64Movl, index_operand, value_operand);
        }
      }
      // Generate a table lookup.
      return EmitTableSwitch(sw, index_operand);
    }
  }

  // Generate a tree of conditional jumps.
  return EmitBinarySearchSwitch(sw, value_operand);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32Equal(node_t node) {
  node_t user = node;
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    USE(user);
    UNIMPLEMENTED();
  } else {
    Int32BinopMatcher m(user);
    if (m.right().Is(0)) {
      return VisitWordCompareZero(m.node(), m.left().node(), &cont);
    }
  }
  VisitWord32EqualImpl(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32LessThan(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWordCompare(this, node, kX64Cmp32, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32LessThanOrEqual(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWordCompare(this, node, kX64Cmp32, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32LessThan(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWordCompare(this, node, kX64Cmp32, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint32LessThanOrEqual(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWordCompare(this, node, kX64Cmp32, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64Equal(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
    Int64BinopMatcher m(node);
    if (m.right().Is(0)) {
      // Try to combine the equality check with a comparison.
      Node* const user = m.node();
      Node* const value = m.left().node();
      if (CanCover(user, value)) {
        switch (value->opcode()) {
          case IrOpcode::kInt64Sub:
            return VisitWordCompare(this, value, kX64Cmp, &cont);
          case IrOpcode::kWord64And:
            return VisitWordCompare(this, value, kX64Test, &cont);
          default:
            break;
        }
      }
    }
    VisitWord64EqualImpl(this, node, &cont);
  }
}

template <>
Node* InstructionSelectorT<TurbofanAdapter>::FindProjection(
    Node* node, size_t projection_index) {
  return NodeProperties::FindProjection(node, projection_index);
}

template <>
turboshaft::OpIndex InstructionSelectorT<TurboshaftAdapter>::FindProjection(
    turboshaft::OpIndex node, size_t projection_index) {
  // TODO(nicohartmann@): This is a bit of a hack and doesn't cover all cases
  // (projections need not be placed in the same block). We should find a better
  // way to get those (e.g. by always placing them right after the op).
  for (node_t n : this->nodes(current_block_)) {
    if (const turboshaft::ProjectionOp* projection =
            this->Get(n).TryCast<turboshaft::ProjectionOp>()) {
      if (projection->input() == node &&
          projection->index == projection_index) {
        return n;
      }
    }
  }
  UNIMPLEMENTED();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32AddWithOverflow(node_t node) {
  node_t ovf = FindProjection(node, 1);
  if (Adapter::valid(ovf)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX64Add32, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX64Add32, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32SubWithOverflow(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
      FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
      return VisitBinop(this, node, kX64Sub32, &cont);
    }
    FlagsContinuation cont;
    VisitBinop(this, node, kX64Sub32, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64LessThan(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
    VisitWordCompare(this, node, kX64Cmp, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64LessThanOrEqual(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont =
        FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
    VisitWordCompare(this, node, kX64Cmp, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64LessThan(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWordCompare(this, node, kX64Cmp, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitUint64LessThanOrEqual(Node* node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    FlagsContinuation cont =
        FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
    VisitWordCompare(this, node, kX64Cmp, &cont);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32Equal(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnorderedEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32LessThan(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThan, node);
  VisitFloat32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat32LessThanOrEqual(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThanOrEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64Equal(node_t node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnorderedEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64LessThan(node_t node) {
  if constexpr (Adapter::IsTurboshaft) {
    // TODO(nicohartmann@): Implement this.
    UNIMPLEMENTED();
  } else {
    Float64BinopMatcher m(node);
    if (m.left().Is(0.0) && m.right().IsFloat64Abs()) {
      // This matches the pattern
      //
      //   Float64LessThan(#0.0, Float64Abs(x))
      //
      // which TurboFan generates for NumberToBoolean in the general case,
      // and which evaluates to false if x is 0, -0 or NaN. We can compile
      // this to a simple (v)ucomisd using not_equal flags condition, which
      // avoids the costly Float64Abs.
      FlagsContinuation cont = FlagsContinuation::ForSet(kNotEqual, node);
      InstructionCode const opcode =
          IsSupported(AVX) ? kAVXFloat64Cmp : kSSEFloat64Cmp;
      return VisitCompare(this, opcode, m.left().node(), m.right().InputAt(0),
                          &cont, false);
    }
  }
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThan, node);
  VisitFloat64Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64LessThanOrEqual(node_t node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThanOrEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64InsertLowWord32(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  Float64Matcher mleft(left);
  if (mleft.HasResolvedValue() &&
      (base::bit_cast<uint64_t>(mleft.ResolvedValue()) >> 32) == 0u) {
    Emit(kSSEFloat64LoadLowWord32, g.DefineAsRegister(node), g.Use(right));
    return;
  }
  Emit(kSSEFloat64InsertLowWord32, g.DefineSameAsFirst(node),
       g.UseRegister(left), g.Use(right));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64InsertHighWord32(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  Emit(kSSEFloat64InsertHighWord32, g.DefineSameAsFirst(node),
       g.UseRegister(left), g.Use(right));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitFloat64SilenceNaN(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kSSEFloat64SilenceNaN, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitMemoryBarrier(Node* node) {
  // x64 is no weaker than release-acquire and only needs to emit an instruction
  // for SeqCst memory barriers.
  AtomicMemoryOrder order = OpParameter<AtomicMemoryOrder>(node->op());
  if (order == AtomicMemoryOrder::kSeqCst) {
    X64OperandGeneratorT<Adapter> g(this);
    Emit(kX64MFence, g.NoOutput());
    return;
  }
  DCHECK_EQ(AtomicMemoryOrder::kAcqRel, order);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32AtomicLoad(Node* node) {
  AtomicLoadParameters atomic_load_params = AtomicLoadParametersOf(node->op());
  LoadRepresentation load_rep = atomic_load_params.representation();
  DCHECK(IsIntegral(load_rep.representation()) ||
         IsAnyTagged(load_rep.representation()) ||
         (COMPRESS_POINTERS_BOOL &&
          CanBeCompressedPointer(load_rep.representation())));
  DCHECK_NE(load_rep.representation(), MachineRepresentation::kWord64);
  DCHECK(!load_rep.IsMapWord());
  // The memory order is ignored as both acquire and sequentially consistent
  // loads can emit MOV.
  // https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html
  VisitLoad(node, node, GetLoadOpcode(load_rep));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64AtomicLoad(Node* node) {
  AtomicLoadParameters atomic_load_params = AtomicLoadParametersOf(node->op());
  DCHECK(!atomic_load_params.representation().IsMapWord());
  // The memory order is ignored as both acquire and sequentially consistent
  // loads can emit MOV.
  // https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html
  VisitLoad(node, node, GetLoadOpcode(atomic_load_params.representation()));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32AtomicStore(node_t node) {
  auto store = this->store_view(node);
  DCHECK_NE(store.stored_rep().representation(),
            MachineRepresentation::kWord64);
  DCHECK_IMPLIES(
      CanBeTaggedOrCompressedPointer(store.stored_rep().representation()),
      kTaggedSize == 4);
  VisitStoreCommon(this, store);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64AtomicStore(node_t node) {
  auto store = this->store_view(node);
  DCHECK_IMPLIES(
      CanBeTaggedOrCompressedPointer(store.stored_rep().representation()),
      kTaggedSize == 8);
  VisitStoreCommon(this, store);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord32AtomicExchange(Node* node) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Int8()) {
    opcode = kAtomicExchangeInt8;
  } else if (params.type() == MachineType::Uint8()) {
    opcode = kAtomicExchangeUint8;
  } else if (params.type() == MachineType::Int16()) {
    opcode = kAtomicExchangeInt16;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = kAtomicExchangeUint16;
  } else if (params.type() == MachineType::Int32()
    || params.type() == MachineType::Uint32()) {
    opcode = kAtomicExchangeWord32;
  } else {
    UNREACHABLE();
  }
  VisitAtomicExchange(this, node, opcode, AtomicWidth::kWord32, params.kind());
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitWord64AtomicExchange(Node* node) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Uint8()) {
    opcode = kAtomicExchangeUint8;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = kAtomicExchangeUint16;
  } else if (params.type() == MachineType::Uint32()) {
    opcode = kAtomicExchangeWord32;
  } else if (params.type() == MachineType::Uint64()) {
    opcode = kX64Word64AtomicExchangeUint64;
  } else {
    UNREACHABLE();
  }
  VisitAtomicExchange(this, node, opcode, AtomicWidth::kWord64, params.kind());
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitWord32AtomicCompareExchange(
    Node* node) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Int8()) {
    opcode = kAtomicCompareExchangeInt8;
  } else if (params.type() == MachineType::Uint8()) {
    opcode = kAtomicCompareExchangeUint8;
  } else if (params.type() == MachineType::Int16()) {
    opcode = kAtomicCompareExchangeInt16;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = kAtomicCompareExchangeUint16;
  } else if (params.type() == MachineType::Int32()
    || params.type() == MachineType::Uint32()) {
    opcode = kAtomicCompareExchangeWord32;
  } else {
    UNREACHABLE();
  }
  VisitAtomicCompareExchange(this, node, opcode, AtomicWidth::kWord32,
                             params.kind());
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitWord64AtomicCompareExchange(
    Node* node) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Uint8()) {
    opcode = kAtomicCompareExchangeUint8;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = kAtomicCompareExchangeUint16;
  } else if (params.type() == MachineType::Uint32()) {
    opcode = kAtomicCompareExchangeWord32;
  } else if (params.type() == MachineType::Uint64()) {
    opcode = kX64Word64AtomicCompareExchangeUint64;
  } else {
    UNREACHABLE();
  }
  VisitAtomicCompareExchange(this, node, opcode, AtomicWidth::kWord64,
                             params.kind());
}

template <>
void InstructionSelectorT<TurboshaftAdapter>::VisitWord32AtomicBinaryOperation(
    turboshaft::OpIndex node, ArchOpcode int8_op, ArchOpcode uint8_op,
    ArchOpcode int16_op, ArchOpcode uint16_op, ArchOpcode word32_op) {
  UNIMPLEMENTED();
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitWord32AtomicBinaryOperation(
    Node* node, ArchOpcode int8_op, ArchOpcode uint8_op, ArchOpcode int16_op,
    ArchOpcode uint16_op, ArchOpcode word32_op) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Int8()) {
    opcode = int8_op;
  } else if (params.type() == MachineType::Uint8()) {
    opcode = uint8_op;
  } else if (params.type() == MachineType::Int16()) {
    opcode = int16_op;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = uint16_op;
  } else if (params.type() == MachineType::Int32()
    || params.type() == MachineType::Uint32()) {
    opcode = word32_op;
  } else {
    UNREACHABLE();
  }
  VisitAtomicBinop(this, node, opcode, AtomicWidth::kWord32, params.kind());
}

#define VISIT_ATOMIC_BINOP(op)                                            \
  template <typename Adapter>                                             \
  void InstructionSelectorT<Adapter>::VisitWord32Atomic##op(Node* node) { \
    VisitWord32AtomicBinaryOperation(                                     \
        node, kAtomic##op##Int8, kAtomic##op##Uint8, kAtomic##op##Int16,  \
        kAtomic##op##Uint16, kAtomic##op##Word32);                        \
  }
VISIT_ATOMIC_BINOP(Add)
VISIT_ATOMIC_BINOP(Sub)
VISIT_ATOMIC_BINOP(And)
VISIT_ATOMIC_BINOP(Or)
VISIT_ATOMIC_BINOP(Xor)
#undef VISIT_ATOMIC_BINOP

template <>
void InstructionSelectorT<TurboshaftAdapter>::VisitWord64AtomicBinaryOperation(
    node_t node, ArchOpcode uint8_op, ArchOpcode uint16_op,
    ArchOpcode uint32_op, ArchOpcode word64_op) {
  UNIMPLEMENTED();
}

template <>
void InstructionSelectorT<TurbofanAdapter>::VisitWord64AtomicBinaryOperation(
    Node* node, ArchOpcode uint8_op, ArchOpcode uint16_op, ArchOpcode uint32_op,
    ArchOpcode word64_op) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Uint8()) {
    opcode = uint8_op;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = uint16_op;
  } else if (params.type() == MachineType::Uint32()) {
    opcode = uint32_op;
  } else if (params.type() == MachineType::Uint64()) {
    opcode = word64_op;
  } else {
    UNREACHABLE();
  }
  VisitAtomicBinop(this, node, opcode, AtomicWidth::kWord64, params.kind());
}

#define VISIT_ATOMIC_BINOP(op)                                                 \
  template <typename Adapter>                                                  \
  void InstructionSelectorT<Adapter>::VisitWord64Atomic##op(Node* node) {      \
    VisitWord64AtomicBinaryOperation(node, kAtomic##op##Uint8,                 \
                                     kAtomic##op##Uint16, kAtomic##op##Word32, \
                                     kX64Word64Atomic##op##Uint64);            \
  }
VISIT_ATOMIC_BINOP(Add)
VISIT_ATOMIC_BINOP(Sub)
VISIT_ATOMIC_BINOP(And)
VISIT_ATOMIC_BINOP(Or)
VISIT_ATOMIC_BINOP(Xor)
#undef VISIT_ATOMIC_BINOP

#define SIMD_BINOP_SSE_AVX_LIST(V) \
  V(I64x2ExtMulLowI32x4S)          \
  V(I64x2ExtMulHighI32x4S)         \
  V(I64x2ExtMulLowI32x4U)          \
  V(I64x2ExtMulHighI32x4U)         \
  V(I32x4DotI16x8S)                \
  V(I32x8DotI16x16S)               \
  V(I32x4ExtMulLowI16x8S)          \
  V(I32x4ExtMulHighI16x8S)         \
  V(I32x4ExtMulLowI16x8U)          \
  V(I32x4ExtMulHighI16x8U)         \
  V(I16x8SConvertI32x4)            \
  V(I16x8UConvertI32x4)            \
  V(I16x8ExtMulLowI8x16S)          \
  V(I16x8ExtMulHighI8x16S)         \
  V(I16x8ExtMulLowI8x16U)          \
  V(I16x8ExtMulHighI8x16U)         \
  V(I16x8Q15MulRSatS)              \
  V(I16x8RelaxedQ15MulRS)          \
  V(I8x16SConvertI16x8)            \
  V(I8x16UConvertI16x8)            \
  V(I16x16SConvertI32x8)           \
  V(I16x16UConvertI32x8)           \
  V(I8x32SConvertI16x16)           \
  V(I8x32UConvertI16x16)           \
  V(I64x4ExtMulI32x4S)             \
  V(I64x4ExtMulI32x4U)             \
  V(I32x8ExtMulI16x8S)             \
  V(I32x8ExtMulI16x8U)             \
  V(I16x16ExtMulI8x16S)            \
  V(I16x16ExtMulI8x16U)

#define SIMD_BINOP_SSE_AVX_LANE_SIZE_VECTOR_LENGTH_LIST(V)  \
  V(F64x2Add, FAdd, kL64, kV128)                            \
  V(F64x4Add, FAdd, kL64, kV256)                            \
  V(F32x4Add, FAdd, kL32, kV128)                            \
  V(F32x8Add, FAdd, kL32, kV256)                            \
  V(I64x2Add, IAdd, kL64, kV128)                            \
  V(I64x4Add, IAdd, kL64, kV256)                            \
  V(I32x8Add, IAdd, kL32, kV256)                            \
  V(I16x16Add, IAdd, kL16, kV256)                           \
  V(I8x32Add, IAdd, kL8, kV256)                             \
  V(I32x4Add, IAdd, kL32, kV128)                            \
  V(I16x8Add, IAdd, kL16, kV128)                            \
  V(I8x16Add, IAdd, kL8, kV128)                             \
  V(F64x4Sub, FSub, kL64, kV256)                            \
  V(F64x2Sub, FSub, kL64, kV128)                            \
  V(F32x4Sub, FSub, kL32, kV128)                            \
  V(F32x8Sub, FSub, kL32, kV256)                            \
  V(I64x2Sub, ISub, kL64, kV128)                            \
  V(I64x4Sub, ISub, kL64, kV256)                            \
  V(I32x8Sub, ISub, kL32, kV256)                            \
  V(I16x16Sub, ISub, kL16, kV256)                           \
  V(I8x32Sub, ISub, kL8, kV256)                             \
  V(I32x4Sub, ISub, kL32, kV128)                            \
  V(I16x8Sub, ISub, kL16, kV128)                            \
  V(I8x16Sub, ISub, kL8, kV128)                             \
  V(F64x2Mul, FMul, kL64, kV128)                            \
  V(F32x4Mul, FMul, kL32, kV128)                            \
  V(F64x4Mul, FMul, kL64, kV256)                            \
  V(F32x8Mul, FMul, kL32, kV256)                            \
  V(I32x8Mul, IMul, kL32, kV256)                            \
  V(I16x16Mul, IMul, kL16, kV256)                           \
  V(I32x4Mul, IMul, kL32, kV128)                            \
  V(I16x8Mul, IMul, kL16, kV128)                            \
  V(F64x2Div, FDiv, kL64, kV128)                            \
  V(F32x4Div, FDiv, kL32, kV128)                            \
  V(F64x4Div, FDiv, kL64, kV256)                            \
  V(F32x8Div, FDiv, kL32, kV256)                            \
  V(I16x8AddSatS, IAddSatS, kL16, kV128)                    \
  V(I16x16AddSatS, IAddSatS, kL16, kV256)                   \
  V(I8x16AddSatS, IAddSatS, kL8, kV128)                     \
  V(I8x32AddSatS, IAddSatS, kL8, kV256)                     \
  V(I16x8SubSatS, ISubSatS, kL16, kV128)                    \
  V(I16x16SubSatS, ISubSatS, kL16, kV256)                   \
  V(I8x16SubSatS, ISubSatS, kL8, kV128)                     \
  V(I8x32SubSatS, ISubSatS, kL8, kV256)                     \
  V(I16x8AddSatU, IAddSatU, kL16, kV128)                    \
  V(I16x16AddSatU, IAddSatU, kL16, kV256)                   \
  V(I8x16AddSatU, IAddSatU, kL8, kV128)                     \
  V(I8x32AddSatU, IAddSatU, kL8, kV256)                     \
  V(I16x8SubSatU, ISubSatU, kL16, kV128)                    \
  V(I16x16SubSatU, ISubSatU, kL16, kV256)                   \
  V(I8x16SubSatU, ISubSatU, kL8, kV128)                     \
  V(I8x32SubSatU, ISubSatU, kL8, kV256)                     \
  V(F64x2Eq, FEq, kL64, kV128)                              \
  V(F32x4Eq, FEq, kL32, kV128)                              \
  V(F32x8Eq, FEq, kL32, kV256)                              \
  V(F64x4Eq, FEq, kL64, kV256)                              \
  V(I8x32Eq, IEq, kL8, kV256)                               \
  V(I16x16Eq, IEq, kL16, kV256)                             \
  V(I32x8Eq, IEq, kL32, kV256)                              \
  V(I64x4Eq, IEq, kL64, kV256)                              \
  V(I64x2Eq, IEq, kL64, kV128)                              \
  V(I32x4Eq, IEq, kL32, kV128)                              \
  V(I16x8Eq, IEq, kL16, kV128)                              \
  V(I8x16Eq, IEq, kL8, kV128)                               \
  V(F64x2Ne, FNe, kL64, kV128)                              \
  V(F32x4Ne, FNe, kL32, kV128)                              \
  V(F32x8Ne, FNe, kL32, kV256)                              \
  V(F64x4Ne, FNe, kL64, kV256)                              \
  V(I32x4GtS, IGtS, kL32, kV128)                            \
  V(I16x8GtS, IGtS, kL16, kV128)                            \
  V(I8x16GtS, IGtS, kL8, kV128)                             \
  V(I8x32GtS, IGtS, kL8, kV256)                             \
  V(I16x16GtS, IGtS, kL16, kV256)                           \
  V(I32x8GtS, IGtS, kL32, kV256)                            \
  V(I64x4GtS, IGtS, kL64, kV256)                            \
  V(F64x2Lt, FLt, kL64, kV128)                              \
  V(F32x4Lt, FLt, kL32, kV128)                              \
  V(F64x4Lt, FLt, kL64, kV256)                              \
  V(F32x8Lt, FLt, kL32, kV256)                              \
  V(F64x2Le, FLe, kL64, kV128)                              \
  V(F32x4Le, FLe, kL32, kV128)                              \
  V(F64x4Le, FLe, kL64, kV256)                              \
  V(F32x8Le, FLe, kL32, kV256)                              \
  V(I32x4MinS, IMinS, kL32, kV128)                          \
  V(I16x8MinS, IMinS, kL16, kV128)                          \
  V(I8x16MinS, IMinS, kL8, kV128)                           \
  V(I32x4MinU, IMinU, kL32, kV128)                          \
  V(I16x8MinU, IMinU, kL16, kV128)                          \
  V(I8x16MinU, IMinU, kL8, kV128)                           \
  V(I32x4MaxS, IMaxS, kL32, kV128)                          \
  V(I16x8MaxS, IMaxS, kL16, kV128)                          \
  V(I8x16MaxS, IMaxS, kL8, kV128)                           \
  V(I32x4MaxU, IMaxU, kL32, kV128)                          \
  V(I16x8MaxU, IMaxU, kL16, kV128)                          \
  V(I8x16MaxU, IMaxU, kL8, kV128)                           \
  V(I32x8MinS, IMinS, kL32, kV256)                          \
  V(I16x16MinS, IMinS, kL16, kV256)                         \
  V(I8x32MinS, IMinS, kL8, kV256)                           \
  V(I32x8MinU, IMinU, kL32, kV256)                          \
  V(I16x16MinU, IMinU, kL16, kV256)                         \
  V(I8x32MinU, IMinU, kL8, kV256)                           \
  V(I32x8MaxS, IMaxS, kL32, kV256)                          \
  V(I16x16MaxS, IMaxS, kL16, kV256)                         \
  V(I8x32MaxS, IMaxS, kL8, kV256)                           \
  V(I32x8MaxU, IMaxU, kL32, kV256)                          \
  V(I16x16MaxU, IMaxU, kL16, kV256)                         \
  V(I8x32MaxU, IMaxU, kL8, kV256)                           \
  V(I16x8RoundingAverageU, IRoundingAverageU, kL16, kV128)  \
  V(I16x16RoundingAverageU, IRoundingAverageU, kL16, kV256) \
  V(I8x16RoundingAverageU, IRoundingAverageU, kL8, kV128)   \
  V(I8x32RoundingAverageU, IRoundingAverageU, kL8, kV256)   \
  V(S128And, SAnd, kL8, kV128)                              \
  V(S256And, SAnd, kL8, kV256)                              \
  V(S128Or, SOr, kL8, kV128)                                \
  V(S256Or, SOr, kL8, kV256)                                \
  V(S128Xor, SXor, kL8, kV128)                              \
  V(S256Xor, SXor, kL8, kV256)

#define SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH_LIST(V) \
  V(F64x2Min, FMin, kL64, kV128)                   \
  V(F32x4Min, FMin, kL32, kV128)                   \
  V(F64x4Min, FMin, kL64, kV256)                   \
  V(F32x8Min, FMin, kL32, kV256)                   \
  V(F64x2Max, FMax, kL64, kV128)                   \
  V(F32x4Max, FMax, kL32, kV128)                   \
  V(F64x4Max, FMax, kL64, kV256)                   \
  V(F32x8Max, FMax, kL32, kV256)                   \
  V(I64x2Ne, INe, kL64, kV128)                     \
  V(I32x4Ne, INe, kL32, kV128)                     \
  V(I16x8Ne, INe, kL16, kV128)                     \
  V(I8x16Ne, INe, kL8, kV128)                      \
  V(I64x4Ne, INe, kL64, kV256)                     \
  V(I32x8Ne, INe, kL32, kV256)                     \
  V(I16x16Ne, INe, kL16, kV256)                    \
  V(I8x32Ne, INe, kL8, kV256)                      \
  V(I32x4GtU, IGtU, kL32, kV128)                   \
  V(I16x8GtU, IGtU, kL16, kV128)                   \
  V(I8x16GtU, IGtU, kL8, kV128)                    \
  V(I32x8GtU, IGtU, kL32, kV256)                   \
  V(I16x16GtU, IGtU, kL16, kV256)                  \
  V(I8x32GtU, IGtU, kL8, kV256)                    \
  V(I32x4GeS, IGeS, kL32, kV128)                   \
  V(I16x8GeS, IGeS, kL16, kV128)                   \
  V(I8x16GeS, IGeS, kL8, kV128)                    \
  V(I32x8GeS, IGeS, kL32, kV256)                   \
  V(I16x16GeS, IGeS, kL16, kV256)                  \
  V(I8x32GeS, IGeS, kL8, kV256)                    \
  V(I32x4GeU, IGeU, kL32, kV128)                   \
  V(I16x8GeU, IGeU, kL16, kV128)                   \
  V(I8x16GeU, IGeU, kL8, kV128)                    \
  V(I32x8GeU, IGeU, kL32, kV256)                   \
  V(I16x16GeU, IGeU, kL16, kV256)                  \
  V(I8x32GeU, IGeU, kL8, kV256)

#define SIMD_UNOP_LIST(V)   \
  V(F64x2ConvertLowI32x4S)  \
  V(F64x4ConvertI32x4S)     \
  V(F32x4SConvertI32x4)     \
  V(F32x8SConvertI32x8)     \
  V(F32x4DemoteF64x2Zero)   \
  V(F32x4DemoteF64x4)       \
  V(I64x2SConvertI32x4Low)  \
  V(I64x2SConvertI32x4High) \
  V(I64x4SConvertI32x4)     \
  V(I64x2UConvertI32x4Low)  \
  V(I64x2UConvertI32x4High) \
  V(I64x4UConvertI32x4)     \
  V(I32x4SConvertI16x8Low)  \
  V(I32x4SConvertI16x8High) \
  V(I32x8SConvertI16x8)     \
  V(I32x4UConvertI16x8Low)  \
  V(I32x4UConvertI16x8High) \
  V(I32x8UConvertI16x8)     \
  V(I16x8SConvertI8x16Low)  \
  V(I16x8SConvertI8x16High) \
  V(I16x16SConvertI8x16)    \
  V(I16x8UConvertI8x16Low)  \
  V(I16x8UConvertI8x16High) \
  V(I16x16UConvertI8x16)

#define SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH_LIST(V) \
  V(F32x4Abs, FAbs, kL32, kV128)                  \
  V(I32x4Abs, IAbs, kL32, kV128)                  \
  V(I16x8Abs, IAbs, kL16, kV128)                  \
  V(I8x16Abs, IAbs, kL8, kV128)                   \
  V(F32x4Neg, FNeg, kL32, kV128)                  \
  V(I32x4Neg, INeg, kL32, kV128)                  \
  V(I16x8Neg, INeg, kL16, kV128)                  \
  V(I8x16Neg, INeg, kL8, kV128)                   \
  V(F64x2Sqrt, FSqrt, kL64, kV128)                \
  V(F32x4Sqrt, FSqrt, kL32, kV128)                \
  V(I64x2BitMask, IBitMask, kL64, kV128)          \
  V(I32x4BitMask, IBitMask, kL32, kV128)          \
  V(I16x8BitMask, IBitMask, kL16, kV128)          \
  V(I8x16BitMask, IBitMask, kL8, kV128)           \
  V(I64x2AllTrue, IAllTrue, kL64, kV128)          \
  V(I32x4AllTrue, IAllTrue, kL32, kV128)          \
  V(I16x8AllTrue, IAllTrue, kL16, kV128)          \
  V(I8x16AllTrue, IAllTrue, kL8, kV128)           \
  V(S128Not, SNot, kL8, kV128)                    \
  V(F32x8Abs, FAbs, kL32, kV256)                  \
  V(I32x8Abs, IAbs, kL32, kV256)                  \
  V(I16x16Abs, IAbs, kL16, kV256)                 \
  V(I8x32Abs, IAbs, kL8, kV256)                   \
  V(F32x8Neg, FNeg, kL32, kV256)                  \
  V(I32x8Neg, INeg, kL32, kV256)                  \
  V(I16x16Neg, INeg, kL16, kV256)                 \
  V(I8x32Neg, INeg, kL8, kV256)                   \
  V(F64x4Sqrt, FSqrt, kL64, kV256)                \
  V(F32x8Sqrt, FSqrt, kL32, kV256)                \
  V(S256Not, SNot, kL8, kV256)

#define SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(V) \
  V(I64x2Shl, IShl, kL64, kV128)                      \
  V(I32x4Shl, IShl, kL32, kV128)                      \
  V(I16x8Shl, IShl, kL16, kV128)                      \
  V(I32x4ShrS, IShrS, kL32, kV128)                    \
  V(I16x8ShrS, IShrS, kL16, kV128)                    \
  V(I64x2ShrU, IShrU, kL64, kV128)                    \
  V(I32x4ShrU, IShrU, kL32, kV128)                    \
  V(I16x8ShrU, IShrU, kL16, kV128)                    \
  V(I64x4Shl, IShl, kL64, kV256)                      \
  V(I32x8Shl, IShl, kL32, kV256)                      \
  V(I16x16Shl, IShl, kL16, kV256)                     \
  V(I32x8ShrS, IShrS, kL32, kV256)                    \
  V(I16x16ShrS, IShrS, kL16, kV256)                   \
  V(I64x4ShrU, IShrU, kL64, kV256)                    \
  V(I32x8ShrU, IShrU, kL32, kV256)                    \
  V(I16x16ShrU, IShrU, kL16, kV256)

#define SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(V) \
  V(I8x16Shl, IShl, kL8, kV128)                              \
  V(I8x16ShrS, IShrS, kL8, kV128)                            \
  V(I8x16ShrU, IShrU, kL8, kV128)

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS128Const(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  static const int kUint32Immediates = kSimd128Size / sizeof(uint32_t);
  uint32_t val[kUint32Immediates];
  memcpy(val, S128ImmediateParameterOf(node->op()).data(), kSimd128Size);
  // If all bytes are zeros or ones, avoid emitting code for generic constants
  bool all_zeros = !(val[0] || val[1] || val[2] || val[3]);
  bool all_ones = val[0] == UINT32_MAX && val[1] == UINT32_MAX &&
                  val[2] == UINT32_MAX && val[3] == UINT32_MAX;
  InstructionOperand dst = g.DefineAsRegister(node);
  if (all_zeros) {
    Emit(kX64SZero | VectorLengthField::encode(kV128), dst);
  } else if (all_ones) {
    Emit(kX64SAllOnes | VectorLengthField::encode(kV128), dst);
  } else {
    Emit(kX64S128Const, dst, g.UseImmediate(val[0]), g.UseImmediate(val[1]),
         g.UseImmediate(val[2]), g.UseImmediate(val[3]));
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS128Zero(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64SZero | VectorLengthField::encode(kV128), g.DefineAsRegister(node));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS256Zero(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64SZero | VectorLengthField::encode(kV256), g.DefineAsRegister(node));
}
// Name, LaneSize, VectorLength
#define SIMD_INT_TYPES_FOR_SPLAT(V) \
  V(I64x2, kL64, kV128)             \
  V(I32x4, kL32, kV128)             \
  V(I16x8, kL16, kV128)             \
  V(I8x16, kL8, kV128)              \
  V(I64x4, kL64, kV256)             \
  V(I32x8, kL32, kV256)             \
  V(I16x16, kL16, kV256)            \
  V(I8x32, kL8, kV256)

// Splat with an optimization for const 0.
#define VISIT_INT_SIMD_SPLAT(Type, LaneSize, VectorLength)                   \
  template <typename Adapter>                                                \
  void InstructionSelectorT<Adapter>::Visit##Type##Splat(Node* node) {       \
    X64OperandGeneratorT<Adapter> g(this);                                   \
    Node* input = node->InputAt(0);                                          \
    if (g.CanBeImmediate(input) && g.GetImmediateIntegerValue(input) == 0) { \
      Emit(kX64SZero | VectorLengthField::encode(VectorLength),              \
           g.DefineAsRegister(node));                                        \
    } else {                                                                 \
      Emit(kX64ISplat | LaneSizeField::encode(LaneSize) |                    \
               VectorLengthField::encode(VectorLength),                      \
           g.DefineAsRegister(node), g.Use(input));                          \
    }                                                                        \
  }
SIMD_INT_TYPES_FOR_SPLAT(VISIT_INT_SIMD_SPLAT)
#undef VISIT_INT_SIMD_SPLAT
#undef SIMD_INT_TYPES_FOR_SPLAT

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2Splat(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64FSplat | LaneSizeField::encode(kL64) |
           VectorLengthField::encode(kV128),
       g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4Splat(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64FSplat | LaneSizeField::encode(kL32) |
           VectorLengthField::encode(kV128),
       g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x4Splat(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64FSplat | LaneSizeField::encode(kL64) |
           VectorLengthField::encode(kV256),
       g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x8Splat(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64FSplat | LaneSizeField::encode(kL32) |
           VectorLengthField::encode(kV256),
       g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}

#define SIMD_VISIT_EXTRACT_LANE(IF, Type, Sign, LaneSize, VectorLength)  \
  template <typename Adapter>                                            \
  void InstructionSelectorT<Adapter>::Visit##Type##ExtractLane##Sign(    \
      Node* node) {                                                      \
    X64OperandGeneratorT<Adapter> g(this);                               \
    int32_t lane = OpParameter<int32_t>(node->op());                     \
    Emit(kX64##IF##ExtractLane##Sign | LaneSizeField::encode(LaneSize) | \
             VectorLengthField::encode(VectorLength),                    \
         g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),      \
         g.UseImmediate(lane));                                          \
  }
SIMD_VISIT_EXTRACT_LANE(F, F64x2, , kL64, kV128)
SIMD_VISIT_EXTRACT_LANE(F, F32x4, , kL32, kV128)
SIMD_VISIT_EXTRACT_LANE(I, I64x2, , kL64, kV128)
SIMD_VISIT_EXTRACT_LANE(I, I32x4, , kL32, kV128)
SIMD_VISIT_EXTRACT_LANE(I, I16x8, S, kL16, kV128)
SIMD_VISIT_EXTRACT_LANE(I, I8x16, S, kL8, kV128)
#undef SIMD_VISIT_EXTRACT_LANE

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x8ExtractLaneU(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  int32_t lane = OpParameter<int32_t>(node->op());
  Emit(kX64Pextrw, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
       g.UseImmediate(lane));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16ExtractLaneU(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  int32_t lane = OpParameter<int32_t>(node->op());
  Emit(kX64Pextrb, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
       g.UseImmediate(lane));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4ReplaceLane(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  int32_t lane = OpParameter<int32_t>(node->op());
  Emit(kX64FReplaceLane | LaneSizeField::encode(kL32) |
           VectorLengthField::encode(kV128),
       g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),
       g.UseImmediate(lane), g.Use(node->InputAt(1)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2ReplaceLane(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  int32_t lane = OpParameter<int32_t>(node->op());
  // When no-AVX, define dst == src to save a move.
  InstructionOperand dst =
      IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);
  Emit(kX64FReplaceLane | LaneSizeField::encode(kL64) |
           VectorLengthField::encode(kV128),
       dst, g.UseRegister(node->InputAt(0)), g.UseImmediate(lane),
       g.UseRegister(node->InputAt(1)));
}

#define VISIT_SIMD_REPLACE_LANE(TYPE, OPCODE)                                \
  template <typename Adapter>                                                \
  void InstructionSelectorT<Adapter>::Visit##TYPE##ReplaceLane(Node* node) { \
    X64OperandGeneratorT<Adapter> g(this);                                   \
    int32_t lane = OpParameter<int32_t>(node->op());                         \
    Emit(OPCODE, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),  \
         g.UseImmediate(lane), g.Use(node->InputAt(1)));                     \
  }

#define SIMD_TYPES_FOR_REPLACE_LANE(V) \
  V(I64x2, kX64Pinsrq)                 \
  V(I32x4, kX64Pinsrd)                 \
  V(I16x8, kX64Pinsrw)                 \
  V(I8x16, kX64Pinsrb)

SIMD_TYPES_FOR_REPLACE_LANE(VISIT_SIMD_REPLACE_LANE)
#undef SIMD_TYPES_FOR_REPLACE_LANE
#undef VISIT_SIMD_REPLACE_LANE

#define VISIT_SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(                  \
    Name, Opcode, LaneSize, VectorLength)                                  \
  template <typename Adapter>                                              \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) {            \
    X64OperandGeneratorT<Adapter> g(this);                                 \
    InstructionOperand dst = IsSupported(AVX) ? g.DefineAsRegister(node)   \
                                              : g.DefineSameAsFirst(node); \
    if (g.CanBeImmediate(node->InputAt(1))) {                              \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                \
               VectorLengthField::encode(VectorLength),                    \
           dst, g.UseRegister(node->InputAt(0)),                           \
           g.UseImmediate(node->InputAt(1)));                              \
    } else {                                                               \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                \
               VectorLengthField::encode(VectorLength),                    \
           dst, g.UseRegister(node->InputAt(0)),                           \
           g.UseRegister(node->InputAt(1)));                               \
    }                                                                      \
  }
SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(
    VISIT_SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES)

#undef VISIT_SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES
#undef SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES

#define VISIT_SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(            \
    Name, Opcode, LaneSize, VectorLength)                                   \
  template <typename Adapter>                                               \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) {             \
    X64OperandGeneratorT<Adapter> g(this);                                  \
    InstructionOperand output =                                             \
        IsSupported(AVX) ? g.UseRegister(node) : g.DefineSameAsFirst(node); \
    if (g.CanBeImmediate(node->InputAt(1))) {                               \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                 \
               VectorLengthField::encode(VectorLength),                     \
           output, g.UseRegister(node->InputAt(0)),                         \
           g.UseImmediate(node->InputAt(1)));                               \
    } else {                                                                \
      InstructionOperand temps[] = {g.TempSimd128Register()};               \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                 \
               VectorLengthField::encode(VectorLength),                     \
           output, g.UseUniqueRegister(node->InputAt(0)),                   \
           g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps); \
    }                                                                       \
  }
SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(
    VISIT_SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES)
#undef VISIT_SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES
#undef SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES

#define VISIT_SIMD_UNOP(Opcode)                                   \
  template <typename Adapter>                                     \
  void InstructionSelectorT<Adapter>::Visit##Opcode(Node* node) { \
    X64OperandGeneratorT<Adapter> g(this);                        \
    Emit(kX64##Opcode, g.DefineAsRegister(node),                  \
         g.UseRegister(node->InputAt(0)));                        \
  }
SIMD_UNOP_LIST(VISIT_SIMD_UNOP)
#undef VISIT_SIMD_UNOP
#undef SIMD_UNOP_LIST

#define VISIT_SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH(Name, Opcode, LaneSize, \
                                                VectorLength)           \
  template <typename Adapter>                                           \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) {         \
    X64OperandGeneratorT<Adapter> g(this);                              \
    Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |               \
             VectorLengthField::encode(VectorLength),                   \
         g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));    \
  }

SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH_LIST(VISIT_SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH)

#undef VISIT_SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH
#undef SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH_LIST

#define VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH(Name, Opcode, LaneSize, \
                                                 VectorLength)           \
  template <typename Adapter>                                            \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) {          \
    X64OperandGeneratorT<Adapter> g(this);                               \
    Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                \
             VectorLengthField::encode(VectorLength),                    \
         g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),     \
         g.UseRegister(node->InputAt(1)));                               \
  }

SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH_LIST(
    VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH)

#undef VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH
#undef SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH_LIST

#define VISIT_SIMD_BINOP(Opcode)                                              \
  template <typename Adapter>                                                 \
  void InstructionSelectorT<Adapter>::Visit##Opcode(Node* node) {             \
    X64OperandGeneratorT<Adapter> g(this);                                    \
    if (IsSupported(AVX)) {                                                   \
      Emit(kX64##Opcode, g.DefineAsRegister(node),                            \
           g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1))); \
    } else {                                                                  \
      Emit(kX64##Opcode, g.DefineSameAsFirst(node),                           \
           g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1))); \
    }                                                                         \
  }

SIMD_BINOP_SSE_AVX_LIST(VISIT_SIMD_BINOP)
#undef VISIT_SIMD_BINOP
#undef SIMD_BINOP_SSE_AVX_LIST

#define VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH(Name, Opcode, LaneSize, \
                                                 VectorLength)           \
  template <typename Adapter>                                            \
  void InstructionSelectorT<Adapter>::Visit##Name(Node* node) {          \
    X64OperandGeneratorT<Adapter> g(this);                               \
    if (IsSupported(AVX)) {                                              \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |              \
               VectorLengthField::encode(VectorLength),                  \
           g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),    \
           g.UseRegister(node->InputAt(1)));                             \
    } else {                                                             \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |              \
               VectorLengthField::encode(VectorLength),                  \
           g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),   \
           g.UseRegister(node->InputAt(1)));                             \
    }                                                                    \
  }

SIMD_BINOP_SSE_AVX_LANE_SIZE_VECTOR_LENGTH_LIST(
    VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH)
#undef VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH
#undef SIMD_BINOP_SSE_AVX_LANE_SIZE_VECTOR_LENGTH_LIST

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitV128AnyTrue(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64V128AnyTrue, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS128Select(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand dst =
      IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);
  Emit(kX64SSelect | VectorLengthField::encode(kV128), dst,
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)),
       g.UseRegister(node->InputAt(2)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS256Select(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64SSelect | VectorLengthField::encode(kV256), g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)),
       g.UseRegister(node->InputAt(2)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS128AndNot(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  // andnps a b does ~a & b, but we want a & !b, so flip the input.
  Emit(kX64SAndNot | VectorLengthField::encode(kV128),
       g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(1)),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitS256AndNot(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  // andnps a b does ~a & b, but we want a & !b, so flip the input.
  Emit(kX64SAndNot | VectorLengthField::encode(kV256),
       g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(1)),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2Abs(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0),
                 kX64FAbs | LaneSizeField::encode(kL64) |
                     VectorLengthField::encode(kV128));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2Neg(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0),
                 kX64FNeg | LaneSizeField::encode(kL64) |
                     VectorLengthField::encode(kV128));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4UConvertI32x4(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64F32x4UConvertI32x4, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x8UConvertI32x8(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64F32x8UConvertI32x8, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)));
}

#define VISIT_SIMD_QFMOP(Opcode)                                             \
  template <typename Adapter>                                                \
  void InstructionSelectorT<Adapter>::Visit##Opcode(Node* node) {            \
    X64OperandGeneratorT<Adapter> g(this);                                   \
    Emit(kX64##Opcode, g.UseRegister(node), g.UseRegister(node->InputAt(0)), \
         g.UseRegister(node->InputAt(1)), g.UseRegister(node->InputAt(2)));  \
  }
VISIT_SIMD_QFMOP(F64x2Qfma)
VISIT_SIMD_QFMOP(F64x2Qfms)
VISIT_SIMD_QFMOP(F32x4Qfma)
VISIT_SIMD_QFMOP(F32x4Qfms)
#undef VISIT_SIMD_QFMOP

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2Neg(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  // If AVX unsupported, make sure dst != src to avoid a move.
  InstructionOperand operand0 = IsSupported(AVX)
                                    ? g.UseRegister(node->InputAt(0))
                                    : g.UseUnique(node->InputAt(0));
  Emit(
      kX64INeg | LaneSizeField::encode(kL64) | VectorLengthField::encode(kV128),
      g.DefineAsRegister(node), operand0);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2ShrS(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand dst =
      IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);

  if (g.CanBeImmediate(node->InputAt(1))) {
    Emit(kX64IShrS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         dst, g.UseRegister(node->InputAt(0)),
         g.UseImmediate(node->InputAt(1)));
  } else {
    InstructionOperand temps[] = {g.TempSimd128Register()};
    Emit(kX64IShrS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         dst, g.UseUniqueRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)), arraysize(temps), temps);
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2Mul(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand temps[] = {g.TempSimd128Register()};
  Emit(
      kX64IMul | LaneSizeField::encode(kL64) | VectorLengthField::encode(kV128),
      g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x4Mul(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand temps[] = {g.TempSimd256Register()};
  Emit(
      kX64IMul | LaneSizeField::encode(kL64) | VectorLengthField::encode(kV256),
      g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4SConvertF32x4(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64I32x4SConvertF32x4,
       IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4UConvertF32x4(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand temps[] = {g.TempSimd128Register(),
                                g.TempSimd128Register()};
  Emit(kX64I32x4UConvertF32x4, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)), arraysize(temps), temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x8UConvertF32x8(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand temps[] = {g.TempSimd256Register(),
                                g.TempSimd256Register()};
  Emit(kX64I32x8UConvertF32x8, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)), arraysize(temps), temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt32AbsWithOverflow(Node* node) {
  UNREACHABLE();
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitInt64AbsWithOverflow(Node* node) {
  UNREACHABLE();
}

#if V8_ENABLE_WEBASSEMBLY
namespace {

// Returns true if shuffle can be decomposed into two 16x4 half shuffles
// followed by a 16x8 blend.
// E.g. [3 2 1 0 15 14 13 12].
bool TryMatch16x8HalfShuffle(uint8_t* shuffle16x8, uint8_t* blend_mask) {
  *blend_mask = 0;
  for (int i = 0; i < 8; i++) {
    if ((shuffle16x8[i] & 0x4) != (i & 0x4)) return false;
    *blend_mask |= (shuffle16x8[i] > 7 ? 1 : 0) << i;
  }
  return true;
}

struct ShuffleEntry {
  uint8_t shuffle[kSimd128Size];
  ArchOpcode opcode;
  bool src0_needs_reg;
  bool src1_needs_reg;
  // If AVX is supported, this shuffle can use AVX's three-operand encoding, so
  // does not require same as first. We conservatively set this to false
  // (original behavior), and selectively enable for specific arch shuffles.
  bool no_same_as_first_if_avx = false;
};

// Shuffles that map to architecture-specific instruction sequences. These are
// matched very early, so we shouldn't include shuffles that match better in
// later tests, like 32x4 and 16x8 shuffles. In general, these patterns should
// map to either a single instruction, or be finer grained, such as zip/unzip or
// transpose patterns.
static const ShuffleEntry arch_shuffles[] = {
    {{0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23},
     kX64S64x2UnpackLow,
     true,
     true,
     true},
    {{8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31},
     kX64S64x2UnpackHigh,
     true,
     true,
     true},
    {{0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23},
     kX64S32x4UnpackLow,
     true,
     true,
     true},
    {{8, 9, 10, 11, 24, 25, 26, 27, 12, 13, 14, 15, 28, 29, 30, 31},
     kX64S32x4UnpackHigh,
     true,
     true,
     true},
    {{0, 1, 16, 17, 2, 3, 18, 19, 4, 5, 20, 21, 6, 7, 22, 23},
     kX64S16x8UnpackLow,
     true,
     true,
     true},
    {{8, 9, 24, 25, 10, 11, 26, 27, 12, 13, 28, 29, 14, 15, 30, 31},
     kX64S16x8UnpackHigh,
     true,
     true,
     true},
    {{0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23},
     kX64S8x16UnpackLow,
     true,
     true,
     true},
    {{8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31},
     kX64S8x16UnpackHigh,
     true,
     true,
     true},

    {{0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29},
     kX64S16x8UnzipLow,
     true,
     true},
    {{2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31},
     kX64S16x8UnzipHigh,
     true,
     true},
    {{0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30},
     kX64S8x16UnzipLow,
     true,
     true},
    {{1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31},
     kX64S8x16UnzipHigh,
     true,
     true},
    {{0, 16, 2, 18, 4, 20, 6, 22, 8, 24, 10, 26, 12, 28, 14, 30},
     kX64S8x16TransposeLow,
     true,
     true},
    {{1, 17, 3, 19, 5, 21, 7, 23, 9, 25, 11, 27, 13, 29, 15, 31},
     kX64S8x16TransposeHigh,
     true,
     true},
    {{7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8},
     kX64S8x8Reverse,
     true,
     true},
    {{3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12},
     kX64S8x4Reverse,
     true,
     true},
    {{1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14},
     kX64S8x2Reverse,
     true,
     true}};

bool TryMatchArchShuffle(const uint8_t* shuffle, const ShuffleEntry* table,
                         size_t num_entries, bool is_swizzle,
                         const ShuffleEntry** arch_shuffle) {
  uint8_t mask = is_swizzle ? kSimd128Size - 1 : 2 * kSimd128Size - 1;
  for (size_t i = 0; i < num_entries; ++i) {
    const ShuffleEntry& entry = table[i];
    int j = 0;
    for (; j < kSimd128Size; ++j) {
      if ((entry.shuffle[j] & mask) != (shuffle[j] & mask)) {
        break;
      }
    }
    if (j == kSimd128Size) {
      *arch_shuffle = &entry;
      return true;
    }
  }
  return false;
}

bool TryMatchShufps(const uint8_t* shuffle32x4) {
  DCHECK_GT(8, shuffle32x4[2]);
  DCHECK_GT(8, shuffle32x4[3]);
  // shufps can be used if the first 2 indices select the first input [0-3], and
  // the other 2 indices select the second input [4-7].
  return shuffle32x4[0] < 4 && shuffle32x4[1] < 4 && shuffle32x4[2] > 3 &&
         shuffle32x4[3] > 3;
}

static bool IsV128ZeroConst(Node* node) {
  if (node->opcode() == IrOpcode::kS128Zero) {
    return true;
  }
  // If the node is a V128 const, check all the elements
  auto m = V128ConstMatcher(node);
  if (m.HasResolvedValue()) {
    auto imms = m.ResolvedValue().immediate();
    return std::all_of(imms.begin(), imms.end(), [](auto i) { return i == 0; });
  }
  return false;
}

static bool TryMatchOneInputIsZeros(Node* node, uint8_t* shuffle,
                                    bool* needs_swap) {
  *needs_swap = false;
  bool input0_is_zero = IsV128ZeroConst(node->InputAt(0));
  bool input1_is_zero = IsV128ZeroConst(node->InputAt(1));
  if (!input0_is_zero && !input1_is_zero) {
    return false;
  }

  if (input0_is_zero) {
    *needs_swap = true;
  }
  return true;
}

}  // namespace

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16Shuffle(Node* node) {
  uint8_t shuffle[kSimd128Size];
  bool is_swizzle;
  CanonicalizeShuffle(node, shuffle, &is_swizzle);

  int imm_count = 0;
  static const int kMaxImms = 6;
  uint32_t imms[kMaxImms];
  int temp_count = 0;
  static const int kMaxTemps = 2;
  InstructionOperand temps[kMaxTemps];

  X64OperandGeneratorT<Adapter> g(this);
  // Swizzles don't generally need DefineSameAsFirst to avoid a move.
  bool no_same_as_first = is_swizzle;
  // We generally need UseRegister for input0, Use for input1.
  // TODO(v8:9198): We don't have 16-byte alignment for SIMD operands yet, but
  // we retain this logic (continue setting these in the various shuffle match
  // clauses), but ignore it when selecting registers or slots.
  bool src0_needs_reg = true;
  bool src1_needs_reg = false;
  ArchOpcode opcode = kX64I8x16Shuffle;  // general shuffle is the default

  uint8_t offset;
  uint8_t shuffle32x4[4];
  uint8_t shuffle16x8[8];
  int index;
  const ShuffleEntry* arch_shuffle;
  bool needs_swap;
  if (wasm::SimdShuffle::TryMatchConcat(shuffle, &offset)) {
    if (wasm::SimdShuffle::TryMatch32x4Rotate(shuffle, shuffle32x4,
                                              is_swizzle)) {
      uint8_t shuffle_mask = wasm::SimdShuffle::PackShuffle4(shuffle32x4);
      opcode = kX64S32x4Rotate;
      imms[imm_count++] = shuffle_mask;
    } else {
      // Swap inputs from the normal order for (v)palignr.
      SwapShuffleInputs(node);
      is_swizzle = false;  // It's simpler to just handle the general case.
      no_same_as_first = CpuFeatures::IsSupported(AVX);
      // TODO(v8:9608): also see v8:9083
      src1_needs_reg = true;
      opcode = kX64S8x16Alignr;
      // palignr takes a single imm8 offset.
      imms[imm_count++] = offset;
    }
  } else if (TryMatchArchShuffle(shuffle, arch_shuffles,
                                 arraysize(arch_shuffles), is_swizzle,
                                 &arch_shuffle)) {
    opcode = arch_shuffle->opcode;
    src0_needs_reg = arch_shuffle->src0_needs_reg;
    // SSE can't take advantage of both operands in registers and needs
    // same-as-first.
    src1_needs_reg = arch_shuffle->src1_needs_reg;
    no_same_as_first =
        IsSupported(AVX) && arch_shuffle->no_same_as_first_if_avx;
  } else if (wasm::SimdShuffle::TryMatch32x4Shuffle(shuffle, shuffle32x4)) {
    uint8_t shuffle_mask = wasm::SimdShuffle::PackShuffle4(shuffle32x4);
    if (is_swizzle) {
      if (wasm::SimdShuffle::TryMatchIdentity(shuffle)) {
        // Bypass normal shuffle code generation in this case.
        EmitIdentity(node);
        return;
      } else {
        // pshufd takes a single imm8 shuffle mask.
        opcode = kX64S32x4Swizzle;
        no_same_as_first = true;
        // TODO(v8:9083): This doesn't strictly require a register, forcing the
        // swizzles to always use registers until generation of incorrect memory
        // operands can be fixed.
        src0_needs_reg = true;
        imms[imm_count++] = shuffle_mask;
      }
    } else {
      // 2 operand shuffle
      // A blend is more efficient than a general 32x4 shuffle; try it first.
      if (wasm::SimdShuffle::TryMatchBlend(shuffle)) {
        opcode = kX64S16x8Blend;
        uint8_t blend_mask = wasm::SimdShuffle::PackBlend4(shuffle32x4);
        imms[imm_count++] = blend_mask;
        no_same_as_first = CpuFeatures::IsSupported(AVX);
      } else if (TryMatchShufps(shuffle32x4)) {
        opcode = kX64Shufps;
        uint8_t mask = wasm::SimdShuffle::PackShuffle4(shuffle32x4);
        imms[imm_count++] = mask;
        src1_needs_reg = true;
        no_same_as_first = IsSupported(AVX);
      } else {
        opcode = kX64S32x4Shuffle;
        no_same_as_first = true;
        // TODO(v8:9083): src0 and src1 is used by pshufd in codegen, which
        // requires memory to be 16-byte aligned, since we cannot guarantee that
        // yet, force using a register here.
        src0_needs_reg = true;
        src1_needs_reg = true;
        imms[imm_count++] = shuffle_mask;
        uint8_t blend_mask = wasm::SimdShuffle::PackBlend4(shuffle32x4);
        imms[imm_count++] = blend_mask;
      }
    }
  } else if (wasm::SimdShuffle::TryMatch16x8Shuffle(shuffle, shuffle16x8)) {
    uint8_t blend_mask;
    if (wasm::SimdShuffle::TryMatchBlend(shuffle)) {
      opcode = kX64S16x8Blend;
      blend_mask = wasm::SimdShuffle::PackBlend8(shuffle16x8);
      imms[imm_count++] = blend_mask;
      no_same_as_first = CpuFeatures::IsSupported(AVX);
    } else if (wasm::SimdShuffle::TryMatchSplat<8>(shuffle, &index)) {
      opcode = kX64S16x8Dup;
      src0_needs_reg = false;
      imms[imm_count++] = index;
    } else if (TryMatch16x8HalfShuffle(shuffle16x8, &blend_mask)) {
      opcode = is_swizzle ? kX64S16x8HalfShuffle1 : kX64S16x8HalfShuffle2;
      // Half-shuffles don't need DefineSameAsFirst or UseRegister(src0).
      no_same_as_first = true;
      src0_needs_reg = false;
      uint8_t mask_lo = wasm::SimdShuffle::PackShuffle4(shuffle16x8);
      uint8_t mask_hi = wasm::SimdShuffle::PackShuffle4(shuffle16x8 + 4);
      imms[imm_count++] = mask_lo;
      imms[imm_count++] = mask_hi;
      if (!is_swizzle) imms[imm_count++] = blend_mask;
    }
  } else if (wasm::SimdShuffle::TryMatchSplat<16>(shuffle, &index)) {
    opcode = kX64S8x16Dup;
    no_same_as_first = false;
    src0_needs_reg = true;
    imms[imm_count++] = index;
  } else if (TryMatchOneInputIsZeros(node, shuffle, &needs_swap)) {
    is_swizzle = true;
    // Swap zeros to input1
    if (needs_swap) {
      SwapShuffleInputs(node);
      for (int i = 0; i < kSimd128Size; ++i) {
        shuffle[i] ^= kSimd128Size;
      }
    }
    if (wasm::SimdShuffle::TryMatchByteToDwordZeroExtend(shuffle)) {
      opcode = kX64I32X4ShiftZeroExtendI8x16;
      no_same_as_first = true;
      src0_needs_reg = true;
      imms[imm_count++] = shuffle[0];
    } else {
      // If the most significant bit (bit 7) of each byte of the shuffle control
      // mask is set, then constant zero is written in the result byte. Input1
      // is zeros now, we can avoid using input1 by setting bit 7 of shuffle[i]
      // to 1.
      for (int i = 0; i < kSimd128Size; ++i) {
        if (shuffle[i] >= kSimd128Size) {
          shuffle[i] = 0x80;
        }
      }
    }
  }
  if (opcode == kX64I8x16Shuffle) {
    // Use same-as-first for general swizzle, but not shuffle.
    no_same_as_first = !is_swizzle;
    src0_needs_reg = !no_same_as_first;
    imms[imm_count++] = wasm::SimdShuffle::Pack4Lanes(shuffle);
    imms[imm_count++] = wasm::SimdShuffle::Pack4Lanes(shuffle + 4);
    imms[imm_count++] = wasm::SimdShuffle::Pack4Lanes(shuffle + 8);
    imms[imm_count++] = wasm::SimdShuffle::Pack4Lanes(shuffle + 12);
    temps[temp_count++] = g.TempSimd128Register();
  }

  // Use DefineAsRegister(node) and Use(src0) if we can without forcing an extra
  // move instruction in the CodeGenerator.
  Node* input0 = node->InputAt(0);
  InstructionOperand dst =
      no_same_as_first ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);
  // TODO(v8:9198): Use src0_needs_reg when we have memory alignment for SIMD.
  // We only need a unique register for input0 if we use temp registers.
  InstructionOperand src0 =
      temp_count ? g.UseUniqueRegister(input0) : g.UseRegister(input0);
  USE(src0_needs_reg);

  int input_count = 0;
  InstructionOperand inputs[2 + kMaxImms + kMaxTemps];
  inputs[input_count++] = src0;
  if (!is_swizzle) {
    Node* input1 = node->InputAt(1);
    // TODO(v8:9198): Use src1_needs_reg when we have memory alignment for SIMD.
    // We only need a unique register for input1 if we use temp registers.
    inputs[input_count++] =
        temp_count ? g.UseUniqueRegister(input1) : g.UseRegister(input1);
    USE(src1_needs_reg);
  }
  for (int i = 0; i < imm_count; ++i) {
    inputs[input_count++] = g.UseImmediate(imms[i]);
  }
  Emit(opcode, 1, &dst, input_count, inputs, temp_count, temps);
}
#else
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16Shuffle(Node* node) {
  UNREACHABLE();
}
#endif  // V8_ENABLE_WEBASSEMBLY

#if V8_ENABLE_WEBASSEMBLY
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16Swizzle(Node* node) {
  InstructionCode op = kX64I8x16Swizzle;

  bool relaxed = OpParameter<bool>(node->op());
  if (relaxed) {
    op |= MiscField::encode(true);
  } else {
    auto m = V128ConstMatcher(node->InputAt(1));
    if (m.HasResolvedValue()) {
      // If the indices vector is a const, check if they are in range, or if the
      // top bit is set, then we can avoid the paddusb in the codegen and simply
      // emit a pshufb.
      auto imms = m.ResolvedValue().immediate();
      op |= MiscField::encode(wasm::SimdSwizzle::AllInRangeOrTopBitSet(imms));
    }
  }

  X64OperandGeneratorT<Adapter> g(this);
  Emit(op,
       IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)));
}

namespace {
template <typename Adapter>
void VisitRelaxedLaneSelect(InstructionSelectorT<Adapter>* selector, Node* node,
                            InstructionCode code = kX64Pblendvb) {
  X64OperandGeneratorT<Adapter> g(selector);
  // pblendvb/blendvps/blendvpd copies src2 when mask is set, opposite from Wasm
  // semantics. Node's inputs are: mask, lhs, rhs (determined in
  // wasm-compiler.cc).
  if (selector->IsSupported(AVX)) {
    selector->Emit(
        code, g.DefineAsRegister(node), g.UseRegister(node->InputAt(2)),
        g.UseRegister(node->InputAt(1)), g.UseRegister(node->InputAt(0)));
  } else {
    // SSE4.1 pblendvb/blendvps/blendvpd requires xmm0 to hold the mask as an
    // implicit operand.
    selector->Emit(
        code, g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(2)),
        g.UseRegister(node->InputAt(1)), g.UseFixed(node->InputAt(0), xmm0));
  }
}
}  // namespace

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16RelaxedLaneSelect(Node* node) {
  VisitRelaxedLaneSelect(this, node);
}
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x8RelaxedLaneSelect(Node* node) {
  VisitRelaxedLaneSelect(this, node);
}
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4RelaxedLaneSelect(Node* node) {
  VisitRelaxedLaneSelect(this, node, kX64Blendvps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2RelaxedLaneSelect(Node* node) {
  VisitRelaxedLaneSelect(this, node, kX64Blendvpd);
}
#else
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16Swizzle(Node* node) {
  UNREACHABLE();
}
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16RelaxedLaneSelect(Node* node) {
  UNREACHABLE();
}
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x8RelaxedLaneSelect(Node* node) {
  UNREACHABLE();
}
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4RelaxedLaneSelect(Node* node) {
  UNREACHABLE();
}
template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2RelaxedLaneSelect(Node* node) {
  UNREACHABLE();
}
#endif  // V8_ENABLE_WEBASSEMBLY

namespace {
// Used for pmin/pmax and relaxed min/max.
template <typename Adapter>
void VisitMinOrMax(InstructionSelectorT<Adapter>* selector, Node* node,
                   ArchOpcode opcode, bool flip_inputs) {
  X64OperandGeneratorT<Adapter> g(selector);
  InstructionOperand dst = selector->IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  if (flip_inputs) {
    // Due to the way minps/minpd work, we want the dst to be same as the second
    // input: b = pmin(a, b) directly maps to minps b a.
    selector->Emit(opcode, dst, g.UseRegister(node->InputAt(1)),
                   g.UseRegister(node->InputAt(0)));
  } else {
    selector->Emit(opcode, dst, g.UseRegister(node->InputAt(0)),
                   g.UseRegister(node->InputAt(1)));
  }
}
}  // namespace

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4Pmin(Node* node) {
  VisitMinOrMax(this, node, kX64Minps, true);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4Pmax(Node* node) {
  VisitMinOrMax(this, node, kX64Maxps, true);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2Pmin(Node* node) {
  VisitMinOrMax(this, node, kX64Minpd, true);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2Pmax(Node* node) {
  VisitMinOrMax(this, node, kX64Maxpd, true);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x8Pmin(Node* node) {
  VisitMinOrMax(this, node, kX64F32x8Pmin, true);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x8Pmax(Node* node) {
  VisitMinOrMax(this, node, kX64F32x8Pmax, true);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x4Pmin(Node* node) {
  VisitMinOrMax(this, node, kX64F64x4Pmin, true);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x4Pmax(Node* node) {
  VisitMinOrMax(this, node, kX64F64x4Pmax, true);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4RelaxedMin(Node* node) {
  VisitMinOrMax(this, node, kX64Minps, false);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF32x4RelaxedMax(Node* node) {
  VisitMinOrMax(this, node, kX64Maxps, false);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2RelaxedMin(Node* node) {
  VisitMinOrMax(this, node, kX64Minpd, false);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2RelaxedMax(Node* node) {
  VisitMinOrMax(this, node, kX64Maxpd, false);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4ExtAddPairwiseI16x8S(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand dst = CpuFeatures::IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  Emit(kX64I32x4ExtAddPairwiseI16x8S, dst, g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x8ExtAddPairwiseI16x16S(
    Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64I32x8ExtAddPairwiseI16x16S, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4ExtAddPairwiseI16x8U(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand dst = CpuFeatures::IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  Emit(kX64I32x4ExtAddPairwiseI16x8U, dst, g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x8ExtAddPairwiseI16x16U(
    Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64I32x8ExtAddPairwiseI16x16U, g.DefineAsRegister(node),
       g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x8ExtAddPairwiseI8x16S(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  // Codegen depends on dst != src.
  Emit(kX64I16x8ExtAddPairwiseI8x16S, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x16ExtAddPairwiseI8x32S(
    Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64I16x16ExtAddPairwiseI8x32S, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x8ExtAddPairwiseI8x16U(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand dst = CpuFeatures::IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  Emit(kX64I16x8ExtAddPairwiseI8x16U, dst, g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x16ExtAddPairwiseI8x32U(
    Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64I16x16ExtAddPairwiseI8x32U, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI8x16Popcnt(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand temps[] = {g.TempSimd128Register()};
  Emit(kX64I8x16Popcnt, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)), arraysize(temps), temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2ConvertLowI32x4U(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand dst =
      IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);
  Emit(kX64F64x2ConvertLowI32x4U, dst, g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4TruncSatF64x2SZero(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  if (CpuFeatures::IsSupported(AVX)) {
    // Requires dst != src.
    Emit(kX64I32x4TruncSatF64x2SZero, g.DefineAsRegister(node),
         g.UseUniqueRegister(node->InputAt(0)));
  } else {
    Emit(kX64I32x4TruncSatF64x2SZero, g.DefineSameAsFirst(node),
         g.UseRegister(node->InputAt(0)));
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4TruncSatF64x2UZero(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand dst = CpuFeatures::IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  Emit(kX64I32x4TruncSatF64x2UZero, dst, g.UseRegister(node->InputAt(0)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4RelaxedTruncF64x2SZero(
    Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Cvttpd2dq);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4RelaxedTruncF64x2UZero(
    Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64I32x4TruncF64x2UZero);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4RelaxedTruncF32x4S(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Cvttps2dq);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4RelaxedTruncF32x4U(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64I32x4TruncF32x4U);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2GtS(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  if (CpuFeatures::IsSupported(AVX)) {
    Emit(kX64IGtS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)));
  } else if (CpuFeatures::IsSupported(SSE4_2)) {
    Emit(kX64IGtS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)));
  } else {
    Emit(kX64IGtS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
         g.UseUniqueRegister(node->InputAt(1)));
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2GeS(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  if (CpuFeatures::IsSupported(AVX)) {
    Emit(kX64IGeS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)));
  } else if (CpuFeatures::IsSupported(SSE4_2)) {
    Emit(kX64IGeS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)));
  } else {
    Emit(kX64IGeS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
         g.UseUniqueRegister(node->InputAt(1)));
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x4GeS(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  DCHECK(CpuFeatures::IsSupported(AVX2));
  Emit(
      kX64IGeS | LaneSizeField::encode(kL64) | VectorLengthField::encode(kV256),
      g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
      g.UseRegister(node->InputAt(1)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI64x2Abs(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  if (CpuFeatures::IsSupported(AVX)) {
    Emit(kX64IAbs | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)));
  } else {
    Emit(kX64IAbs | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)));
  }
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitF64x2PromoteLowF32x4(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionCode code = kX64F64x2PromoteLowF32x4;
  Node* input = node->InputAt(0);
  LoadTransformMatcher m(input);

  if (m.Is(LoadTransformation::kS128Load64Zero) && CanCover(node, input)) {
    if (m.ResolvedValue().kind == MemoryAccessKind::kProtected) {
      code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
    }
    // LoadTransforms cannot be eliminated, so they are visited even if
    // unused. Mark it as defined so that we don't visit it.
    MarkAsDefined(input);
    VisitLoad(node, input, code);
    return;
  }

  VisitRR(this, node, code);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI16x8DotI8x16I7x16S(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  Emit(kX64I16x8DotI8x16I7x16S, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)));
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::VisitI32x4DotI8x16I7x16AddS(Node* node) {
  X64OperandGeneratorT<Adapter> g(this);
  InstructionOperand temps[] = {g.TempSimd128Register()};
  Emit(kX64I32x4DotI8x16I7x16AddS, g.DefineSameAsInput(node, 2),
       g.UseUniqueRegister(node->InputAt(0)),
       g.UseUniqueRegister(node->InputAt(1)),
       g.UseUniqueRegister(node->InputAt(2)), arraysize(temps), temps);
}

template <typename Adapter>
void InstructionSelectorT<Adapter>::AddOutputToSelectContinuation(
    OperandGenerator* g, int first_input_index, node_t node) {
  continuation_outputs_.push_back(
      g->DefineSameAsInput(node, first_input_index));
}

// static
MachineOperatorBuilder::Flags
InstructionSelector::SupportedMachineOperatorFlags() {
  MachineOperatorBuilder::Flags flags =
      MachineOperatorBuilder::kWord32ShiftIsSafe |
      MachineOperatorBuilder::kWord32Ctz | MachineOperatorBuilder::kWord64Ctz |
      MachineOperatorBuilder::kWord32Rol | MachineOperatorBuilder::kWord64Rol |
      MachineOperatorBuilder::kWord32Select |
      MachineOperatorBuilder::kWord64Select;
  if (CpuFeatures::IsSupported(POPCNT)) {
    flags |= MachineOperatorBuilder::kWord32Popcnt |
             MachineOperatorBuilder::kWord64Popcnt;
  }
  if (CpuFeatures::IsSupported(SSE4_1)) {
    flags |= MachineOperatorBuilder::kFloat32RoundDown |
             MachineOperatorBuilder::kFloat64RoundDown |
             MachineOperatorBuilder::kFloat32RoundUp |
             MachineOperatorBuilder::kFloat64RoundUp |
             MachineOperatorBuilder::kFloat32RoundTruncate |
             MachineOperatorBuilder::kFloat64RoundTruncate |
             MachineOperatorBuilder::kFloat32RoundTiesEven |
             MachineOperatorBuilder::kFloat64RoundTiesEven;
  }
  return flags;
}

// static
MachineOperatorBuilder::AlignmentRequirements
InstructionSelector::AlignmentRequirements() {
  return MachineOperatorBuilder::AlignmentRequirements::
      FullUnalignedAccessSupport();
}

template class EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE)
    InstructionSelectorT<TurbofanAdapter>;
template class EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE)
    InstructionSelectorT<TurboshaftAdapter>;

}  // namespace compiler
}  // namespace internal
}  // namespace v8
