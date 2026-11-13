// Android library: the native engine (ygopro-core + search) plus a Kotlin API.
//
// The asset pack (card data + all card scripts) is generated at build time by
// tools/bootstrap.sh and tools/make_pack.py, so the build machine needs git and python3.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

// android/ygoe -> repository root (also correct when this module is included from another project).
val engineRoot: File = projectDir.parentFile.parentFile
val packDir = layout.buildDirectory.dir("generated/ygoePack")

android {
    namespace = "com.ygoengine"
    compileSdk = 35

    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")
        ndk {
            // Phones, plus the emulator.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DYGOE_BUILD_JNI=ON", "-DYGOE_BUILD_CLI=OFF", "-DANDROID_STL=c++_static")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("${engineRoot}/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
        debug {
            // The search is far too slow unoptimised; build the native code optimised in debug too.
            externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" } }
        }
    }

    sourceSets["main"].assets.srcDir(packDir)
    // The Kotlin API is shared with the desktop (JVM) module, ygoe-jvm.
    sourceSets["main"].java.srcDir("src/shared/kotlin")

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
}

// Fetch the pinned rules core/scripts/database and build assets/ygoe/*.
val ygoeEnginePack = tasks.register<Exec>("ygoeEnginePack") {
    val out = packDir.get().dir("ygoe").asFile
    workingDir = engineRoot
    inputs.file(File(engineRoot, "tools/bootstrap.sh"))
    inputs.file(File(engineRoot, "tools/make_pack.py"))
    inputs.file(File(engineRoot, "data/passcode_map.tsv"))
    outputs.dir(out)
    commandLine(
        "bash", "-c",
        "tools/bootstrap.sh && python3 tools/make_pack.py third_party/CardScripts data '${out.absolutePath}'",
    )
}

tasks.named("preBuild") { dependsOn(ygoeEnginePack) }
// CMake needs third_party/ in place before it configures.
tasks.matching { it.name.startsWith("configureCMake") || it.name.startsWith("buildCMake") }
    .configureEach { dependsOn(ygoeEnginePack) }
