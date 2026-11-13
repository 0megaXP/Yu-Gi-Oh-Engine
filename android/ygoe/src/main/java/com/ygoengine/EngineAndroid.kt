package com.ygoengine

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Installs the bundled pack (first run only) and loads the engine. */
suspend fun YgoEngine.Companion.open(context: Context): YgoEngine = withContext(Dispatchers.IO) {
    open(EnginePack.install(context.applicationContext))
}
