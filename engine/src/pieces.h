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
#include <string>
#include <vector>

#include "carddb.h"
#include "duel.h"
#include "search.h"

namespace ygoe {

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

}  // namespace ygoe
