// Monte Carlo tree search over real duel states.
//
// Each iteration rebuilds a duel (Duel::replay), walks the tree choosing actions with UCB,
// plays the rest of the turn with a cheap policy, and scores the position with the
// Evaluator. Decisions are keyed by what they mean ("Activate X", "Select Y") rather than
// by index, so one tree is shared across determinizations of hidden information
// (single-observer information-set MCTS).
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "deck.h"
#include "duel.h"
#include "evaluate.h"

namespace ygoe {

// One concrete state to search from: a duel setup plus the responses already played.
struct RootState {
	DuelSetup setup;
	std::vector<std::vector<uint8_t>> history;
};

struct Step {
	uint8_t player = 0;
	DecisionType type = DecisionType::None;
	std::string label;
	bool minor = false;
};

struct Trajectory {
	size_t root = 0;                          // index into the RootState list
	std::vector<std::vector<uint8_t>> responses;  // after the root's history
	std::vector<Step> steps;
	double reward = 0;
};

struct SearchConfig {
	int iterations = 4000;
	double seconds = 30;
	int threads = 0;              // 0 = hardware concurrency
	int horizon_turns = 1;        // stop when this many new turns have started
	double exploration = 0.6;
	int max_rollout_steps = 600;
	int keep_trajectories = 6;    // best trajectories kept per first move
};

struct MoveStats {
	std::string label;
	int visits = 0;
	double mean = 0;
	std::vector<Trajectory> best;  // best complete trajectories through this move
};

struct SearchOutput {
	std::vector<MoveStats> moves;     // answers to the root decision, most visited first
	std::vector<MoveStats> my_moves;  // grouped by the searching player's first real action, most played first
	int iterations = 0;
	double seconds = 0;
	int failures = 0;
	uint8_t root_player = 0;
	DecisionType root_type = DecisionType::None;
};

class Searcher {
public:
	Searcher(const CardDB& db, ScriptCache& scripts, const Evaluator& eval, std::vector<uint32_t> declare_pool);
	// `me` maximises; the other side minimises (adversarial on open information).
	SearchOutput run(const std::vector<RootState>& roots, uint8_t me, const SearchConfig& cfg,
	                 const std::function<void(int, double)>& progress = {});
	// Replays a trajectory and returns the board where it stopped.
	bool final_board(const RootState& root, const Trajectory& t, Board& out, Duel** keep = nullptr);

private:
	const CardDB& db_;
	ScriptCache& scripts_;
	const Evaluator& eval_;
	std::vector<uint32_t> pool_;
};

// Labels of the non-trivial steps a player took, merged into readable lines.
// Steps by `player` are written plainly, the other side's with `other_prefix`.
std::vector<std::string> describe(const Trajectory& t, uint8_t player, bool include_other = false,
                                  const std::string& other_prefix = "Opp: ");
// The first non-trivial action `player` takes in a trajectory ("Pass" if none).
std::string first_action(const Trajectory& t, uint8_t player);

}  // namespace ygoe
