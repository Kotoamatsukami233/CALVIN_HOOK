package com.io.hook;

import java.lang.reflect.Member;

/**
 * Abstract base class for method hook callbacks.
 * Override {@link #before} and/or {@link #after} to intercept method calls.
 */
public abstract class MethodHook implements Comparable<MethodHook> {

    public static final int PRIORITY_DEFAULT = 50;
    public static final int PRIORITY_HIGHEST = 10000;
    public static final int PRIORITY_LOWEST = -10000;

    public final int priority;

    public MethodHook() {
        this.priority = PRIORITY_DEFAULT;
    }

    public MethodHook(int priority) {
        this.priority = priority;
    }

    protected void before(HookParam param) throws Throwable {}

    protected void after(HookParam param) throws Throwable {}

    @Override
    public int compareTo(MethodHook other) {
        if (this == other) return 0;
        if (other.priority != this.priority)
            return other.priority - this.priority;
        return Integer.compare(System.identityHashCode(this), System.identityHashCode(other));
    }

    /**
     * Parameter wrapper for hook callbacks.
     */
    public static class HookParam {
        public Member method;
        public Object thisObject;
        public Object[] args;

        private Object result;
        private Throwable throwable;
        boolean returnEarly;

        public Object getResult() {
            return result;
        }

        public void setResult(Object result) {
            this.result = result;
            this.throwable = null;
            this.returnEarly = true;
        }

        public Throwable getThrowable() {
            return throwable;
        }

        public boolean hasThrowable() {
            return throwable != null;
        }

        public void setThrowable(Throwable throwable) {
            this.throwable = throwable;
            this.result = null;
            this.returnEarly = true;
        }

        public Object getResultOrThrowable() throws Throwable {
            if (throwable != null) throw throwable;
            return result;
        }
    }

    /**
     * Handle returned from {@link HookBridge#hookMethod} to remove the hook.
     */
    public class Unhook {
        private final Member hookedMethod;

        Unhook(Member hookedMethod) {
            this.hookedMethod = hookedMethod;
        }

        public Member getHookedMethod() {
            return hookedMethod;
        }

        public MethodHook getCallback() {
            return MethodHook.this;
        }

        public void unhook() {
            HookBridge.unhookMethod(hookedMethod, MethodHook.this);
        }
    }
}
