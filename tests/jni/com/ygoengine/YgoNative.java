package com.ygoengine;

/** Desktop stand-in for the Kotlin object of the same name: same JNI symbols. */
public final class YgoNative {
    static { System.loadLibrary("ygoe_jni"); }
    public static native long create(String dataDir, String scriptsDir);
    public static native String error(long handle);
    public static native void destroy(long handle);
    public static native String analyze(long handle, String request);
    public static native String profile(long handle, String request);
    public static native String side(long handle, String request);
    public static native String reference(long handle, String request);
    public static native void cancel(long handle);
    public static native String progress(long handle);
    public static native long liveCreate(long handle, String request);
    public static native String lastError();
    public static native String liveState(long duel);
    public static native boolean liveRespond(long duel, int index);
    public static native String liveBot(long duel, int level);
    public static native void liveCancel(long duel);
    public static native void liveDestroy(long duel);
}
