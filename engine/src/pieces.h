// Combo pieces: a line split into sub-combos, each with what it needs and what it gives.
//
// A line is replayed action by action while tracking where every card came from (the
// starting hand, or the action that put it in the hand, on the field or in the GY). The
// line is then cut where the fewest cards carry over from one part to the next, so each
// part is a sub-combo: "with these cards (and the Normal Summon), this part gives you that".
// Replaying the line with hand traps in the opponent's hand also shows which of them can
// stop each part.
//
// Pieces from many lines of a deck are collected into a library (see reference.h).
#pragma once
#include <random>
#include <string>
#include <vector>

#include "carddb.h"
#include "duel.h"
#include "search.h"

namespace ygoe {

// Conditions a line was played under (ComboPiece::under, PieceResources::under).
enum : uint8_t {
	kUnderMulcharmy = 1,  // the opponent's Mulcharmy (Fuwalos, Purulia...): they draw when you summon
	kUnderDroll = 2,      // Droll & Lock Bird: no more adding cards from the Deck this turn
};
// The JSON names of those bits ("mulcharmy", "droll").
std::vector<std::string> under_names(uint8_t under);
uint8_t under_bits(const std::vector<std::string>& names);

struct PieceCard {
	uint32_t code = 0;    // script id (the JSON gives the official passcode)
	std::string where;    // "hand", "field", "gy", "banished"
};

struct ComboPiece {
	int first = 0, last = 0;               // action indices in the line (inclusive)
	std::vector<std::string> steps;        // one per action, as describe() writes them
	std::vector<uint32_t> needs_hand;      // starting-hand cards it uses
	std::vector<PieceCard> needs_earlier;  // cards earlier parts left for it
	bool normal_summon = false;            // uses the turn's Normal Summon
	std::vector<PieceCard> gives;          // cards it leaves for later parts
	std::vector<PieceCard> leaves;         // cards it leaves on the end board / in hand at the end
	// Hand traps that can respond during it (script id, first step of the piece they can answer),
	// earliest first.
	std::vector<std::pair<uint32_t, int>> stopped_by;
	// Played while the opponent's draw / search punisher was active (kUnder* bits): a line for
	// those turns, e.g. one that plays through Mulcharmy Fuwalos or Droll & Lock Bird.
	uint8_t under = 0;
	bool starter() const { return needs_earlier.empty(); }
};

struct LineSplit {
	int actions = 0;
	std::vector<ComboPiece> pieces;
	int piece_of(int action) const;  // -1 if out of range
};

// Splits `t` (played from `setup` with an empty history, player 0 going first). When
// `hand_traps` (official ids) is non-empty the line is replayed a second time with them in
// the opponent's hand to find which part each can stop.
LineSplit split_line(const CardDB& db, ScriptCache& scripts, const std::vector<uint32_t>& declare_pool,
                     const DuelSetup& setup, const Trajectory& t, const std::vector<uint32_t>& hand_traps);

// Pieces with the same needs, merged across lines (the best-valued example is kept).
struct LibraryPiece {
	ComboPiece piece;   // one example
	int count = 0;      // lines it appeared in
	double value = 0;   // average end-board value of those lines
	double best = 0;    // value of the line the example comes from
};

std::string pieces_json(const CardDB& db, const std::vector<ComboPiece>& pieces);
std::string library_json(const CardDB& db, const std::vector<LibraryPiece>& lib, int lines);
// Adds the pieces of one line (with its end value) to `lib`.
void add_to_library(std::vector<LibraryPiece>& lib, const LineSplit& split, double value);
void sort_library(std::vector<LibraryPiece>& lib, size_t max);
// Reads pieces back from pieces_json / library_json output (official ids → script ids).
std::vector<LibraryPiece> parse_library(const CardDB& db, const std::string& json_array);
namespace json { class Value; }
std::vector<LibraryPiece> parse_library(const CardDB& db, const json::Value& array);

// What a player has to work with right now: cards in hand, and everything else they can use
// (field, GY, banished). Script ids.
struct PieceResources {
	std::vector<uint32_t> hand;
	std::vector<uint32_t> other;
	bool normal_summon_used = false;
	uint8_t under = 0;  // conditions active right now (kUnder* bits)
};
PieceResources resources_of(const CardDB& db, const Board& b, uint8_t player);

// Chains library pieces into plans for a search to try first.
//
// A plan starts from a piece the current cards allow and keeps adding pieces whose needs are
// met by what the hand still holds plus what earlier pieces gave, so pieces found in different
// lines get recombined. The result is the labels of their steps, in order; the search boosts an
// option matching the next label (see SearchConfig::guide).
class PieceGuide {
public:
	// With `db`, the guide also learns from the pieces' steps which card searched which ("Strategy
	// Score → Angelechy Opposition (Deck)"): a plan can then start by searching a card a piece
	// needs that isn't in the hand.
	explicit PieceGuide(std::vector<LibraryPiece> lib, const CardDB* db = nullptr);
	bool empty() const { return lib_.empty(); }
	size_t size() const { return lib_.size(); }
	std::vector<std::string> sample(const PieceResources& r, std::mt19937_64& rng, int max_pieces = 5) const;
	// The pieces that could start right now (for display / debugging).
	std::vector<const ComboPiece*> playable(const PieceResources& r) const;

private:
	std::vector<LibraryPiece> lib_;
	struct Bridge {
		uint32_t from = 0, to = 0;  // the card used, the card it adds from the Deck
		std::string step;           // the step as the pieces wrote it
	};
	std::vector<Bridge> bridges_;
};

}  // namespace ygoe
