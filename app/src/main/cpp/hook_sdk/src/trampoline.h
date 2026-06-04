#pragma once

#include <cstdint>
#include "assembler.h"

namespace arm64 {

// Describes the jump patch to write at the target function entry.
struct JumpPatch {
    CodeBuffer code;     // bytes to write at target
    uint32_t patch_size; // how many bytes at target this occupies
};

// Build a jump patch from `from` to `to`.
// Chooses the smallest encoding:
//   - B (4B) if within +/-128MB
//   - ADRP+ADD+BR (12B) if within +/-4GB pages
//   - LDR+BR+literal (20B) otherwise
JumpPatch BuildJumpPatch(uintptr_t from, uintptr_t to);

// Build the full relocated code for the original function.
//   original_addr:      runtime address of the target function
//   bytes_to_overwrite: how many bytes will be overwritten by the jump patch
//   relocated_addr:     where the relocated code will be placed (for future use)
// Returns the code buffer containing relocated instructions + branch back.
CodeBuffer BuildRelocatedCode(uintptr_t original_addr, uint32_t bytes_to_overwrite,
                              uintptr_t relocated_addr);

}  // namespace arm64
