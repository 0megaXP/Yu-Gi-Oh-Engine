package com.ygoengine

/** Raw JNI bindings (engine/jni/ygoe_jni.cpp). Use [YgoEngine] instead. */
internal object YgoNative {
    init {
        System.loadLibrary("ygoe_jni")
    }

    @JvmStatic external fun create(dataDir: String, scriptsDir: String): Long
    @JvmStatic external fun error(handle: Long): String
    @JvmStatic external fun destroy(handle: Long)
    @JvmStatic external fun analyze(handle: Long, request: String): String
    @JvmStatic external fun profile(handle: Long, request: String): String
    @JvmStatic external fun side(handle: Long, request: String): String
    @JvmStatic external fun cancel(handle: Long)
    /** "phase\tdone\ttotal" */
    @JvmStatic external fun progress(handle: Long): String
}
