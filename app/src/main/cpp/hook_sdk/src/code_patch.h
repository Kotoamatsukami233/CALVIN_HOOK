#pragma once

#include <cstdint>
#include <cstddef>

// Patch `size` bytes at `target` with the contents of `patch`.
// Handles page alignment, mprotect, memcpy, and icache flush.
// Returns 0 on success, -1 on failure.
int PatchCode(void *target, const void *patch, size_t size);

// Make memory at [addr, addr+size) readable and executable.
// Returns 0 on success, -1 on failure.
int MakeMemoryRX(void *addr, size_t size);

// Make memory at [addr, addr+size) readable, writable, executable.
// Returns 0 on success, -1 on failure.
int MakeMemoryRWX(void *addr, size_t size);
