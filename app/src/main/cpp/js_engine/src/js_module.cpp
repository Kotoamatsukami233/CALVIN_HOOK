#include "js_module.h"
#include "js_ptr.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>

static JSValue js_find_export_by_name(JSContext *ctx, JSValueConst,
                                       int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;

    const char *module_name = nullptr;
    if (!JS_IsUndefined(argv[0])) {
        module_name = JS_ToCString(ctx, argv[0]);
        if (!module_name) return JS_EXCEPTION;
    }

    const char *symbol_name = JS_ToCString(ctx, argv[1]);
    if (!symbol_name) {
        if (module_name) JS_FreeCString(ctx, module_name);
        return JS_EXCEPTION;
    }

    void *handle = nullptr;
    bool opened = false;

    if (module_name && strlen(module_name) > 0) {
        handle = dlopen(module_name, RTLD_LAZY);
        opened = true;
    } else {
        handle = RTLD_DEFAULT;
    }

    void *addr = nullptr;
    if (handle) {
        addr = dlsym(handle, symbol_name);
    }

    if (opened && handle) {
        dlclose(handle);
    }

    JS_FreeCString(ctx, symbol_name);
    if (module_name) JS_FreeCString(ctx, module_name);

    if (!addr) return JS_NULL;
    return js_ptr_new(ctx, addr);
}

static JSValue js_get_base_address(JSContext *ctx, JSValueConst,
                                    int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    const char *module_name = JS_ToCString(ctx, argv[0]);
    if (!module_name) return JS_EXCEPTION;

    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        JS_FreeCString(ctx, module_name);
        return JS_NULL;
    }

    uintptr_t base = 0;
    char line[512];
    size_t name_len = strlen(module_name);

    while (fgets(line, sizeof(line), f)) {
        // Format: start-end perms offset dev inode pathname
        char *path = strchr(line, '/');
        if (!path) {
            // Try [anon:...] or other patterns
            path = strchr(line, '[');
        }
        if (!path) continue;

        // Trim newline
        char *nl = strchr(path, '\n');
        if (nl) *nl = '\0';

        // Check if path ends with /module_name
        size_t path_len = strlen(path);
        if (path_len >= name_len) {
            const char *tail = path + path_len - name_len;
            if (strcmp(tail, module_name) == 0 &&
                (tail == path || *(tail - 1) == '/')) {
                // Parse base address
                uintptr_t start;
                if (sscanf(line, "%lx-", &start) == 1) {
                    base = start;
                    break;
                }
            }
        }
    }

    fclose(f);
    JS_FreeCString(ctx, module_name);

    if (base == 0) return JS_NULL;
    return js_ptr_new(ctx, reinterpret_cast<void *>(base));
}

static const JSCFunctionListEntry module_funcs[] = {
    JS_CFUNC_DEF("findExportByName", 2, js_find_export_by_name),
    JS_CFUNC_DEF("getBaseAddress", 1, js_get_base_address),
};

void js_module_init(JSContext *ctx, JSValue ns) {
    JSValue mod = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, mod, module_funcs,
                               sizeof(module_funcs) / sizeof(module_funcs[0]));
    JS_DefinePropertyValueStr(ctx, ns, "Module", mod, JS_PROP_C_W_E);
}
