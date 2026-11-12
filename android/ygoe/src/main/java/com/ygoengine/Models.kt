package com.ygoengine

import org.json.JSONArray
import org.json.JSONObject

// ------------------------------------------------------------------ requests

/** A decklist by official passcodes (the ids YGOPRODeck uses). */
data class EngineDeck(
    val name: String,
    val main: List<Long>,
    val extra: List<Long>,
    val side: List<Long> = emptyList(),
) {
    internal fun toJson() = JSONObject()
        .put("name", name)
        .put("main", JSONArray(main))
        .put("extra", JSONArray(extra))
        .put("side", JSONArray(side))
}

enum class Knowledge(internal val wire: String) {
    /** The engine sees both hands (training, proxy hands). */
    PRACTICE("practice"),
    /** The engine knows only your hand and the public board; the opponent's hand is sampled. */
    REAL("real"),
}

enum class Zone(internal val wire: String) { MONSTER("mzone"), SPELL_TRAP("szone") }
enum class Position(internal val wire: String) { ATTACK("atk"), DEFENSE("def"), SET("set"), FACE_UP("up") }

/** A card already on the field. [seq] is the zone index 0–4 (5 = Field Zone for SPELL_TRAP). */
data class FieldCard(val code: Long, val zone: Zone, val seq: Int, val position: Position? = null)

/** Everything about one side of the position. A null [hand] means unknown: dealt from the deck. */
data class SideState(
    val deck: EngineDeck,
    val hand: List<Long>? = null,
    val handSize: Int = 5,
    val lp: Int = 8000,
    val field: List<FieldCard> = emptyList(),
    val graveyard: List<Long> = emptyList(),
    val banished: List<Long> = emptyList(),
)

data class SearchSettings(
    /** Search iterations for your turn. More finds longer combos; ~200–400 per second per core. */
    val iterations: Int = 2000,
    val maxSeconds: Double = 45.0,
    /** 0 = all cores. */
    val threads: Int = 0,
    /** Hidden-information samples (real mode / proxy hands). */
    val samples: Int = 12,
    val lines: Int = 3,
    /** Also play the opponent's next turn against each candidate board. */
    val verify: Boolean = true,
    val verifyIterations: Int = 300,
    /** Turns to search; 0 = through the end of your next turn. */
    val horizonTurns: Int = 0,
)

/**
 * A position to analyse. Side 0 always takes turn 1; to analyse a position where the
 * opponent is the turn player, put them in [first] and set [me] = 1.
 */
data class AnalysisRequest(
    val first: SideState,
    val second: SideState,
    val me: Int = 0,
    val knowledge: Knowledge = Knowledge.PRACTICE,
    val seed: Long = 1,
    val settings: SearchSettings = SearchSettings(),
    /** Your deck's combo pieces ([ReferenceResult.libraryJson]): the search tries plans chained from them first. */
    val library: String? = null,
) {
    internal fun toJson(): JSONObject {
        val sides = listOf(first, second)
        fun perSide(f: (SideState) -> Any?) = JSONArray(sides.map { f(it) ?: JSONObject.NULL })
        return JSONObject()
            .put("library", library?.let { JSONArray(it) } ?: JSONObject.NULL)
            .put("decks", perSide { it.deck.toJson() })
            .put("me", me)
            .put("mode", knowledge.wire)
            .put("seed", seed)
            .put("hands", perSide { s -> s.hand?.let { JSONArray(it) } })
            .put("hand_sizes", perSide { it.handSize })
            .put("lp", perSide { it.lp })
            .put("field", perSide { s ->
                JSONArray(s.field.map { c ->
                    JSONObject().put("code", c.code).put("zone", c.zone.wire).put("seq", c.seq)
                        .put("pos", c.position?.wire ?: "")
                })
            })
            .put("grave", perSide { JSONArray(it.graveyard) })
            .put("banished", perSide { JSONArray(it.banished) })
            .put("config", JSONObject()
                .put("iterations", settings.iterations)
                .put("seconds", settings.maxSeconds)
                .put("threads", settings.threads)
                .put("samples", settings.samples)
                .put("lines", settings.lines)
                .put("verify", settings.verify)
                .put("verify_iterations", settings.verifyIterations)
                .put("horizon", settings.horizonTurns))
    }
}

// ------------------------------------------------------------------ results

data class Progress(val phase: String, val done: Int, val total: Int) {
    val fraction: Float get() = if (total > 0) (done.toFloat() / total).coerceIn(0f, 1f) else 0f
    val running: Boolean get() = phase != "idle" && phase.isNotEmpty()
}

/** A card in a report. [code] is the official passcode, usable for card images. */
data class EngineCard(val code: Long, val name: String, val where: String, val note: String)

data class Line(
    val firstMove: String,
    /** Your steps in order; the opponent's responses are prefixed "Opp: ". */
    val moves: List<String>,
    /** 0..1: static value of the end board against this opponent. */
    val ownTurnScore: Double,
    /** 0..1 after the opponent's strongest turn found, or null if not checked. */
    val afterOpponent: Double?,
    /** Ranking score. */
    val combined: Double,
    /** Deck-neutral value of the end board; compare with [ReferenceResult.percentile]. */
    val boardValue: Double = 0.0,
    val visits: Int,
    val endBoard: List<EngineCard>,
    /** What the end board can stop, valued against this opponent. */
    val interrupts: List<EngineCard>,
    /** The opponent's strongest turn found; your responses are prefixed "You: ". */
    val opponentReply: List<String>,
    val boardAfter: List<EngineCard>,
)

data class DeckProfile(
    val name: String,
    val monsterEffects: Double,
    val handEffects: Double,
    val gyEffects: Double,
    val spellTrap: Double,
    val searches: Double,
    val specialSummons: Double,
    val summary: String,
    val keyCards: List<EngineCard>,
    val interrupts: List<DeckInterrupt>,
)

data class DeckInterrupt(val card: EngineCard, val copies: Int, val what: String, val from: String, val baseValue: Double, val engine: Boolean)

data class AnalysisResult(
    val me: Int,
    val knowledge: Knowledge,
    /** 0..1 for you, before any move. */
    val currentScore: Double,
    val currentInterrupts: List<EngineCard>,
    val myProfile: DeckProfile,
    val opponentProfile: DeckProfile,
    val lines: List<Line>,
    val iterations: Int,
    val seconds: Double,
    /** True when [YgoEngine.cancel] stopped the search; results so far are still included. */
    val cancelled: Boolean,
    /** Cards in either deck the rules engine can't play yet (no script); they do nothing in the analysis. */
    val missingCards: List<Long> = emptyList(),
)

data class SideCard(val card: EngineCard, val count: Int, val value: Double, val why: String)
data class SidePlan(val sideIn: List<SideCard>, val sideOut: List<SideCard>)
data class BoardShape(val samples: Int, val monsters: Double, val spellTrapFaceUp: Double, val spellTrapSet: Double, val handTraps: Double)
data class SideResult(
    val opponent: DeckProfile,
    val boardShape: BoardShape,
    val goingFirst: SidePlan,
    val goingSecond: SidePlan,
    val cancelled: Boolean,
    val missingCards: List<Long> = emptyList(),
)

/** One opening hand from the reference run, with the best board the engine built from it. */
data class ReferenceSample(
    val hand: List<Long>,
    /** Deck-neutral value of the position after the turn (field, set cards, cards kept in hand). */
    val value: Double,
    /** Interruptions the board holds. */
    val interrupts: Int,
    val board: List<EngineCard>,
    val moves: List<String>,
    /** The line split into sub-combos (empty for references built before pieces existed). */
    val pieces: List<ComboPiece> = emptyList(),
)

data class CardShare(val code: Long, val name: String, val share: Double)

/** A card a combo piece needs or gives; [where] is "hand", "field", "gy" or "banished" ("" for starting-hand cards). */
data class PieceCard(val code: Long, val name: String, val where: String)

/** A hand trap that can stop a piece, and the first of the piece's steps it can answer (0-based). */
data class PieceStop(val code: Long, val name: String, val step: Int)

/**
 * A sub-combo: a stretch of a line that turns what it [needsHand] (starting-hand cards), what earlier
 * pieces left ([needsEarlier]) and maybe the Normal Summon into what later pieces use ([gives]) and
 * what stays at the end ([leaves]).
 */
data class ComboPiece(
    val first: Int,
    val last: Int,
    /** Needs nothing from earlier pieces: it can start a turn. */
    val starter: Boolean,
    val steps: List<String>,
    val needsHand: List<PieceCard>,
    val needsEarlier: List<PieceCard>,
    val normalSummon: Boolean,
    val gives: List<PieceCard>,
    val leaves: List<PieceCard>,
    /** Earliest first. */
    val stoppedBy: List<PieceStop>,
)

/** A piece found in several reference lines. */
data class LibraryPiece(
    val piece: ComboPiece,
    /** Lines it appeared in, and their share. */
    val count: Int,
    val share: Double,
    /** Average deck-neutral end value of those lines. */
    val value: Double,
)

/** What a deck usually builds going first, found by the engine on random hands (no opponent interaction). */
data class ReferenceResult(
    val deck: String,
    val iterations: Int,
    /** Best board first. */
    val samples: List<ReferenceSample>,
    /** Cards on the field at the end, by share of boards. */
    val commonCards: List<CardShare>,
    /** Sub-combos across the samples: starters first, then most common. */
    val library: List<LibraryPiece> = emptyList(),
    val cancelled: Boolean,
    val missingCards: List<Long>,
    /** The raw result, to save and load again with [ReferenceResult.fromJson]. */
    val json: String,
) {
    val median: Double get() = samples.map { it.value }.sorted().let { if (it.isEmpty()) 0.0 else it[it.size / 2] }
    val best: Double get() = samples.maxOfOrNull { it.value } ?: 0.0

    /** The raw piece library, to pass to analyze / comboTree / newDuel; null if this reference has none. */
    val libraryJson: String? get() = runCatching { JSONObject(json).optJSONArray("library") }.getOrNull()
        ?.takeIf { it.length() > 0 }?.toString()

    /** Share (0..1) of reference boards that [value] beats or equals. */
    fun percentile(value: Double): Double =
        if (samples.isEmpty()) 0.0 else samples.count { it.value <= value + 1e-9 }.toDouble() / samples.size

    companion object {
        fun fromJson(text: String): ReferenceResult = Parse.reference(text)
    }
}

/** What happens when the opponent uses [handTrap] at the worst moment for the main line. */
data class ComboBranch(
    val handTrap: DuelCard,
    /** It has no point in the main line where it can be used. */
    val stopsNothing: Boolean,
    /** Your action it answers (the step just before it was used). */
    val usedOn: String,
    /** How many places in the main line it could have been used. */
    val chokePoints: Int,
    /** Index in [ComboTree.mainParts] of the part it interrupts, -1 if none. */
    val hitsPart: Int = -1,
    /** Your best continuation from the interruption to the end of the turn. */
    val continuation: List<String>,
    val board: List<EngineCard>,
    /** Deck-neutral board value (as reference boards). */
    val value: Double,
    /** 0..1, same scale as the main line's score. */
    val score: Double,
)

data class ComboTree(
    val mainMoves: List<String>,
    val mainBoard: List<EngineCard>,
    val mainValue: Double,
    val mainScore: Double,
    /** The main line split into sub-combos. */
    val mainParts: List<ComboPiece> = emptyList(),
    /** False when no opponent deck was given and the most played hand traps were used. */
    val opponentKnown: Boolean,
    /** Most damaging first; hand traps that stop nothing last. */
    val branches: List<ComboBranch>,
    val cancelled: Boolean,
    val missingCards: List<Long>,
)

class EngineException(message: String) : Exception(message)

// ------------------------------------------------------------------ parsing

internal object Parse {
    fun checked(text: String): JSONObject {
        val o = JSONObject(text)
        if (o.has("error")) throw EngineException(o.getString("error"))
        return o
    }

    private fun <T> JSONArray?.map(f: (JSONObject) -> T): List<T> =
        if (this == null) emptyList() else (0 until length()).map { f(getJSONObject(it)) }

    private fun JSONArray?.strings(): List<String> =
        if (this == null) emptyList() else (0 until length()).map { getString(it) }

    private fun JSONArray?.longs(): List<Long> =
        if (this == null) emptyList() else (0 until length()).map { getLong(it) }

    fun card(o: JSONObject) = EngineCard(
        code = o.optLong("code"),
        name = o.optString("name"),
        where = o.optString("where"),
        note = o.optString("note"),
    )

    fun profile(o: JSONObject) = DeckProfile(
        name = o.optString("name"),
        monsterEffects = o.optDouble("monster_effects", 0.0),
        handEffects = o.optDouble("hand_effects", 0.0),
        gyEffects = o.optDouble("gy_effects", 0.0),
        spellTrap = o.optDouble("spell_trap", 0.0),
        searches = o.optDouble("searches", 0.0),
        specialSummons = o.optDouble("special_summons", 0.0),
        summary = o.optString("summary"),
        keyCards = o.optJSONArray("key_cards").map(::card),
        interrupts = o.optJSONArray("interrupts").map {
            DeckInterrupt(card(it.getJSONObject("card")), it.optInt("copies"), it.optString("what"),
                it.optString("from"), it.optDouble("base_value", 0.0), it.optBoolean("engine"))
        },
    )

    fun analysis(o: JSONObject) = AnalysisResult(
        me = o.optInt("me"),
        knowledge = if (o.optString("mode") == "real") Knowledge.REAL else Knowledge.PRACTICE,
        currentScore = o.optDouble("current_score", 0.5),
        currentInterrupts = o.optJSONArray("current_interrupts").map(::card),
        myProfile = profile(o.getJSONObject("my_profile")),
        opponentProfile = profile(o.getJSONObject("opponent_profile")),
        lines = o.optJSONArray("lines").map { l ->
            val after = l.optDouble("after_opponent", -1.0)
            Line(
                firstMove = l.optString("first_move"),
                moves = l.optJSONArray("moves").strings(),
                ownTurnScore = l.optDouble("own_turn_score", 0.0),
                afterOpponent = if (after < 0) null else after,
                combined = l.optDouble("combined", 0.0),
                boardValue = l.optDouble("board_value", 0.0),
                visits = l.optInt("visits"),
                endBoard = l.optJSONArray("end_board").map(::card),
                interrupts = l.optJSONArray("interrupts").map(::card),
                opponentReply = l.optJSONArray("opponent_reply").strings(),
                boardAfter = l.optJSONArray("board_after").map(::card),
            )
        },
        iterations = o.optInt("iterations"),
        seconds = o.optDouble("seconds", 0.0),
        cancelled = o.optBoolean("cancelled"),
        missingCards = o.optJSONArray("missing_cards").longs(),
    )

    fun piece(o: JSONObject): ComboPiece {
        fun cards(a: JSONArray?) = a.map { PieceCard(it.optLong("code"), it.optString("name"), it.optString("where")) }
        return ComboPiece(
            first = o.optInt("first"),
            last = o.optInt("last"),
            starter = o.optBoolean("starter"),
            steps = o.optJSONArray("steps").strings(),
            needsHand = cards(o.optJSONArray("needs_hand")),
            needsEarlier = cards(o.optJSONArray("needs_earlier")),
            normalSummon = o.optBoolean("normal_summon"),
            gives = cards(o.optJSONArray("gives")),
            leaves = cards(o.optJSONArray("leaves")),
            stoppedBy = o.optJSONArray("stopped_by").map { PieceStop(it.optLong("code"), it.optString("name"), it.optInt("step")) },
        )
    }

    fun comboTree(text: String): ComboTree {
        val o = checked(text)
        val main = o.getJSONObject("main")
        return ComboTree(
            mainMoves = main.optJSONArray("moves").strings(),
            mainBoard = main.optJSONArray("board").map(::card),
            mainValue = main.optDouble("value", 0.0),
            mainScore = main.optDouble("score", 0.0),
            mainParts = main.optJSONArray("parts").map(::piece),
            opponentKnown = o.optBoolean("opponent_known"),
            branches = o.optJSONArray("branches").map { b ->
                val ht = b.getJSONObject("hand_trap")
                ComboBranch(
                    handTrap = DuelCard(ht.optLong("code"), ht.optString("name")),
                    stopsNothing = b.optBoolean("stops_nothing"),
                    usedOn = b.optString("used_on"),
                    chokePoints = b.optInt("choke_points"),
                    hitsPart = b.optInt("hits_part", -1),
                    continuation = b.optJSONArray("continuation").strings(),
                    board = b.optJSONArray("board").map(::card),
                    value = b.optDouble("value", 0.0),
                    score = b.optDouble("score", 0.0),
                )
            },
            cancelled = o.optBoolean("cancelled"),
            missingCards = o.optJSONArray("missing_cards").longs(),
        )
    }

    fun reference(text: String): ReferenceResult {
        val o = checked(text)
        return ReferenceResult(
            deck = o.optString("deck"),
            iterations = o.optInt("iterations"),
            samples = o.optJSONArray("samples").map { s ->
                ReferenceSample(
                    hand = s.optJSONArray("hand").longs(),
                    value = s.optDouble("value", 0.0),
                    interrupts = s.optInt("interrupts"),
                    board = s.optJSONArray("board").map(::card),
                    moves = s.optJSONArray("moves").strings(),
                    pieces = s.optJSONArray("pieces").map(::piece),
                )
            },
            library = o.optJSONArray("library").map {
                LibraryPiece(piece(it), it.optInt("count"), it.optDouble("share", 0.0), it.optDouble("value", 0.0))
            },
            commonCards = o.optJSONArray("common_cards").map { CardShare(it.optLong("code"), it.optString("name"), it.optDouble("share", 0.0)) },
            cancelled = o.optBoolean("cancelled"),
            missingCards = o.optJSONArray("missing_cards").longs(),
            json = text,
        )
    }

    private fun plan(o: JSONObject): SidePlan {
        fun list(a: JSONArray?) = a.map {
            SideCard(card(it.getJSONObject("card")), it.optInt("count"), it.optDouble("value", 0.0), it.optString("why"))
        }
        return SidePlan(list(o.optJSONArray("side_in")), list(o.optJSONArray("side_out")))
    }

    fun side(o: JSONObject): SideResult {
        val s = o.getJSONObject("board_shape")
        return SideResult(
            opponent = profile(o.getJSONObject("opponent")),
            boardShape = BoardShape(s.optInt("samples"), s.optDouble("monsters", 0.0), s.optDouble("st_faceup", 0.0),
                s.optDouble("st_set", 0.0), s.optDouble("hand_traps", 0.0)),
            goingFirst = plan(o.getJSONObject("going_first")),
            goingSecond = plan(o.getJSONObject("going_second")),
            cancelled = o.optBoolean("cancelled"),
            missingCards = o.optJSONArray("missing_cards").longs(),
        )
    }
}
