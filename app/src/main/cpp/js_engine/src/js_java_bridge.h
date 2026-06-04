#pragma once

#include "quickjs.h"

void js_java_bridge_init(JSContext *ctx, JSValue ns);
void js_java_bridge_cleanup();
