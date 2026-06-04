#pragma once

#include "quickjs.h"

// Register Module C functions on the given namespace object.
void js_module_init(JSContext *ctx, JSValue ns);
