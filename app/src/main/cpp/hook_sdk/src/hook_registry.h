#pragma once

#include "hook_entry.h"
#include <vector>

class HookRegistry {
public:
    static HookRegistry &Shared();

    void Add(HookEntry *entry);
    HookEntry *Find(uintptr_t target);
    HookEntry *Remove(uintptr_t target);

private:
    HookRegistry() = default;
    std::vector<HookEntry *> entries_;
};
