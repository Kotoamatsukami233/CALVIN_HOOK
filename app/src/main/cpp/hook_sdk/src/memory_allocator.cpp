#include "memory_allocator.h"
#include "code_patch.h"
#include "logging.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>

static long GetPageSize() {
    static long ps = sysconf(_SC_PAGESIZE);
    return ps;
}

NearCodeAllocator &NearCodeAllocator::Shared() {
    static NearCodeAllocator instance;
    return instance;
}

void *NearCodeAllocator::AllocPage() {
    void *page = mmap(nullptr, GetPageSize(),
                      PROT_READ | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        LOGE("mmap page failed");
        return nullptr;
    }

    LinearPage lp;
    lp.base = page;
    lp.used = 0;
    lp.capacity = static_cast<uint32_t>(GetPageSize());
    pages_.push_back(lp);
    LOGD("Allocated new page at %p", page);
    return page;
}

void *NearCodeAllocator::AllocFixedPage(uintptr_t addr) {
    // Align to page boundary
    uintptr_t aligned = addr & ~(static_cast<uintptr_t>(GetPageSize()) - 1);
    void *page = mmap(reinterpret_cast<void *>(aligned), GetPageSize(),
                      PROT_READ | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (page == MAP_FAILED) {
        // Try MAP_FIXED as fallback
        page = mmap(reinterpret_cast<void *>(aligned), GetPageSize(),
                    PROT_READ | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (page == MAP_FAILED) {
            LOGE("mmap fixed page at %p failed", reinterpret_cast<void *>(aligned));
            return nullptr;
        }
    }

    LinearPage lp;
    lp.base = page;
    lp.used = 0;
    lp.capacity = static_cast<uint32_t>(GetPageSize());
    pages_.push_back(lp);
    LOGD("Allocated fixed page at %p", page);
    return page;
}

int NearCodeAllocator::BeginWrite(void *addr, size_t size) {
    return MakeMemoryRWX(addr, size);
}

int NearCodeAllocator::EndWrite(void *addr, size_t size) {
    // Flush icache
    __builtin___clear_cache(
        reinterpret_cast<char *>(addr),
        reinterpret_cast<char *>(addr) + size);
    return MakeMemoryRX(addr, size);
}

void *NearCodeAllocator::AllocNearCode(size_t size, uintptr_t near_addr, size_t range) {
    // Round up size to 8-byte alignment
    size = (size + 7) & ~static_cast<size_t>(7);

    // Tier 1: Try existing pages
    void *result = TryAllocFromExisting(size, near_addr, range);
    if (result) return result;

    // Tier 2: Try gaps in /proc/self/maps
    result = TryAllocInGap(size, near_addr, range);
    if (result) return result;

    // Tier 3: Fallback — allocate anywhere
    LOGW("Could not allocate near %p (range %zu), falling back to any address",
         reinterpret_cast<void *>(near_addr), range);
    void *page = AllocPage();
    if (!page) return nullptr;

    // Use the newly allocated page
    pages_.back().used = static_cast<uint32_t>(size);
    return page;
}

void *NearCodeAllocator::TryAllocFromExisting(size_t size, uintptr_t near_addr, size_t range) {
    uintptr_t lo = (near_addr > range) ? (near_addr - range) : 0;
    uintptr_t hi = near_addr + range;

    for (auto &page : pages_) {
        uint32_t remaining = page.capacity - page.used;
        if (remaining < size) continue;

        uintptr_t alloc_addr = reinterpret_cast<uintptr_t>(page.base) + page.used;
        if (alloc_addr >= lo && alloc_addr <= hi) {
            void *result = reinterpret_cast<void *>(alloc_addr);
            page.used += static_cast<uint32_t>(size);
            return result;
        }
    }
    return nullptr;
}

void *NearCodeAllocator::TryAllocInGap(size_t size, uintptr_t near_addr, size_t range) {
    auto regions = ParseMaps();
    if (regions.empty()) return nullptr;

    uintptr_t lo = (near_addr > range) ? (near_addr - range) : 0;
    uintptr_t hi = near_addr + range;
    long page_size = GetPageSize();

    // Sort regions by start address
    std::sort(regions.begin(), regions.end(),
              [](const MemRegion &a, const MemRegion &b) {
                  return a.start < b.start;
              });

    // Check gaps between consecutive regions
    for (size_t i = 0; i + 1 < regions.size(); i++) {
        uintptr_t gap_start = regions[i].end;
        uintptr_t gap_end = regions[i + 1].start;

        if (gap_end <= gap_start) continue;

        // Align gap start up to page boundary
        uintptr_t candidate = (gap_start + page_size - 1) & ~(static_cast<uintptr_t>(page_size) - 1);
        if (candidate + static_cast<uintptr_t>(page_size) > gap_end) continue;

        // Check if within range
        if (candidate >= lo && candidate <= hi) {
            void *page = AllocFixedPage(candidate);
            if (page) {
                pages_.back().used = static_cast<uint32_t>(size);
                return page;
            }
        }
    }

    // Also check before the first region and after the last
    if (!regions.empty()) {
        // Before first region
        uintptr_t before = (lo + page_size - 1) & ~(static_cast<uintptr_t>(page_size) - 1);
        if (before < regions[0].start && before >= lo) {
            void *page = AllocFixedPage(before);
            if (page) {
                pages_.back().used = static_cast<uint32_t>(size);
                return page;
            }
        }
    }

    return nullptr;
}

std::vector<MemRegion> NearCodeAllocator::ParseMaps() {
    std::vector<MemRegion> regions;
    std::ifstream maps("/proc/self/maps");
    std::string line;

    while (std::getline(maps, line)) {
        MemRegion r{};
        char perms[5] = {};

        // Format: start-end perms offset dev inode pathname
        unsigned long start = 0, end = 0;
        int parsed = sscanf(line.c_str(), "%lx-%lx %4s",
                           &start, &end, perms);
        r.start = static_cast<uintptr_t>(start);
        r.end = static_cast<uintptr_t>(end);
        if (parsed != 3) continue;

        r.readable = (perms[0] == 'r');
        r.writable = (perms[1] == 'w');
        r.executable = (perms[2] == 'x');

        regions.push_back(r);
    }

    return regions;
}
