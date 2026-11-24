// Thin, replayable wrapper around one ygopro-core duel.
//
// ocgcore has no way to copy a running duel, so a game state is identified by
// (DuelSetup, list of responses given so far). Duel::replay() rebuilds any state
// deterministically from that pair; the search uses this to branch.
#pragma once
#include <functional>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "carddb.h"

namespace ygoe {

class ScriptCache;

// A card that exists only in the engine's own test duels (reach.cpp's trigger probe): a
// Continuous Spell whose effect moves a chosen card the way a real card's effect or cost would.
constexpr uint32_t kProbeCardCode = 0x7ffffe01;

struct PlacedCard {
	uint32_t code = 0;   // official passcode (remapped to script id internally)
	uint32_t loc = 0;    // LOCATION_*
	uint32_t seq = 0;
	uint32_t pos = 0;    // POS_*; 0 = default for the location
};

struct PlayerSetup {
	std::vector<uint32_t> main;    // remaining Main Deck (order irrelevant unless no_shuffle)
	std::vector<uint32_t> extra;
	std::vector<uint32_t> hand;    // starts in hand
	std::vector<PlacedCard> placed;  // field / GY / banished
	uint32_t lp = 8000;
};

struct DuelSetup {
	std::array<uint64_t, 4> seed{1, 2, 3, 4};
	std::array<PlayerSetup, 2> players;
	// Player 0 always takes turn 1. To analyse a state where the other side is the
	// turn player, put that side's cards in players[0].
	bool no_shuffle = false;    // keep Main Deck order as given (top = last element)
	uint64_t flags = 0;         // 0 = Master Rule 5 (current TCG/OCG)
	// Hidden information for a search (one guess of what the searching player can't see): after
	// `hidden_at` responses of a replay, each entry's hand cards at the indices with a code are
	// swapped with a Main Deck card of that code, and that player's Deck is shuffled.
	struct Hidden {
		uint8_t player = 0;
		std::vector<uint32_t> hand;  // per hand index (Board::hand order): script id, 0 = keep
		uint64_t seed = 0;           // shuffle seed (0: keep the order)
		// A guessed Deck (the other player's list isn't known): the hand cards above and the Main
		// Deck (script ids) are made anew instead of swapped.
		bool guessed = false;
		std::vector<uint32_t> deck;
	};
	std::vector<Hidden> hidden;
	size_t hidden_at = 0;
};

enum class DecisionType : uint8_t {
	None, Idle, Battle, EffectYesNo, YesNo, Option, Card, Chain, Place, Position,
	Tribute, Counter, Sum, Sort, UnselectCard, AnnounceRace, AnnounceAttrib,
	AnnounceCard, AnnounceNumber, RockPaperScissors
};
const char* decision_name(DecisionType t);

// What an option does, used by the rollout policy and evaluator.
enum class OptKind : uint8_t {
	Pass, Summon, SpSummon, Reposition, MSet, SSet, Activate, ToBattle, ToMain2, ToEnd,
	Attack, Select, Yes, No, Choose, Shuffle
};

struct Option {
	OptKind kind = OptKind::Choose;
	uint32_t code = 0;         // card involved, if any
	uint8_t con = 0, loc = 0;  // where that card is
	uint32_t seq = 0;
	uint64_t desc = 0;         // effect description id
	std::vector<uint8_t> response;
	std::string label;
	// The cards a selection picks (Card, Tribute and Sum choices): codes, locations and sequences.
	std::vector<uint32_t> codes;
	std::vector<uint8_t> locs;
	std::vector<uint32_t> seqs;
};

struct Decision {
	DecisionType type = DecisionType::None;
	uint8_t player = 0;
	uint64_t hint = 0;           // last HINT_SELECTMSG for this player
	bool minor = false;          // zone/position/sort style choice: resolved by policy, not branched
	std::vector<Option> options;
};

struct CardView {
	uint32_t code = 0;
	uint8_t con = 0;
	uint8_t loc = 0;
	uint32_t seq = 0;
	uint32_t pos = 0;
	uint32_t type = 0;
	int32_t atk = 0, def = 0;
	uint32_t level = 0, rank = 0, link = 0;
	uint32_t status = 0;
	bool is_public = false;
	uint8_t owner = 0;
	std::vector<uint32_t> overlay;
};

struct Board {
	std::array<int32_t, 2> lp{8000, 8000};
	uint32_t turn = 0;
	uint8_t turn_player = 0;
	uint16_t phase = 0;
	// [player] -> cards in each location
	std::array<std::vector<CardView>, 2> hand, mzone, szone, grave, banished, extra, deck;
	// Lasting effects on each player shown as hints (descriptions, e.g. "Affected by Dominus
	// Spark: cannot activate the effects of EARTH, ... monsters").
	std::array<std::vector<uint64_t>, 2> hints;
	int count_field(uint8_t p) const;
};

// Notable things that happened while processing, used to explain lines.
struct Event {
	enum Kind : uint8_t { Chaining, Negated, SpSummoned, Summoned, Set, Destroyed, Draw, NewTurn, NewPhase, Win } kind;
	uint8_t player = 0;
	uint32_t code = 0;
	uint32_t value = 0;
};

/** What the rules core's card and script readers need. */
struct DuelReaders {
	const CardDB* db;
	ScriptCache* scripts;
};

class Duel {
public:
	Duel(const CardDB& db, ScriptCache& scripts);
	~Duel();
	Duel(const Duel&) = delete;
	Duel& operator=(const Duel&) = delete;

	// Builds the duel from setup and plays it until the first decision.
	bool start(const DuelSetup& setup, std::string* error = nullptr);
	// Runs a piece of Lua in the duel (sets globals for kProbeCardCode's script). False on an error.
	bool run_lua(const std::string& code);
	// Applies one response; processes until the next decision. Returns false if the core rejected it.
	bool respond(const std::vector<uint8_t>& response);
	// Rebuilds a state: start(setup) then respond() each entry. False if any step fails.
	bool replay(const DuelSetup& setup, const std::vector<std::vector<uint8_t>>& responses, std::string* error = nullptr);

	const Decision& decision() const { return decision_; }
	bool ended() const { return ended_; }
	int winner() const { return winner_; }  // 0/1, 2 = draw, -1 = not ended
	uint32_t turn() const { return turn_; }
	uint8_t turn_player() const { return turn_player_; }
	uint16_t phase() const { return phase_; }
	const std::vector<Event>& events() const { return events_; }
	// Human-readable log (only collected when enabled; "[P0]"/"[P1]" stand for the players).
	void enable_log(bool on) { log_enabled_ = on; }
	std::vector<std::string> take_log() { auto out = std::move(log_); log_.clear(); return out; }
	// Who each log line is about (0/1, -1 = nobody), in step with take_log(). Take both together.
	std::vector<int8_t> take_log_players() { auto out = std::move(log_players_); log_players_.clear(); return out; }
	// A card moving between places, for animations (only collected with the log).
	struct Move {
		uint32_t code;
		uint8_t from_con, from_loc, to_con, to_loc;
		uint32_t from_seq, from_pos, to_seq, to_pos;
	};
	std::vector<Move> take_moves() { auto out = std::move(moves_); moves_.clear(); return out; }
	// A card or effect being activated, for the activation effect (only collected with the log).
	// `after` = how many of the moves taken together with it happened before the activation.
	struct Activation {
		uint32_t code;
		uint8_t con, loc;
		uint32_t seq, pos;
		uint32_t after;
	};
	std::vector<Activation> take_activations() { auto out = std::move(activations_); activations_.clear(); return out; }
	void clear_events() { events_.clear(); }
	size_t steps() const { return steps_; }
	// Links on the chain being built (0 = an open window: a phase change, after a summon...).
	int chain_depth() const { return chain_depth_; }
	// Who activated the last link on the chain (-1 when empty).
	int chain_last_player() const { return chain_depth_ ? chain_last_player_ : -1; }
	// Whose play the last link is: its controller, or the player who put that card on the
	// controller's field (Angelechy Seneschal summoned to the opponent's field by Opposition: its
	// forced effect is part of the summoning player's combo, not the opponent's answer). -1 when empty.
	int chain_last_owner() const { return chain_depth_ ? chain_last_owner_ : -1; }
	// A card in either deck cares about columns (Angelechy, Infinite Impermanence): zone choices
	// are real decisions, with a few distinct zones offered. Otherwise the first free zone.
	bool zone_choices() const { return zone_choices_; }
	// The card of the last link on the chain (script id, 0 when empty).
	uint32_t chain_last_code() const { return chain_depth_ ? chain_last_code_ : 0; }
	// Where the card of the last link was activated from (LOCATION_*, 0 when empty).
	uint8_t chain_last_loc() const { return chain_depth_ ? chain_last_loc_ : 0; }

	Board board(bool include_decks = false) const;
	const CardDB& db() const { return db_; }
	// Cards each player is known to play, used to restrict "declare a card name" choices.
	void set_declare_pool(std::vector<uint32_t> pool) { declare_pool_ = std::move(pool); }
	// Every raw message batch the rules core sends, as it comes (null: none). For following recorded duels.
	void set_tap(std::function<void(const uint8_t*, uint32_t)> tap) { tap_ = std::move(tap); }
	// Puts these Main Deck cards (script ids) on top of `player`'s Deck, codes[0] topmost.
	void set_deck_top(uint8_t player, const std::vector<uint32_t>& codes);
	// See DuelSetup::Hidden.
	void set_hidden(const DuelSetup::Hidden& h);
	// Cards that went into a hand since the last call, for what the other player knows:
	// drawn (the other player doesn't see them) or shown to the other player (MSG_CONFIRM_CARDS).
	struct HandNews {
		uint8_t player;  // whose hand
		uint32_t code;   // script id
		bool drawn;      // false: shown
	};
	std::vector<HandNews> take_hand_news() { auto out = std::move(hand_news_); hand_news_.clear(); return out; }

private:
	void process();
	void parse_messages(const uint8_t* data, uint32_t len);
	void build_decision(uint8_t msg, const uint8_t* p, const uint8_t* end);
	std::string card_label(uint32_t code, uint64_t desc = 0) const;

	const CardDB& db_;
	ScriptCache& scripts_;
	// What the core's card and script readers get (their "payload"): this duel's own, so a duel
	// can be continued from any thread (it used to be per thread, see duel.cpp).
	DuelReaders readers_{&db_, &scripts_};
	void* duel_ = nullptr;
	std::function<void(const uint8_t*, uint32_t)> tap_;
	Decision decision_;
	bool ended_ = false;
	bool retry_ = false;
	int winner_ = -1;
	uint32_t turn_ = 0;
	uint8_t turn_player_ = 0;
	uint16_t phase_ = 0;
	std::array<uint64_t, 2> last_hint_{0, 0};
	std::vector<Event> events_;
	std::vector<HandNews> hand_news_;
	bool log_enabled_ = false;
	std::vector<std::string> log_;
	std::vector<int8_t> log_players_;
	std::vector<Move> moves_;
	std::vector<Activation> activations_;
	void push_log(int player, std::string line) {
		log_.push_back(std::move(line));
		log_players_.push_back(static_cast<int8_t>(player < 2 ? player : -1));
	}
	std::vector<uint32_t> chain_codes_;  // codes on the current chain, for "negated" lines
	std::vector<uint8_t> chain_players_;  // controller of each chain link
	void log_message(uint8_t type, const uint8_t* p, const uint8_t* end);
	std::vector<uint32_t> declare_pool_;
	std::array<std::vector<uint64_t>, 2> player_hints_;
	int chain_depth_ = 0;
	int chain_last_player_ = -1;
	int chain_last_owner_ = -1;
	// Some card in the duel cares about columns: zones are a real choice (else the first free one).
	bool zone_choices_ = false;
	// field slots (controller << 16 | location << 8 | sequence) holding a card another player put there
	std::map<uint32_t, uint8_t> given_;
	uint32_t chain_last_code_ = 0;
	uint8_t chain_last_loc_ = 0;
	size_t steps_ = 0;
};

// Loads scripts from the CardScripts checkout, keeping file contents in memory.
class ScriptCache {
public:
	// `scripts_dir` is either a CardScripts checkout or a folder holding scripts.pack
	// (built by tools/make_pack.py), which is what the Android app ships.
	explicit ScriptCache(std::string scripts_dir, bool bytecode = true);
	// Returns the script text, or nullptr when no such script exists.
	const std::string* get(const std::string& name);
	// Adds a script that isn't in the scripts folder (kept as source; one of that name stays).
	void put(const std::string& name, std::string text);
	bool has_card_script(uint32_t code) { return get("c" + std::to_string(code) + ".lua") != nullptr; }

private:
	std::string dir_;
	bool bytecode_;
	std::mutex mu_;
	struct PackEntry {
		uint64_t offset = 0;
		uint32_t size = 0;
	};
	std::unordered_map<std::string, PackEntry> pack_;
	std::string pack_path_;
	bool read_source(const std::string& name, std::string& out);
	std::unordered_map<std::string, std::unique_ptr<std::string>> cache_;
};

// Encoding helpers for responses.
std::vector<uint8_t> resp_i32(int32_t v);
std::string loc_name(uint8_t loc);

}  // namespace ygoe
