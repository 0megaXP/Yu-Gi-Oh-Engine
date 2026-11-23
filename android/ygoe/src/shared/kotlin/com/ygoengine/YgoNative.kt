package com.ygoengine

/** Raw JNI bindings (engine/jni/ygoe_jni.cpp). Use [YgoEngine] instead. */
internal object YgoNative {
    init {
        // Android finds libygoe_jni.so in the APK. A desktop app sets ygoe.library to the full
        // path of the library it ships (ygoe_jni.dll on Windows) before the engine is first used.
        val path = System.getProperty("ygoe.library")
        if (path != null) System.load(path) else System.loadLibrary("ygoe_jni")
    }

    @JvmStatic external fun create(dataDir: String, scriptsDir: String): Long
    @JvmStatic external fun error(handle: Long): String
    @JvmStatic external fun destroy(handle: Long)
    @JvmStatic external fun analyze(handle: Long, request: String): String
    @JvmStatic external fun profile(handle: Long, request: String): String
    @JvmStatic external fun side(handle: Long, request: String): String
    @JvmStatic external fun reference(handle: Long, request: String): String
    @JvmStatic external fun comboTree(handle: Long, request: String): String
    @JvmStatic external fun videoLines(handle: Long, request: String): String
    @JvmStatic external fun replayPositions(handle: Long, request: String): String
    @JvmStatic external fun followReplay(handle: Long, request: String): String
    @JvmStatic external fun cancel(handle: Long)
    /** "phase\tdone\ttotal" */
    @JvmStatic external fun progress(handle: Long): String

    // Live duels
    @JvmStatic external fun liveCreate(handle: Long, request: String): Long
    /** Where the engine notes an error that ends the app, before it does (see ygoe_jni.cpp). */
    @JvmStatic external fun crashFile(path: String)
    @JvmStatic external fun lastError(): String
    @JvmStatic external fun liveState(duel: Long): String
    @JvmStatic external fun liveRespond(duel: Long, index: Int): Boolean
    @JvmStatic external fun liveBot(duel: Long, level: Int): String
    @JvmStatic external fun liveCancel(duel: Long)
    @JvmStatic external fun liveDestroy(duel: Long)
}
