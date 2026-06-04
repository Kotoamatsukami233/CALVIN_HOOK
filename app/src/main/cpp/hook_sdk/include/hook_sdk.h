#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install an inline hook on the target function.
//   target:    address of the function to hook
//   replace:   address of the replacement function
//   orig_func: [out] pointer to a callable trampoline that executes the original function
// Returns 0 on success, negative on error.
int HookInstall(void *target, void *replace, void **orig_func);

// Remove a previously installed hook and restore original code.
//   target: address of the hooked function
// Returns 0 on success, negative on error.
int HookUninstall(void *target);

#ifdef __cplusplus
}
#endif
