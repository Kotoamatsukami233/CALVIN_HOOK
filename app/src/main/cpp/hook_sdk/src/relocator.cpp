#include "relocator.h"
#include "arm64_constants.h"
#include "logging.h"

namespace arm64 {

RelocateResult RelocateInstructions(uintptr_t src_addr, uint32_t min_bytes, uintptr_t dst_addr) {
    RelocateResult result;
    result.bytes_consumed = 0;
    result.valid = false;

    if (min_bytes == 0 || (min_bytes % 4) != 0) {
        LOGE("RelocateInstructions: invalid min_bytes %u", min_bytes);
        return result;
    }

    uint32_t *src = reinterpret_cast<uint32_t *>(src_addr);
    Assembler assem;

    uint32_t offset = 0;
    uint32_t target_count = min_bytes / 4;

    // We may need to consume more than min_bytes if the last instruction
    // is a branch that would be split. For safety, process exactly target_count.
    for (uint32_t i = 0; i < target_count; i++) {
        uint32_t inst = src[i];
        uintptr_t pc = src_addr + offset;

        if (IsB(inst)) {
            // B target → LDR X17, =target; BR X17
            int64_t branch_offset = DecodeImm26(inst);
            uintptr_t target = pc + branch_offset;
            assem.EmitLdrX17Branch(target);
            LOGD("Relocate B at %p → %p", reinterpret_cast<void *>(pc),
                 reinterpret_cast<void *>(target));
        }
        else if (IsBL(inst)) {
            // BL target → LDR X17, =target; MOV LR, X17; BLR LR
            int64_t branch_offset = DecodeImm26(inst);
            uintptr_t target = pc + branch_offset;
            assem.EmitLdrLRBranch(target);
            LOGD("Relocate BL at %p → %p", reinterpret_cast<void *>(pc),
                 reinterpret_cast<void *>(target));
        }
        else if (IsBCond(inst)) {
            // B.cond target → B.cond.inverted +3; LDR X17, =target; BR X17
            int64_t branch_offset = DecodeImm19(inst);
            uintptr_t target = pc + branch_offset;
            uint32_t cond = CondFromBCond(inst);
            uint32_t inv_cond = InvertCond(cond);

            // Inverted B.cond, skip 3 instructions (12 bytes ahead)
            uint32_t inv_bcond = kOpBCond | (3 << 5) | inv_cond;
            assem.Buffer().EmitU32(inv_bcond);

            // LDR X17, =target; BR X17
            assem.EmitLdrX17Branch(target);
            LOGD("Relocate B.cond at %p cond=%u → %p", reinterpret_cast<void *>(pc),
                 cond, reinterpret_cast<void *>(target));
        }
        else if (IsCBZ(inst) || IsCBNZ(inst)) {
            // CBZ/CBNZ Rn, target → inverted +3; LDR X17, =target; BR X17
            int64_t branch_offset = DecodeCompImm19(inst);
            uintptr_t target = pc + branch_offset;

            // Invert: CBZ ↔ CBNZ (flip bit 24)
            uint32_t inverted = inst ^ (1u << 24);
            // Set offset to skip 3 instructions (12 bytes), keep Rt
            uint32_t inv_inst = (inverted & ~0x00FFFFE0) | (3 << 5);
            assem.Buffer().EmitU32(inv_inst);

            assem.EmitLdrX17Branch(target);
            LOGD("Relocate CBZ/CBNZ at %p → %p", reinterpret_cast<void *>(pc),
                 reinterpret_cast<void *>(target));
        }
        else if (IsTBZ(inst) || IsTBNZ(inst)) {
            // TBZ/TBNZ Rn, #bit, target → inverted +3; LDR X17, =target; BR X17
            int64_t branch_offset = DecodeImm14(inst);
            uintptr_t target = pc + branch_offset;

            // Invert: TBZ ↔ TBNZ (flip bit 24)
            uint32_t inverted = inst ^ (1u << 24);
            uint32_t inv_inst = (inverted & ~0x0007FFE0) | (3 << 5);
            assem.Buffer().EmitU32(inv_inst);

            assem.EmitLdrX17Branch(target);
            LOGD("Relocate TBZ/TBNZ at %p → %p", reinterpret_cast<void *>(pc),
                 reinterpret_cast<void *>(target));
        }
        else if (IsLdrLiteral(inst)) {
            // LDR Rt, [PC, #offset] → LDR X17, =abs_addr; LDR Rt, [X17]
            int64_t ldr_offset = DecodeLdrLitImm19(inst);
            uintptr_t target = pc + ldr_offset;
            uint32_t rt = GetRt(inst);

            // Check if it's loading an X register (64-bit) or W (32-bit)
            uint32_t opc = Bits(inst, 31, 30);
            if (opc == 1) {
                // LDR Xt → load 8 bytes from abs_addr
                uint32_t ldr_off = assem.EmitLdrX17Placeholder();
                assem.PlaceDataLabel(target, ldr_off);
                assem.EmitLdrReg(rt, kRegX17);
            } else if (opc == 0) {
                // LDR Wt → load 4 bytes
                uint32_t ldr_off = assem.EmitLdrX17Placeholder();
                assem.PlaceDataLabel(target, ldr_off);
                // LDR Wt, [X17]
                uint32_t ldr_w = 0xB9400200 | (kRegX17 << 5) | rt;
                assem.Buffer().EmitU32(ldr_w);
            } else {
                // LDR St/Dt/Qt — just copy verbatim for simplicity
                assem.Buffer().EmitU32(inst);
                LOGW("Relocate LDR literal FP/SIMD at %p: copied verbatim",
                     reinterpret_cast<void *>(pc));
            }
            LOGD("Relocate LDR literal at %p → %p", reinterpret_cast<void *>(pc),
                 reinterpret_cast<void *>(target));
        }
        else if (IsADR(inst)) {
            // ADR Xd, label → LDR X17, =abs_addr; MOV Xd, X17
            int64_t adr_offset = DecodeAdrImm(inst);
            uintptr_t target = pc + adr_offset;
            uint32_t rd = GetRd(inst);

            uint32_t ldr_off = assem.EmitLdrX17Placeholder();
            assem.PlaceDataLabel(target, ldr_off);
            // MOV Xd, X17  →  ORR Xd, XZR, X17
            uint32_t mov_inst = 0xAA0003E0 | (kRegX17 << 16) | rd;
            assem.Buffer().EmitU32(mov_inst);
            LOGD("Relocate ADR at %p → %p", reinterpret_cast<void *>(pc),
                 reinterpret_cast<void *>(target));
        }
        else if (IsADRP(inst)) {
            // ADRP Xd, page → LDR X17, =page_addr; MOV Xd, X17
            int64_t adrp_offset = DecodeAdrpImm(inst);
            uintptr_t page_addr = (pc & ~0xFFFULL) + adrp_offset;
            uint32_t rd = GetRd(inst);

            uint32_t ldr_off = assem.EmitLdrX17Placeholder();
            assem.PlaceDataLabel(page_addr, ldr_off);
            // MOV Xd, X17
            uint32_t mov_inst = 0xAA0003E0 | (kRegX17 << 16) | rd;
            assem.Buffer().EmitU32(mov_inst);
            LOGD("Relocate ADRP at %p → page %p", reinterpret_cast<void *>(pc),
                 reinterpret_cast<void *>(page_addr));
        }
        else {
            // Non-PC-relative instruction: copy verbatim
            assem.Buffer().EmitU32(inst);
        }

        offset += 4;
    }

    // Append branch back to original code after the overwritten region
    uintptr_t return_addr = src_addr + offset;
    assem.EmitLdrX17Branch(return_addr);

    // Finalize: place data labels and patch LDR instructions
    assem.Finalize();

    result.buffer = std::move(assem.Buffer());
    result.bytes_consumed = offset;
    result.valid = true;
    return result;
}

}  // namespace arm64
