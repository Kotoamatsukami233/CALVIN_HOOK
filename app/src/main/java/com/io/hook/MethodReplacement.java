package com.io.hook;

/**
 * Completely replaces the original method with a custom implementation.
 * The original method will not be called.
 */
public abstract class MethodReplacement extends MethodHook {

    public MethodReplacement() {
        super();
    }

    public MethodReplacement(int priority) {
        super(priority);
    }

    @Override
    protected final void before(HookParam param) throws Throwable {
        try {
            Object result = replace(param);
            param.setResult(result);
        } catch (Throwable t) {
            param.setThrowable(t);
        }
    }

    @Override
    protected final void after(HookParam param) throws Throwable {
    }

    protected abstract Object replace(HookParam param) throws Throwable;

    /**
     * Predefined callback that skips the method and returns null.
     */
    public static final MethodReplacement DO_NOTHING = new MethodReplacement(PRIORITY_HIGHEST * 2) {
        @Override
        protected Object replace(HookParam param) throws Throwable {
            return null;
        }
    };

    /**
     * Creates a callback that always returns the specified value.
     */
    public static MethodReplacement returnConstant(final Object value) {
        return returnConstant(PRIORITY_DEFAULT, value);
    }

    public static MethodReplacement returnConstant(int priority, final Object value) {
        return new MethodReplacement(priority) {
            @Override
            protected Object replace(HookParam param) throws Throwable {
                return value;
            }
        };
    }
}
