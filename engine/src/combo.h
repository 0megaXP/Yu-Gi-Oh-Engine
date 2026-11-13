// Combo tree: the main line for a hand, and how to continue when the opponent interrupts it.
//
// For each hand trap the opponent is likely to hold, the main line is replayed with that card
// in their hand to find every point where they could use it (choke points). At each one the
// interruption is forced and the rest of the turn is searched again; the point that hurts
// most is what a good opponent would pick, and the best continuation from there is the branch.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "analyze.h"
#include "deck.h"
#include "pieces.h"

namespace ygoe {

struct ComboBranch {
	uint32_t hand_trap = 0;               // official passcode
	std::string hand_trap_name;
	bool stops_nothing = false;           // no point in the main line where it can be used
	std::string used_on;                  // your action it answers (the step before it)
	int used_at_step = -1;                // index into the main line's moves
	int choke_points = 0;                 // how many places it could have been used
	int hits_part = -1;                   // the main line's part it interrupts (ComboTree::main_parts)
	std::vector<std::string> continuation;  // from the interruption to the end of the turn
	std::vector<CardSummary> board;
	double value = 0;                     // deck-neutral board value (as reference boards)
	double score = 0;                     // 0..1, same scale as the main line's
};

struct ComboTree {
	std::vector<std::string> main_moves;
	std::vector<CardSummary> main_board;
	double main_value = 0, main_score = 0;
	std::vector<ComboPiece> main_parts;   // the main line split into sub-combos
	bool opponent_known = false;
	std::vector<ComboBranch> branches;    // most damaging first
	bool cancelled = false;
	std::string to_json(const CardDB& db) const;
};

struct ComboTreeConfig {
	int iterations = 1500;          // main line search
	int branch_iterations = 300;    // per choke point
	int max_hand_traps = 6;
	int max_choke_points = 4;       // per hand trap
	uint64_t seed = 1;
	std::shared_ptr<const PieceGuide> guide;  // your deck's combo pieces, if known
	std::vector<SearchConfig::Imitation> imitate;  // replay choices the engine doesn't understand yet
	std::shared_ptr<const class Policy> policy;  // learned move policy, if any
	const std::atomic<bool>* cancel = nullptr;
	std::function<void(const std::string& phase, int done, int total)> progress;
};

// `opponent` may be null: then the most played hand traps are used.
ComboTree build_combo_tree(const CardDB& db, ScriptCache& scripts, const Deck& mine, const std::vector<uint32_t>& hand,
                           const Deck* opponent, const ComboTreeConfig& cfg);

// The hand traps considered against `opponent` (by copies in Main + Side), or the default set.
std::vector<uint32_t> likely_hand_traps(const CardDB& db, const Deck* opponent, int max);

}  // namespace ygoe
