#pragma once

#include "quickjs.h"

struct NativePointer {
    void *value;
};

// Register the NativePointer class on the given context.
// Returns the class ID.
uint32_t js_ptr_init(JSContext *ctx, JSValue ns);

// Create a new NativePointer JSValue from C.
JSValue js_ptr_new(JSContext *ctx, void *ptr);

// Extract the raw pointer from a JSValue (NativePointer or number/string).
// Returns nullptr and throws JS exception on failure.
void *js_ptr_unwrap(JSContext *ctx, JSValueConst val);

// Get the NativePointer class ID
uint32_t js_ptr_class_id();
