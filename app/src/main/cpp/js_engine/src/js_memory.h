#pragma once

#include "quickjs.h"

// Register Memory C functions on the given namespace object.
void js_memory_init(JSContext *ctx, JSValue ns);
