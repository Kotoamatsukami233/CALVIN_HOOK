#pragma once

#include <cstdint>
#include "assembler.h"

namespace arm64 {

struct RelocateResult {
    CodeBuffer buffer;
    uint32_t bytes_consumed;  // how many bytes of original code were consumed
    bool valid;
};

// Relocate ARM64 instructions from src to a new location.
//   src_addr:     runtime address of the original instructions
//   min_bytes:    minimum number of bytes to consume (must be multiple of 4)
//   dst_addr:     runtime address where relocated code will be placed (0 if unknown)
//
// Returns the relocated code buffer and how many bytes were consumed.
// The buffer ends with a branch back to src_addr + bytes_consumed.
RelocateResult RelocateInstructions(uintptr_t src_addr, uint32_t min_bytes, uintptr_t dst_addr);

}  // namespace arm64
