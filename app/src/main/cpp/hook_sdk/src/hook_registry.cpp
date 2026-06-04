#include "hook_registry.h"

HookRegistry &HookRegistry::Shared() {
    static HookRegistry instance;
    return instance;
}

void HookRegistry::Add(HookEntry *entry) {
    entries_.push_back(entry);
}

HookEntry *HookRegistry::Find(uintptr_t target) {
    for (auto *e : entries_) {
        if (e->target_addr == target) return e;
    }
    return nullptr;
}

HookEntry *HookRegistry::Remove(uintptr_t target) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if ((*it)->target_addr == target) {
            HookEntry *entry = *it;
            entries_.erase(it);
            return entry;
        }
    }
    return nullptr;
}
