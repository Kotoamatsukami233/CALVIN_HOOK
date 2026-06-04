#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

struct HookEntry {
    uintptr_t target_addr;         // original function address
    uintptr_t replace_addr;        // replacement function address
    void     *orig_func;           // trampoline to call original function
    uint8_t   saved_bytes[32];     // original bytes backed up for uninstall
    uint32_t  saved_size;          // how many bytes were overwritten
    void     *relocated_mem;       // allocated executable memory for relocated code
    uint32_t  relocated_size;      // size of relocated code
    bool      active;

    HookEntry()
        : target_addr(0), replace_addr(0), orig_func(nullptr),
          saved_size(0), relocated_mem(nullptr), relocated_size(0),
          active(false) {
        memset(saved_bytes, 0, sizeof(saved_bytes));
    }
};
