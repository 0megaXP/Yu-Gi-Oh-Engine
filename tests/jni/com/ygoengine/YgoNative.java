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
    public static native void cancel(long handle);
    public static native String progress(long handle);
}
