#include "trampoline.h"
#include "relocator.h"
#include "arm64_constants.h"
#include "logging.h"

#include <cstdlib>

namespace arm64 {

JumpPatch BuildJumpPatch(uintptr_t from, uintptr_t to) {
    JumpPatch patch;
    int64_t distance = static_cast<int64_t>(to) - static_cast<int64_t>(from);

    // Strategy 1: B instruction (4 bytes, +/-128MB)
    if (distance >= -kMaxBRange && distance < kMaxBRange && (distance % 4) == 0) {
        Assembler assem;
        assem.EmitB(distance);
        assem.Finalize();
        patch.code = std::move(assem.Buffer());
        patch.patch_size = 4;
        LOGD("JumpPatch: using B (4B), distance=%lld", static_cast<long long>(distance));
        return patch;
    }

    // Strategy 2: ADRP+ADD+BR (12 bytes, +/-4GB pages)
    int64_t from_page = static_cast<int64_t>(from) & ~0xFFFULL;
    int64_t to_page = static_cast<int64_t>(to) & ~0xFFFULL;
    int64_t page_dist = to_page - from_page;
    if (page_dist >= -(1LL << 32) && page_dist < (1LL << 32)) {
        Assembler assem;
        int rd = kRegX17;
        assem.EmitAdrp(rd, page_dist);
        uint32_t page_off = static_cast<uint32_t>(to & 0xFFF);
        assem.EmitAddImm(rd, rd, page_off);
        assem.EmitBR(rd);
        assem.Finalize();
        patch.code = std::move(assem.Buffer());
        patch.patch_size = 12;
        LOGD("JumpPatch: using ADRP+ADD+BR (12B)");
        return patch;
    }

    // Strategy 3: LDR X17 + BR X17 + literal (20 bytes, arbitrary distance)
    Assembler assem;
    assem.EmitLdrX17Branch(to);
    assem.Finalize();
    patch.code = std::move(assem.Buffer());
    patch.patch_size = 20;
    LOGD("JumpPatch: using LDR+BR+literal (20B)");
    return patch;
}

CodeBuffer BuildRelocatedCode(uintptr_t original_addr, uint32_t bytes_to_overwrite,
                              uintptr_t relocated_addr) {
    auto result = RelocateInstructions(original_addr, bytes_to_overwrite, relocated_addr);
    if (!result.valid) {
        LOGE("BuildRelocatedCode: RelocateInstructions failed");
        return {};
    }
    return std::move(result.buffer);
}

}  // namespace arm64
