package com.io.hook;

/**
 * JS scripting engine bridge.
 * Provides JNI methods to load and execute JavaScript scripts via QuickJS.
 */
public class ScriptEngine {

    public interface MessageListener {
        void onMessage(String msg);
    }

    private static MessageListener listener;

    public static void setMessageListener(MessageListener l) {
        listener = l;
    }

    /**
     * Called from native code when send() is invoked in JS.
     */
    public static void onNativeMessage(String msg) {
        if (listener != null) {
            listener.onMessage(msg);
        }
    }

    /**
     * Load and evaluate a JavaScript script.
     *
     * @param source The JavaScript source code to evaluate.
     * @return The string representation of the result, or an error message.
     */
    public static String loadScript(String source) {
        return nativeLoadScript(source);
    }

    private static native String nativeLoadScript(String source);
}
