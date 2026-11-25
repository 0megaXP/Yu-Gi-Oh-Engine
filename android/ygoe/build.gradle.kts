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
            // Phones, plus the emulator. A build can ask for fewer with -Pygoe.abis=arm64-v8a
            // (the CI release build does: every native ABI is a full compile of the engine).
            val abis = (findProperty("ygoe.abis") as String?)?.split(',')?.map { it.trim() }?.filter { it.isNotEmpty() }
            abiFilters += abis ?: listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DYGOE_BUILD_JNI=ON", "-DYGOE_BUILD_CLI=OFF", "-DANDROID_STL=c++_static")
                // YGOE_CCACHE=1: compile through ccache, so an unchanged engine rebuilds in seconds on CI.
                if (System.getenv("YGOE_CCACHE") != null)
                    arguments += listOf("-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
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
            // Optimised without debug info (the default RelWithDebInfo compiles slower and the
            // debug info is stripped from the app anyway).
            externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" } }
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
    inputs.dir(File(engineRoot, "tools/patches"))
    inputs.file(File(engineRoot, "tools/make_pack.py"))
    inputs.file(File(engineRoot, "data/passcode_map.tsv"))
    // the learned value network (tools/ml), when there is one
    File(engineRoot, "data/value_net.json").takeIf { it.exists() }?.let { inputs.file(it) }
    for (learned in listOf("policy.json", "nonengine_timing.json", "nonengine_values.json", "deck_library.json"))
        File(engineRoot, "data/$learned").takeIf { it.exists() }?.let { inputs.file(it) }
    // the format's decks (format_decks.json: what a deck learns against on a computer)
    File(engineRoot, "decks").takeIf { it.isDirectory }?.let { inputs.dir(it) }
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
