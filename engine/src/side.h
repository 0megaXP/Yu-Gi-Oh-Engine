// Side-deck suggestions for one matchup, separately for going first and going second.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "carddb.h"
#include "deck.h"
#include "duel.h"
#include "profile.h"

namespace ygoe {

// What the opponent's first-turn boards tend to contain, measured by simulation
// (see the `side` command). Shares are per board, averaged.
struct BoardShape {
	double monsters = 0;        // face-up monsters
	double st_faceup = 0;       // face-up cards in the Spell/Trap zone (incl. monsters placed there)
	double st_set = 0;          // set Spell/Trap cards
	double hand_traps = 0;      // interrupts left in hand
	double gy_interrupts = 0;   // quick effects available from the GY
	int samples = 0;
};

struct CardValue {
	uint32_t code = 0;
	double value = 0;
	std::string why;
};

struct SideSwap {
	uint32_t out = 0, in = 0;
	double gain = 0;
};

struct SidePlan {
	bool going_first = true;
	std::vector<SideSwap> swaps;
	std::vector<CardValue> side_values;   // every side card, best first
	std::vector<CardValue> flex_values;   // non-engine Main Deck cards, weakest first
};

// Plays `samples` first turns for `theirs` on proxy hands and averages what their boards contain.
BoardShape simulate_board_shape(const CardDB& db, ScriptCache& scripts, const Deck& theirs, const Deck& mine,
                                int samples, int iterations, const std::atomic<bool>* cancel = nullptr,
                                const std::function<void(int sample, const BoardShape& so_far, int monsters,
                                                         int st_faceup, int st_set)>& on_sample = {});

SidePlan plan_side(const CardDB& db, const Deck& mine, const Deck& theirs, const DeckProfile& opp,
                   const BoardShape& shape, bool going_first, int max_swaps = 8);

}  // namespace ygoe
