package com.ygoengine

import android.content.Context
import java.io.File

/**
 * Copies the engine's asset pack (card data + all card scripts, ~40 MB) from the APK
 * into app storage. Runs once per pack version; later calls return immediately.
 */
object EnginePack {
    private const val ASSET_DIR = "ygoe"
    private val FILES = listOf("scripts.pack", "cards.tsv", "effects.tsv", "strings.tsv", "passcode_map.tsv")
    /** The trained value network (tools/ml), when the pack has one. */
    private val OPTIONAL = listOf("value_net.json", "policy.json")

    /** Installs the pack if needed and returns the folder to pass to [YgoEngine.open]. Blocking. */
    fun install(context: Context): File {
        val dir = File(context.filesDir, ASSET_DIR)
        val wanted = context.assets.open("$ASSET_DIR/VERSION").bufferedReader().use { it.readText().trim() }
        val versionFile = File(dir, "VERSION")
        if (versionFile.exists() && versionFile.readText().trim() == wanted) return dir

        dir.mkdirs()
        versionFile.delete()  // marks the folder incomplete until every file is copied
        val bundled = context.assets.list(ASSET_DIR).orEmpty().toSet()
        for (name in FILES + OPTIONAL.filter { it in bundled }) {
            val tmp = File(dir, "$name.tmp")
            context.assets.open("$ASSET_DIR/$name").use { input ->
                tmp.outputStream().use { output -> input.copyTo(output, 1 shl 16) }
            }
            if (!tmp.renameTo(File(dir, name))) error("could not install $name")
        }
        versionFile.writeText(wanted)
        return dir
    }

    /** Version of the pack inside this APK. */
    fun bundledVersion(context: Context): String =
        context.assets.open("$ASSET_DIR/VERSION").bufferedReader().use { it.readText().trim() }
}
