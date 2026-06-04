#pragma once

#include "quickjs.h"

// Register NativeFunction and NativeCallback on the given namespace.
void js_native_init(JSContext *ctx, JSValue ns);
