package com.ygoengine

import java.io.Closeable
import java.io.File
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

/**
 * The Yu-Gi-Oh! engine: best lines for a position, deck profiles and side plans.
 *
 * ```
 * val engine = YgoEngine.open(context)          // once; installs the asset pack on first run
 * val result = engine.analyze(request) { p -> progress = p.fraction }
 * ```
 *
 * Calls suspend on a background thread. Cancelling the calling coroutine stops the native
 * search; [cancel] instead stops it but still returns the results found so far.
 * One analysis runs at a time per engine; a second call waits for the first.
 */
class YgoEngine private constructor(private var handle: Long) : Closeable {

    companion object {
        /**
         * Before an error inside the engine ends the app, the engine appends a line to [path]:
         * the time (seconds), the engine call that was running and the error message.
         */
        fun crashFile(path: String) = YgoNative.crashFile(path)

        /** Loads the engine from a folder holding scripts.pack and the .tsv data files. Blocking. */
        fun open(packDir: File): YgoEngine {
            val h = YgoNative.create(packDir.absolutePath, packDir.absolutePath)
            val err = YgoNative.error(h)
            if (err.isNotEmpty()) {
                YgoNative.destroy(h)
                throw EngineException(err)
            }
            return YgoEngine(h)
        }
    }

    /** Finds your best lines and checks each end board against the opponent's next turn. */
    suspend fun analyze(request: AnalysisRequest, onProgress: ((Progress) -> Unit)? = null): AnalysisResult =
        Parse.analysis(Parse.checked(runNative(onProgress) { YgoNative.analyze(it, request.toJson().toString()) }))

    /** What a deck relies on, its key engine cards and its interruptions. Fast. */
    suspend fun profile(deck: EngineDeck): DeckProfile = withContext(Dispatchers.Default) {
        Parse.profile(Parse.checked(YgoNative.profile(live(), JSONObject().put("deck", deck.toJson()).toString())))
    }

    /**
     * Side-deck plan for [mine] against [theirs], going first and going second.
     * With [simulateTurns] > 0 the engine first plays that many opening turns for the opponent
     * and rates your board breakers against what their boards actually contain (slower).
     */
    suspend fun side(
        mine: EngineDeck,
        theirs: EngineDeck,
        simulateTurns: Int = 0,
        iterationsPerTurn: Int = 400,
        onProgress: ((Progress) -> Unit)? = null,
    ): SideResult {
        val req = JSONObject()
            .put("mine", mine.toJson())
            .put("theirs", theirs.toJson())
            .put("simulate", simulateTurns)
            .put("iterations", iterationsPerTurn)
        return Parse.side(Parse.checked(runNative(onProgress) { YgoNative.side(it, req.toString()) }))
    }

    /**
     * Starts a duel between [first] (player 0, goes first) and [second] (player 1).
     * Close the returned duel before closing the engine.
     */
    suspend fun newDuel(
        first: EngineDeck,
        second: EngineDeck,
        seed: Long = System.nanoTime(),
        /** Combo pieces for each player's bot ([ReferenceResult.libraryJson]), or null. */
        libraries: List<String?> = listOf(null, null),
        /** The player a person controls (0 or 1), or -1 for bot vs bot. Their zone choices are asked. */
        human: Int = -1,
        /** Learned move policy for the bots' searches ([ReferenceResult.policyJson]), or null. */
        policy: String? = null,
        /** Per player, what is known about their deck from past games (the other bot uses it), or null. */
        knowledge: List<DeckKnowledge?> = listOf(null, null),
        /** Per player: replay choices the engine doesn't understand yet, followed by that player's bot. */
        imitate: List<List<Imitation>> = listOf(emptyList(), emptyList()),
        /** Per player: deck-guide advice for that player's bot, or null. */
        priorities: List<CardPriorities?> = listOf(null, null),
    ): LiveDuel =
        withContext(Dispatchers.Default) {
            val req = JSONObject()
                .put("decks", org.json.JSONArray().put(first.toJson()).put(second.toJson()))
                .put("seed", seed)
                .put("human", human)
                .put("libraries", org.json.JSONArray(libraries.map { l -> l?.let { org.json.JSONArray(it) } ?: JSONObject.NULL }))
                .put("policy", policy?.let { JSONObject(it) } ?: JSONObject.NULL)
                .put("knowledge", org.json.JSONArray(knowledge.map { it?.toJson() ?: JSONObject.NULL }))
                .put("imitate", org.json.JSONArray(imitate.map { l -> org.json.JSONArray(l.map { it.toJson() }) }))
                .put("priorities", org.json.JSONArray(priorities.map { it?.takeUnless { p -> p.isEmpty }?.toJson() ?: JSONObject.NULL }))
            val d = YgoNative.liveCreate(live(), req.toString())
            if (d == 0L) throw EngineException(YgoNative.lastError().ifEmpty { "could not start the duel" })
            LiveDuel(d)
        }

    /**
     * Reference end boards for [deck]: the engine plays [hands] random opening hands going first
     * (no opponent interaction) and keeps the best board for each. About [iterations] search
     * iterations per hand; 20 hands × 1500 takes a minute or two on a phone.
     */
    suspend fun reference(
        deck: EngineDeck,
        hands: Int = 20,
        iterations: Int = 1500,
        seed: Long = 1,
        /**
         * The move policy learned so far ([ReferenceResult.policyJson]), or null to start one. The
         * best line of every hand trains it further; the result carries the updated policy.
         */
        policy: String? = null,
        onProgress: ((Progress) -> Unit)? = null,
    ): ReferenceResult {
        val req = JSONObject().put("deck", deck.toJson()).put("hands", hands).put("iterations", iterations).put("seed", seed)
            .put("policy", policy?.let { JSONObject(it) } ?: JSONObject.NULL)
        return Parse.reference(runNative(onProgress) { YgoNative.reference(it, req.toString()) })
    }

    /**
     * Replays combos read from videos ([lines]) from their opening hands, going first with no
     * opponent interaction, and checks each by the real rules. Verified lines come back with
     * their combo pieces, and [VideoLinesResult.libraryJson] holds them merged, ready to pass
     * as `library` like a reference's pieces. A few seconds per line on a phone.
     */
    suspend fun videoLines(
        deck: EngineDeck,
        lines: List<VideoLineInput>,
        seconds: Double = 60.0,
        seed: Long = 1,
        /** The move policy learned so far, or null; verified lines train it (the result has the update). */
        policy: String? = null,
        /** > 0: also explain each line's choices ([VideoLineCheck.choices]), searching each alternative this much (200–300). */
        explain: Int = 0,
        onProgress: ((Progress) -> Unit)? = null,
    ): VideoLinesResult {
        val req = JSONObject().put("deck", deck.toJson()).put("seconds", seconds).put("seed", seed).put("explain", explain)
            .put("policy", policy?.let { JSONObject(it) } ?: JSONObject.NULL)
            .put("lines", JSONArray(lines.map { it.toJson() }))
        return Parse.videoLines(runNative(onProgress) { YgoNative.videoLines(it, req.toString()) })
    }

    /**
     * Combo tree for a going-first [hand]: the main line, then for each hand trap the opponent
     * is likely to hold (from [opponent]'s list, or the most played ones when it is null), where
     * it hurts most and your best continuation from there.
     */
    suspend fun comboTree(
        deck: EngineDeck,
        hand: List<Long>,
        opponent: EngineDeck? = null,
        iterations: Int = 1500,
        branchIterations: Int = 300,
        seed: Long = 1,
        /** Your deck's combo pieces ([ReferenceResult.libraryJson]), or null. */
        library: String? = null,
        /** Learned move policy ([ReferenceResult.policyJson]), or null. */
        policy: String? = null,
        /** Replay choices the engine doesn't understand yet, followed by the search. */
        imitate: List<Imitation> = emptyList(),
        onProgress: ((Progress) -> Unit)? = null,
    ): ComboTree {
        val req = JSONObject()
            .put("deck", deck.toJson())
            .put("hand", org.json.JSONArray(hand))
            .put("opponent", opponent?.toJson() ?: JSONObject.NULL)
            .put("iterations", iterations)
            .put("branch_iterations", branchIterations)
            .put("seed", seed)
            .put("library", library?.let { org.json.JSONArray(it) } ?: JSONObject.NULL)
            .put("imitate", org.json.JSONArray(imitate.map { it.toJson() }))
            .put("policy", policy?.let { JSONObject(it) } ?: JSONObject.NULL)
        return Parse.comboTree(runNative(onProgress) { YgoNative.comboTree(it, req.toString()) })
    }

    /** Stops the running search early; the call returns what it found, with cancelled = true. */
    fun cancel() {
        if (handle != 0L) YgoNative.cancel(handle)
    }

    fun progress(): Progress {
        val parts = YgoNative.progress(live()).split('\t')
        return Progress(parts.getOrElse(0) { "" }, parts.getOrNull(1)?.toIntOrNull() ?: 0, parts.getOrNull(2)?.toIntOrNull() ?: 0)
    }

    /** Frees the native engine. A running job is cancelled first (this call then waits for it). */
    override fun close() {
        val h = handle
        handle = 0
        if (h != 0L) YgoNative.destroy(h)
    }

    private fun live(): Long {
        check(handle != 0L) { "engine is closed" }
        return handle
    }

    // Runs a blocking native call off the caller's thread, reporting progress on the caller's
    // dispatcher, and turns coroutine cancellation into a native cancel.
    private suspend fun runNative(onProgress: ((Progress) -> Unit)?, call: (Long) -> String): String = coroutineScope {
        val h = live()
        val work = async(Dispatchers.Default) { call(h) }
        val poller = onProgress?.let { report ->
            launch {
                while (work.isActive) {
                    report(progress())
                    delay(250)
                }
            }
        }
        try {
            work.await()
        } catch (e: CancellationException) {
            YgoNative.cancel(h)
            throw e
        } finally {
            poller?.cancel()
        }
    }
}
