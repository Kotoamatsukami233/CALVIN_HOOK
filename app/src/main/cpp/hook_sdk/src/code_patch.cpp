#include "code_patch.h"
#include "logging.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

static uintptr_t PageAlign(uintptr_t addr) {
    static long page_size = sysconf(_SC_PAGESIZE);
    return addr & ~(static_cast<uintptr_t>(page_size) - 1);
}

static long PageSize() {
    static long page_size = sysconf(_SC_PAGESIZE);
    return page_size;
}

int MakeMemoryRWX(void *addr, size_t size) {
    uintptr_t page = PageAlign(reinterpret_cast<uintptr_t>(addr));
    size_t page_size = static_cast<size_t>(PageSize());
    uintptr_t end = reinterpret_cast<uintptr_t>(addr) + size;
    uintptr_t page_end = (end + page_size - 1) & ~(page_size - 1);
    size_t total = page_end - page;

    if (mprotect(reinterpret_cast<void *>(page), total,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect RWX failed for %p size %zu", reinterpret_cast<void *>(page), total);
        return -1;
    }
    return 0;
}

int MakeMemoryRX(void *addr, size_t size) {
    uintptr_t page = PageAlign(reinterpret_cast<uintptr_t>(addr));
    size_t page_size = static_cast<size_t>(PageSize());
    uintptr_t end = reinterpret_cast<uintptr_t>(addr) + size;
    uintptr_t page_end = (end + page_size - 1) & ~(page_size - 1);
    size_t total = page_end - page;

    if (mprotect(reinterpret_cast<void *>(page), total,
                 PROT_READ | PROT_EXEC) != 0) {
        LOGE("mprotect RX failed for %p size %zu", reinterpret_cast<void *>(page), total);
        return -1;
    }
    return 0;
}

int PatchCode(void *target, const void *patch, size_t size) {
    if (target == nullptr || patch == nullptr || size == 0) return -1;

    // Make target writable
    if (MakeMemoryRWX(target, size) != 0) return -1;

    // Write the patch
    memcpy(target, patch, size);

    // Restore to RX
    MakeMemoryRX(target, size);

    // Flush instruction cache (critical on ARM64)
    __builtin___clear_cache(
        reinterpret_cast<char *>(target),
        reinterpret_cast<char *>(target) + size);

    return 0;
}
