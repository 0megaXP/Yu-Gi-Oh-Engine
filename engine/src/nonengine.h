// Non-engine: the cards a deck plays besides its own combo (hand traps, board breakers,
// floodgates, interrupting traps), what each is worth in a matchup, and when to use them.
//
//  1. Classification: each card of a deck is engine (its archetype's cards and the cards they
//     need) or non-engine, with a role and what it stops. From the script analysis (profile.h),
//     no card special-cased.
//  2. Timing: when a bot can use a non-engine card on the opponent's play, a learned model
//     (data/nonengine_timing.json, trained by tools/ml/nonengine.py from self-play) estimates how
//     much using it now rather than holding it changes the chance to win. The bot adds that to
//     what its search sees (the search can't see the better moment a card is being held for).
//  3. Values: how much each non-engine card changes the win rate in a matchup, going first and
//     going second (data/nonengine_values.json, measured from self-play: the games where it was
//     drawn early against those where it wasn't).
//  4. Choice: the non-engine slots for a deck across the format's matchups, and per-matchup
//     siding (side.h blends the measured values into its plans).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "carddb.h"
#include "deck.h"

namespace ygoe {

struct Board;

enum class NeRole : uint8_t { Engine, HandTrap, Interrupt, BoardBreaker, Floodgate, Staple };
const char* ne_role_name(NeRole r);

// What a non-engine card stops (bits): the Interrupt kinds of profile.h.
enum : uint32_t {
	kStopsActivation = 1, kStopsEffect = 2, kStopsSummon = 4, kRemoves = 8, kStopsGy = 16, kStopsDeck = 32,
	kDraws = 64, kOther = 128,
};
std::vector<std::string> stops_names(uint32_t bits);
struct CardRole;
// What a card stops, from its role (profile.h).
uint32_t ne_stops(const CardRole& r);

struct NeCard {
	uint32_t code = 0;  // as in the deck list (official passcode)
	NeRole role = NeRole::Engine;
	uint32_t stops = 0;
	bool going_second = false;  // mostly useful going second (board breakers, Mulcharmy cards)
	int copies = 0;             // in the Main Deck
	int side_copies = 0;        // in the Side Deck
	bool engine() const { return role == NeRole::Engine; }
};

// The deck's archetype: its most played archetype code (0 if none dominates).
uint16_t archetype_key(const CardDB& db, const Deck& deck);
// Every distinct card of the Main and Side Deck, classified.
std::vector<NeCard> classify_nonengine(const CardDB& db, const Deck& deck);
NeCard classify_card(const CardDB& db, uint32_t code, const std::vector<uint16_t>& deck_sets);

// ---------------------------------------------------------------- timing (2)

// The situation when a bot could use a non-engine card on the opponent's play.
struct NeSituation {
	double search_gain = 0;    // the search's average with the card used minus held
	int their_plays = 0;       // the opponent's activations this turn so far
	bool their_turn = true;
	bool deck_play = false;    // the chain's top card searches / summons / sends from the Deck
	bool choke = false;        // a known choke point of their deck (replays)
	int my_interruptions = 0;  // hand traps in hand and interrupting Sets
	int turn = 1;
	int their_hand = 0, their_field = 0, my_field = 0;
	int chain_depth = 0;
	uint32_t stops = 0;        // the card's NeCard::stops
	bool hand_trap = false;
};
std::vector<float> ne_features(const NeSituation& s);
constexpr int kNeFeatures = 20;

// P(win) if the card is used now and if it is held: two logistic models on the same features.
class NeTiming {
public:
	bool parse(const std::string& json_text, std::string* err = nullptr);
	bool empty() const { return use_.empty(); }
	// How much using the card now rather than holding it changes the chance to win (-1..1).
	double gain(const NeSituation& s) const;

private:
	std::vector<float> use_, hold_;
};

// ---------------------------------------------------------------- values (3)

class NeValues {
public:
	struct Entry {
		double delta = 0;  // win rate when drawn early minus when not, -1..1
		int games = 0;     // games the card was in the deck
	};
	bool parse(const std::string& json_text, std::string* err = nullptr);
	bool empty() const { return table_.empty(); }
	// The measured value of `code` for a deck of archetype `me` against `opp` (0: against the
	// whole format), going first or second. Null when never measured.
	const Entry* get(uint16_t me, uint16_t opp, bool first, uint32_t script_id) const;

private:
	std::map<std::tuple<uint16_t, uint16_t, bool, uint32_t>, Entry> table_;
};

// ---------------------------------------------------------------- choice (4)

struct NeChoice {
	NeCard card;
	double value_first = 0, value_second = 0;  // average measured value over the opponents
	int games = 0;
	bool measured = false;                     // either
	bool measured_first = false, measured_second = false;
};
// The deck's non-engine cards (Main and Side Deck) ranked by their measured value against
// `opponents` (the format), going first and second averaged.
std::vector<NeChoice> rank_nonengine(const CardDB& db, const Deck& deck, const std::vector<Deck>& opponents);

}  // namespace ygoe
