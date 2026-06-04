#include "hook_sdk.h"
#include "hook_entry.h"
#include "hook_registry.h"
#include "trampoline.h"
#include "memory_allocator.h"
#include "code_patch.h"
#include "logging.h"
#include "arm64_constants.h"

#include <cstring>
#include <cstdlib>

int HookInstall(void *target, void *replace, void **orig_func) {
    if (!target || !replace || !orig_func) {
        LOGE("HookInstall: null argument");
        return -1;
    }

    uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
    uintptr_t replace_addr = reinterpret_cast<uintptr_t>(replace);

    // 1. Check for duplicate
    if (HookRegistry::Shared().Find(target_addr) != nullptr) {
        LOGE("HookInstall: target %p already hooked", target);
        return -2;
    }

    // 2. Determine jump patch size
    arm64::JumpPatch patch = arm64::BuildJumpPatch(target_addr, replace_addr);
    if (patch.code.size == 0) {
        LOGE("HookInstall: failed to build jump patch");
        return -3;
    }

    LOGI("Jump patch: %u bytes at %p → %p", patch.patch_size, target, replace);

    // 3. Build relocated code for original function
    //    Worst case: each 4-byte instruction expands to ~20 bytes when relocated
    uint32_t max_reloc_size = (patch.patch_size / 4) * 20 + 64;

    // 4. Allocate executable memory near target for relocated code
    NearCodeAllocator &alloc = NearCodeAllocator::Shared();
    void *relocated_mem = alloc.AllocNearCode(max_reloc_size, target_addr,
                                               static_cast<size_t>(arm64::kMaxBRange));
    if (!relocated_mem) {
        LOGE("HookInstall: failed to allocate near code memory");
        return -4;
    }

    uintptr_t relocated_addr = reinterpret_cast<uintptr_t>(relocated_mem);

    // 5. Build relocated instructions
    arm64::CodeBuffer relocated = arm64::BuildRelocatedCode(
        target_addr, patch.patch_size, relocated_addr);
    if (relocated.size == 0) {
        LOGE("HookInstall: failed to build relocated code");
        return -5;
    }

    LOGI("Relocated code: %u bytes at %p", relocated.size, relocated_mem);

    // 6. Write relocated code to allocated executable memory
    alloc.BeginWrite(relocated_mem, relocated.size);
    memcpy(relocated_mem, relocated.data, relocated.size);
    alloc.EndWrite(relocated_mem, relocated.size);

    // 7. Backup original bytes
    HookEntry *entry = new HookEntry();
    entry->target_addr = target_addr;
    entry->replace_addr = replace_addr;
    entry->saved_size = patch.patch_size;
    memcpy(entry->saved_bytes, target, patch.patch_size);
    entry->relocated_mem = relocated_mem;
    entry->relocated_size = relocated.size;
    entry->orig_func = relocated_mem;
    entry->active = true;

    // 8. Patch target function entry — jump to replace function
    int ret = PatchCode(target, patch.code.data, patch.patch_size);
    if (ret != 0) {
        LOGE("HookInstall: PatchCode failed");
        delete entry;
        return -6;
    }

    // 9. Register
    HookRegistry::Shared().Add(entry);

    // 10. Return original function trampoline
    *orig_func = entry->orig_func;

    LOGI("HookInstall: success, target=%p replace=%p orig=%p",
         target, replace, *orig_func);
    return 0;
}

int HookUninstall(void *target) {
    if (!target) return -1;

    uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);

    HookEntry *entry = HookRegistry::Shared().Remove(target_addr);
    if (!entry) {
        LOGE("HookUninstall: target %p not found", target);
        return -2;
    }

    // Restore original bytes
    int ret = PatchCode(target, entry->saved_bytes, entry->saved_size);
    if (ret != 0) {
        LOGE("HookUninstall: failed to restore original code");
        // Re-add to registry since we couldn't uninstall
        entry->active = false;
        HookRegistry::Shared().Add(entry);
        return -3;
    }

    LOGI("HookUninstall: success, target=%p", target);
    delete entry;
    return 0;
}
