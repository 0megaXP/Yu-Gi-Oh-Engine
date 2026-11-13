// Desktop (JVM) library: the same Kotlin API as the Android module, for apps on Windows, macOS
// or Linux. It doesn't build the native engine or the asset pack: the app ships those itself
// (ygoe_jni.dll / libygoe_jni.so, and the folder made by tools/make_pack.py), sets the system
// property "ygoe.library" to the library's path, and calls YgoEngine.open(packDir).
plugins {
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(17)
}

sourceSets {
    main {
        kotlin.srcDir("../ygoe/src/shared/kotlin")
    }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    // Android has org.json built in; on the desktop it is a library.
    api("org.json:json:20240303")
}
