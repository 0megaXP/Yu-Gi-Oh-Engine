// Reference end boards: what a deck usually builds going first, found by the engine itself.
//
// Many random opening hands are searched with no opponent interaction ("goldfish"); the best
// line for each is kept with its end board and a deck-neutral board value. Any other board
// (a Play engine line, a duel) can then be placed in that distribution.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "analyze.h"
#include "deck.h"
#include "pieces.h"

namespace ygoe {

struct ReferenceSample {
	std::vector<uint32_t> hand;          // official passcodes
	double value = 0;                    // deck-neutral value of the end position
	int interrupts = 0;                  // interruptions the board holds
	std::vector<CardSummary> board;      // field + hand at the end of the turn
	std::vector<std::string> moves;
	std::vector<ComboPiece> pieces;      // the line split into sub-combos
};

struct ReferenceResult {
	std::string deck;
	int iterations = 0;                  // per hand
	std::vector<ReferenceSample> samples;  // best first
	std::vector<std::pair<uint32_t, double>> common_cards;  // field cards by share of boards
	std::vector<LibraryPiece> library;   // sub-combos merged across the samples
	std::string policy;                  // the move policy after learning from these lines (JSON), if one was given
	std::string to_json(const CardDB& db) const;
};

class Policy;
struct Scenario;

// With `policy`, the best line of every hand is also learned from (policy.h).
// With `boards` (real end boards: the positions of real games at the start of turn 2, follow
// "start" lines), a `second_share` of the hands is played going second against one of them:
// the deck's 6 cards against that board (the deck can attack), the line scored by the turn
// goal (a win this turn best, else the opponent's board broken and what is left); its pieces
// are marked `second`.
ReferenceResult build_reference(const CardDB& db, ScriptCache& scripts, const Deck& deck, int hands, int iterations,
                                uint64_t seed, const std::atomic<bool>* cancel = nullptr,
                                const std::function<void(int done, int total)>& progress = {}, Policy* policy = nullptr,
                                const std::vector<Scenario>* boards = nullptr, double second_share = 0.4);

// The real end boards to go second against: the "start" lines (follow_replay) at turn 2 whose
// player going first left something on the field. At most `max`.
std::vector<Scenario> load_end_boards(const std::string& starts_path, size_t max = 400);

// The same deck-neutral value for any board (player `p`, going into the opponent's turn).
double board_value(const CardDB& db, const Deck& deck, const Board& b, uint8_t p);

}  // namespace ygoe
