#pragma once

#include "quickjs.h"

// Register Interceptor C functions on the given namespace object.
void js_interceptor_init(JSContext *ctx, JSValue ns);

// Clean up all interceptor entries (call during engine shutdown).
void js_interceptor_cleanup();
