#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

struct MemRegion {
    uintptr_t start;
    uintptr_t end;
    bool readable;
    bool writable;
    bool executable;
};

// Allocates executable memory near a target address.
// Uses a 3-tier search strategy:
//   1. Scan existing allocated pages for free space within range
//   2. Parse /proc/self/maps for gaps between mappings, mmap in gap
//   3. Scan existing executable regions for zero-byte padding (code caves)
class NearCodeAllocator {
public:
    static NearCodeAllocator &Shared();

    // Allocate `size` bytes of executable memory within `range` bytes of `near_addr`.
    // Returns nullptr on failure.
    void *AllocNearCode(size_t size, uintptr_t near_addr, size_t range);

    // Allocate an executable page at a fixed address (for gap allocation).
    void *AllocFixedPage(uintptr_t addr);

    // Allocate an executable page at any address.
    void *AllocPage();

    // Temporarily make an allocated region writable for writing code.
    // Returns 0 on success.
    int BeginWrite(void *addr, size_t size);

    // Make the region executable again after writing.
    // Returns 0 on success.
    int EndWrite(void *addr, size_t size);

private:
    NearCodeAllocator() = default;

    struct LinearPage {
        void *base;
        uint32_t used;
        uint32_t capacity;
    };

    std::vector<LinearPage> pages_;

    // Parse /proc/self/maps into a list of memory regions.
    std::vector<MemRegion> ParseMaps();

    // Try to allocate from existing pages within range.
    void *TryAllocFromExisting(size_t size, uintptr_t near_addr, size_t range);

    // Try to allocate in a gap between mappings.
    void *TryAllocInGap(size_t size, uintptr_t near_addr, size_t range);
};
