// Thin, replayable wrapper around one ygopro-core duel.
//
// ocgcore has no way to copy a running duel, so a game state is identified by
// (DuelSetup, list of responses given so far). Duel::replay() rebuilds any state
// deterministically from that pair; the search uses this to branch.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "carddb.h"

namespace ygoe {

class ScriptCache;

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
	int count_field(uint8_t p) const;
};

// Notable things that happened while processing, used to explain lines.
struct Event {
	enum Kind : uint8_t { Chaining, Negated, SpSummoned, Summoned, Set, Destroyed, Draw, NewTurn, NewPhase, Win } kind;
	uint8_t player = 0;
	uint32_t code = 0;
	uint32_t value = 0;
};

class Duel {
public:
	Duel(const CardDB& db, ScriptCache& scripts);
	~Duel();
	Duel(const Duel&) = delete;
	Duel& operator=(const Duel&) = delete;

	// Builds the duel from setup and plays it until the first decision.
	bool start(const DuelSetup& setup, std::string* error = nullptr);
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
	void clear_events() { events_.clear(); }
	size_t steps() const { return steps_; }

	Board board(bool include_decks = false) const;
	const CardDB& db() const { return db_; }
	// Cards each player is known to play, used to restrict "declare a card name" choices.
	void set_declare_pool(std::vector<uint32_t> pool) { declare_pool_ = std::move(pool); }

private:
	void process();
	void parse_messages(const uint8_t* data, uint32_t len);
	void build_decision(uint8_t msg, const uint8_t* p, const uint8_t* end);
	std::string card_label(uint32_t code, uint64_t desc = 0) const;

	const CardDB& db_;
	ScriptCache& scripts_;
	void* duel_ = nullptr;
	Decision decision_;
	bool ended_ = false;
	bool retry_ = false;
	int winner_ = -1;
	uint32_t turn_ = 0;
	uint8_t turn_player_ = 0;
	uint16_t phase_ = 0;
	std::array<uint64_t, 2> last_hint_{0, 0};
	std::vector<Event> events_;
	bool log_enabled_ = false;
	std::vector<std::string> log_;
	std::vector<uint32_t> chain_codes_;  // codes on the current chain, for "negated" lines
	void log_message(uint8_t type, const uint8_t* p, const uint8_t* end);
	std::vector<uint32_t> declare_pool_;
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
