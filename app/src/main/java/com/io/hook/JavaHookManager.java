package com.io.hook;

import java.lang.reflect.Member;
import java.lang.reflect.Method;

/**
 * Low-level Java hook manager. Delegates to {@link HookBridge}.
 * <p>
 * For new code, use {@link HookBridge} directly with the {@link MethodHook} API.
 */
public class JavaHookManager {

    public static Object hook(Object targetMethod, Object hookerObject, Object callbackMethod) {
        return HookBridge.nHook(hookerObject, (Member) targetMethod, (Method) callbackMethod);
    }

    public static boolean unhook(Object targetMethod) {
        return HookBridge.nUnhook((Member) targetMethod);
    }

    public static boolean isInitialized() {
        return HookBridge.isInitialized();
    }
}
