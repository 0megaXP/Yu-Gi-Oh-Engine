package com.ygoengine

import java.io.Closeable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

enum class BotLevel(internal val wire: Int, val label: String) {
    /** Plays from simple priorities, no lookahead. Instant. */
    EASY(0, "Easy"),
    /** Plans each turn with a short search. */
    NORMAL(1, "Normal"),
    /** Plans each turn with a longer search. */
    STRONG(2, "Strong"),
}

/**
 * One duel played step by step. Player 0 goes first; both start with 5 random cards.
 *
 * Get one from [YgoEngine.newDuel]. Calls are serialised; close it before closing the engine.
 * The bot plans with full knowledge of the game state (it can see your hand).
 */
class LiveDuel internal constructor(private var handle: Long) : Closeable {
    private val lock = Mutex()

    suspend fun state(): DuelState = call { DuelParse.state(JSONObject(YgoNative.liveState(it))) }

    /** Answers the current decision with option [index]; returns false if that is not allowed now. */
    suspend fun respond(index: Int): Boolean = call { YgoNative.liveRespond(it, index) }

    /** Lets the bot answer the current decision (for whichever player has to decide). Returns its move. */
    suspend fun botMove(level: BotLevel): String = call { YgoNative.liveBot(it, level.wire) }

    /** Makes a thinking bot decide now with what it has found so far. */
    fun hurryBot() {
        if (handle != 0L) YgoNative.liveCancel(handle)
    }

    override fun close() {
        val h = handle
        handle = 0
        if (h != 0L) {
            YgoNative.liveCancel(h)
            kotlinx.coroutines.runBlocking { lock.withLock { YgoNative.liveDestroy(h) } }
        }
    }

    private suspend fun <T> call(f: (Long) -> T): T = withContext(Dispatchers.Default) {
        lock.withLock {
            check(handle != 0L) { "duel is closed" }
            f(handle)
        }
    }
}

// ------------------------------------------------------------------ state

data class DuelCard(val code: Long, val name: String)

/** A card in a Monster or Spell & Trap Zone. [position] is "atk", "def" or "set". */
data class ZoneCard(
    val code: Long,
    val name: String,
    val position: String,
    val atk: Int,
    val def: Int,
    val materials: Int,
    val isMonster: Boolean,
) {
    val faceDown: Boolean get() = position == "set"
}

data class PlayerState(
    val lp: Int,
    val hand: List<DuelCard>,
    val deckCount: Int,
    val extra: List<DuelCard>,
    val graveyard: List<DuelCard>,
    val banished: List<DuelCard>,
    /** 7 slots: 0–4 Main Monster Zones, 5–6 Extra Monster Zones (shared). Null = empty. */
    val monsters: List<ZoneCard?>,
    /** 8 slots: 0–4 Spell & Trap Zones, 5 Field Zone, 6–7 Pendulum Zones. Null = empty. */
    val spells: List<ZoneCard?>,
)

data class DuelOption(val label: String, val code: Long, val kind: String)

data class DuelDecision(val player: Int, val type: String, val prompt: String, val options: List<DuelOption>)

data class DuelState(
    val turn: Int,
    val turnPlayer: Int,
    val phase: String,
    val ended: Boolean,
    /** 0 or 1, 2 for a draw, -1 while playing. */
    val winner: Int,
    val lastMove: String,
    val players: List<PlayerState>,
    /** Who must decide what now; null when the duel is over. */
    val decision: DuelDecision?,
    /** Log lines since the previous state; "[P0]"/"[P1]" stand for the players. */
    val log: List<String>,
)

internal object DuelParse {
    private fun cards(a: JSONArray?): List<DuelCard> =
        if (a == null) emptyList() else (0 until a.length()).map { a.getJSONObject(it).let { o -> DuelCard(o.optLong("code"), o.optString("name")) } }

    private fun slots(a: JSONArray?): List<ZoneCard?> =
        if (a == null) emptyList() else (0 until a.length()).map { i ->
            if (a.isNull(i)) null else a.getJSONObject(i).let { o ->
                ZoneCard(o.optLong("code"), o.optString("name"), o.optString("pos"), o.optInt("atk"), o.optInt("def"),
                    o.optInt("materials"), o.optBoolean("monster"))
            }
        }

    fun state(o: JSONObject): DuelState {
        if (o.has("error")) throw EngineException(o.getString("error"))
        val players = o.getJSONArray("players").let { a ->
            (0 until a.length()).map { i ->
                val p = a.getJSONObject(i)
                PlayerState(
                    lp = p.optInt("lp"),
                    hand = cards(p.optJSONArray("hand")),
                    deckCount = p.optInt("deck"),
                    extra = cards(p.optJSONArray("extra")),
                    graveyard = cards(p.optJSONArray("grave")),
                    banished = cards(p.optJSONArray("banished")),
                    monsters = slots(p.optJSONArray("monsters")),
                    spells = slots(p.optJSONArray("spells")),
                )
            }
        }
        val decision = o.optJSONObject("decision")?.let { d ->
            val opts = d.getJSONArray("options")
            DuelDecision(
                player = d.optInt("player"),
                type = d.optString("type"),
                prompt = d.optString("prompt"),
                options = (0 until opts.length()).map { i ->
                    opts.getJSONObject(i).let { DuelOption(it.optString("label"), it.optLong("code"), it.optString("kind")) }
                },
            )
        }
        val log = o.optJSONArray("log")?.let { a -> (0 until a.length()).map { a.getString(it) } } ?: emptyList()
        return DuelState(
            turn = o.optInt("turn"),
            turnPlayer = o.optInt("turn_player"),
            phase = o.optString("phase"),
            ended = o.optBoolean("ended"),
            winner = o.optInt("winner", -1),
            lastMove = o.optString("last_move"),
            players = players,
            decision = decision,
            log = log,
        )
    }
}
