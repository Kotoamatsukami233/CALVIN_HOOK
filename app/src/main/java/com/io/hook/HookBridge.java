package com.io.hook;

import android.util.Log;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Main API for Java method hooking.
 * Provides Xposed-style before/after hook dispatch with priority support.
 */
public final class HookBridge {

    private static final String TAG = "HB";

    private static final Object[] EMPTY_ARRAY = new Object[0];
    private static final Map<Member, HookEntry> entries = new HashMap<>();
    private static final Method dispatchMethod;

    static {
        try {
            dispatchMethod = HookEntry.class.getMethod("dispatch", Object[].class);
        } catch (NoSuchMethodException e) {
            throw new RuntimeException("Internal error", e);
        }
        System.loadLibrary("hook");
    }

    // --- Native methods (registered via RegisterNatives in JNI_OnLoad) ---

    static native Method nHook(Object entry, Member original, Method callback);

    static native boolean nUnhook(Member target);

    static native boolean nDeoptimize(Member target);

    static native boolean nIsHooked(Member target);

    public static native boolean isInitialized();

    // --- Public API ---

    /**
     * Hook a method or constructor with the specified callback.
     *
     * @param method   The method or constructor to hook.
     * @param callback Callback to execute when the hooked method is called.
     * @return An Unhook handle to remove this hook.
     */
    public static MethodHook.Unhook hookMethod(Member method, MethodHook callback) {
        checkMethod(method);
        if (callback == null) throw new NullPointerException("callback must not be null");

        HookEntry entry;
        synchronized (entries) {
            entry = entries.get(method);
            if (entry == null) {
                entry = new HookEntry(method);
                Method backup = nHook(entry, method, dispatchMethod);
                if (backup == null) throw new IllegalStateException("Failed to hook method");
                entry.backup = backup;
                entries.put(method, entry);
            }
        }

        entry.callbacks.add(callback);
        return callback.new Unhook(method);
    }

    /**
     * Hook all methods with the given name declared in the class.
     */
    public static Set<MethodHook.Unhook> hookAllMethods(Class<?> clazz, String name, MethodHook callback) {
        Set<MethodHook.Unhook> result = new HashSet<>();
        for (Member method : clazz.getDeclaredMethods()) {
            if (method.getName().equals(name)) {
                result.add(hookMethod(method, callback));
            }
        }
        return result;
    }

    /**
     * Hook all constructors of the class.
     */
    public static Set<MethodHook.Unhook> hookAllConstructors(Class<?> clazz, MethodHook callback) {
        Set<MethodHook.Unhook> result = new HashSet<>();
        for (Member ctor : clazz.getDeclaredConstructors()) {
            result.add(hookMethod(ctor, callback));
        }
        return result;
    }

    /**
     * Remove a specific callback from a hooked method.
     * If no callbacks remain, the hook is fully removed.
     */
    public static void unhookMethod(Member method, MethodHook callback) {
        synchronized (entries) {
            HookEntry entry = entries.get(method);
            if (entry != null) {
                entry.callbacks.remove(callback);
                if (entry.callbacks.size() == 0) {
                    entries.remove(method);
                    nUnhook(method);
                }
            }
        }
    }

    /**
     * Call the original (unhooked) method implementation.
     */
    public static Object invokeOriginalMethod(Member method, Object thisObj, Object[] args)
            throws IllegalAccessException, InvocationTargetException, InstantiationException {
        if (args == null) args = EMPTY_ARRAY;

        HookEntry entry = entries.get(method);
        if (entry != null) {
            return invokeMember(entry.backup, thisObj, args);
        }

        checkMethod(method);
        return invokeMember(method, thisObj, args);
    }

    /**
     * Check if a method is currently hooked.
     */
    public static boolean isHooked(Member method) {
        return entries.containsKey(method);
    }

    /**
     * Deoptimize a method to prevent ART inlining.
     */
    public static boolean deoptimizeMethod(Member method) {
        checkMethod(method);
        return nDeoptimize(method);
    }

    // --- Internal ---

    private static void checkMethod(Member method) {
        if (method == null) throw new NullPointerException("method must not be null");
        if (!(method instanceof Method || method instanceof Constructor<?>))
            throw new IllegalArgumentException("method must be a Method or Constructor");
        if (Modifier.isAbstract(method.getModifiers()))
            throw new IllegalArgumentException("method must not be abstract");
    }

    private static Object invokeMember(Member member, Object thisObj, Object[] args)
            throws IllegalAccessException, InvocationTargetException, InstantiationException {
        if (member instanceof Method) {
            var m = (Method) member;
            m.setAccessible(true);
            return m.invoke(thisObj, args);
        } else {
            var c = (Constructor<?>) member;
            c.setAccessible(true);
            return c.newInstance(args);
        }
    }

    // --- Hook Entry: dispatches before/after callbacks ---

    /**
     * Internal entry holding all callbacks for a hooked method.
     * Public so it can be used as the LSPlant context object.
     */
    public static class HookEntry {
        Member backup;
        private final Member method;
        final CopyOnWriteSortedSet<MethodHook> callbacks = new CopyOnWriteSortedSet<>();
        private final boolean isStatic;
        private final Class<?> returnType;

        public HookEntry(Member method) {
            this.method = method;
            this.isStatic = Modifier.isStatic(method.getModifiers());
            if (method instanceof Method) {
                var rt = ((Method) method).getReturnType();
                if (!rt.isPrimitive()) {
                    returnType = rt;
                    return;
                }
            }
            returnType = null;
        }

        /**
         * Called by LSPlant when the hooked method is invoked.
         * Dispatches to all registered before/after callbacks.
         */
        public Object dispatch(Object[] args) throws Throwable {
            var param = new MethodHook.HookParam();
            param.method = method;

            if (isStatic) {
                param.thisObject = null;
                param.args = args;
            } else {
                param.thisObject = args[0];
                param.args = new Object[args.length - 1];
                System.arraycopy(args, 1, param.args, 0, args.length - 1);
            }

            Object[] snapshot = callbacks.getSnapshot();
            MethodHook[] hooks = new MethodHook[snapshot.length];
            for (int i = 0; i < snapshot.length; i++) hooks[i] = (MethodHook) snapshot[i];
            var hookCount = hooks.length;

            if (hookCount == 0) {
                try {
                    return invokeMember(backup, param.thisObject, param.args);
                } catch (InvocationTargetException e) {
                    throw e.getCause();
                }
            }

            // Before hooks (highest priority first)
            int beforeIdx = 0;
            do {
                try {
                    hooks[beforeIdx].before(param);
                } catch (Throwable t) {
                    Log.e(TAG, "before callback error", t);
                    param.setResult(null);
                    param.returnEarly = false;
                    continue;
                }
                if (param.returnEarly) {
                    beforeIdx++;
                    break;
                }
            } while (++beforeIdx < hookCount);

            // Call original if not intercepted
            if (!param.returnEarly) {
                try {
                    param.setResult(invokeMember(backup, param.thisObject, param.args));
                } catch (InvocationTargetException e) {
                    param.setThrowable(e.getCause());
                }
            }

            // After hooks (reverse order, so highest priority runs last = final control)
            int afterIdx = beforeIdx - 1;
            do {
                Object lastResult = param.getResult();
                Throwable lastThrowable = param.getThrowable();

                try {
                    hooks[afterIdx].after(param);
                } catch (Throwable t) {
                    Log.e(TAG, "after callback error", t);
                    if (lastThrowable == null) param.setResult(lastResult);
                    else param.setThrowable(lastThrowable);
                }
            } while (--afterIdx >= 0);

            var result = param.getResultOrThrowable();
            if (returnType != null) result = returnType.cast(result);
            return result;
        }
    }

    // --- Thread-safe sorted set for callbacks ---

    static final class CopyOnWriteSortedSet<E extends Comparable<E>> {
        private transient volatile Object[] elements = EMPTY_ARRAY;

        public int size() {
            return elements.length;
        }

        public synchronized boolean add(E e) {
            for (int i = 0; i < elements.length; i++) {
                if (e.equals(elements[i])) return false;
            }
            Object[] newElements = new Object[elements.length + 1];
            System.arraycopy(elements, 0, newElements, 0, elements.length);
            newElements[elements.length] = e;
            Arrays.sort(newElements);
            elements = newElements;
            return true;
        }

        public synchronized boolean remove(E e) {
            int index = -1;
            for (int i = 0; i < elements.length; i++) {
                if (e.equals(elements[i])) {
                    index = i;
                    break;
                }
            }
            if (index == -1) return false;

            Object[] newElements = new Object[elements.length - 1];
            System.arraycopy(elements, 0, newElements, 0, index);
            System.arraycopy(elements, index + 1, newElements, index, elements.length - index - 1);
            elements = newElements;
            return true;
        }

        public Object[] getSnapshot() {
            return elements;
        }
    }
}
